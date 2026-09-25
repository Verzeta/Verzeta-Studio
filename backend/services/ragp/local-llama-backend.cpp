// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file local-llama-backend.cpp
 * @brief Implementation of the sidecar-bridge RAGP backend. The former
 *        in-process engine (model loading, sampler, decode loops) was
 *        MOVED into the verzeta-inference sidecar; this file keeps the
 *        orchestration only: eager warm with loadSucceeded/loadFailed,
 *        classify = buildPrompt → sidecar complete(json_stop) →
 *        parseProviderContent, one-shot = sidecar complete. Contention-
 *        ledger instrumented (class=ragp/confirm, provider="sidecar")
 *        so the B.0 baseline has a direct local counterpart.
 * @layer Service (RAGP subsystem)
 * @dependencies InferenceSidecarHost (blocking bridge), RemoteBackend
 *               statics (prompt + parse), ContentionLedger, Qt6::Core.
 */

#include "local-llama-backend.h"

#include "../../utils/contention-ledger.h"
#include "../../utils/logger.h"
#include "../inference-sidecar-host.h"
#include "remote-ragp-backend.h"

#include <QThread>

#include <QFileInfo>
#include <QPromise>

namespace Ragp {

namespace detail {

/**
 * @brief RAII completion guard for a classify promise: if the queued
 *        worker invocation is discarded (backend destroyed → thread
 *        quits with the lambda still queued), the guard's destructor
 *        resolves the caller's future with an UNKNOWN classification
 *        instead of leaving it forever pending.
 */
struct ClassifyPromiseGuard {
    std::shared_ptr<QPromise<Classification>> promise;  ///< Promise to complete.
    bool resolved = false;  ///< Set once the worker has completed the promise.
    ~ClassifyPromiseGuard() {
        if (resolved || !promise)
            return;
        Classification c;
        c.source = QStringLiteral("local:sidecar:abandoned");
        c.confidence = 0.0;
        promise->addResult(c);
        promise->finish();
    }
};

/**
 * @brief RAII completion guard for a one-shot promise (empty text =
 *        could-not-determine on abandonment).
 */
struct OneShotPromiseGuard {
    std::shared_ptr<QPromise<QString>> promise;  ///< Promise to complete.
    bool resolved = false;                       ///< Set once the worker has completed the promise.
    ~OneShotPromiseGuard() {
        if (resolved || !promise)
            return;
        promise->addResult(QString());
        promise->finish();
    }
};


/**
 * @brief Worker-thread resident running the BLOCKING sidecar calls.
 *        One request at a time by construction (queued invocations on
 *        one thread); the sidecar host additionally serializes across
 *        all its callers. Never touches the backend object; results
 *        travel through the per-call QPromise and queued signal
 *        emissions bound with QPointer guards by the dispatch sites.
 */
class BridgeWorker : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Configures the worker's fixed inputs (set once before
     *        the thread starts; worker-thread reads only).
     * @param host      Sidecar host (QPointer-guarded per call).
     * @param modelPath GGUF this backend serves.
     */
    void configure(Verzeta::Infer::InferenceSidecarHost* host, const QString& modelPath) {
        m_host = host;
        m_modelPath = modelPath;
    }

  public slots:
    /**
     * @brief Eager model warm: loads the GGUF in the sidecar so the
     *        first classify doesn't pay the load. Reports through
     *        warmed(ok, reason).
     */
    void doWarm() {
        QString err;
        if (!m_host) {
            emit warmed(false, QStringLiteral("no inference sidecar attached"));
            return;
        }
        const bool ok = m_host->warmModelBlocking(QStringLiteral("ragp"), m_modelPath, &err);
        emit warmed(ok, err);
    }

    /**
     * @brief One classify: prompt → sidecar complete(json_stop) →
     *        parse. Resolves the promise exactly once.
     * @param req       Classification request (copied in).
     * @param guard     Promise guard (resolved exactly once; its RAII
     *                  fallback covers queue-discard on shutdown).
     * @param timeoutMs Wait budget for the sidecar call.
     */
    void doClassify(Ragp::Request req, std::shared_ptr<ClassifyPromiseGuard> guard, int timeoutMs) {
        auto& promise = guard->promise;
        guard->resolved = true;
        const quint64 ledgerId = ContentionLedger::begin(ContentionLedger::CallClass::Ragp,
                                                         QStringLiteral("sidecar"),
                                                         QFileInfo(m_modelPath).fileName());
        ContentionLedger::markDispatched(ledgerId);

        Classification result;
        result.source = QStringLiteral("local:sidecar");
        result.confidence = 0.0;

        QString err;
        QString text;
        if (m_host) {
            // wrapChatTemplate=true — the sidecar wraps the prompt
            // body in the model's own chat template, matching what
            // the remote path gets implicitly from its chat endpoint
            // (a bare body derails instruct models: the live LFM2.5
            // emitted "```"+EOG on 76 % of classify calls without it).
            text = m_host->completeBlocking(RemoteBackend::buildPrompt(req),
                                            m_modelPath,
                                            /*maxTokens=*/512,
                                            /*jsonStop=*/true,
                                            &err,
                                            timeoutMs,
                                            /*wrapChatTemplate=*/true);
        } else {
            err = QStringLiteral("no inference sidecar attached");
        }
        if (!err.isEmpty()) {
            qCWarning(verzetaUi) << "LocalLlamaBackend: sidecar classify failed:" << err;
            result.source = QStringLiteral("local:sidecar:error");
        } else {
            result = RemoteBackend::parseProviderContent(text, QStringLiteral("local:sidecar"));
        }
        ContentionLedger::end(ledgerId, result.confidence > 0.0, result.source);
        promise->addResult(result);
        promise->finish();
    }

    /**
     * @brief One generic one-shot completion (the intent-confirm
     *        workload). Empty result signals could-not-determine,
     *        matching the remote backend's contract.
     * @param prompt    Full prompt text.
     * @param guard     Promise guard (resolved exactly once; RAII
     *                  fallback resolves empty on queue-discard).
     * @param timeoutMs Wait budget for the sidecar call.
     */
    void doOneShot(QString prompt, std::shared_ptr<OneShotPromiseGuard> guard, int timeoutMs) {
        auto& promise = guard->promise;
        guard->resolved = true;
        const quint64 ledgerId = ContentionLedger::begin(ContentionLedger::CallClass::Confirm,
                                                         QStringLiteral("sidecar"),
                                                         QFileInfo(m_modelPath).fileName());
        ContentionLedger::markDispatched(ledgerId);

        QString err;
        QString text;
        if (m_host) {
            // Same chat-template treatment as classify (one-shot
            // confirms are bare prompt bodies too).
            text = m_host->completeBlocking(prompt,
                                            m_modelPath,
                                            /*maxTokens=*/16,
                                            /*jsonStop=*/false,
                                            &err,
                                            timeoutMs,
                                            /*wrapChatTemplate=*/true);
        } else {
            err = QStringLiteral("no inference sidecar attached");
        }
        if (!err.isEmpty()) {
            qCWarning(verzetaUi) << "LocalLlamaBackend: sidecar one-shot failed:" << err;
            text = QString();  // empty == could not determine
        }
        ContentionLedger::end(ledgerId, err.isEmpty());
        promise->addResult(text);
        promise->finish();
    }

  signals:
    /**
     * @brief Warm outcome (queued to the main thread).
     * @param ok     True when the model is loaded and resident.
     * @param reason Failure reason when @p ok is false.
     */
    void warmed(bool ok, const QString& reason);

  private:
    QPointer<Verzeta::Infer::InferenceSidecarHost> m_host;
    QString m_modelPath;
};

}  // namespace detail

LocalLlamaBackend::LocalLlamaBackend(QString modelPath,
                                     Verzeta::Infer::InferenceSidecarHost* host,
                                     QObject* parent)
    : QObject(parent), m_modelPath(std::move(modelPath)), m_host(host) {
    m_thread = std::make_unique<QThread>();
    m_thread->setObjectName(QStringLiteral("verzeta-ragp-bridge"));

    auto* worker = new detail::BridgeWorker();
    worker->configure(host, m_modelPath);
    worker->moveToThread(m_thread.get());
    m_worker = worker;
    connect(m_thread.get(), &QThread::finished, worker, &QObject::deleteLater);

    // Warm outcome → availability + the load signals the cascade's
    // auto-fallback already consumes (behavioral parity with the old
    // in-process engine's eager load).
    connect(worker, &detail::BridgeWorker::warmed, this, [this](bool ok, const QString& reason) {
        m_available.store(ok, std::memory_order_release);
        if (ok) {
            qCInfo(verzetaUi) << "LocalLlamaBackend: model warmed in sidecar ("
                              << QFileInfo(m_modelPath).fileName() << ")";
            emit loadSucceeded();
        } else {
            emit loadFailed(reason);
        }
    });

    m_thread->start();
    QMetaObject::invokeMethod(worker, &detail::BridgeWorker::doWarm, Qt::QueuedConnection);
}

LocalLlamaBackend::~LocalLlamaBackend() {
    m_shuttingDown.store(true, std::memory_order_release);
    if (m_thread) {
        m_thread->quit();
        // Bounded by the sidecar-call timeout: a worker blocked inside
        // the host returns as soon as the request resolves/times out
        // (and immediately on host teardown, which fail-releases every
        // waiter). Unbounded join is therefore deterministic here.
        m_thread->wait();
    }
}

QFuture<Classification> LocalLlamaBackend::classifyAsync(const Request& req) {
    auto promise = std::make_shared<QPromise<Classification>>();
    promise->start();
    QFuture<Classification> future = promise->future();

    if (m_shuttingDown.load(std::memory_order_acquire) || !m_worker) {
        Classification c;
        c.source = QStringLiteral("local:sidecar:shutdown");
        c.confidence = 0.0;
        promise->addResult(c);
        promise->finish();
        return future;
    }
    detail::BridgeWorker* worker = m_worker.data();
    const Request reqCopy = req;
    const int timeoutMs = m_classifyTimeoutMs;
    auto guard = std::make_shared<detail::ClassifyPromiseGuard>();
    guard->promise = promise;
    QMetaObject::invokeMethod(
        worker,
        [worker, reqCopy, guard, timeoutMs]() { worker->doClassify(reqCopy, guard, timeoutMs); },
        Qt::QueuedConnection);
    return future;
}

QFuture<QString> LocalLlamaBackend::oneShotCompleteAsync(const QString& prompt) {
    auto promise = std::make_shared<QPromise<QString>>();
    promise->start();
    QFuture<QString> future = promise->future();

    if (m_shuttingDown.load(std::memory_order_acquire) || !m_worker) {
        promise->addResult(QString());
        promise->finish();
        return future;
    }
    detail::BridgeWorker* worker = m_worker.data();
    const QString promptCopy = prompt;
    const int timeoutMs = m_classifyTimeoutMs;
    auto guard = std::make_shared<detail::OneShotPromiseGuard>();
    guard->promise = promise;
    QMetaObject::invokeMethod(
        worker,
        [worker, promptCopy, guard, timeoutMs]() {
            worker->doOneShot(promptCopy, guard, timeoutMs);
        },
        Qt::QueuedConnection);
    return future;
}

bool LocalLlamaBackend::isAvailable() const {
    return m_available.load(std::memory_order_acquire) && m_host && m_host->isAvailable();
}

QString LocalLlamaBackend::backendName() const {
    // Historical contract (pinned by tests + consumed in logs/cache
    // keys/fallback sources): "local:llama.cpp[:<model filename>]".
    const QString base = QStringLiteral("local:llama.cpp");
    const QString file = QFileInfo(m_modelPath).fileName();
    return file.isEmpty() ? base : base + QLatin1Char(':') + file;
}

}  // namespace Ragp

#include "local-llama-backend.moc"

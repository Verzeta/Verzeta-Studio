// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file inference-sidecar-host.cpp
 * @brief Implementation of InferenceSidecarHost + its IO-thread worker.
 *        One request is in flight at a time (callers serialize through
 *        a mutex; the sidecar itself is sequential), correlated by id;
 *        a caller that times out abandons its shared request state and
 *        a late response is dropped by id matching. Spawn is lazy;
 *        three consecutive spawn failures latch the host unavailable.
 * @layer Service
 * @dependencies Qt6::Core (QProcess on the IO thread), Verzeta::Infer
 *               codec, QCoreApplication::applicationDirPath().
 */

#include "inference-sidecar-host.h"

#include "../inference/infer-protocol.h"
#include "../utils/thread-discipline.h"

#include <QThread>

#include <memory>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSemaphore>

// Default level QtInfoMsg: keep genuine lifecycle milestones (sidecar
// spawned) at Info, but the piped sidecar stderr echo is at Debug and off
// by default. Enable with QT_LOGGING_RULES="verzeta.infer.debug=true".
Q_LOGGING_CATEGORY(verzetaInfer, "verzeta.infer", QtInfoMsg)

namespace Verzeta::Infer {

namespace {

/** Consecutive spawn failures after which the host latches
 *  unavailable (a missing/broken binary must degrade once, quietly,
 *  never with a spawn retry storm). Reset by setBinaryPathOverride(). */
constexpr int kMaxConsecutiveSpawnFailures = 3;

/**
 * @brief Shared state of one blocking request. Held by shared_ptr from
 *        both the waiting caller and the IO worker so a timed-out
 *        caller can abandon it safely (the worker's late fill then
 *        just expires with the last reference).
 */
struct PendingRequest {
    quint64 id = 0;       ///< Frame id for response correlation
    QJsonObject request;  ///< Request header to send
    QSemaphore done;      ///< Released exactly once on outcome
    // Outcome (valid after done.release()):
    bool ok = false;
    QString error;
    QJsonObject response;  ///< Response header
    QByteArray blob;       ///< Response blob (embed vectors)
    // Streaming (chat) requests: no waiter blocks on `done` — the
    // outcome travels through the worker's stream signals instead.
    bool isStream = false;
    quint64 hostStreamId = 0;
};
using PendingPtr = std::shared_ptr<PendingRequest>;

}  // namespace

/**
 * @brief IO-thread resident: owns the QProcess (correct Qt affinity),
 *        the frame reader, per-slot loaded-model tracking, and the
 *        single in-flight request queue. All slots run on the IO
 *        thread; the ONLY cross-thread touch is done.release() on a
 *        PendingRequest, which QSemaphore permits by design.
 */
class SidecarIoWorker : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Queues one request; starts it immediately when idle.
     *        Invoked via QueuedConnection from caller threads.
     * @param req Shared request state (outcome filled + semaphore
     *            released exactly once).
     */
    void submit(PendingPtr req) {
        m_queue.append(std::move(req));
        pumpQueue();
    }

    /**
     * @brief Test/config override of the sidecar binary path; resets
     *        the unavailable latch.
     * @param path Absolute path to the sidecar executable.
     */
    void setBinaryOverride(const QString& path) {
        m_binaryOverride = path;
        m_spawnFailures = 0;
    }

    /**
     * @brief Availability latch read (plain int read; ordered by the
     *        caller mutex for its consumers).
     * @returns True after repeated consecutive spawn failures.
     */
    bool latchedUnavailable() const { return m_spawnFailures >= kMaxConsecutiveSpawnFailures; }

    /**
     * @brief Shutdown on the IO thread BEFORE the thread quits:
     *        fail every queued/pending request, then stop the child
     *        (stdin EOF → bounded waitForFinished → terminate/kill).
     */
    void shutdown() {
        failAll(QStringLiteral("host shutting down"));
        stopProcess();
    }

  private:
    /** @brief Resolves the sidecar binary path (override or beside
     *         the application executable). */
    QString binaryPath() const {
        if (!m_binaryOverride.isEmpty())
            return m_binaryOverride;
        QString name = QStringLiteral("verzeta-inference");
#ifdef Q_OS_WIN
        name += QStringLiteral(".exe");
#endif
        return QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
    }

    /** @brief Ensures the process is running and greeted; spawns
     *         lazily. Returns false with @p err set on failure. */
    bool ensureProcess(QString* err) {
        if (m_proc && m_proc->state() == QProcess::Running)
            return true;
        if (latchedUnavailable()) {
            if (err)
                *err = QStringLiteral(
                    "the local inference engine failed to start several times and is now disabled");
            return false;
        }
        // A fresh process invalidates every loaded-slot assumption.
        m_loadedEmbedPath.clear();
        m_loadedRagpPath.clear();
        m_loadedChatPath.clear();
        m_loadedChatCtx = 0;
        m_reader.clear();

        if (!m_proc) {
            m_proc = new QProcess(this);  // IO-thread affinity
            connect(m_proc, &QProcess::readyReadStandardOutput, this, &SidecarIoWorker::onStdout);
            connect(m_proc, &QProcess::readyReadStandardError, this, &SidecarIoWorker::onStderr);
            connect(m_proc, &QProcess::finished, this, &SidecarIoWorker::onFinished);
        }
        const QString bin = binaryPath();
        if (!QFileInfo::exists(bin)) {
            ++m_spawnFailures;
            if (err)
                *err = QStringLiteral("local inference engine not found: %1").arg(bin);
            return false;
        }
        m_proc->start(bin, QStringList{});
        if (!m_proc->waitForStarted(5000)) {
            ++m_spawnFailures;
            if (err)
                *err = QStringLiteral("local inference engine failed to start: %1")
                           .arg(m_proc->errorString());
            return false;
        }
        m_spawnFailures = 0;
        qCInfo(verzetaInfer) << "InferenceSidecarHost: spawned" << bin << "pid"
                             << m_proc->processId();
        return true;
    }

    /** @brief Sends one encoded frame to the child's stdin. */
    bool sendFrame(const QJsonObject& header, QString* err) {
        const QByteArray bytes = encodeInferFrame(header);
        if (bytes.isEmpty()) {
            if (err)
                *err = QStringLiteral("frame exceeds protocol cap");
            return false;
        }
        if (m_proc->write(bytes) != bytes.size()) {
            if (err)
                *err = QStringLiteral("pipe write failed");
            return false;
        }
        return true;
    }

    /** @brief Starts the next queued request when none is in flight.
     *         Handles the implicit load_model step when the request's
     *         gguf differs from the slot's loaded model. */
    void pumpQueue() {
        if (m_current || m_queue.isEmpty())
            return;
        m_current = m_queue.takeFirst();

        QString err;
        if (!ensureProcess(&err)) {
            finishCurrent(false, err);
            // Fail the rest too — same terminal condition, no storms.
            if (latchedUnavailable())
                failAll(err);
            pumpQueue();
            return;
        }

        // Implicit slot load when the caller's model differs.
        const QString slot = m_current->request.value(QStringLiteral("__slot")).toString();
        const QString gguf = m_current->request.value(QStringLiteral("__gguf")).toString();
        const int nCtx = m_current->request.value(QStringLiteral("__nctx")).toInt(0);
        QString& loaded =
            (slot == QLatin1String("embed"))
                ? m_loadedEmbedPath
                : (slot == QLatin1String("chat") ? m_loadedChatPath : m_loadedRagpPath);
        const bool ctxChanged =
            (slot == QLatin1String("chat") && nCtx > 0 && nCtx != m_loadedChatCtx);
        if (!gguf.isEmpty() && (gguf != loaded || ctxChanged)) {
            QJsonObject load;
            load.insert(QStringLiteral("op"), QStringLiteral("load_model"));
            load.insert(QStringLiteral("id"), static_cast<qint64>(nextFrameId()));
            load.insert(QStringLiteral("slot"), slot);
            load.insert(QStringLiteral("path"), gguf);
            if (slot == QLatin1String("chat") && nCtx > 0) {
                load.insert(QStringLiteral("n_ctx"), nCtx);
            }
            m_loadInFlightFor = gguf;
            m_loadInFlightCtx = nCtx;
            m_awaitingLoad = true;
            if (!sendFrame(load, &err)) {
                m_awaitingLoad = false;
                finishCurrent(false, err);
                pumpQueue();
            }
            return;  // real request follows once load_result arrives
        }
        dispatchCurrent();
    }

    /** @brief Sends the current request's real frame (post any load). */
    void dispatchCurrent() {
        QJsonObject header = m_current->request;
        header.remove(QStringLiteral("__slot"));
        header.remove(QStringLiteral("__gguf"));
        header.remove(QStringLiteral("__nctx"));
        m_current->id = nextFrameId();
        header.insert(QStringLiteral("id"), static_cast<qint64>(m_current->id));
        QString err;
        if (!sendFrame(header, &err)) {
            finishCurrent(false, err);
            pumpQueue();
        }
    }

    /** @brief Completes the in-flight request exactly once: via the
     *         semaphore for blocking callers, via the stream signals
     *         for streaming requests. */
    void finishCurrent(bool ok,
                       const QString& error,
                       const QJsonObject& response = QJsonObject(),
                       const QByteArray& blob = QByteArray()) {
        if (!m_current)
            return;
        if (m_current->isStream) {
            emit streamFinished(m_current->hostStreamId,
                                ok,
                                response.value(QStringLiteral("finish")).toString(),
                                response.value(QStringLiteral("tokens")).toInt(0),
                                error);
            m_current.reset();
            return;
        }
        m_current->ok = ok;
        m_current->error = error;
        m_current->response = response;
        m_current->blob = blob;
        m_current->done.release();
        m_current.reset();
    }

    /** @brief Fails the in-flight and every queued request. */
    void failAll(const QString& error) {
        finishCurrent(false, error);
        while (!m_queue.isEmpty()) {
            PendingPtr r = m_queue.takeFirst();
            if (r->isStream) {
                emit streamFinished(r->hostStreamId, false, QString(), 0, error);
                continue;
            }
            r->ok = false;
            r->error = error;
            r->done.release();
        }
    }

    /** @brief Stops the child politely (EOF) with bounded escalation. */
    void stopProcess() {
        if (!m_proc || m_proc->state() == QProcess::NotRunning)
            return;
        m_proc->closeWriteChannel();  // stdin EOF = clean exit
        if (!m_proc->waitForFinished(2000)) {
            m_proc->terminate();
            if (!m_proc->waitForFinished(1000)) {
                m_proc->kill();
                m_proc->waitForFinished(500);
            }
        }
    }

    quint64 nextFrameId() {
        return ++m_frameId;
    }

  public:
    /**
     * @brief Cancels a live or queued stream by host id. Invoked via
     *        QueuedConnection from the main thread.
     * @param hostStreamId Host-side stream id.
     */
    void cancelStream(quint64 hostStreamId) {
        if (m_current && m_current->isStream && m_current->hostStreamId == hostStreamId) {
            QJsonObject c;
            c.insert(QStringLiteral("op"), QStringLiteral("cancel"));
            c.insert(QStringLiteral("id"), static_cast<qint64>(m_current->id));
            QString err;
            (void)sendFrame(c, &err);  // best-effort; poll lands it
            return;
        }
        for (int i = 0; i < m_queue.size(); ++i) {
            if (m_queue[i]->isStream && m_queue[i]->hostStreamId == hostStreamId) {
                PendingPtr r = m_queue.takeAt(i);
                emit streamFinished(hostStreamId, true, QStringLiteral("cancelled"), 0, QString());
                return;
            }
        }
    }

  signals:
    /**
     * @brief One stream delta (relayed to the host's public signal).
     * @param hostStreamId Host-side stream id.
     * @param delta        Generated text piece.
     */
    void streamChunk(quint64 hostStreamId, const QString& delta);

    /**
     * @brief Terminal stream outcome (exactly once per stream).
     * @param hostStreamId Host-side stream id.
     * @param ok           False on transport/slot failure.
     * @param finishReason "stop"|"length"|"cancelled" when ok.
     * @param tokens       Generated piece count when ok.
     * @param error        Reason when not ok.
     */
    void streamFinished(quint64 hostStreamId,
                        bool ok,
                        const QString& finishReason,
                        int tokens,
                        const QString& error);

    /**
     * @brief Acceleration reported by a successful model load. The
     *        sidecar derives it from the ggml device table, so it is
     *        what ACTUALLY runs ("gpu (<device>)" or "cpu"), never a
     *        compile-time guess.
     * @param slot  "embed" | "ragp" | "chat".
     * @param accel The reported acceleration label.
     */
    void slotAccelerationReported(const QString& slot, const QString& accel);

  private slots:
    /** @brief Drains stdout frames; correlates responses by id. */
    void onStdout() {
        const bool streamOk = m_reader.feed(m_proc->readAllStandardOutput(),
                                            [this](const InferFrame& frame) { onFrame(frame); });
        if (!streamOk) {
            qCWarning(verzetaInfer) << "InferenceSidecarHost: protocol error — restarting";
            failAll(QStringLiteral(
                "the local inference engine sent an unreadable reply and was restarted"));
            stopProcess();
        }
    }

    /** @brief Relays the sidecar's stderr into our logger. */
    void onStderr() {
        const QList<QByteArray> lines = m_proc->readAllStandardError().split('\n');
        for (const QByteArray& l : lines) {
            if (!l.trimmed().isEmpty()) {
                qCDebug(verzetaInfer).noquote() << "[sidecar]" << QString::fromUtf8(l.trimmed());
            }
        }
    }

    /**
     * @brief Child exit: fail whatever was waiting; a later request
     *        respawns (subject to the failure latch).
     * @param exitCode Child process exit code.
     * @param status   Normal exit vs crash (logged).
     */
    void onFinished(int exitCode, QProcess::ExitStatus status) {
        if (m_shutdownSeen)
            return;
        qCWarning(verzetaInfer) << "InferenceSidecarHost: sidecar exited" << exitCode << "status"
                                << int(status);
        m_loadedEmbedPath.clear();
        m_loadedRagpPath.clear();
        m_loadedChatPath.clear();
        m_loadedChatCtx = 0;
        m_awaitingLoad = false;
        failAll(QStringLiteral("local inference engine stopped (exit code %1)").arg(exitCode));
    }

  private:
    /** @brief Handles one decoded frame from the sidecar. */
    void onFrame(const InferFrame& frame) {
        const QString op = frame.header.value(QStringLiteral("op")).toString();
        if (op == QLatin1String("hello"))
            return;  // greeting

        if (m_awaitingLoad) {
            // The implicit load_model step for m_current.
            m_awaitingLoad = false;
            if (op == QLatin1String("load_result") &&
                frame.header.value(QStringLiteral("ok")).toBool()) {
                const QString slot =
                    m_current ? m_current->request.value(QStringLiteral("__slot")).toString()
                              : QString();
                if (slot == QLatin1String("embed")) {
                    m_loadedEmbedPath = m_loadInFlightFor;
                } else if (slot == QLatin1String("chat")) {
                    m_loadedChatPath = m_loadInFlightFor;
                    m_loadedChatCtx = m_loadInFlightCtx;
                } else {
                    m_loadedRagpPath = m_loadInFlightFor;
                }
                emit slotAccelerationReported(
                    slot.isEmpty() ? QStringLiteral("ragp") : slot,
                    frame.header.value(QStringLiteral("accel")).toString());
                dispatchCurrent();
            } else {
                finishCurrent(false,
                              frame.header.value(QStringLiteral("error"))
                                  .toString(QStringLiteral("load failed")));
                pumpQueue();
            }
            return;
        }

        if (!m_current ||
            frame.header.value(QStringLiteral("id")).toVariant().toULongLong() != m_current->id) {
            return;  // late response for an abandoned request — drop
        }
        if (m_current->isStream && op == QLatin1String("chunk")) {
            emit streamChunk(m_current->hostStreamId,
                             frame.header.value(QStringLiteral("delta")).toString());
            return;  // stream continues; complete_result terminates it
        }
        const bool ok = frame.header.value(QStringLiteral("ok")).toBool(false);
        if (ok && op == QLatin1String("load_result")) {
            // Explicit load (warmModelBlocking) — same acceleration
            // report as the implicit-load path above.
            emit slotAccelerationReported(
                m_current->request.value(QStringLiteral("slot")).toString(),
                frame.header.value(QStringLiteral("accel")).toString());
        }
        finishCurrent(
            ok, frame.header.value(QStringLiteral("error")).toString(), frame.header, frame.blob);
        pumpQueue();
    }

  public:
    /** Set on the IO thread right before shutdown() so onFinished
     *  does not double-fail during the intentional stop. */
    bool m_shutdownSeen = false;

  private:
    QProcess* m_proc = nullptr;
    InferFrameReader m_reader;
    QList<PendingPtr> m_queue;
    PendingPtr m_current;
    quint64 m_frameId = 0;
    QString m_binaryOverride;
    int m_spawnFailures = 0;
    QString m_loadedEmbedPath;
    QString m_loadedRagpPath;
    QString m_loadedChatPath;
    int m_loadedChatCtx = 0;
    QString m_loadInFlightFor;
    int m_loadInFlightCtx = 0;
    bool m_awaitingLoad = false;
};

// ---------------------------------------------------------------------------
// InferenceSidecarHost — the blocking facade
// ---------------------------------------------------------------------------

namespace {
/** One mutex serializes all blocking callers (the sidecar is
 *  sequential anyway); function-local so tests constructing several
 *  hosts still behave (each host has its own worker; the shared mutex
 *  only orders callers, which is harmless). */
QMutex& callerMutex() {
    static QMutex m;
    return m;
}
}  // namespace

InferenceSidecarHost::InferenceSidecarHost(QObject* parent) : QObject(parent) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("verzeta-infer-host"));
    m_worker = new SidecarIoWorker();
    m_worker->moveToThread(m_ioThread);
    connect(m_ioThread, &QThread::finished, m_worker, &QObject::deleteLater);
    // Stream relays: worker (IO thread) → public signals (queued to
    // consumers' threads automatically).
    connect(m_worker, &SidecarIoWorker::streamChunk, this, &InferenceSidecarHost::chatStreamChunk);
    connect(m_worker,
            &SidecarIoWorker::streamFinished,
            this,
            &InferenceSidecarHost::chatStreamFinished);
    // Acceleration reports: cached on the main thread (queued delivery)
    // so slotAcceleration() is a cheap main-thread read for the UI.
    connect(m_worker,
            &SidecarIoWorker::slotAccelerationReported,
            this,
            [this](const QString& slot, const QString& accel) {
                if (accel.isEmpty())
                    return;
                if (slot == QLatin1String("embed")) {
                    m_embedAccel = accel;
                } else if (slot == QLatin1String("chat")) {
                    m_chatAccel = accel;
                } else {
                    m_ragpAccel = accel;
                }
                emit slotAccelerationChanged(slot, accel);
            });
    m_ioThread->start();
}

InferenceSidecarHost::~InferenceSidecarHost() {
    VERZETA_ASSERT_MAIN_THREAD();
    m_shuttingDown.store(true, std::memory_order_release);
    // Ordering contract: consumers' worker threads are already joined
    // (this host is declared BEFORE them in AppController), so no
    // caller can be waiting. Stop the child ON the IO thread, then
    // join the IO thread. The IO thread never blocks on main, so a
    // plain wait() cannot deadlock (the wire-bridge lesson).
    QMetaObject::invokeMethod(
        m_worker,
        [w = m_worker]() {
            w->m_shutdownSeen = true;
            w->shutdown();
        },
        Qt::BlockingQueuedConnection);
    m_ioThread->quit();
    m_ioThread->wait();
}

QString InferenceSidecarHost::slotAcceleration(const QString& slot) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (slot == QLatin1String("embed"))
        return m_embedAccel;
    if (slot == QLatin1String("chat"))
        return m_chatAccel;
    return m_ragpAccel;
}

void InferenceSidecarHost::setBinaryPathOverride(const QString& path) {
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker, path]() { w->setBinaryOverride(path); }, Qt::QueuedConnection);
}

bool InferenceSidecarHost::isAvailable() const {
    return !m_shuttingDown.load(std::memory_order_acquire) && m_worker &&
           !m_worker->latchedUnavailable();
}

QVector<float> InferenceSidecarHost::embedBlocking(const QString& text,
                                                   const QString& ggufPath,
                                                   QString* outModelId,
                                                   QString* outError,
                                                   int timeoutMs) {
    Q_ASSERT(QThread::currentThread() != QCoreApplication::instance()->thread());
    if (m_shuttingDown.load(std::memory_order_acquire)) {
        if (outError)
            *outError = QStringLiteral("host shutting down");
        return {};
    }
    QMutexLocker lock(&callerMutex());

    auto req = std::make_shared<PendingRequest>();
    req->request.insert(QStringLiteral("op"), QStringLiteral("embed"));
    req->request.insert(QStringLiteral("texts"), QJsonArray{text});
    req->request.insert(QStringLiteral("__slot"), QStringLiteral("embed"));
    req->request.insert(QStringLiteral("__gguf"), ggufPath);
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker, req]() { w->submit(req); }, Qt::QueuedConnection);

    if (!req->done.tryAcquire(1, timeoutMs)) {
        if (outError)
            *outError = QStringLiteral("sidecar embed timed out (%1 ms)").arg(timeoutMs);
        return {};
    }
    if (!req->ok) {
        if (outError)
            *outError = req->error;
        return {};
    }
    const int dim = req->response.value(QStringLiteral("dim")).toInt(0);
    if (dim <= 0 || req->blob.size() != static_cast<qsizetype>(dim * sizeof(float))) {
        if (outError)
            *outError = QStringLiteral("malformed embed result");
        return {};
    }
    if (outModelId) {
        *outModelId = req->response.value(QStringLiteral("model")).toString();
    }
    QVector<float> vec(dim);
    memcpy(vec.data(), req->blob.constData(), static_cast<size_t>(dim) * sizeof(float));
    return vec;
}

quint64 InferenceSidecarHost::startChatStream(
    const QString& prompt, const QString& ggufPath, int nCtx, int maxTokens, double temperature) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_shuttingDown.load(std::memory_order_acquire))
        return 0;
    const quint64 hostId = ++m_nextStreamId;

    auto req = std::make_shared<PendingRequest>();
    req->isStream = true;
    req->hostStreamId = hostId;
    req->request.insert(QStringLiteral("op"), QStringLiteral("complete_stream"));
    req->request.insert(QStringLiteral("prompt"), prompt);
    req->request.insert(QStringLiteral("max_tokens"), maxTokens);
    req->request.insert(QStringLiteral("temperature"), temperature);
    req->request.insert(QStringLiteral("__slot"), QStringLiteral("chat"));
    req->request.insert(QStringLiteral("__gguf"), ggufPath);
    req->request.insert(QStringLiteral("__nctx"), nCtx);
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker, req]() { w->submit(req); }, Qt::QueuedConnection);
    return hostId;
}

void InferenceSidecarHost::cancelChatStream(quint64 streamId) {
    if (streamId == 0 || !m_worker)
        return;
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker, streamId]() { w->cancelStream(streamId); }, Qt::QueuedConnection);
}

bool InferenceSidecarHost::warmModelBlocking(const QString& slot,
                                             const QString& ggufPath,
                                             QString* outError,
                                             int timeoutMs) {
    Q_ASSERT(QThread::currentThread() != QCoreApplication::instance()->thread());
    if (m_shuttingDown.load(std::memory_order_acquire)) {
        if (outError)
            *outError = QStringLiteral("host shutting down");
        return false;
    }
    QMutexLocker lock(&callerMutex());
    // A status request carrying __slot/__gguf rides the worker's
    // implicit-load step: the model loads (or is already resident),
    // then status returns — no inference happens.
    auto req = std::make_shared<PendingRequest>();
    req->request.insert(QStringLiteral("op"), QStringLiteral("status"));
    req->request.insert(QStringLiteral("__slot"), slot);
    req->request.insert(QStringLiteral("__gguf"), ggufPath);
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker, req]() { w->submit(req); }, Qt::QueuedConnection);
    if (!req->done.tryAcquire(1, timeoutMs)) {
        if (outError)
            *outError = QStringLiteral("model warm timed out (%1 ms)").arg(timeoutMs);
        return false;
    }
    if (!req->ok && outError)
        *outError = req->error;
    return req->ok;
}

QString InferenceSidecarHost::completeBlocking(const QString& prompt,
                                               const QString& ggufPath,
                                               int maxTokens,
                                               bool jsonStop,
                                               QString* outError,
                                               int timeoutMs,
                                               bool wrapChatTemplate) {
    Q_ASSERT(QThread::currentThread() != QCoreApplication::instance()->thread());
    if (m_shuttingDown.load(std::memory_order_acquire)) {
        if (outError)
            *outError = QStringLiteral("host shutting down");
        return QString();
    }
    QMutexLocker lock(&callerMutex());

    auto req = std::make_shared<PendingRequest>();
    req->request.insert(QStringLiteral("op"), QStringLiteral("complete"));
    req->request.insert(QStringLiteral("prompt"), prompt);
    req->request.insert(QStringLiteral("max_tokens"), maxTokens);
    req->request.insert(QStringLiteral("json_stop"), jsonStop);
    req->request.insert(QStringLiteral("chat_template"), wrapChatTemplate);
    req->request.insert(QStringLiteral("__slot"), QStringLiteral("ragp"));
    req->request.insert(QStringLiteral("__gguf"), ggufPath);
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker, req]() { w->submit(req); }, Qt::QueuedConnection);

    if (!req->done.tryAcquire(1, timeoutMs)) {
        if (outError)
            *outError = QStringLiteral("sidecar complete timed out (%1 ms)").arg(timeoutMs);
        return QString();
    }
    if (!req->ok) {
        if (outError)
            *outError = req->error;
        return QString();
    }
    // Non-null even when empty — "" is a valid completion outcome.
    QString text = req->response.value(QStringLiteral("text")).toString();
    text.detach();
    return text;
}

}  // namespace Verzeta::Infer

#include "inference-sidecar-host.moc"

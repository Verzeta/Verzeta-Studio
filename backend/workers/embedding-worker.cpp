// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file embedding-worker.cpp
 * @brief Implementation of EmbeddingWorker.
 *        OpenAI backend: synchronous HTTPS call to text-embedding-3-small.
 *        LlamaCpp backend: bridges through the verzeta-inference sidecar
 *        (the app's single llama.cpp integration; no llama code here).
 * @layer Worker
 * @dependencies HttpClient, Qt6::Core, Qt6::Network
 */


#include "embedding-worker.h"

#include "../utils/contention-ledger.h"
#include "utils/logger.h"

#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

namespace {
/** Per-request embedding timeout (ms). Bounds a hung endpoint so a RAG
 *  retrieval can never stall a turn. On timeout the request errors and the
 *  caller proceeds un-augmented. */
constexpr int kEmbedRequestTimeoutMs = 30000;
}  // namespace

/*
 * @brief Constructs an EmbeddingWorker.
 * @param parent Optional Qt parent.
 */
EmbeddingWorker::EmbeddingWorker(QObject* parent) : QObject(parent) {
    // The QNetworkAccessManager is created locally inside computeRemote() on
    // the worker thread (where this object lives after moveToThread). A member
    // NAM constructed here on the main thread would not move with the worker
    // (it has no parent) and would be used cross-thread — Qt then refuses to
    // create the reply's children. A worker-thread-local NAM avoids that.
}

EmbeddingWorker::~EmbeddingWorker() {}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/*
 * @brief Sets the embedding backend.
 * @param backend Target backend.
 */
void EmbeddingWorker::setBackend(EmbeddingBackend backend) {
    // Mutex-guarded: the backend can be switched from the main thread (live
    // settings change) while computeEmbedding runs on the worker thread.
    QMutexLocker lock(&m_configMutex);
    m_backend = backend;
}

EmbeddingWorker::EmbeddingBackend EmbeddingWorker::backend() const {
    QMutexLocker lock(&m_configMutex);
    return m_backend;
}

/*
 * @brief Sets the OpenAI API key.
 * @param key API key.
 */
void EmbeddingWorker::setApiKey(const QString& key) {
    QMutexLocker lock(&m_configMutex);
    m_apiKey = key;
}

/*
 * @brief Sets the llama.cpp embedding model path.
 * @param path Path to a GGUF embedding model.
 */
void EmbeddingWorker::setModelPath(const QString& path) {
    // Mutex-guarded — see setBackend(). computeLocal() snapshots it on the
    // worker thread and reloads the model if the path changed.
    QMutexLocker lock(&m_configMutex);
    m_modelPath = path;
}

void EmbeddingWorker::setSidecarHost(Verzeta::Infer::InferenceSidecarHost* host) {
    QMutexLocker lock(&m_configMutex);
    m_sidecarHost = host;
}

/*
 * @brief Sets the OpenAI-compatible embeddings endpoint base URL.
 *        Ignored when empty so the OpenAI default is preserved.
 * @param baseUrl Endpoint base URL.
 */
void EmbeddingWorker::setEndpointBaseUrl(const QString& baseUrl) {
    QMutexLocker lock(&m_configMutex);
    if (!baseUrl.isEmpty()) {
        m_endpointBaseUrl = baseUrl;
    }
}

/*
 * @brief Sets the embedding model name. Ignored when empty so the default
 *        (`text-embedding-3-small`) is preserved.
 * @param model Model identifier.
 */
void EmbeddingWorker::setModel(const QString& model) {
    QMutexLocker lock(&m_configMutex);
    if (!model.isEmpty()) {
        m_model = model;
    }
}

QString EmbeddingWorker::embeddingsUrl(const QString& baseUrl) {
    QString base = baseUrl;
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    if (base.endsWith(QStringLiteral("/embeddings"))) {
        return base;
    }
    return base + QStringLiteral("/embeddings");
}

// ---------------------------------------------------------------------------
// Public slots
// ---------------------------------------------------------------------------

/**
 * @brief Computes embedding for a single text chunk.
 * @param id         Identifier for tracking.
 * @param text       Text to embed.
 * @param sourceType "message" or "document".
 */
void EmbeddingWorker::computeEmbedding(const QString& id,
                                       const QString& text,
                                       const QString& /*sourceType*/) {
    // Honour stop requests before any work. requestStop() can be called
    // from the main thread during app shutdown.
    if (m_stopRequested.loadAcquire() != 0) {
        return;
    }
    if (text.isEmpty()) {
        emit errorOccurred(id, QStringLiteral("The text is empty, so there is nothing to embed."));
        return;
    }

    QVector<float> embedding;
    QString modelId;

    // Contention ledger — the embed runs synchronously on THIS worker
    // thread; begin/dispatch collapse to the same instant. Covers both
    // the single path and computeBatch (which loops through here).
    EmbeddingBackend backend;
    QString ledgerProvider;
    {
        QMutexLocker lock(&m_configMutex);
        backend = m_backend;
        if (backend == EmbeddingBackend::LlamaCpp) {
            ledgerProvider = QStringLiteral("local-gguf");
        } else {
            const QUrl u(m_endpointBaseUrl);
            ledgerProvider = u.host() + QLatin1Char(':') + QString::number(u.port(443));
        }
    }
    const quint64 ledgerId =
        ContentionLedger::begin(ContentionLedger::CallClass::Embed, ledgerProvider, QString());
    ContentionLedger::markDispatched(ledgerId);
    if (backend == EmbeddingBackend::LlamaCpp) {
        embedding = computeLocal(text, modelId);
    } else {
        embedding = computeRemote(text, modelId);
    }
    ContentionLedger::end(ledgerId, !embedding.isEmpty());

    // If stopped mid-call, don't emit — the receiver may already be gone.
    if (m_stopRequested.loadAcquire() != 0) {
        return;
    }
    if (embedding.isEmpty()) {
        emit errorOccurred(id, QStringLiteral("Failed to compute embedding for id: %1").arg(id));
    } else {
        emit embeddingReady(id, embedding, modelId);
    }
}

/**
 * @brief Computes embeddings for a batch of texts.
 * @param items List of (id, text, sourceType) tuples.
 */
void EmbeddingWorker::computeBatch(const QList<std::tuple<QString, QString, QString>>& items) {
    for (const auto& [id, text, sourceType] : items) {
        if (m_stopRequested.loadAcquire() != 0)
            break;
        computeEmbedding(id, text, sourceType);
    }
}

void EmbeddingWorker::requestStop() {
    // Thread-safe via QAtomicInt. Also aborts any in-flight HTTP reply
    // to unblock a pending QEventLoop inside computeRemote().
    m_stopRequested.storeRelease(1);
    if (m_currentReply) {
        // QNetworkReply::abort is callable from any thread; it triggers
        // the reply's finished() signal which unblocks the event loop.
        QMetaObject::invokeMethod(m_currentReply, "abort", Qt::QueuedConnection);
    }
}

// ---------------------------------------------------------------------------
// OpenAI backend
// ---------------------------------------------------------------------------

/**
 * @brief Synchronously calls the configured OpenAI-compatible embeddings
 *        endpoint (blocks the worker thread until the HTTP response).
 * @param text Input text.
 * @return Float32 vector, or empty on error.
 *
 * API: POST {endpointBaseUrl}/embeddings
 * Body: {"input":"<text>","model":"<model>"}
 * Response: {"data":[{"embedding":[...]}]}
 */
QVector<float> EmbeddingWorker::computeRemote(const QString& text, QString& outModelId) {
    // Snapshot the mutex-guarded HTTP config once (setters may run on the
    // main thread). All work below uses these locals.
    QString apiKey;
    QString baseUrl;
    QString model;
    {
        QMutexLocker lock(&m_configMutex);
        apiKey = m_apiKey;
        baseUrl = m_endpointBaseUrl;
        model = m_model;
    }
    // Report the exact model that produces this vector so the caller can
    // stamp the stored row (retrieval filters by model/dimension).
    outModelId = model;

    // Build request body
    QJsonObject body;
    body[QStringLiteral("input")] = text;
    body[QStringLiteral("model")] = model;
    const QByteArray bodyBytes = QJsonDocument(body).toJson(QJsonDocument::Compact);

    // Build HTTP request against the configured OpenAI-compatible endpoint.
    QNetworkRequest req{QUrl(embeddingsUrl(baseUrl))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Deterministic timeout (async safety, rule 9): a hung embedding endpoint
    // must NOT stall a turn forever. On timeout the reply errors out, the
    // QEventLoop below unblocks, and errorOccurred is emitted → RagService
    // delivers an empty retrieval and the turn proceeds un-augmented (N4).
    req.setTransferTimeout(kEmbedRequestTimeoutMs);
    // Bearer auth only when a key is configured — local endpoints
    // (Ollama / LM Studio / llama.cpp server) typically need none.
    if (!apiKey.isEmpty()) {
        req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());
    }

    // Synchronous HTTP call via QEventLoop (this runs on the worker thread).
    // The NAM is created here, on the worker thread, so the reply and its
    // children share the worker's thread affinity. Track the reply in
    // m_currentReply so requestStop() can abort it.
    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.post(req, bodyBytes);
    m_currentReply = reply;

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    m_currentReply.clear();

    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(verzetaRag) << "EmbeddingWorker: HTTP error" << reply->errorString();
        reply->deleteLater();
        return {};
    }

    const QByteArray responseData = reply->readAll();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson(responseData);
    const QJsonArray dataArr = doc.object()[QStringLiteral("data")].toArray();
    if (dataArr.isEmpty()) {
        qCWarning(verzetaRag) << "EmbeddingWorker: empty data array in response";
        return {};
    }

    const QJsonArray embArr = dataArr[0].toObject()[QStringLiteral("embedding")].toArray();

    QVector<float> result;
    result.reserve(embArr.size());
    for (const QJsonValue& v : embArr) {
        result.append(static_cast<float>(v.toDouble()));
    }
    return result;
}

// ---------------------------------------------------------------------------
// LlamaCpp backend (guarded)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Local backend — a pure BRIDGE to the verzeta-inference sidecar (the
// app's single llama.cpp integration). No llama code, no llama link,
// no model RAM in this process: the sidecar loads the GGUF lazily on
// the first request and keeps it resident while selected.
// ---------------------------------------------------------------------------

#include "../services/inference-sidecar-host.h"

QVector<float> EmbeddingWorker::computeLocal(const QString& text, QString& outModelId) {
    // Snapshot the mutex-guarded config (setters may run on the main
    // thread). All work below uses these locals.
    QString modelPath;
    Verzeta::Infer::InferenceSidecarHost* host = nullptr;
    {
        QMutexLocker lock(&m_configMutex);
        modelPath = m_modelPath;
        host = m_sidecarHost;
    }
    if (modelPath.isEmpty()) {
        qCWarning(verzetaRag) << "EmbeddingWorker: no local embedding model path set";
        return {};
    }
    if (!host) {
        qCWarning(verzetaRag) << "EmbeddingWorker: no inference sidecar attached — local"
                                 " embedding unavailable";
        return {};
    }
    QString err;
    QString modelId;
    const QVector<float> vec = host->embedBlocking(text, modelPath, &modelId, &err);
    if (vec.isEmpty()) {
        qCWarning(verzetaRag) << "EmbeddingWorker: sidecar embed failed:" << err;
        return {};
    }
    outModelId = modelId;
    return vec;
}

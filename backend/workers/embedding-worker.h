// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file embedding-worker.h
 * @brief Background worker computing vector embeddings for RAG.
 *        Supports local embedding (a GGUF served by the verzeta-
 *        inference sidecar, the app's single llama.cpp integration)
 *        or remote via any
 *        OpenAI-compatible /embeddings endpoint (OpenAI, Ollama, LM Studio,
 *        a llama.cpp server, …). The endpoint base URL and model are
 *        configurable. Designed to run in a QThread; configuration setters
 *        are mutex-guarded so they are safe to call from any thread.
 * @layer Worker
 * @dependencies Qt6::Network (QNetworkAccessManager), Qt6::Core; optional llama.cpp
 */


#pragma once

#include <QAtomicInt>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

namespace Verzeta::Infer {
class InferenceSidecarHost;
}

/**
 * @brief Background worker that computes dense vector embeddings for text chunks.
 *
 * ## Usage
 * 1. Move to a QThread.
 * 2. Call setBackend() + setApiKey() / setModelPath().
 * 3. Connect embeddingReady() signal to RagService slot.
 * 4. Call computeEmbedding() or computeBatch() via queued connection from RagService.
 *
 * ## Backend selection
 * - EmbeddingBackend::OpenAI (default): calls text-embedding-3-small (1536-dim).
 * - EmbeddingBackend::LlamaCpp: bridges to the verzeta-inference sidecar with a
 *   small GGUF model (e.g. all-minilm-l6-v2.gguf, 384-dim).
 *   Only available when VERZETA_ENABLE_LLAMACPP is defined.
 */
class EmbeddingWorker : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Selects the embedding computation backend.
     */
    enum class EmbeddingBackend {
        OpenAI,   ///< Remote via OpenAI embeddings API (text-embedding-3-small)
        LlamaCpp  ///< Local via llama.cpp embedding model (requires VERZETA_ENABLE_LLAMACPP)
    };
    Q_ENUM(EmbeddingBackend)

    /**
     * @brief Constructs an EmbeddingWorker with the OpenAI backend by default.
     * @param parent Optional Qt parent (should be nullptr when moving to QThread).
     */
    explicit EmbeddingWorker(QObject* parent = nullptr);
    ~EmbeddingWorker() override;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    /**
     * @brief Sets the embedding backend.
     * @param backend EmbeddingBackend::OpenAI or EmbeddingBackend::LlamaCpp.
     * @sideeffects Resets any active llama.cpp context when switching backends.
     */
    void setBackend(EmbeddingBackend backend);

    /**
     * @brief Returns the current backend.
     * @returns The currently-configured EmbeddingBackend
     *          (`EmbeddingBackend::OpenAI` or `EmbeddingBackend::LlamaCpp`).
     */
    EmbeddingBackend backend() const;

    /**
     * @brief Sets the OpenAI API key (used only by EmbeddingBackend::OpenAI).
     * @param key API key string.
     */
    void setApiKey(const QString& key);

    /**
     * @brief Sets the path to the local GGUF embedding model file.
     *        Used only by EmbeddingBackend::LlamaCpp.
     * @param path Absolute path to a GGUF embedding model.
     */
    void setModelPath(const QString& path);

    /**
     * @brief Attaches the inference-sidecar host the LlamaCpp backend
     *        bridges through. Non-owning; the host is AppController-
     *        owned and declared to outlive this worker's thread.
     *        Thread-safe (config mutex).
     * @param host Sidecar host; nullptr detaches (local embedding then
     *             degrades with an explicit error).
     */
    void setSidecarHost(Verzeta::Infer::InferenceSidecarHost* host);

    /**
     * @brief Sets the base URL of the OpenAI-compatible embeddings endpoint.
     *        Used only by EmbeddingBackend::OpenAI. Default
     *        `https://api.openai.com/v1`; set e.g. `http://localhost:11434/v1`
     *        for a local Ollama / LM Studio / llama.cpp server.
     * @param baseUrl Endpoint base URL (with or without a trailing
     *                `/embeddings`; see embeddingsUrl()). An empty value
     *                is ignored, so the current URL is kept. Thread-safe.
     */
    void setEndpointBaseUrl(const QString& baseUrl);

    /**
     * @brief Sets the embedding model name sent in the request body.
     *        Used only by EmbeddingBackend::OpenAI. Default
     *        `text-embedding-3-small`. Thread-safe.
     * @param model Model identifier understood by the configured endpoint.
     *              An empty value is ignored, so the current model is kept.
     */
    void setModel(const QString& model);

    /**
     * @brief Resolves the full embeddings URL from a base URL, idempotently.
     * @param baseUrl Endpoint base URL.
     * @returns `baseUrl` unchanged if it already ends with `/embeddings`,
     *          otherwise `baseUrl` (trailing slash trimmed) + `/embeddings`.
     *          Mirrors the image-worker path-join fix so a user pasting a full
     *          endpoint URL does not get the path doubled.
     */
    static QString embeddingsUrl(const QString& baseUrl);

  public slots:
    /**
     * @brief Computes the embedding vector for a single text chunk.
     * @param id         Unique identifier for tracking (e.g., message or document ID + chunk
     * index).
     * @param text       The text to embed.
     * @param sourceType Source context tag: "message" | "document".
     * @sideeffects Emits embeddingReady() on success or errorOccurred() on failure.
     *              Network call (OpenAI) or model inference (LlamaCpp).
     */
    void computeEmbedding(const QString& id, const QString& text, const QString& sourceType);

    /**
     * @brief Computes embeddings for multiple texts, one by one.
     * @param items List of (id, text, sourceType) tuples.
     * @sideeffects Emits embeddingReady() for each successful item.
     * @complexity O(n) calls to computeEmbedding().
     */
    void computeBatch(const QList<std::tuple<QString, QString, QString>>& items);

    /**
     * @brief Requests the worker to stop processing.
     *        Sets an atomic flag, aborts any in-flight HTTP reply,
     *        and causes computeBatch() to break out of its loop.
     *        Safe to call from any thread.
     */
    void requestStop();

  signals:
    /**
     * @brief Emitted when an embedding is successfully computed.
     * @param id        The id passed to computeEmbedding().
     * @param embedding Dense float32 vector.
     * @param modelId   Identifier of the model that produced this vector
     *                  (the configured remote model name, or the local model
     *                  file's basename). Lets the store record the exact model
     *                  per row so retrieval can filter by dimension/model.
     */
    void embeddingReady(const QString& id, const QVector<float>& embedding, const QString& modelId);

    /**
     * @brief Emitted when an embedding computation fails.
     * @param id    The id passed to computeEmbedding().
     * @param error Human-readable error description.
     */
    void errorOccurred(const QString& id, const QString& error);

  private:
    EmbeddingBackend m_backend = EmbeddingBackend::OpenAI;
    QString m_modelPath;
    QAtomicInt m_stopRequested{0};                 ///< Set by requestStop(); checked in hot loops
    QPointer<class QNetworkReply> m_currentReply;  ///< In-flight HTTP reply for abort()

    // HTTP backend config (apiKey + endpoint + model) is read on the worker
    // thread by computeRemote() but written by setters that may be called from
    // the main thread (e.g. on a settings change), so it is mutex-guarded.
    mutable QMutex m_configMutex;
    QString m_apiKey;  ///< guarded by m_configMutex
    QString m_endpointBaseUrl =
        QStringLiteral("https://api.openai.com/v1");             ///< guarded by m_configMutex
    QString m_model = QStringLiteral("text-embedding-3-small");  ///< guarded by m_configMutex

    /** Non-owning host of the ONE verzeta-inference sidecar process,
     *  the single llama.cpp integration. computeLocal() bridges through
     *  it; no llama code lives in this class (or this process) anymore.
     *  Guarded by m_configMutex (set from main, read on the worker). */
    Verzeta::Infer::InferenceSidecarHost* m_sidecarHost = nullptr;  ///< guarded by m_configMutex

    /**
     * @brief Local-embedding bridge: rides the configured GGUF through
     *        the inference sidecar (blocking on THIS worker thread).
     * @param text Input text to embed.
     * @param outModelId Set to the producing model id (file basename)
     *                   so stored rows keep their model_used semantics.
     * @return Float32 embedding vector, or empty on error (no host
     *         attached, sidecar unavailable, model load failure, …).
     */
    QVector<float> computeLocal(const QString& text, QString& outModelId);

    /**
     * @brief Calls the configured OpenAI-compatible embeddings endpoint
     *        synchronously (blocks the worker thread until the HTTP response).
     * @param text Input text to embed.
     * @param outModelId Set to the model name actually used for this request
     *                   (the configured remote model), so the caller can stamp
     *                   the stored row with the exact producing model.
     * @return Float32 embedding vector, or empty on error.
     */
    QVector<float> computeRemote(const QString& text, QString& outModelId);
};

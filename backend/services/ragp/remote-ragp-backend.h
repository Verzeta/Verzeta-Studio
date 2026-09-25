// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file remote-ragp-backend.h
 * @brief RAGP backend that classifies via the conversation's active
 *        remote LLM provider. Fully async: no nested event loops, no
 *        blocking on the main thread.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Network
 *
 * The call is non-blocking: classifyAsync() returns a QFuture
 * immediately, and the reply completes it. Timeouts are enforced by a
 * single-shot QTimer that aborts the reply and completes the future
 * with UNKNOWN.
 *
 * Provider support:
 *   - "ollama": Ollama's /api/generate with format=json.
 *   - Any other active provider (OpenAI, Anthropic, Gemini, OpenRouter,
 *     DeepSeek, remote llama.cpp, custom servers): a separate provider
 *     instance owned by the classifier sends one completion request.
 *   - With no active model, or while a classification is already in
 *     flight, the future completes with UNKNOWN.
 *
 * Threading:
 *   - classifyAsync MUST be called on the main thread
 *   - QNetworkAccessManager is created without a parent (m_net is a
 *     value member of RemoteBackend; a Qt parent would cause a
 *     double-free at teardown because value members are auto-destroyed)
 *   - Reply signals fire on the main thread; future completion runs
 *     on the main thread so caller's QFutureWatcher callback can
 *     safely mutate main-thread-only ChatController state.
 *
 * Lifetime invariants:
 *   - Pending QNetworkReply* objects are children of m_net (default
 *     ownership). When RemoteBackend is destroyed, m_net's destructor
 *     deletes all child replies in flight, which is safe.
 *   - If a reply fires after the backend is destroyed (e.g. racing),
 *     the connection is broken because the lambda captures a
 *     QPointer-guarded reference that null-checks.
 *   - The QPromise<Classification> is held in a std::shared_ptr so
 *     it survives even if RemoteBackend is destroyed before the
 *     reply completes, so the future the caller holds still resolves.
 */

#pragma once

#include "classifier-provider-factory.h"
#include "iragp-backend.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>

class QJsonObject;

class ModelRouter;
class SettingsService;

namespace Ragp {

/**
 * @brief RemoteBackend inherits QObject for proper signal wiring and
 *        as a safe parent for the QNetworkAccessManager.
 */
class RemoteBackend : public QObject, public IRagpBackend {
    Q_OBJECT

  public:
    /**
     * @brief Construct the remote backend.
     * @param router          Source of active provider id + model name.
     * @param settingsService Source of provider URLs (Ollama base URL etc.).
     * @param parent          Optional QObject owner for the backend itself.
     *                        m_net is parented to this RemoteBackend (not
     *                        to the caller) so value-member lifetime is
     *                        correct.
     */
    RemoteBackend(ModelRouter& router, SettingsService& settingsService, QObject* parent = nullptr);
    ~RemoteBackend() override;

    /**
     * @brief IRagpBackend: dispatch a classify via the active
     *        provider.
     * @param req Classification request.
     * @returns Future resolved with the Classification (or UNKNOWN on
     *          timeout / network error).
     */
    QFuture<Classification> classifyAsync(const Request& req) override;

    /**
     * @brief IRagpBackend: whether the active provider is reachable.
     * @returns true when the provider can be contacted.
     */
    bool isAvailable() const override;

    /**
     * @brief IRagpBackend: short identifier for diagnostics.
     * @returns Canonical name, e.g. "remote:ollama".
     */
    QString backendName() const override;

    /**
     * @brief IRagpBackend: one-shot generic completion on the active
     *        remote provider (the same instance @-mention classification
     *        uses). Resolves to the raw model text, empty on failure.
     * @param prompt Full user-role prompt.
     * @returns QFuture<QString> with the completion text or empty.
     */
    QFuture<QString> oneShotCompleteAsync(const QString& prompt) override;

    /**
     * @brief Override the HTTP timeout. Default 20000 ms.
     * @param ms New timeout in milliseconds.
     */
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }

    /**
     * @brief The current HTTP timeout.
     * @returns Timeout in milliseconds.
     */
    int timeoutMs() const { return m_timeoutMs; }

    /**
     * @brief Build the strict, minimal classification prompt.
     *        Exposed for unit tests that want to verify prompt shape.
     * @param req Classification request.
     * @returns Rendered prompt string.
     */
    static QString buildPrompt(const Request& req);

    /**
     * @brief Parse a raw JSON response body into a Classification.
     *        Returns an UNKNOWN-filled Classification on any parse
     *        error. Exposed for unit tests.
     * @param responseJson Raw response body from the provider.
     * @returns Parsed classification.
     */
    static Classification parseResponse(const QByteArray& responseJson);

    /**
     * @brief Parse the classifier's inner JSON object from accumulated
     *        provider-path content. Unlike parseResponse (which strips
     *        the Ollama /api/chat envelope first), this expects the
     *        model's own output text directly, because provider classes have
     *        already stripped their wire envelopes. Tolerates output
     *        wrapped in markdown code fences or surrounded by stray
     *        prose by extracting the outermost `{...}` object before
     *        parsing. Exposed for unit tests.
     * @param accumulatedContent Concatenated chunk deltas from a
     *                            provider-path classification stream.
     * @param sourceTag           Source label stamped on the result
     *                            (e.g. "remote:openai").
     * @returns Parsed classification; UNKNOWN-filled (confidence 0)
     *          when no parseable JSON object is present.
     */
    static Classification parseProviderContent(const QString& accumulatedContent,
                                               const QString& sourceTag);

  private:
    /**
     * @brief Fills targets / confidence / pending_action from a parsed
     *        classifier root object. This is the ONE schema reader shared by
     *        parseResponse and parseProviderContent so the two wire
     *        paths can never drift.
     * @param root   The classifier's outer JSON object.
     * @param result Classification to fill (source/latency untouched).
     */
    static void fillClassificationFromRoot(const QJsonObject& root, Classification& result);

    /**
     * @brief Initiate an async Ollama classification. The returned
     *        future completes on the main thread when the reply
     *        finishes or the timeout fires, whichever comes first.
     *
     *        This raw-HTTP path is retained for Ollama (rather than
     *        the generic provider path) because it carries latency-
     *        measured tuning: /api/chat session reuse, keep_alive 30m,
     *        think:false, format=json, HTTP/1.1 forced. Measured warm
     *        latency ~336 ms.
     */
    QFuture<Classification> classifyOllamaAsync(const Request& req);

    /**
     * @brief Initiate an async classification through a classifier-
     *        owned ILLMProvider instance for the active main provider.
     *        Covers every non-Ollama provider: openai, anthropic,
     *        gemini, openrouter, deepseek, llamacpp_remote and
     *        custom.\<slug\> OpenAI-compatible servers.
     *
     *        The provider instance is created lazily via
     *        `ensureClassifierProvider()` and rebuilt when the active
     *        provider id changes. Chunk deltas are accumulated; the
     *        terminal requestFinished / requestError signal resolves
     *        the future. A guaranteed QTimer::singleShot timeout (no
     *        QObject context) resolves with UNKNOWN if the provider
     *        neither finishes nor errors within `m_timeoutMs`,
     *        mirroring the Ollama path's lifetime guarantees.
     */
    QFuture<Classification> classifyViaProviderAsync(const Request& req);

    /**
     * @brief (Re)build `m_cls` for the current active provider when
     *        missing or stale (provider id changed). Returns false
     *        with `whyNot` populated when construction is impossible
     *        (e.g. missing API key).
     * @param whyNot Receives the human-readable unavailability reason.
     * @returns true iff m_cls.valid() after the call.
     */
    bool ensureClassifierProvider(QString* whyNot);

    /**
     * @brief Return an immediately-ready future with an UNKNOWN result.
     *        Used when the provider is unsupported or missing config.
     */
    QFuture<Classification> makeUnknownFuture(const QString& source) const;

    ModelRouter& m_router;
    SettingsService& m_settings;
    QNetworkAccessManager* m_net;  ///< Child QObject, parented to `this`
    int m_timeoutMs = 20000;

    /** Classifier-owned provider bundle for the generic (non-Ollama)
     *  path. Rebuilt on active-provider change. The bundle's members
     *  are unique_ptrs; destruction order inside the struct (provider
     *  first, then HttpClient) is dependency-correct by declaration
     *  order. */
    ClassifierProvider m_cls;

    /** Single-fire guard: true while a provider-path classification is
     *  in flight on m_cls.provider. ILLMProvider instances are
     *  single-request; a second classify during flight returns
     *  UNKNOWN("remote:busy") rather than corrupting the stream.
     *  RAGP calls are serialized by the cascade in practice, so this
     *  guard is defensive rather than load-bearing. */
    bool m_clsBusy = false;
};

}  // namespace Ragp

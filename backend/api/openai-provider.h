// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file openai-provider.h
 * @brief LLM provider adapter for the OpenAI API.
 *        Supports GPT-4o and other chat models with SSE streaming, tool calling,
 *        and vision (multimodal image input).
 * @layer API
 * @dependencies HttpClient (Utility), ILLMProvider (API)
 */

#pragma once

#include "../services/chat/inline-thinking-hoister.h"
#include "../utils/http-client.h"
#include "llm-interface.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Forward declaration
struct ToolSchema;

/**
 * @brief LLM provider adapter for the OpenAI API.
 *
 * Endpoint: POST /v1/chat/completions
 * Auth: Authorization: Bearer {api_key}
 * Streaming: Server-Sent Events (SSE), one JSON object per "data:" line
 * Finish: "data: [DONE]" sentinel
 *
 * Default model list (not fetched dynamically, since it requires an API key for /v1/models):
 *   gpt-6-astra, gpt-6-sol, gpt-6-luna, gpt-5.5, gpt-5.4
 */
class OpenAIProvider : public ILLMProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the OpenAI provider.
     * @param http Reference to a shared HttpClient instance.
     * @param parent Optional Qt parent.
     */
    explicit OpenAIProvider(HttpClient& http, QObject* parent = nullptr);

    /**
     * @brief Sets the OpenAI API key.
     * @param key API key string. Never logged.
     */
    void setApiKey(const QString& key);

    /**
     * @brief Sets the base URL (allows pointing to OpenAI-compatible proxies).
     * @param url Base URL (default: "https://api.openai.com/v1").
     */
    void setBaseUrl(const QString& url);

    // -----------------------------------------------------------------------
    // ILLMProvider interface
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the provider id used by ModelRouter for dispatch.
     * @returns "openai".
     */
    QString providerId() const override;

    /**
     * @brief Human-readable name shown in the UI provider selector.
     * @returns "OpenAI".
     */
    QString displayName() const override;

    /**
     * @brief Returns the cached list of OpenAI model identifiers.
     * @returns Model name strings (e.g. "gpt-4o", "gpt-4o-mini").
     */
    QStringList availableModels() override;

    /**
     * @brief Refreshes the model list via GET /v1/models.
     *
     * Requires a configured API key. Emits modelsRefreshed() with the
     * returned ids on success, or leaves the cached list unchanged on
     * failure.
     */
    void refreshModels() override;

    /**
     * @brief Reports SSE streaming support.
     * @returns true. OpenAI's Chat Completions API streams via SSE.
     */
    bool supportsStreaming() const override;

    /**
     * @brief Reports function/tool calling support.
     * @returns true. GPT-4 and GPT-3.5-turbo support the tools schema.
     */
    bool supportsToolCalling() const override;

    /**
     * @brief Reports image-input support.
     * @returns true. gpt-4o and gpt-4-turbo accept image_url content parts.
     */
    bool supportsVision() const override;

    /**
     * @brief Initiates a streaming POST /v1/chat/completions call.
     * @param req  Complete LLM request payload (messages + config + tools).
     *
     * Asynchronous. It communicates via chunkReceived / requestFinished /
     * requestError signals.
     */
    void sendRequest(const LlmRequest& req) override;

    /**
     * @brief Aborts the in-flight HTTP request, if any.
     *
     * Safe to call when no request is active.
     */
    void cancelRequest() override;

    /**
     * @brief Resolves a model's max context window.
     * @param model Model identifier.
     * @returns The model's max context window in tokens, or 0 when unknown.
     *
     * Resolution order:
     *   1. The live cache (`m_ctxCache`), populated opportunistically when a
     *      model-list refresh response carries a `context_length` /
     *      `max_context_length` field (OpenRouter's /api/v1/models, LM Studio's
     *      /api/v0/models). Hosted OpenAI's /v1/models carries no such field,
     *      so its cache stays empty.
     *   2. The published-family map via `publishedContextWindow()` (the
     *      OpenAI hosted-model map in the base class; overridden by
     *      Anthropic-less subclasses such as DeepSeek / OpenRouter heuristic).
     *
     * Subclasses with a dedicated live endpoint (OpenAICompat /api/v0,
     * LlamaCppRemote /props) override this method to add their own
     * single-flight async warm path.
     */
    int contextWindowFor(const QString& model) override;

    /**
     * @brief Reports whether this provider requires a non-empty API
     *        key for the chat-completion dispatch path.
     * @returns true for OpenAI (and any subclass that does not
     *          override); false for subclasses targeting auth-less
     *          servers when no key has been configured.
     *
     * Called from `sendRequest` before the request is built; when
     * this returns true AND `m_apiKey` is empty, the dispatch is
     * short-circuited with a `requestError("...API key not
     * configured")` signal.  When it returns false, the request
     * proceeds and `buildHeaders()` decides whether to emit an
     * Authorization header.
     *
     * Default: `true` (matches OpenAI's actual API contract).
     * Subclasses targeting servers that may be keyless
     * (LlamaCppRemoteProvider against a local llama.cpp server, and
     * OpenAICompatProvider for custom OpenAI-compatible servers) override to
     * return `false` when no key is configured.
     */
    virtual bool requiresApiKey() const;

  protected:
    /// Protected (not private) so OpenAI-compatible subclasses
    /// (OpenRouter / DeepSeek / LlamaCppRemote) can override defaults
    /// in their constructor and access the key + URL when building
    /// per-provider request quirks. The base URL defaults to OpenAI;
    /// subclasses overwrite via setBaseUrl() in their ctor.
    HttpClient& m_http;
    QString m_apiKey;  ///< API key; empty when none is configured. Never logged.
    QString m_baseUrl = QStringLiteral(
        "https://api.openai.com/v1");  ///< API base URL, without a trailing endpoint.

    /**
     * @brief model id → context window (tokens) cache.
     *
     * Populated by the base model-list response handler when an entry carries
     * a `context_length` (OpenRouter) or `max_context_length` (LM Studio
     * /api/v0) field, and by subclass live-endpoint warm queries. Empty for
     * hosted OpenAI (whose /v1/models has no context field). Shared with all
     * subclasses (protected). Main-thread-only.
     */
    QHash<QString, int> m_ctxCache;

    /// Model ids returned by availableModels(). Starts with a built-in
    /// list and is replaced by a successful refreshModels().
    QStringList m_models = {
        QStringLiteral("gpt-6-astra"),
        QStringLiteral("gpt-6-sol"),
        QStringLiteral("gpt-6-luna"),
        QStringLiteral("gpt-5.5"),
        QStringLiteral("gpt-5.4"),
    };

    /**
     * @brief Builds headers including Authorization: Bearer.
     * @returns Map of header name → value. API key never logged.
     *
     * Declared `virtual` so OpenAI-compatible subclasses can append
     * provider-specific headers (OpenRouter's HTTP-Referer + X-Title,
     * LlamaCppRemote's optional bearer for proxied servers, etc.).
     * The base implementation returns Authorization only; subclass
     * overrides typically call the base + add to the map.
     */
    virtual QMap<QString, QString> buildHeaders() const;

    /**
     * @brief Whether a model is one of OpenAI's reasoning models, which
     *        accept only their default sampling values.
     * @param model Model id sent in the request.
     * @returns true for the o-series, GPT-5 and GPT-6 (except chat variants)
     *          when this provider is the OpenAI API itself.
     */
    bool isOpenAiReasoningModel(const QString& model) const;

    /**
     * @brief Whether a listed model id can take chat completions.
     * @param id Model id from the list endpoint.
     * @returns false for the OpenAI API's embedding, speech, image,
     *          moderation, realtime and Responses-only models; true for
     *          every id on other providers.
     */
    bool isChatModel(const QString& id) const;

    /**
     * @brief Returns the published-family context window for @p model.
     * @param model Model identifier.
     * @returns Context window in tokens from the provider's published map, or
     *          0 when the family is unknown.
     *
     * Pure, synchronous, no network. The base implementation encodes OpenAI's
     * hosted-model map: gpt-4o / gpt-4.1 / o3 / o4 / gpt-4-turbo → 128000;
     * gpt-3.5 → 16385; gpt-4-32k → 32768; plain gpt-4 → 8192; any other
     * (modern) id → 128000. Subclasses override:
     *   - DeepSeekProvider: always 131072.
     *   - OpenRouterProvider: embedded-family heuristic (llama-3 → 131072,
     *     qwen → 32768, …) used only as a fallback; the live cache is primary.
     *   - OpenAICompatProvider / LlamaCppRemoteProvider: return 0 (no
     *     meaningful published map; they rely solely on their live endpoint).
     */
    virtual int publishedContextWindow(const QString& model) const;

    /**
     * @brief Hook called from `buildRequestBody` near the end so
     *        subclasses can inject provider-specific thinking /
     *        reasoning control parameters driven by
     *        `req.config.thinkingMode` without overriding the full
     *        body assembly.
     * @param root  Mutable reference to the in-progress request
     *              body.  The hook may add top-level keys or mutate
     *              existing ones.
     * @param req   The full request being dispatched.  Subclasses
     *              read `req.config.thinkingMode` for the toggle
     *              state and `req.config.modelName` for any model
     *              gating their parameter requires.
     *
     * Base implementation (OpenAIProvider) gates on model name, because
     * OpenAI's `reasoning_effort` parameter is only accepted by the
     * o1 / o3 reasoning families; sending it to gpt-4o etc. would
     * cause the API to reject the request.  When the model matches,
     * the toggle maps `thinkingMode=true` → `"high"` and `false` →
     * `"low"`; `"low"` is sent rather than omitting the field so
     * the wire shape is consistent across both toggle states.
     *
     * Overriders in the family:
     *   - `OpenRouterProvider`: adds standardised `reasoning:
     *     {effort: ...}` block; OpenRouter routes the effort hint
     *     to the matched upstream provider per its abstraction.
     *   - `LlamaCppRemoteProvider` and `OpenAICompatProvider`: add
     *     `chat_template_kwargs: {enable_thinking: ...}`, the
     *     standardised hand-off for HuggingFace-tokenizer-style
     *     chat templates (Qwen 3, DeepSeek-R1, gpt-oss-thinking
     *     etc. all read this kwarg).  Servers and templates that
     *     don't reference the kwarg silently ignore it.
     *   - `DeepSeekProvider`: no override.  `deepseek-reasoner`
     *     always thinks server-side; `deepseek-chat` does not.
     *     The toggle has no per-request effect for DeepSeek.
     */
    virtual void applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const;

    /**
     * @brief Hook called from `buildRequestBody` after the `messages`
     *        array is assembled (and before it is stored on the body)
     *        so subclasses can rewrite it to satisfy a stricter server.
     * @param messages  Mutable reference to the assembled OpenAI
     *                  `messages` array. The hook may merge / re-role
     *                  entries but MUST preserve their text content.
     *
     * Base implementation is a NO-OP: OpenAI / OpenRouter / DeepSeek /
     * llama.cpp accept Verzeta's multi-agent history verbatim, so their
     * wire shape is unchanged. `OpenAICompatProvider` overrides this to
     * normalise the array for strict chat templates (e.g. LM Studio's
     * Qwen MLX template, which rejects mid-conversation `system` turns
     * and consecutive same-role turns). See that override for the rules.
     */
    virtual void normalizeMessages(QJsonArray& messages) const;

  protected:
    /// Model-list requests use their own client so a refresh never cancels
    /// a chat stream in flight on m_http.
    HttpClient m_modelsHttp;

  private:
    /**
     * @brief One tool call being assembled from streamed deltas, keyed by
     *        the delta's `index` so several calls in one response stay
     *        separate.
     */
    struct PendingToolCall {
        QString id;
        QString name;
        QString args;
    };
    QMap<int, PendingToolCall> m_pendingToolCalls;

    /// API function name (letters, digits, _ and -) to the app's tool name,
    /// filled from the tools sent with each request.
    QHash<QString, QString> m_toolNames;

    /**
     * @brief Per-stream extractor that pulls inline `\<think\>...\</think\>`
     *        blocks out of `delta.content` before the rest of the
     *        parser sees the text.  Reset at sendRequest so leftover
     *        state from a partial prior stream does not bleed into
     *        the next.
     *
     *        Captures the inline-`\<think\>` case for any OpenAI-
     *        compatible upstream whose model emits thinking inline
     *        rather than through a separate channel, as is typical for
     *        Qwen 3 served by llama.cpp server with `--jinja` (the
     *        `LlamaCppRemoteProvider` path), vLLM without
     *        `--reasoning-parser`, and LM Studio with reasoning
     *        models loaded.  Shared across all four
     *        `OpenAIProvider` subclasses via inheritance as the single
     *        source of truth for inline thinking extraction across
     *        the OpenAI-family parser.
     *
     *        The separate channels (`delta.reasoning` for vLLM /
     *        Qwen / openrouter-normalised, `delta.reasoning_content`
     *        for DeepSeek-style) are captured alongside the
     *        hoister's output and both land in the same emitted
     *        `LlmChunk::thinkingDelta`.
     */
    Chat::InlineThinkingHoister m_thinkingHoister;

    /**
     * @brief Empty-stream safety net.
     *
     * True iff requestFinished() has been emitted for the current in-flight
     * request. Set whenever requestFinished is about to be emitted (from
     * parseSseLine on a finish_reason chunk, OR from the streamFinished
     * handler on tool_call completion). Reset to false in sendRequest().
     *
     * Why this exists: if an upstream OpenAI-compatible server finishes
     * the SSE stream WITHOUT emitting either a finish_reason chunk OR a
     * tool_call (i.e. empty stream), the stream parser produces nothing
     * and the streamFinished handler's previous guard
     * `if (!m_pendingToolName.isEmpty())` would not emit requestFinished.
     * ChatController was then left waiting forever for the terminal
     * signal: m_isGenerating stayed true, the cascade stalled, the
     * conversation deadlocked. OllamaProvider already had this safety
     * net; OpenAIProvider did not.
     */
    bool m_finishedEmitted = false;

    /**
     * @brief Builds the JSON body for POST /v1/chat/completions.
     * @param req LLM request.
     * @return Compact JSON bytes.
     *
     * Format:
     * {"model":"gpt-4o","messages":[{"role":"user","content":"..."}],
     *  "stream":true,"temperature":0.7,"max_tokens":4096,
     *  "tools":[{"type":"function","function":{...}}]}
     */
    QByteArray buildRequestBody(const LlmRequest& req);

    /**
     * @brief Parses a single SSE "data:" event line from the OpenAI stream.
     * @param line Raw bytes of the JSON payload (without "data: " prefix).
     * @sideeffects Emits chunkReceived() for content tokens and tool calls.
     *              Emits requestFinished() when finish_reason is set.
     *
     * Handles:
     *   - delta.content (text tokens)
     *   - delta.tool_calls (function call arguments streaming)
     *   - finish_reason: "stop" | "tool_calls" | "length"
     */
    void parseSseLine(const QByteArray& line);

    /**
     * @brief Converts a ToolSchema to OpenAI function definition format.
     * @param schema Tool schema.
     * @return QJsonObject with "type":"function" and "function":{name,description,parameters}.
     */
    QJsonObject toolSchemaToOpenAI(const ToolSchema& schema);
};

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ollama-provider.h
 * @brief LLM provider adapter for Ollama (local model hosting server).
 *        Communicates via Ollama's REST API (default: http://localhost:11434).
 *        Supports streaming NDJSON responses and tool calling (Ollama 0.4+).
 * @layer API
 * @dependencies HttpClient (Utility), ILLMProvider (API)
 */

#pragma once

#include "../services/chat/inline-thinking-hoister.h"
#include "../utils/http-client.h"
#include "llm-interface.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

/**
 * @brief LLM provider adapter for Ollama.
 *
 * Ollama serves local models via a REST API compatible with OpenAI's chat format.
 * Streaming uses NDJSON: one JSON object per line, terminated by {"done":true}.
 *
 * API endpoints used:
 *   POST /api/chat: send message, receive streaming NDJSON response
 *   GET  /api/tags: list available models
 */
class OllamaProvider : public ILLMProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the Ollama provider.
     * @param http Reference to a shared HttpClient instance.
     * @param parent Optional Qt parent.
     */
    explicit OllamaProvider(HttpClient& http, QObject* parent = nullptr);

    /**
     * @brief Sets the base URL for the Ollama server.
     * @param url Base URL string (e.g., "http://localhost:11434").
     */
    void setBaseUrl(const QString& url);

    /**
     * @brief Returns the configured Ollama base URL.
     * @return Base URL string.
     */
    QString baseUrl() const;

    // -----------------------------------------------------------------------
    // ILLMProvider interface
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the provider id used by ModelRouter for dispatch.
     * @returns "ollama".
     */
    QString providerId() const override;

    /**
     * @brief Human-readable name shown in the UI provider selector.
     * @returns "Ollama (Local)".
     */
    QString displayName() const override;

    /**
     * @brief Returns the cached list of locally-pulled Ollama models.
     * @returns Model name strings (populated by refreshModels()).
     */
    QStringList availableModels() override;

    /**
     * @brief Refreshes the model list via GET /api/tags.
     *
     * Queries the configured Ollama server for its locally-pulled models.
     * Emits modelsRefreshed() on success.
     */
    void refreshModels() override;

    /**
     * @brief Reports NDJSON streaming support.
     * @returns true. /api/chat streams responses as newline-delimited JSON.
     */
    bool supportsStreaming() const override;

    /**
     * @brief Reports tool calling support.
     * @returns true. Ollama 0.4+ supports the `tools` request field.
     */
    bool supportsToolCalling() const override;

    /**
     * @brief Reports image-input support.
     * @returns true. Vision works on multimodal models such as llava and bakllava.
     */
    bool supportsVision() const override;

    /**
     * @brief Resolves a model's max context window from Ollama's /api/show.
     * @param model The Ollama model tag (e.g. "gemma4:e4b").
     * @returns The cached context length in tokens, or 0 when not yet known.
     *
     * On a cache MISS this fires a single-flight async POST
     * `{baseUrl}/api/show {"model":"\<id\>"}` on a dedicated network manager
     * (kept separate from the streaming HttpClient so it never collides with
     * an in-flight chat). The reply's `model_info` is scanned for the single
     * key ending in `.context_length` (e.g. `gemma4.context_length`), which is
     * cached for subsequent calls. Returns 0 on the cold first call, because Ollama
     * has no published-family fallback. Non-blocking; main-thread-safe.
     */
    int contextWindowFor(const QString& model) override;

    /**
     * @brief Initiates a streaming POST /api/chat call.
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

  private:
    HttpClient& m_http;
    QString m_baseUrl = QStringLiteral("http://localhost:11434");
    QStringList m_models;

    /**
     * @brief model tag → resolved context length (tokens) cache.
     *
     * Populated by the async /api/show warm query. Read synchronously by
     * contextWindowFor(). Main-thread-only (all access is on the GUI thread).
     */
    QHash<QString, int> m_ctxCache;

    /**
     * @brief Models with an /api/show warm query currently in flight.
     *
     * Single-flight guard: a model present here does NOT trigger a second
     * query. Entries are removed when the reply lands (success or error).
     */
    QSet<QString> m_ctxInFlight;

    /**
     * @brief Dedicated network manager for the out-of-band /api/show warm
     *        queries.
     *
     * Separate from the shared streaming HttpClient (which is single-request
     * per instance and busy during a chat) so a context-window probe can never
     * collide with an in-flight completion. Parented to this provider (RAII).
     */
    QNetworkAccessManager m_ctxNam;

    /**
     * @brief Fires the single-flight async /api/show probe for @p model.
     * @param model The Ollama model tag to resolve.
     * @sideeffects Inserts @p model into m_ctxInFlight; on reply, parses
     *              model_info for `*.context_length` and updates m_ctxCache.
     */
    void warmContextWindow(const QString& model);
    bool m_hasPendingToolCall = false;  ///< True when tool_calls seen during stream
    bool m_doneReceived = false;        ///< True when done:true chunk was processed
    int m_thinkingCharsSeen = 0;  ///< Sum of `message.thinking` deltas across the in-flight stream
    int m_contentCharsSeen = 0;   ///< Sum of `message.content` deltas across the in-flight stream
    int m_streamContentFramesSeen = 0;  ///< Count of non-done frames carrying any raw
                                        ///< content/thinking (0 ⇒ Ollama emitted an empty stream)

    /** Bounded excerpt (400 chars) of the most recent stream frame whose
     *  message object carried data in a field this parser does not consume
     *  (no content / thinking / tool_calls). Printed by the
     *  zero-chars-with-eval-tokens diagnostic so the offending field is
     *  identified from the log instead of requiring a raw-stream replay.
     *  Cleared at every sendRequest. */
    QString m_lastUnconsumedFrameExcerpt;

    /**
     * @brief Carry-over buffer for content delta that may contain a
     *        partially-streamed in-band tool-call tag (qwen3 emits tool
     *        intent as `<|tool_call|>name=X arguments={...}</|tool_call|>`
     *        blocks inside the message body when the function-calling
     *        protocol slips). We hold the tail across chunks until either
     *        a complete block can be extracted (and re-emitted as a
     *        structured tool_call chunk) or we can confidently flush the
     *        buffered text. Reset at sendRequest.
     */
    QString m_inbandToolBuf;

    /**
     * @brief Per-stream extractor that pulls inline `\<think\>...\</think\>`
     *        blocks out of `message.content` before the tool-call
     *        hoister sees the text.  Reset at sendRequest so leftover
     *        state from a partial prior stream does not bleed into
     *        the next.
     *
     *        Captures from this hoister AND from the separate
     *        `message.thinking` field both land in the same
     *        `LlmChunk::thinkingDelta` so reasoning models that emit
     *        thinking inline (Qwen template that doesn't honour
     *        `think:false`) and those that emit through the
     *        dedicated channel both surface in the same
     *        message-bubble disclosure.
     */
    Chat::InlineThinkingHoister m_thinkingHoister;

    /**
     * @brief Builds the JSON body for POST /api/chat.
     * @param req The LLM request payload.
     * @return JSON body as QByteArray.
     *
     * Ollama format:
     * {"model":"llama3:8b","messages":[{"role":"user","content":"..."}],
     *  "stream":true,"options":{"temperature":0.7}}
     *
     * Tool calling format (Ollama 0.4+):
     * {"model":"...","messages":[...],"tools":[{"type":"function","function":{...}}]}
     */
    QByteArray buildRequestBody(const LlmRequest& req);

    /**
     * @brief Parses a single NDJSON streaming line from Ollama.
     * @param line Raw JSON bytes (one complete JSON object).
     * @sideeffects Emits chunkReceived() with parsed delta and finish info.
     *
     * Streaming format:
     *   {"model":"...","message":{"role":"assistant","content":"token"},"done":false}
     *   {"model":"...","done":true,"total_duration":...,"eval_count":256}
     */
    void parseStreamChunk(const QByteArray& line);

    /**
     * @brief Parses the model list from the GET /api/tags response.
     * @param body JSON response body.
     * @return List of model name strings.
     */
    QStringList parseModelList(const QByteArray& body);
};

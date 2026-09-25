// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file anthropic-provider.h
 * @brief LLM provider adapter for Anthropic Claude API.
 *        Uses the Messages API with SSE streaming and tool_use support.
 *        Implements typed SSE events: message_start, content_block_delta, message_delta, etc.
 * @layer API
 * @dependencies HttpClient (Utility), ILLMProvider (API)
 */

#pragma once

#include "../utils/http-client.h"
#include "llm-interface.h"

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QStringList>

// Forward declaration
struct ToolSchema;

/**
 * @brief LLM provider adapter for Anthropic Claude API.
 *
 * Endpoint: POST /v1/messages
 * Auth: x-api-key header + anthropic-version header
 * Streaming: Server-Sent Events with typed events (event: / data: pairs)
 *
 * SSE event flow:
 *   message_start       → input token count
 *   content_block_start → new text or tool_use block begins
 *   content_block_delta → text_delta or input_json_delta
 *   content_block_stop  → block ends
 *   message_delta       → stop_reason, output token count
 *   message_stop        → final event
 *
 * Default models:
 *   claude-opus-5-5, claude-sonnet-5, claude-fable-5-1, claude-haiku-4-5-20251001
 *
 * Tool result format (user turn):
 *   {"role":"user","content":[{"type":"tool_result","tool_use_id":"...","content":"..."}]}
 */
class AnthropicProvider : public ILLMProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the Anthropic provider.
     * @param http Reference to a shared HttpClient instance.
     * @param parent Optional Qt parent.
     */
    explicit AnthropicProvider(HttpClient& http, QObject* parent = nullptr);

    /**
     * @brief Sets the Anthropic API key.
     * @param key API key string. Never logged.
     */
    void setApiKey(const QString& key);

    /**
     * @brief Overrides the API base URL (default https://api.anthropic.com/v1).
     * @param url Base URL without a trailing slash; used by tests and proxies.
     */
    void setBaseUrl(const QString& url);

    // -----------------------------------------------------------------------
    // ILLMProvider interface
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the provider id used by ModelRouter for dispatch.
     * @returns "anthropic".
     */
    QString providerId() const override;

    /**
     * @brief Human-readable name shown in the UI provider selector.
     * @returns "Anthropic Claude".
     */
    QString displayName() const override;

    /**
     * @brief Returns the currently cached list of Claude model identifiers.
     * @returns Model name strings (e.g. "claude-opus-5-5").
     */
    QStringList availableModels() override;

    /**
     * @brief No-op for Anthropic, because there is no public model listing endpoint.
     *
     * The hardcoded model list is used as-is; this method exists to satisfy
     * the ILLMProvider contract but does not emit modelsRefreshed().
     */
    void refreshModels() override;

    /**
     * @brief Reports SSE streaming support.
     * @returns true. Anthropic's Messages API is SSE-streamed.
     */
    bool supportsStreaming() const override;

    /**
     * @brief Reports tool_use support.
     * @returns true. Claude 3+ models support function/tool calling.
     */
    bool supportsToolCalling() const override;

    /**
     * @brief Reports image-input support.
     * @returns true. Claude 3+ models accept image content blocks.
     */
    bool supportsVision() const override;

    /**
     * @brief Initiates a streaming POST /v1/messages call.
     * @param req  Complete LLM request payload (messages + config + tools).
     *
     * Asynchronous: results arrive through the chunkReceived,
     * requestFinished and requestError signals.
     *
     * @sideeffects Opens an HTTPS streaming request and resets the
     *              per-request SSE parser state (pending event, tool-use
     *              buffer and token counts).
     */
    void sendRequest(const LlmRequest& req) override;

    /**
     * @brief Aborts the in-flight HTTP request, if any.
     *
     * Safe to call when no request is active. Does not emit requestError.
     */
    void cancelRequest() override;

    /**
     * @brief Resolves a model's max context window from the published map.
     * @param model Model identifier.
     * @returns 200000 for any `claude*` model; 200000 otherwise (Claude's
     *          documented window).
     *
     * Pure, synchronous, no network. Anthropic exposes no model-listing
     * endpoint with a context field, so the published value is the only source.
     */
    int contextWindowFor(const QString& model) override;

  private:
    HttpClient& m_http;
    QString m_apiKey;
    QString m_apiVersion = QStringLiteral("2023-06-01");
    QString m_baseUrl = QStringLiteral("https://api.anthropic.com/v1");
    QStringList m_models = {
        QStringLiteral("claude-opus-5-5"),
        QStringLiteral("claude-sonnet-5"),
        QStringLiteral("claude-fable-5-1"),
        QStringLiteral("claude-haiku-4-5-20251001"),
    };

    // SSE state machine — accumulates typed events across calls
    QString m_pendingEventType;      ///< Current "event:" type being accumulated
    QString m_pendingToolUseId;      ///< tool_use block id (for tool calls)
    QString m_pendingToolName;       ///< tool_use block name
    QString m_pendingToolInputJson;  ///< accumulated tool input (input_json_delta)
    int m_inputTokens = 0;           ///< from message_start usage
    int m_outputTokens = 0;          ///< from message_delta usage

    /**
     * @brief Empty-stream safety net.
     *
     * True iff requestFinished() has been emitted for the current in-flight
     * request (from parseSseLine's message_delta stop_reason handler).
     * The streamFinished lambda uses this to emit a fallback
     * requestFinished("stop") when Anthropic closes the SSE stream WITHOUT
     * emitting message_delta; otherwise ChatController hangs forever
     * waiting for a terminal signal. Reset in sendRequest(). Mirrors the
     * fallback OllamaProvider implements at the equivalent site in
     * ollama-provider.cpp.
     */
    bool m_finishedEmitted = false;

    // Assistant turn being streamed, as the ordered content blocks Anthropic
    // sent (thinking, redacted_thinking, text, tool_use). Current models
    // require a tool-use turn to be sent back unchanged, signatures included,
    // when its tool results are returned.
    QJsonArray m_turnBlocks;
    QString m_blockType;       ///< Type of the content block being streamed.
    QString m_blockText;       ///< Accumulated text or thinking text.
    QString m_blockSignature;  ///< Thinking block signature.
    QString m_blockData;       ///< redacted_thinking payload.

    /// Finished tool-use turns keyed by their tool_use ids, replayed when
    /// the matching tool results are sent. Bounded; oldest entries drop.
    QHash<QString, QJsonArray> m_toolTurns;
    QStringList m_toolTurnOrder;

    /// Model-list requests use their own client so a refresh never cancels
    /// a chat stream in flight on m_http.
    HttpClient m_modelsHttp;

    /// API tool name (only letters, digits, _ and -) to the app's tool name,
    /// filled from the tools sent with each request; MCP tools are named
    /// "server:tool", which the API rejects.
    QHash<QString, QString> m_toolNames;

    /// Canonical stop reason of the turn being streamed, from message_delta;
    /// reported by requestFinished at message_stop.
    QString m_stopReason;

    /**
     * @brief Stores the streamed turn for replay when it contains tool_use
     *        blocks, keyed by the joined tool_use ids.
     */
    void rememberToolTurn();

    /**
     * @brief Builds the Authorization and version headers.
     * @return Map of header name → value. API key never logged.
     */
    QMap<QString, QString> buildHeaders() const;

    /**
     * @brief Builds the JSON body for POST /v1/messages.
     * @param req LLM request payload.
     * @return Compact JSON bytes.
     *
     * Anthropic format:
     * {
     *   "model": "claude-opus-5-5",
     *   "max_tokens": 32000,
     *   "system": "...",
     *   "messages": [{"role":"user","content":"..."},...],
     *   "stream": true,
     *   "tools": [...]   (optional)
     * }
     * Note: system prompt is a top-level field, not inside messages.
     */
    QByteArray buildRequestBody(const LlmRequest& req);

    /**
     * @brief Parses a single raw SSE line from the Anthropic stream.
     * @param line Raw byte line (may start with "event:" or "data:").
     * @sideeffects Accumulates event type or dispatches to parseSseEvent().
     */
    void parseSseLine(const QByteArray& line);

    /**
     * @brief Dispatches a complete Anthropic SSE event for processing.
     * @param eventType The "event:" type string.
     * @param data The "data:" JSON payload.
     * @sideeffects Emits chunkReceived(), requestFinished(), or requestError().
     *
     * Handled event types:
     *   - message_start        → capture input_tokens from usage
     *   - content_block_start  → capture tool_use id/name; text blocks ignored
     *   - content_block_delta  → emit text delta or accumulate tool input JSON
     *   - content_block_stop   → if tool pending, emit LlmChunk with toolCallJson
     *   - message_delta        → capture stop_reason and output_tokens
     *   - message_stop         → emit requestFinished with total tokens
     *   - error                → emit requestError
     */
    void parseSseEvent(const QString& eventType, const QByteArray& data);

    /**
     * @brief Converts a ToolSchema to Anthropic tool format.
     * @param schema Tool schema to convert.
     * @return JSON object with name, description, input_schema.
     */
    QJsonObject toolSchemaToAnthropic(const ToolSchema& schema);
};

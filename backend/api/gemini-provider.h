// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file gemini-provider.h
 * @brief LLM provider adapter for Google Gemini API.
 *        Supports the Gemini model family via streamGenerateContent.
 *        Uses API key as query parameter rather than Authorization header.
 * @layer API
 * @dependencies HttpClient (Utility), ILLMProvider (API)
 */

#pragma once

#include "../utils/http-client.h"
#include "llm-interface.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

// Forward declaration
struct ToolSchema;

/**
 * @brief LLM provider adapter for Google Gemini API.
 *
 * Endpoint: POST /v1beta/models/{model}:streamGenerateContent?key={apiKey}&alt=sse
 * Auth: API key as query parameter (?key=...)
 * Streaming: SSE wrapping JSON objects (each event is one array element)
 *
 * Role mapping:
 *   LLM "user"      → Gemini "user"
 *   LLM "assistant" → Gemini "model"
 *   LLM "system"    → systemInstruction top-level field
 *
 * Streaming response:
 *   Each "data:" event contains a JSON object with:
 *   candidates[0].content.parts[].text: text delta
 *   candidates[0].content.parts[].functionCall: tool call
 *   candidates[0].finishReason: "STOP"|"MAX_TOKENS"|"SAFETY"|"RECITATION"|"OTHER"
 *
 * Default models:
 *   gemini-2.5-pro, gemini-2.5-flash, gemini-2.5-flash-lite
 */
class GeminiProvider : public ILLMProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the Gemini provider.
     * @param http Reference to a shared HttpClient instance.
     * @param parent Optional Qt parent.
     */
    explicit GeminiProvider(HttpClient& http, QObject* parent = nullptr);

    /**
     * @brief Sets the Google AI API key.
     * @param key API key string. Never logged.
     */
    void setApiKey(const QString& key);

    /**
     * @brief Overrides the API base URL (default the v1beta endpoint).
     * @param url Base URL without a trailing slash; used by tests and proxies.
     */
    void setBaseUrl(const QString& url);

    // -----------------------------------------------------------------------
    // ILLMProvider interface
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the provider id used by ModelRouter for dispatch.
     * @returns "gemini".
     */
    QString providerId() const override;

    /**
     * @brief Human-readable name shown in the UI provider selector.
     * @returns "Google Gemini".
     */
    QString displayName() const override;

    /**
     * @brief Returns the cached list of Gemini model identifiers.
     * @returns Model name strings (e.g. "gemini-2.5-pro").
     */
    QStringList availableModels() override;

    /**
     * @brief Refreshes the model list via GET /v1beta/models.
     *
     * Requires a configured API key. Emits modelsRefreshed() with the
     * returned ids on success.
     */
    void refreshModels() override;

    /**
     * @brief Reports SSE streaming support.
     * @returns true. Gemini's streamGenerateContent endpoint streams via SSE.
     */
    bool supportsStreaming() const override;

    /**
     * @brief Reports function-calling support.
     * @returns true. Gemini accepts a `tools.functionDeclarations` array.
     */
    bool supportsToolCalling() const override;

    /**
     * @brief Reports image-input support.
     * @returns true. Gemini accepts inline image parts in user content.
     */
    bool supportsVision() const override;

    /**
     * @brief Initiates a streaming POST .../streamGenerateContent call.
     * @param req  Complete LLM request payload (messages + config + tools).
     *
     * Asynchronous: results arrive through the chunkReceived,
     * requestFinished and requestError signals. The selected model is
     * embedded in the URL path rather than the body.
     *
     * @sideeffects Opens an HTTPS streaming request. The API key is sent
     *              as the `key` URL query parameter, so it appears in the
     *              request URL.
     */
    void sendRequest(const LlmRequest& req) override;

    /**
     * @brief Aborts the in-flight HTTP request, if any.
     *
     * Safe to call when no request is active.
     */
    void cancelRequest() override;

    /**
     * @brief Resolves a model's max context window from the published map.
     * @param model Model identifier (e.g. "gemini-2.5-pro").
     * @returns 1000000 for 1.5 / 2.0 / 2.5 / exp / flash / pro families;
     *          32768 for any older / unrecognised Gemini id.
     *
     * Pure, synchronous, no network. The published value is the source.
     */
    int contextWindowFor(const QString& model) override;

  private:
    HttpClient& m_http;
    QString m_apiKey;
    QString m_baseUrl = QStringLiteral("https://generativelanguage.googleapis.com/v1beta");
    QStringList m_models = {
        QStringLiteral("gemini-3.8-flash"),
        QStringLiteral("gemini-3.7-flash"),
        QStringLiteral("gemini-3.5-flash"),
        QStringLiteral("gemini-3.5-flash-lite"),
        QStringLiteral("gemini-3.1-pro-preview"),
    };

    /// thoughtSignature values received on function calls, keyed by the
    /// synthesised tool call id, returned on the same call when its result
    /// is sent. Bounded; oldest entries drop.
    QHash<QString, QString> m_thoughtSignatures;
    QStringList m_thoughtSignatureOrder;

    /// Model-list requests use their own client so a refresh never cancels
    /// a chat stream in flight on m_http.
    HttpClient m_modelsHttp;

    /// API function name (letters, digits, _ . -) to the app's tool name.
    QHash<QString, QString> m_toolNames;

    /// True once a functionCall part was seen in the response being
    /// streamed; the turn then finishes as "tool_calls".
    bool m_sawFunctionCall = false;

    /**
     * @brief Empty-stream safety net.
     *
     * True iff requestFinished() has been emitted for the current in-flight
     * request (from parseStreamChunk's finishReason handler). The
     * streamFinished lambda uses this to emit a fallback
     * requestFinished("stop") when Gemini closes the SSE stream WITHOUT
     * emitting a chunk carrying finishReason; otherwise ChatController
     * hangs forever waiting for a terminal signal. Reset in sendRequest().
     * Mirrors the fallback OllamaProvider implements at the equivalent
     * site in ollama-provider.cpp.
     */
    bool m_finishedEmitted = false;

    /**
     * @brief Builds the JSON body for POST .../streamGenerateContent.
     * @param req LLM request payload.
     * @return Compact JSON bytes.
     *
     * Gemini format:
     * {
     *   "contents": [{"role":"user","parts":[{"text":"..."}]}, ...],
     *   "systemInstruction": {"parts":[{"text":"..."}]},
     *   "generationConfig": {"temperature":0.7,"maxOutputTokens":4096},
     *   "tools": [{"functionDeclarations":[...]}]
     * }
     * Note: model name is in the URL path, not the request body.
     */
    QByteArray buildRequestBody(const LlmRequest& req);

    /**
     * @brief Parses a single SSE data line from the Gemini stream.
     * @param data JSON payload bytes (after "data: " prefix stripped).
     * @sideeffects Emits chunkReceived() for text/tool chunks;
     *              emits requestFinished() when finishReason is set.
     *
     * Maps Gemini finish reasons to canonical values:
     *   STOP         → "stop"
     *   MAX_TOKENS   → "length"
     *   SAFETY       → "content_filter"
     *   Other/empty  → ignored
     */
    void parseStreamChunk(const QByteArray& data);

    /**
     * @brief Converts a ToolSchema to Gemini function declaration format.
     * @param schema Tool schema to convert.
     * @return Gemini functionDeclaration JSON object.
     */
    QJsonObject toolSchemaToGemini(const ToolSchema& schema);

    /**
     * @brief Converts LLM role string to Gemini role string.
     * @param role "user"|"assistant"|"system" → "user"|"model"
     * @return Gemini role string.
     */
    static QString toGeminiRole(const QString& role);
};

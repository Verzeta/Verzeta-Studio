// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file llm-interface.h
 * @brief Abstract interface defining the unified contract for all LLM providers.
 *        Includes request/response data types shared across the entire provider layer.
 *        This is the foundational contract; changes here affect every provider and consumer.
 * @layer API
 * @dependencies Qt6::Core
 */

#pragma once

#include "../models/llm-config.h"
#include "tool-calling-schema.h"

#include <optional>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

// ---------------------------------------------------------------------------
// LlmMessage — single message in the conversation context
// ---------------------------------------------------------------------------

/**
 * @brief Image attachment for vision-capable models.
 */
struct LlmImageData {
    QByteArray base64;  ///< Base64-encoded image bytes
    QString mimeType;   ///< e.g. "image/png", "image/jpeg"
};

/**
 * @brief Represents a single message in the conversation history sent to an LLM.
 *
 * The `role` field maps to each provider's role naming:
 *   OpenAI/Ollama: user/assistant/system/tool
 *   Anthropic:     user/assistant (system is a top-level field)
 *   Gemini:        user/model
 *
 * When role == "tool", `toolCallId` identifies which tool call this result belongs to.
 * When role == "assistant" with tool calls, `toolCallsJson` carries the raw tool calls.
 */
struct LlmMessage {
    QString role;              ///< "user" | "assistant" | "system" | "tool"
    QString content;           ///< Message text content (may be empty for tool-call-only messages)
    QJsonArray toolCallsJson;  ///< Tool calls in provider-agnostic JSON format (assistant messages)
    QString toolCallId;        ///< Tool call ID for tool result messages (role == "tool")
    QList<LlmImageData> images;  ///< Attached images for vision (user messages only)

    /**
     * @brief OpenAI-style speaker name for multi-agent group chats.
     *
     * When non-empty on a `role="assistant"` message, the provider emits
     * `{"role":"assistant","name":"\<alias\>","content":...}` so the model has
     * a structural speaker identifier in addition to (or instead of) a
     * content-prefix attribution. Empty for the responder's own turns
     * and for 1:1 chats.
     */
    QString speakerName;
};

// ---------------------------------------------------------------------------
// LlmRequest — complete request payload
// ---------------------------------------------------------------------------

/**
 * @brief Complete request payload sent to an LLM provider via ModelRouter.
 *
 * The ModelRouter passes this to the active ILLMProvider::sendRequest().
 * Each provider translates it into their wire format in buildRequestBody().
 */
struct LlmRequest {
    /**
     * @brief Monotonic request id (set by ChatController in buildAndSendRequest).
     *        Zero means "not set". Used by ModelRouter to tag every
     *        chunk/finish/error signal with the id of the request that
     *        originated it, so ChatController can reject stale signals
     *        from cancelled or replaced requests.
     */
    quint64 requestId = 0;
    QString conversationId;            ///< For correlation and logging
    QList<LlmMessage> messages;        ///< Full conversation history
    LlmConfig config;                  ///< Model, temperature, max tokens, streaming flag
    QList<ToolSchema> availableTools;  ///< Empty if tool calling not enabled for this request
    QString systemPrompt;              ///< Per-conversation system instruction

    // -----------------------------------------------------------------------
    // Task-runner execution loop fields
    // -----------------------------------------------------------------------

    /**
     * @brief If true, a text-only completion (finishReason == "stop" with no
     *        tool call) is treated as a validation failure and the response
     *        is discarded without being persisted. The TaskRunner decides
     *        whether to re-dispatch with a corrective nudge or mark the
     *        current step as blocked. Conversational turns always set this
     *        to false.
     */
    bool toolOnlyMode = false;

    /**
     * @brief When non-empty, filters `availableTools` down to this whitelist
     *        before the request is sent to the provider. Used by task-gated
     *        turns to restrict the tool surface (e.g. executor turns only
     *        see submit_result, report_blocked, update_plan_step, plus
     *        execution helpers). Ignored when empty.
     */
    QStringList allowedTools;

    /**
     * @brief Turn kind for dispatch bookkeeping:
     *        "conversational" | "task_executor" | "task_planner" | "task_critic"
     *        Used for logging and for the validator to know whether to apply
     *        tool-only rules.
     */
    QString turnKind;

    /** @brief Plan id for TaskRunner correlation on task turns. */
    QString currentPlanId;
    /** @brief Step id for TaskRunner correlation on task turns. */
    QString currentStepId;
};

// ---------------------------------------------------------------------------
// LlmChunk — streaming response chunk
// ---------------------------------------------------------------------------

/**
 * @brief A single streaming chunk from an LLM provider.
 *
 * Emitted repeatedly by ILLMProvider::chunkReceived() during streaming.
 * When `finishReason` is non-empty, no more chunks will be emitted.
 */
struct LlmChunk {
    QString delta;               ///< Partial text token(s); may be empty for tool-call-only chunks
    QString finishReason;        ///< "stop" | "tool_calls" | "length" | "error" | "" (ongoing)
    QJsonObject toolCallJson;    ///< Populated when the LLM invokes a tool
    int tokenCountEstimate = 0;  ///< Running token estimate (may be 0 until final chunk)

    /**
     * @brief Captured reasoning / thinking content for this chunk.
     *
     * Populated by provider parsers that route the model's thinking
     * channel separately from visible content: Ollama
     * `message.thinking`, OpenAI `delta.reasoning` +
     * `delta.reasoning_content`, Anthropic `content_block_delta` of
     * type `thinking_delta`, Gemini parts where `thought == true`,
     * and inline `\<think\>...\</think\>` blocks extracted by the
     * shared `Chat::InlineThinkingHoister` for Ollama / LlamaCpp
     * in-process / OpenAI-family content streams.
     *
     * Accumulated across the stream by `Chat::StreamingManager`
     * into a parallel buffer that lands on `Message::thinkingContent`
     * at finalise time.  Display-only by contract: never read by
     * RequestBuilder, HistoryBudgeter, ContentSanitizer, or any
     * layer that builds a subsequent request.  Empty for non-
     * reasoning providers and for chunks that carry only content
     * or tool calls.
     */
    QString thinkingDelta;
};

// ---------------------------------------------------------------------------
// ILLMProvider — abstract provider interface
// ---------------------------------------------------------------------------

/**
 * @brief Abstract interface that all LLM provider adapters must implement.
 *
 * Each provider (Ollama, OpenAI, Anthropic, Gemini, LlamaCpp) implements this
 * interface. Providers are registered with ModelRouter, which dispatches requests
 * to the active provider.
 *
 * Communication is fully asynchronous via Qt signals:
 *   - chunkReceived() fires for each streaming token
 *   - requestFinished() fires on clean completion
 *   - requestError() fires on failure
 *
 * Thread safety: All methods are expected to be called from the main thread.
 * For CPU-intensive work (llama.cpp inference), providers use worker threads
 * and marshal results back to the main thread via queued connections.
 */
class ILLMProvider : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the provider base.
     * @param parent Optional Qt parent.
     */
    explicit ILLMProvider(QObject* parent = nullptr) : QObject(parent) {}
    ~ILLMProvider() override = default;

    /**
     * @brief Returns the unique identifier for this provider.
     * @return Provider ID string (e.g., "ollama", "openai", "anthropic", "gemini", "llamacpp").
     */
    virtual QString providerId() const = 0;

    /**
     * @brief Returns a human-readable display name.
     * @return Display name (e.g., "Ollama (Local)", "OpenAI").
     */
    virtual QString displayName() const = 0;

    /**
     * @brief Returns the list of currently cached available models.
     *        Call refreshModels() to update this list from the provider.
     * @return List of model name strings.
     */
    virtual QStringList availableModels() = 0;

    /**
     * @brief Initiates an asynchronous refresh of the model list.
     * @sideeffects For remote providers: makes a network request (GET /models or equivalent).
     *              For llama.cpp: scans the configured model directory.
     *              Emits modelsRefreshed() when complete.
     */
    virtual void refreshModels() = 0;

    /**
     * @brief Returns whether this provider supports streaming token output.
     * @return true if streaming is supported.
     */
    virtual bool supportsStreaming() const = 0;

    /**
     * @brief Returns whether this provider supports function/tool calling.
     * @return true if tool schemas can be sent and tool_calls parsed.
     */
    virtual bool supportsToolCalling() const = 0;

    /**
     * @brief Returns whether this provider supports vision (image input analysis).
     * @return true if image data can be included in messages.
     */
    virtual bool supportsVision() const = 0;

    /**
     * @brief Resolves the model's real maximum input context window in tokens.
     * @param model Model identifier as it appears in `availableModels()` /
     *              `LlmConfig::modelName`.
     * @returns The model's RAW (pre-clamp) maximum context window in tokens, or
     *          0 when the provider cannot determine it.
     *
     * Contract:
     *   - Providers with a PUBLISHED map (OpenAI / Anthropic / Gemini /
     *     DeepSeek) return the family value synchronously, with no network.
     *   - Providers with a LIVE source (Ollama /api/show, OpenRouter
     *     /api/v1/models, OpenAI-compat /api/v0/models, llama.cpp-remote
     *     /props) return the cached value when known. On a cache MISS they
     *     kick a SINGLE-FLIGHT async query using the provider's own network
     *     manager + base URL to warm the cache for next time, and return the
     *     best value available NOW (a published-family fallback if the
     *     provider has one, otherwise 0). This means the FIRST call for a
     *     never-seen model on a live-source provider may return the fallback
     *     (or 0) and only later calls see the resolved value; this cold-first
     *     -turn behaviour is intentional and non-blocking.
     *   - The internal llama.cpp provider returns the loaded context size
     *     synchronously.
     *
     * Non-const to mirror `availableModels()`: implementations mutate an
     * internal cache and in-flight set. Never blocks; never throws; returns 0
     * on any failure path.
     */
    virtual int contextWindowFor(const QString& model) = 0;

    /**
     * @brief Sends a request to the LLM. Asynchronous; it communicates via signals.
     * @param request Complete request payload.
     * @sideeffects Initiates HTTP/network request or local inference.
     *              Emits chunkReceived() repeatedly, then requestFinished() or requestError().
     */
    virtual void sendRequest(const LlmRequest& request) = 0;

    /**
     * @brief Cancels an in-progress request.
     * @sideeffects Aborts network connection (QNetworkReply::abort()) or sets
     *              the llama.cpp cancel flag. Safe to call even if no request is active.
     */
    virtual void cancelRequest() = 0;

  signals:
    /**
     * @brief Emitted for each streaming chunk received from the provider.
     * @param chunk The partial text/tool-call chunk.
     */
    void chunkReceived(const LlmChunk& chunk);

    /**
     * @brief Emitted when the request completes successfully or stops naturally.
     * @param finishReason "stop" | "tool_calls" | "length" | "user_interrupted"
     * @param totalTokens Estimated total token count (input + output) if available.
     */
    void requestFinished(const QString& finishReason, int totalTokens);

    /**
     * @brief Emitted when the request fails.
     * @param errorMessage Human-readable error. Never contains API keys.
     */
    void requestError(const QString& errorMessage);

    /**
     * @brief Emitted when the model list has been refreshed.
     * @param models Updated list of available model names.
     */
    void modelsRefreshed(const QStringList& models);
};

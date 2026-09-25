// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file anthropic-provider.cpp
 * @brief Implementation of the Anthropic Claude LLM provider adapter.
 *        Parses typed SSE streaming events, tool_use blocks, and handles
 *        the Messages API format differences from OpenAI.
 * @layer API
 * @dependencies HttpClient (Utility), Qt6::Core, Qt6::Network
 */

#include "anthropic-provider.h"

#include "../utils/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QUrlQuery>

namespace {

/**
 * @brief Request rules for a Claude model, derived from its model id.
 *
 * Current models (4.6 and later, the 5.x generation, Fable and Mythos) use
 * adaptive thinking; 4.7 and later also reject non-default sampling values;
 * the 5.x generation, Fable and Mythos think by default; Opus 5.5 and later,
 * Fable and Mythos check that replayed thinking still matches the prefix.
 */
struct ClaudeRules {
    bool adaptiveThinking = false;  ///< Accepts thinking.type "adaptive".
    bool thinksByDefault = false;   ///< Thinks unless told otherwise.
    bool bindsThinking = false;     ///< Checks replayed thinking against the prefix.
    bool rejectsSampling = false;   ///< Rejects temperature / top_p / top_k.
    bool legacyThinking = false;    ///< Uses thinking.type "enabled" / "disabled".
    bool legacyOutputCap = false;   ///< Claude 3.x: 4096 output tokens at most.
};

ClaudeRules claudeRules(const QString& model) {
    ClaudeRules r;
    if (model.startsWith(QStringLiteral("claude-3"))) {
        r.legacyOutputCap = true;
        r.legacyThinking = model.startsWith(QStringLiteral("claude-3-7"));
        return r;
    }
    static const QRegularExpression rx(
        QStringLiteral("^claude-(opus|sonnet|haiku|fable|mythos)-(\\d+)(?:-(\\d{1,2}))?(?:-|$)"));
    const QRegularExpressionMatch m = rx.match(model);
    if (!m.hasMatch()) {
        // Unknown naming: treat as a current model, which is the safe
        // choice (no sampling values, adaptive thinking).
        r.adaptiveThinking = r.thinksByDefault = r.rejectsSampling = true;
        return r;
    }
    const QString family = m.captured(1);
    const int version =
        m.captured(2).toInt() * 10 + (m.captured(3).isEmpty() ? 0 : m.captured(3).toInt());
    const bool newFamily = family == QStringLiteral("fable") || family == QStringLiteral("mythos");
    r.adaptiveThinking = newFamily || version >= 46;
    r.thinksByDefault = newFamily || version >= 50;
    r.bindsThinking = newFamily || version >= 55;
    r.rejectsSampling = newFamily || version >= 46;
    r.legacyThinking = !r.adaptiveThinking &&
                       (family == QStringLiteral("opus") || family == QStringLiteral("sonnet"));
    return r;
}

/**
 * @brief Converts one stored tool call to an Anthropic tool_use block.
 *        History keeps the OpenAI shape {id, type, function:{name,
 *        arguments}}; a block already in Anthropic shape passes through.
 */
QJsonObject toToolUseBlock(const QJsonObject& tc) {
    if (tc.value(QStringLiteral("type")).toString() == QStringLiteral("tool_use")) {
        return tc;
    }
    const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
    const QString name = fn.isEmpty() ? tc.value(QStringLiteral("name")).toString()
                                      : fn.value(QStringLiteral("name")).toString();
    const QJsonValue args = fn.isEmpty() ? tc.value(QStringLiteral("arguments"))
                                         : fn.value(QStringLiteral("arguments"));
    QJsonObject input;
    if (args.isObject()) {
        input = args.toObject();
    } else if (args.isString()) {
        const QJsonDocument d = QJsonDocument::fromJson(args.toString().toUtf8());
        if (d.isObject())
            input = d.object();
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                       {QStringLiteral("id"), tc.value(QStringLiteral("id")).toString()},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("input"), input}};
}

/** @brief Joined tool_use ids of a set of tool calls, used as a cache key. */
QString toolTurnKey(const QJsonArray& toolCalls) {
    QStringList ids;
    for (const QJsonValue& v : toolCalls) {
        const QString id = v.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            ids.append(id);
    }
    ids.sort();
    return ids.join(QLatin1Char(','));
}

constexpr int kMaxRememberedToolTurns = 64;

/**
 * @brief Tool name in the form the API accepts: letters, digits, _ and -,
 *        at most 128 characters. ":" (MCP "server:tool") becomes "__".
 */
QString apiToolName(const QString& name) {
    QString out;
    out.reserve(name.size() + 4);
    for (const QChar c : name) {
        if (c == QLatin1Char(':'))
            out += QStringLiteral("__");
        else if (c.isLetterOrNumber() && c.unicode() < 128)
            out += c;
        else if (c == QLatin1Char('_') || c == QLatin1Char('-'))
            out += c;
        else
            out += QLatin1Char('_');
    }
    return out.left(128);
}

/** @brief Maps an Anthropic stop_reason to the app's finish reason. */
QString mapStopReason(const QString& stopReason) {
    if (stopReason == QStringLiteral("tool_use"))
        return QStringLiteral("tool_calls");
    if (stopReason == QStringLiteral("max_tokens") ||
        stopReason == QStringLiteral("model_context_window_exceeded"))
        return QStringLiteral("length");
    if (stopReason == QStringLiteral("refusal"))
        return QStringLiteral("content_filter");
    // end_turn, stop_sequence, pause_turn and anything new end the turn.
    return QStringLiteral("stop");
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the AnthropicProvider.
 * @param http Reference to the shared HttpClient.
 * @param parent Optional Qt parent.
 */
AnthropicProvider::AnthropicProvider(HttpClient& http, QObject* parent)
    : ILLMProvider(parent), m_http(http) {
    // Each raw line from the HTTP stream is forwarded to our line parser
    connect(&m_http, &HttpClient::chunkReceived, this, &AnthropicProvider::parseSseLine);

    connect(&m_http, &HttpClient::streamFinished, this, [this]() {
        if (!m_finishedEmitted) {
            qCWarning(verzetaLlm) << "Anthropic: stream finished without message_delta "
                                     "— emitting synthetic stop so the cascade can "
                                     "finalize cleanly";
            emit requestFinished(QStringLiteral("stop"), m_inputTokens + m_outputTokens);
            m_finishedEmitted = true;
        }
        // Reset per-request SSE state
        m_pendingEventType.clear();
        m_pendingToolUseId.clear();
        m_pendingToolName.clear();
        m_pendingToolInputJson.clear();
        m_inputTokens = 0;
        m_outputTokens = 0;
    });

    connect(&m_http, &HttpClient::errorOccurred, this, [this](const QString& msg) {
        emit requestError(msg);
    });

    // Model list response from GET /v1/models (the only non-streaming
    // request this provider makes). A reply with no usable ids keeps the
    // built-in list, so the selector is never left empty.
    connect(&m_modelsHttp, &HttpClient::errorOccurred, this, [](const QString& msg) {
        qCWarning(verzetaLlm) << "Anthropic model list request failed:" << msg;
    });
    connect(&m_modelsHttp,
            &HttpClient::responseReceived,
            this,
            [this](int statusCode, const QByteArray& body) {
                if (statusCode != 200) {
                    qCWarning(verzetaLlm) << "Anthropic model list returned status" << statusCode;
                    return;
                }
                QJsonParseError err;
                const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
                if (err.error != QJsonParseError::NoError)
                    return;
                QStringList models;
                const QJsonArray data = doc.object()[QStringLiteral("data")].toArray();
                for (const QJsonValue& v : data) {
                    const QString id = v.toObject()[QStringLiteral("id")].toString();
                    if (!id.isEmpty())
                        models.append(id);
                }
                if (models.isEmpty())
                    return;
                m_models = models;
                emit modelsRefreshed(m_models);
            });
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void AnthropicProvider::setApiKey(const QString& key) {
    m_apiKey = key;
}

void AnthropicProvider::setBaseUrl(const QString& url) {
    m_baseUrl = url;
}

// ---------------------------------------------------------------------------
// ILLMProvider interface
// ---------------------------------------------------------------------------

QString AnthropicProvider::providerId() const {
    return QStringLiteral("anthropic");
}

QString AnthropicProvider::displayName() const {
    return QStringLiteral("Anthropic Claude");
}

QStringList AnthropicProvider::availableModels() {
    return m_models;
}

/**
 * @brief Fetches the model list from GET /v1/models.
 * @sideeffects Without an API key, emits modelsRefreshed() with the current
 *              list. With a key, starts the request; the reply replaces the
 *              list and emits modelsRefreshed().
 */
void AnthropicProvider::refreshModels() {
    if (m_apiKey.isEmpty()) {
        emit modelsRefreshed(m_models);
        return;
    }
    QUrl url(m_baseUrl + QStringLiteral("/models"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("1000"));
    url.setQuery(query);
    m_modelsHttp.get(url, buildHeaders());
}

int AnthropicProvider::contextWindowFor(const QString& model) {
    Q_UNUSED(model);
    // All current Claude models expose a 200000-token context window.
    return 200000;
}

bool AnthropicProvider::supportsStreaming() const {
    return true;
}

bool AnthropicProvider::supportsToolCalling() const {
    return true;
}

bool AnthropicProvider::supportsVision() const {
    return true;
}

/*
 * @brief Sends a chat completion request to Anthropic via SSE streaming.
 * @param req Complete LLM request payload.
 * @sideeffects Initiates HTTPS streaming request. Resets SSE state machine.
 */
void AnthropicProvider::sendRequest(const LlmRequest& req) {
    if (m_apiKey.isEmpty()) {
        emit requestError(QStringLiteral("Anthropic API key not configured"));
        return;
    }

    // Reset per-request state
    m_pendingEventType.clear();
    m_pendingToolUseId.clear();
    m_pendingToolName.clear();
    m_pendingToolInputJson.clear();
    m_inputTokens = 0;
    m_outputTokens = 0;
    m_finishedEmitted = false;  // fresh request, no terminal signal yet
    m_turnBlocks = QJsonArray();
    m_blockType.clear();
    m_stopReason.clear();

    const QByteArray body = buildRequestBody(req);
    const QUrl url(m_baseUrl + QStringLiteral("/messages"));

    QMap<QString, QString> headers = buildHeaders();
    if (claudeRules(req.config.modelName).bindsThinking) {
        // Enables thinking.block_binding, so thinking that no longer
        // matches the prefix is dropped instead of failing the request.
        headers.insert(QStringLiteral("anthropic-beta"),
                       QStringLiteral("thinking-binding-controls-2026-08-01"));
    }

    qCDebug(verzetaLlm) << "Anthropic request to" << url.toString()
                        << "model:" << req.config.modelName;
    m_http.postStream(url, body, headers);
}

/**
 * @brief Cancels the in-progress request.
 */
void AnthropicProvider::cancelRequest() {
    m_http.cancel();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Builds the x-api-key and anthropic-version headers.
 * @return Header map. API key never logged.
 */
QMap<QString, QString> AnthropicProvider::buildHeaders() const {
    return {
        {QStringLiteral("x-api-key"), m_apiKey},
        {QStringLiteral("anthropic-version"), m_apiVersion},
    };
}

/**
 * @brief Builds the JSON body for POST /v1/messages.
 * @param req LLM request payload.
 * @return Compact JSON bytes.
 */
QByteArray AnthropicProvider::buildRequestBody(const LlmRequest& req) {
    QJsonObject root;
    root[QStringLiteral("model")] = req.config.modelName;
    // The Anthropic Messages API requires max_tokens. For "Auto"
    // (maxTokens <= 0) Claude 3.x models get 4096, their output limit, and
    // later models more, so long replies and canvas files are not cut off.
    // Explicit positive values pass through unchanged.
    const ClaudeRules rules = claudeRules(req.config.modelName);
    // Thinking counts toward max_tokens, so current models get room for it.
    root[QStringLiteral("max_tokens")] =
        req.config.maxTokens > 0
            ? req.config.maxTokens
            : (rules.legacyOutputCap ? 4096 : (rules.adaptiveThinking ? 32000 : 16384));
    // The response is always read as an SSE stream, so streaming is always
    // requested regardless of the conversation's streaming toggle.
    root[QStringLiteral("stream")] = true;

    // Anthropic: system prompt is a top-level field
    if (!req.systemPrompt.isEmpty()) {
        root[QStringLiteral("system")] = req.systemPrompt;
    }

    // Build messages array. Anthropic uses "user" / "assistant" roles; tool
    // results are user-role tool_result blocks, and every tool_result for
    // one assistant turn must sit in the same user message.
    QJsonArray messages;
    auto appendToolResult = [&messages](const QJsonObject& block) {
        if (!messages.isEmpty()) {
            QJsonObject last = messages.last().toObject();
            if (last.value(QStringLiteral("role")).toString() == QStringLiteral("user") &&
                last.value(QStringLiteral("content")).isArray()) {
                QJsonArray arr = last.value(QStringLiteral("content")).toArray();
                if (!arr.isEmpty() &&
                    arr.last().toObject().value(QStringLiteral("type")).toString() ==
                        QStringLiteral("tool_result")) {
                    arr.append(block);
                    last[QStringLiteral("content")] = arr;
                    messages[messages.size() - 1] = last;
                    return;
                }
            }
        }
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), QJsonArray{block}}});
    };
    for (const LlmMessage& msg : req.messages) {
        if (msg.role == QStringLiteral("tool") && !msg.toolCallId.isEmpty()) {
            appendToolResult(
                QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                            {QStringLiteral("tool_use_id"), msg.toolCallId},
                            // An empty result is rejected as empty text.
                            {QStringLiteral("content"),
                             msg.content.isEmpty() ? QStringLiteral("(no output)") : msg.content}});
            continue;
        }

        QJsonObject m;
        // The messages array only carries user and assistant turns; a
        // system note in the conversation is passed as user content.
        m[QStringLiteral("role")] =
            msg.role == QStringLiteral("system") ? QStringLiteral("user") : msg.role;

        if (!msg.toolCallsJson.isEmpty()) {
            // Assistant message that contains tool_use blocks. When this
            // provider streamed the turn, send it back exactly as received
            // (thinking blocks and signatures included), as current models
            // require when the tool results follow.
            const auto remembered = m_toolTurns.constFind(toolTurnKey(msg.toolCallsJson));
            if (remembered != m_toolTurns.constEnd()) {
                m[QStringLiteral("content")] = remembered.value();
            } else {
                QJsonArray contentArr;
                if (!msg.content.isEmpty()) {
                    contentArr.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                  {QStringLiteral("text"), msg.content}});
                }
                // Tool use blocks, converted from the stored OpenAI shape
                for (const QJsonValue& tc : msg.toolCallsJson) {
                    QJsonObject block = toToolUseBlock(tc.toObject());
                    block[QStringLiteral("name")] =
                        apiToolName(block.value(QStringLiteral("name")).toString());
                    contentArr.append(block);
                }
                m[QStringLiteral("content")] = contentArr;
            }
        } else if (!msg.images.isEmpty()) {
            // Vision: text plus one image block per attachment.
            QJsonArray contentArr;
            for (const LlmImageData& img : msg.images) {
                contentArr.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("image")},
                    {QStringLiteral("source"),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                 {QStringLiteral("media_type"), img.mimeType},
                                 {QStringLiteral("data"), QString::fromLatin1(img.base64)}}}});
            }
            if (!msg.content.isEmpty()) {
                contentArr.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                              {QStringLiteral("text"), msg.content}});
            }
            m[QStringLiteral("content")] = contentArr;
        } else {
            // The API rejects empty text, and a turn with nothing in it
            // carries no information, so it is left out.
            if (msg.content.trimmed().isEmpty())
                continue;
            m[QStringLiteral("content")] = msg.content;
        }

        messages.append(m);
    }
    // Current models reject a request that ends with an assistant turn
    // (prefill). A trailing assistant turn without tool calls, such as a
    // teammate's reply in a group chat, is sent as user content instead.
    if (!messages.isEmpty()) {
        QJsonObject last = messages.last().toObject();
        if (last.value(QStringLiteral("role")).toString() == QStringLiteral("assistant") &&
            last.value(QStringLiteral("content")).isString()) {
            last[QStringLiteral("role")] = QStringLiteral("user");
            messages[messages.size() - 1] = last;
        }
    }
    root[QStringLiteral("messages")] = messages;

    // Temperature (Anthropic uses 0.0–1.0). LlmConfig::temperature
    // defaults to -1 — the documented "use model default" sentinel
    // (llm-config.h); only forward it when the user has actually set a
    // value, otherwise let the model use its own default. Without this
    // gate, a fresh conversation 400s as soon as it's sent.
    // Claude 4.6 and later reject non-default sampling values, so the field
    // is only sent to earlier models.
    // The API takes 0 to 1, and manual extended thinking does not accept a
    // temperature at all.
    const bool legacyThinkingOn = rules.legacyThinking && req.config.thinkingMode;
    if (req.config.temperature >= 0 && !rules.rejectsSampling && !legacyThinkingOn) {
        root[QStringLiteral("temperature")] = qBound(0.0, req.config.temperature, 1.0);
    }

    if (!req.availableTools.isEmpty() && supportsToolCalling()) {
        QJsonArray toolsArr;
        const bool haveWhitelist = !req.allowedTools.isEmpty();
        for (const ToolSchema& tool : req.availableTools) {
            if (haveWhitelist && !req.allowedTools.contains(tool.name)) {
                continue;
            }
            QJsonObject def = tool.toAnthropicTool();
            const QString apiName = apiToolName(tool.name);
            def[QStringLiteral("name")] = apiName;
            m_toolNames.insert(apiName, tool.name);
            toolsArr.append(def);
        }
        if (!toolsArr.isEmpty()) {
            root[QStringLiteral("tools")] = toolsArr;
        }
    }

    // Per-conversation thinking toggle. Current models (4.6 and later)
    // take adaptive thinking; the 5.x generation, Fable and Mythos think by
    // default, and Opus 5.5 and Fable reject "disabled". Earlier Opus and
    // Sonnet 4 models and Claude 3.7 use "enabled" with a budget or
    // "disabled"; older models get no thinking field at all.
    if (rules.adaptiveThinking) {
        if (rules.thinksByDefault && !req.config.thinkingMode) {
            // Thinking cannot be turned off on these models. Low effort keeps
            // reasoning short, which is what callers that turned thinking
            // off (summaries, classifiers, sub-agents) want.
            root[QStringLiteral("output_config")] =
                QJsonObject{{QStringLiteral("effort"), QStringLiteral("low")}};
        }
        if (rules.thinksByDefault && root[QStringLiteral("max_tokens")].toInt() < 4096) {
            // Thinking counts toward max_tokens; a tiny cap would be used up
            // before any text is produced.
            root[QStringLiteral("max_tokens")] = 4096;
        }
        if (rules.thinksByDefault || req.config.thinkingMode) {
            QJsonObject thinking{{QStringLiteral("type"), QStringLiteral("adaptive")}};
            if (req.config.thinkingMode) {
                thinking[QStringLiteral("display")] = QStringLiteral("summarized");
            }
            if (rules.bindsThinking) {
                // Drop thinking that no longer matches the prefix (the
                // system prompt is rebuilt each turn) instead of failing.
                thinking[QStringLiteral("block_binding")] = QJsonObject{
                    {QStringLiteral("prefix_mismatch_behavior"), QStringLiteral("drop_block")}};
            }
            root[QStringLiteral("thinking")] = thinking;
        }
    } else if (rules.legacyThinking) {
        QJsonObject thinking;
        if (req.config.thinkingMode) {
            // budget_tokens must be at least 1024 and below max_tokens.
            if (root[QStringLiteral("max_tokens")].toInt() < 8192) {
                root[QStringLiteral("max_tokens")] = 8192;
            }
            thinking[QStringLiteral("type")] = QStringLiteral("enabled");
            thinking[QStringLiteral("budget_tokens")] = 4096;
        } else {
            thinking[QStringLiteral("type")] = QStringLiteral("disabled");
        }
        root[QStringLiteral("thinking")] = thinking;
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

/**
 * @brief Parses a single raw SSE line from the Anthropic stream.
 * @param line Raw bytes: either "event: <type>" or "data: <json>" or empty.
 * @sideeffects Updates m_pendingEventType; dispatches complete events.
 */
void AnthropicProvider::parseSseLine(const QByteArray& line) {
    // HttpClient hands over each SSE line with the "data: " prefix removed
    // and the "event:" lines dropped, so the event type is read from the
    // JSON "type" field, which the API sets to the event name.
    if (line.isEmpty() || line.startsWith(':'))
        return;
    QByteArray data = line;
    if (data.startsWith(QByteArrayLiteral("event:"))) {
        m_pendingEventType = QString::fromUtf8(data.mid(6).trimmed());
        return;
    }
    if (data.startsWith(QByteArrayLiteral("data:"))) {
        data = data.mid(5).trimmed();
    }
    if (data.isEmpty())
        return;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaLlm) << "Anthropic: failed to parse SSE data:" << err.errorString();
        m_pendingEventType.clear();
        return;
    }
    QString eventType = doc.object().value(QStringLiteral("type")).toString();
    if (eventType.isEmpty())
        eventType = m_pendingEventType;
    m_pendingEventType.clear();
    if (eventType.isEmpty() || eventType == QStringLiteral("ping"))
        return;
    parseSseEvent(eventType, data);
}

/**
 * @brief Dispatches a complete Anthropic SSE event for processing.
 * @param eventType The "event:" type string.
 * @param data The "data:" JSON payload bytes.
 * @sideeffects Emits chunkReceived(), requestFinished(), or requestError().
 */
void AnthropicProvider::parseSseEvent(const QString& eventType, const QByteArray& data) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaLlm) << "Anthropic: failed to parse SSE data for event" << eventType << ":"
                              << err.errorString();
        return;
    }
    const QJsonObject obj = doc.object();

    if (eventType == QStringLiteral("message_start")) {
        // Capture input token count from usage object
        const QJsonObject usage =
            obj[QStringLiteral("message")].toObject()[QStringLiteral("usage")].toObject();
        m_inputTokens = usage[QStringLiteral("input_tokens")].toInt(0);
        m_turnBlocks = QJsonArray();
        m_blockType.clear();
        qCDebug(verzetaLlm) << "Anthropic message_start: input_tokens=" << m_inputTokens;

    } else if (eventType == QStringLiteral("content_block_start")) {
        // Check if this is a tool_use block
        const QJsonObject block = obj[QStringLiteral("content_block")].toObject();
        const QString blockType = block[QStringLiteral("type")].toString();
        // Record the block so the whole turn can be sent back unchanged.
        m_blockType = blockType;
        m_blockText = block[blockType == QStringLiteral("thinking") ? QStringLiteral("thinking")
                                                                    : QStringLiteral("text")]
                          .toString();
        m_blockSignature = block[QStringLiteral("signature")].toString();
        m_blockData = block[QStringLiteral("data")].toString();
        if (blockType == QStringLiteral("tool_use")) {
            m_pendingToolUseId = block[QStringLiteral("id")].toString();
            m_pendingToolName = block[QStringLiteral("name")].toString();
            m_pendingToolInputJson.clear();
            qCDebug(verzetaLlm) << "Anthropic tool_use block started:" << m_pendingToolName;
        }
        // Text blocks: nothing to do — deltas will emit directly

    } else if (eventType == QStringLiteral("content_block_delta")) {
        const QJsonObject delta = obj[QStringLiteral("delta")].toObject();
        const QString deltaType = delta[QStringLiteral("type")].toString();

        if (deltaType == QStringLiteral("signature_delta")) {
            m_blockSignature += delta[QStringLiteral("signature")].toString();
        } else if (deltaType == QStringLiteral("text_delta")) {
            const QString text = delta[QStringLiteral("text")].toString();
            m_blockText += text;
            if (!text.isEmpty()) {
                LlmChunk chunk;
                chunk.delta = text;
                emit chunkReceived(chunk);
            }
        } else if (deltaType == QStringLiteral("input_json_delta")) {
            // Accumulate streamed tool input JSON
            m_pendingToolInputJson += delta[QStringLiteral("partial_json")].toString();
        } else if (deltaType == QStringLiteral("thinking_delta")) {
            // Extended thinking on claude-3-7 / claude-4 streams the
            // model's reasoning here.  Route to the dedicated
            // sidecar so the message-bubble disclosure renders it
            // and the request side stays content-only (per the L3
            // invariant pinned by test-request-builder-no-thinking-leak).
            const QString thinking = delta[QStringLiteral("thinking")].toString();
            m_blockText += thinking;
            if (!thinking.isEmpty()) {
                LlmChunk chunk;
                chunk.thinkingDelta = thinking;
                emit chunkReceived(chunk);
            }
        }
        // Other delta types are ignored. The thinking text and signature
        // are kept only inside this provider, to replay a tool-use turn
        // unchanged; they never enter the conversation history.

    } else if (eventType == QStringLiteral("content_block_stop")) {
        if (m_blockType == QStringLiteral("thinking")) {
            m_turnBlocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("thinking")},
                                            {QStringLiteral("thinking"), m_blockText},
                                            {QStringLiteral("signature"), m_blockSignature}});
        } else if (m_blockType == QStringLiteral("redacted_thinking")) {
            m_turnBlocks.append(
                QJsonObject{{QStringLiteral("type"), QStringLiteral("redacted_thinking")},
                            {QStringLiteral("data"), m_blockData}});
        } else if (m_blockType == QStringLiteral("text") && !m_blockText.isEmpty()) {
            m_turnBlocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                            {QStringLiteral("text"), m_blockText}});
        }
        if (!m_pendingToolName.isEmpty()) {
            QJsonObject argsObj;
            const QByteArray inputBytes = m_pendingToolInputJson.toUtf8();
            if (!inputBytes.isEmpty()) {
                QJsonParseError pErr;
                const QJsonDocument argDoc = QJsonDocument::fromJson(inputBytes, &pErr);
                if (pErr.error == QJsonParseError::NoError && argDoc.isObject()) {
                    argsObj = argDoc.object();
                } else {
                    qCWarning(verzetaLlm)
                        << "Anthropic: tool_use input JSON parse failed"
                        << "— tool:" << m_pendingToolName << "| error:" << pErr.errorString()
                        << "| body:" << inputBytes.left(200);
                }
            }
            m_turnBlocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                                            {QStringLiteral("id"), m_pendingToolUseId},
                                            {QStringLiteral("name"), m_pendingToolName},
                                            {QStringLiteral("input"), argsObj}});
            LlmChunk toolChunk;
            toolChunk.toolCallJson = QJsonObject{
                {QStringLiteral("id"), m_pendingToolUseId},
                {QStringLiteral("name"), m_toolNames.value(m_pendingToolName, m_pendingToolName)},
                {QStringLiteral("arguments"), argsObj}};
            toolChunk.finishReason = QStringLiteral("tool_calls");
            emit chunkReceived(toolChunk);

            m_pendingToolUseId.clear();
            m_pendingToolName.clear();
            m_pendingToolInputJson.clear();
        }
        m_blockType.clear();

    } else if (eventType == QStringLiteral("message_delta")) {
        // Capture stop_reason and output token count
        const QString stopReason =
            obj[QStringLiteral("delta")].toObject()[QStringLiteral("stop_reason")].toString();
        const QJsonObject usage = obj[QStringLiteral("usage")].toObject();
        m_outputTokens = usage[QStringLiteral("output_tokens")].toInt(0);

        if (!stopReason.isEmpty() && stopReason != QStringLiteral("null")) {
            const QString mappedReason = mapStopReason(stopReason);
            m_stopReason = mappedReason;

            LlmChunk finalChunk;
            finalChunk.finishReason = mappedReason;
            finalChunk.tokenCountEstimate = m_outputTokens;
            emit chunkReceived(finalChunk);
        }

    } else if (eventType == QStringLiteral("message_stop")) {
        rememberToolTurn();
        const int totalTokens = m_inputTokens + m_outputTokens;
        qCDebug(verzetaLlm) << "Anthropic message_stop: total_tokens=" << totalTokens;
        // "tool_calls" is what makes the app run the tools it was sent.
        emit requestFinished(m_stopReason.isEmpty() ? QStringLiteral("stop") : m_stopReason,
                             totalTokens);
        m_finishedEmitted = true;

    } else if (eventType == QStringLiteral("error")) {
        const QJsonObject errorObj = obj[QStringLiteral("error")].toObject();
        const QString errorMsg = errorObj[QStringLiteral("message")].toString();
        const QString errorType = errorObj[QStringLiteral("type")].toString();
        qCWarning(verzetaLlm) << "Anthropic error event:" << errorType << errorMsg;
        emit requestError(QStringLiteral("[%1] %2").arg(errorType, errorMsg));

    } else {
        qCDebug(verzetaLlm) << "Anthropic: unhandled event type:" << eventType;
    }
}

/**
 * @brief Stores the streamed turn for replay when it contains tool_use
 *        blocks, keyed by the joined tool_use ids.
 * @sideeffects Adds to the bounded replay cache and clears the turn buffer.
 */
void AnthropicProvider::rememberToolTurn() {
    QJsonArray toolUses;
    for (const QJsonValue& b : m_turnBlocks) {
        if (b.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("tool_use"))
            toolUses.append(b);
    }
    const QString key = toolTurnKey(toolUses);
    if (!key.isEmpty()) {
        if (!m_toolTurns.contains(key))
            m_toolTurnOrder.append(key);
        m_toolTurns.insert(key, m_turnBlocks);
        while (m_toolTurnOrder.size() > kMaxRememberedToolTurns) {
            m_toolTurns.remove(m_toolTurnOrder.takeFirst());
        }
    }
    m_turnBlocks = QJsonArray();
}

/**
 * @brief Converts a ToolSchema to Anthropic tool definition format.
 *
 *        Stub: returns an empty object, and nothing currently calls
 *        it. ToolCallingSchema performs this conversion for all
 *        providers, including the Anthropic tool definition form.
 * @param schema Tool schema to convert.
 * @return An empty object, always, while this stub stands in.
 */
QJsonObject AnthropicProvider::toolSchemaToAnthropic(const ToolSchema& /*schema*/) {
    // Unimplemented: see the note above.
    return {};
}

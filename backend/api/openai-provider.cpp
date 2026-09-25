// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file openai-provider.cpp
 * @brief Implementation of the OpenAI LLM provider adapter.
 *        Parses SSE streaming responses, tool calls, and handles chat completions.
 * @layer API
 * @dependencies HttpClient (Utility), Qt6::Core, Qt6::Network
 */

#include "openai-provider.h"

#include "../utils/json-utils.h"
#include "../utils/logger.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>


// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the OpenAIProvider.
 * @param http Reference to the shared HttpClient.
 * @param parent Optional Qt parent.
 */
OpenAIProvider::OpenAIProvider(HttpClient& http, QObject* parent)
    : ILLMProvider(parent), m_http(http) {
    connect(&m_http, &HttpClient::chunkReceived, this, &OpenAIProvider::parseSseLine);
    connect(&m_http, &HttpClient::streamFinished, this, [this]() {
        if (!m_pendingToolCalls.isEmpty()) {
            for (auto it = m_pendingToolCalls.cbegin(); it != m_pendingToolCalls.cend(); ++it) {
                const PendingToolCall& call = it.value();
                if (call.name.isEmpty())
                    continue;
                QJsonObject argsObj;
                const QByteArray argsBytes = call.args.toUtf8();
                if (!argsBytes.isEmpty()) {
                    QJsonParseError pErr;
                    const QJsonDocument argDoc = QJsonDocument::fromJson(argsBytes, &pErr);
                    if (pErr.error == QJsonParseError::NoError && argDoc.isObject()) {
                        argsObj = argDoc.object();
                    } else {
                        qCWarning(verzetaLlm)
                            << "OpenAI: tool_call arguments JSON parse failed"
                            << "— tool:" << call.name << "| error:" << pErr.errorString()
                            << "| body:" << argsBytes.left(200);
                    }
                }
                LlmChunk toolChunk;
                toolChunk.toolCallJson =
                    QJsonObject{{QStringLiteral("id"), call.id},
                                {QStringLiteral("name"), m_toolNames.value(call.name, call.name)},
                                {QStringLiteral("arguments"), argsObj}};
                toolChunk.finishReason = QStringLiteral("tool_calls");
                emit chunkReceived(toolChunk);
            }
            emit requestFinished(QStringLiteral("tool_calls"), 0);
            m_finishedEmitted = true;
        } else if (!m_finishedEmitted) {
            qCWarning(verzetaLlm) << "OpenAI: stream finished with no finish_reason and "
                                     "no tool_call — emitting synthetic stop so the "
                                     "cascade can finalize cleanly";
            emit requestFinished(QStringLiteral("stop"), 0);
            m_finishedEmitted = true;
        }
        m_pendingToolCalls.clear();
    });
    connect(&m_http, &HttpClient::errorOccurred, this, [this](const QString& msg) {
        emit requestError(msg);
    });
    connect(&m_modelsHttp, &HttpClient::errorOccurred, this, [this](const QString& msg) {
        qCWarning(verzetaLlm) << providerId() << "model list request failed:" << msg;
    });
    connect(&m_modelsHttp,
            &HttpClient::responseReceived,
            this,
            [this](int statusCode, const QByteArray& body) {
                if (statusCode == 200) {
                    // Model list response
                    QJsonParseError err;
                    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
                    if (err.error == QJsonParseError::NoError) {
                        QStringList models;
                        const QJsonArray data = doc.object()[QStringLiteral("data")].toArray();
                        for (const QJsonValue& v : data) {
                            const QJsonObject obj = v.toObject();
                            const QString id = obj[QStringLiteral("id")].toString();
                            if (id.isEmpty() || !isChatModel(id))
                                continue;
                            models.append(id);
                            // Opportunistically warm the context-window cache
                            // from whichever field the upstream exposes:
                            // OpenRouter's /api/v1/models gives top-level
                            // `context_length`; LM Studio's /api/v0/models
                            // gives `max_context_length`. Hosted OpenAI's
                            // /v1/models carries neither, so this is a no-op
                            // there. Single source of truth for both subclasses
                            // that refresh through this handler.
                            int ctx = 0;
                            if (obj.contains(QStringLiteral("context_length"))) {
                                ctx = obj[QStringLiteral("context_length")].toInt(0);
                            } else if (obj.contains(QStringLiteral("max_context_length"))) {
                                ctx = obj[QStringLiteral("max_context_length")].toInt(0);
                            }
                            if (!id.isEmpty() && ctx > 0) {
                                m_ctxCache.insert(id, ctx);
                            }
                        }
                        m_models = models;
                        emit modelsRefreshed(m_models);
                    }
                } else {
                    qCWarning(verzetaLlm) << "OpenAI model list returned status" << statusCode;
                }
            });
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void OpenAIProvider::setApiKey(const QString& key) {
    m_apiKey = key;
}

bool OpenAIProvider::requiresApiKey() const {
    return true;
}

void OpenAIProvider::setBaseUrl(const QString& url) {
    m_baseUrl = url;
}

// ---------------------------------------------------------------------------
// ILLMProvider interface
// ---------------------------------------------------------------------------

QString OpenAIProvider::providerId() const {
    return QStringLiteral("openai");
}

QString OpenAIProvider::displayName() const {
    return QStringLiteral("OpenAI");
}

QStringList OpenAIProvider::availableModels() {
    return m_models;
}

/**
 * @brief Fetches model list from GET /v1/models.
 * @sideeffects Requires API key. Emits modelsRefreshed() on success.
 */
void OpenAIProvider::refreshModels() {
    m_modelsHttp.get(QUrl(m_baseUrl + QStringLiteral("/models")), buildHeaders());
}

bool OpenAIProvider::isChatModel(const QString& id) const {
    // Only the OpenAI API lists models that cannot take chat completions
    // (embeddings, speech, images, moderation, realtime, Responses-only).
    if (providerId() != QStringLiteral("openai"))
        return true;
    const QString m = id.toLower();
    static const QStringList notChat = {
        QStringLiteral("embedding"),     QStringLiteral("tts"),      QStringLiteral("whisper"),
        QStringLiteral("transcribe"),    QStringLiteral("dall-e"),   QStringLiteral("image"),
        QStringLiteral("moderation"),    QStringLiteral("realtime"), QStringLiteral("audio"),
        QStringLiteral("sora"),          QStringLiteral("search"),   QStringLiteral("computer-use"),
        QStringLiteral("deep-research"), QStringLiteral("codex"),    QStringLiteral("instruct"),
        QStringLiteral("davinci"),       QStringLiteral("babbage"),  QStringLiteral("daybreak"),
        QStringLiteral("rosalind"),      QStringLiteral("cyber")};
    for (const QString& word : notChat) {
        if (m.contains(word))
            return false;
    }
    if (m.endsWith(QStringLiteral("-pro")) || m.contains(QStringLiteral("-pro-")))
        return false;
    return true;
}

int OpenAIProvider::contextWindowFor(const QString& model) {
    if (model.isEmpty()) {
        return 0;
    }
    const auto it = m_ctxCache.constFind(model);
    if (it != m_ctxCache.constEnd()) {
        return it.value();
    }
    // No live value cached — fall back to the published-family map.
    return publishedContextWindow(model);
}

int OpenAIProvider::publishedContextWindow(const QString& model) const {
    // OpenAI hosted-model published context-window map. No network — these are
    // the documented maxima for OpenAI's own models.
    const QString m = model.toLower();
    if (m.contains(QStringLiteral("gpt-4-32k"))) {
        return 32768;
    }
    if (m.contains(QStringLiteral("gpt-3.5"))) {
        return 16385;
    }
    if (m.contains(QStringLiteral("gpt-4o")) || m.contains(QStringLiteral("gpt-4.1")) ||
        m.contains(QStringLiteral("o3")) || m.contains(QStringLiteral("o4")) ||
        m.contains(QStringLiteral("gpt-4-turbo"))) {
        return 128000;
    }
    // Plain gpt-4 (not -turbo / -32k / -4o) — the original 8k window.
    if (m.contains(QStringLiteral("gpt-4"))) {
        return 8192;
    }
    // Any other (modern) OpenAI id — assume the current 128k default.
    return 128000;
}

bool OpenAIProvider::supportsStreaming() const {
    return true;
}

bool OpenAIProvider::supportsToolCalling() const {
    return true;
}

bool OpenAIProvider::supportsVision() const {
    return true;
}

/*
 * @brief Sends a chat completion request to OpenAI via SSE streaming.
 * @param req Complete LLM request payload.
 * @sideeffects Initiates HTTPS streaming request.
 */
void OpenAIProvider::sendRequest(const LlmRequest& req) {
    if (requiresApiKey() && m_apiKey.isEmpty()) {
        emit requestError(QStringLiteral("OpenAI API key not configured"));
        return;
    }

    m_pendingToolCalls.clear();
    m_finishedEmitted = false;  // fresh request, no terminal signal yet
    m_thinkingHoister.reset();

    if (providerId() == QStringLiteral("openai") &&
        req.config.modelName.toLower().startsWith(QStringLiteral("gpt-6-astra")) &&
        !req.availableTools.isEmpty() && supportsToolCalling()) {
        // Its tool calling is only offered through the Responses API,
        // which this app does not use; failing clearly beats sending a
        // request the API rejects.
        emit requestError(QStringLiteral(
            "GPT-6 Astra cannot use tools through the Chat Completions API. "
            "Turn off Tools for this conversation, or choose GPT-6 Sol or GPT-6 Luna."));
        return;
    }

    const QByteArray body = buildRequestBody(req);
    const QUrl url(m_baseUrl + QStringLiteral("/chat/completions"));

    qCDebug(verzetaLlm) << "OpenAI request to" << url.toString() << "model:" << req.config.modelName
                        << "body bytes:" << body.size()
                        << "messages in req:" << req.messages.size();

    {
        const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!cacheDir.isEmpty()) {
            QDir().mkpath(cacheDir);
            const QString path =
                cacheDir + QStringLiteral("/last-openai-%1-request.json").arg(providerId());
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(body);
                f.close();
                qCDebug(verzetaLlm) << "OpenAI-family request body dumped to" << path;
            }
        }
    }

    m_http.postStream(url, body, buildHeaders());
}

/**
 * @brief Cancels the in-progress request.
 */
void OpenAIProvider::cancelRequest() {
    m_http.cancel();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Builds the Authorization header. API key is never logged.
 * @return Header map with Authorization: Bearer {key} and other standard headers.
 */
QMap<QString, QString> OpenAIProvider::buildHeaders() const {
    return {
        {QStringLiteral("Authorization"), QStringLiteral("Bearer ") + m_apiKey},
    };
}

namespace {
/** @brief Function name in the form the API accepts (letters, digits, _ and -, at most 64); MCP
 * "server:tool" becomes "server__tool". */
QString apiToolName(const QString& name) {
    QString out;
    for (const QChar c : name) {
        if (c == QLatin1Char(':'))
            out += QStringLiteral("__");
        else if ((c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('_') ||
                 c == QLatin1Char('-'))
            out += c;
        else
            out += QLatin1Char('_');
    }
    return out.left(64);
}
}  // namespace

/**
 * @brief Whether a model is one of OpenAI's reasoning models, which accept
 *        only their default sampling values.
 * @param model Model id sent in the request.
 * @returns true for the o-series, GPT-5 and GPT-6 (except chat variants)
 *          when this provider is the OpenAI API itself; false otherwise.
 */
bool OpenAIProvider::isOpenAiReasoningModel(const QString& model) const {
    if (providerId() != QStringLiteral("openai"))
        return false;
    const QString m = model.toLower();
    if (m.startsWith(QStringLiteral("gpt-5")) || m.startsWith(QStringLiteral("gpt-6")))
        return !m.contains(QStringLiteral("chat"));
    return m.startsWith(QStringLiteral("o1")) || m.startsWith(QStringLiteral("o3")) ||
           m.startsWith(QStringLiteral("o4"));
}

/**
 * @brief Builds the JSON body for POST /v1/chat/completions.
 * @param req LLM request payload.
 * @return Compact JSON bytes.
 */
QByteArray OpenAIProvider::buildRequestBody(const LlmRequest& req) {
    QJsonObject root;
    root[QStringLiteral("model")] = req.config.modelName;
    // The response is always read as a stream, so streaming is always
    // requested regardless of the conversation's streaming toggle.
    root[QStringLiteral("stream")] = true;
    // LlmConfig::temperature defaults to -1 — the documented "use model
    // default" sentinel (llm-config.h). Sending it as-is makes
    // OpenAI-compatible providers 400 ("Expected temperature to be at
    // least 0"). Only forward it when the user has actually set a
    // value, otherwise let the model use its own default — same gate
    // OllamaProvider applies.
    // OpenAI's reasoning models (the o-series, GPT-5 and GPT-6, except chat
    // variants) reject custom sampling values with HTTP 400, so
    // they are left at the model default. This applies to the OpenAI API
    // itself only; OpenAI-compatible subclasses keep every field.
    const bool openAiReasoning = isOpenAiReasoningModel(req.config.modelName);
    if (req.config.temperature >= 0 && !openAiReasoning) {
        root[QStringLiteral("temperature")] = req.config.temperature;
    }
    if (req.config.presencePenalty >= 0 && !openAiReasoning) {
        root[QStringLiteral("presence_penalty")] = req.config.presencePenalty;
    }
    if (req.config.frequencyPenalty >= 0 && !openAiReasoning) {
        root[QStringLiteral("frequency_penalty")] = req.config.frequencyPenalty;
    }
    if (req.config.maxTokens > 0) {
        const bool openAiApi = providerId() == QStringLiteral("openai");
        root[openAiApi ? QStringLiteral("max_completion_tokens") : QStringLiteral("max_tokens")] =
            req.config.maxTokens;
    }

    // Build messages array
    QJsonArray messages;

    // System message (OpenAI: in messages array with role "system")
    if (!req.systemPrompt.isEmpty()) {
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                    {QStringLiteral("content"), req.systemPrompt}});
    }

    for (const LlmMessage& msg : req.messages) {
        QJsonObject m;
        m[QStringLiteral("role")] = msg.role;

        // Vision: use content array with text + image_url objects
        if (!msg.images.isEmpty()) {
            QJsonArray contentArr;
            if (!msg.content.isEmpty()) {
                QJsonObject textPart;
                textPart[QStringLiteral("type")] = QStringLiteral("text");
                textPart[QStringLiteral("text")] = msg.content;
                contentArr.append(textPart);
            }
            for (const LlmImageData& img : msg.images) {
                QJsonObject imgUrl;
                imgUrl[QStringLiteral("url")] =
                    QStringLiteral("data:%1;base64,%2")
                        .arg(img.mimeType, QString::fromLatin1(img.base64));
                QJsonObject imgPart;
                imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
                imgPart[QStringLiteral("image_url")] = imgUrl;
                contentArr.append(imgPart);
            }
            m[QStringLiteral("content")] = contentArr;
        } else {
            m[QStringLiteral("content")] = msg.content;
        }

        if (!msg.toolCallsJson.isEmpty()) {
            QJsonArray normalised;
            // QJsonArray has no reserve(); growing in place is fine —
            // the call sites only see a handful of tool_calls per turn.
            for (const QJsonValue& tcVal : msg.toolCallsJson) {
                if (!tcVal.isObject()) {
                    normalised.append(tcVal);
                    continue;
                }
                QJsonObject tc = tcVal.toObject();
                if (!tc.contains(QStringLiteral("function"))) {
                    // Flat {id, name, arguments} shape, as the agent and
                    // sub-agent loops store it; the API wants the nested
                    // function object.
                    tc = QJsonObject{
                        {QStringLiteral("id"), tc.value(QStringLiteral("id"))},
                        {QStringLiteral("type"), QStringLiteral("function")},
                        {QStringLiteral("function"),
                         QJsonObject{{QStringLiteral("name"), tc.value(QStringLiteral("name"))},
                                     {QStringLiteral("arguments"),
                                      tc.value(QStringLiteral("arguments"))}}}};
                }
                if (tc.contains(QStringLiteral("function"))) {
                    QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
                    fn[QStringLiteral("name")] =
                        apiToolName(fn.value(QStringLiteral("name")).toString());
                    const QJsonValue argsVal = fn.value(QStringLiteral("arguments"));
                    if (!argsVal.isString()) {
                        // Object / array → JSON-stringify (compact, so
                        // it matches what the model would have streamed
                        // as raw argument bytes). null / missing →
                        // "{}" — safe default the upstream validator
                        // accepts as "no arguments".
                        QByteArray stringified;
                        if (argsVal.isObject()) {
                            stringified =
                                QJsonDocument(argsVal.toObject()).toJson(QJsonDocument::Compact);
                        } else if (argsVal.isArray()) {
                            stringified =
                                QJsonDocument(argsVal.toArray()).toJson(QJsonDocument::Compact);
                        }
                        if (stringified.isEmpty()) {
                            stringified = QByteArrayLiteral("{}");
                        }
                        fn[QStringLiteral("arguments")] = QString::fromUtf8(stringified);
                        tc[QStringLiteral("function")] = fn;
                    }
                }
                normalised.append(tc);
            }
            m[QStringLiteral("tool_calls")] = normalised;
        }
        if (!msg.toolCallId.isEmpty()) {
            m[QStringLiteral("tool_call_id")] = msg.toolCallId;
        }

        messages.append(m);
    }
    // Subclass hook: strict-template servers (OpenAICompatProvider)
    // rewrite the array here. Base / OpenAI / OpenRouter / DeepSeek leave
    // it untouched (no-op), so their wire shape is byte-identical.
    normalizeMessages(messages);
    root[QStringLiteral("messages")] = messages;

    // Stream options — request usage stats in stream
    if (req.config.stream) {
        QJsonObject streamOpts;
        streamOpts[QStringLiteral("include_usage")] = true;
        root[QStringLiteral("stream_options")] = streamOpts;
    }

    if (!req.availableTools.isEmpty() && supportsToolCalling()) {
        QJsonArray toolsArr;
        const bool haveWhitelist = !req.allowedTools.isEmpty();
        for (const ToolSchema& tool : req.availableTools) {
            if (haveWhitelist && !req.allowedTools.contains(tool.name)) {
                continue;
            }
            QJsonObject def = tool.toOpenAIFunction();
            QJsonObject fn = def.value(QStringLiteral("function")).toObject();
            const QString apiName = apiToolName(tool.name);
            fn[QStringLiteral("name")] = apiName;
            def[QStringLiteral("function")] = fn;
            m_toolNames.insert(apiName, tool.name);
            toolsArr.append(def);
        }
        if (!toolsArr.isEmpty()) {
            root[QStringLiteral("tools")] = toolsArr;
            root[QStringLiteral("tool_choice")] = QStringLiteral("auto");
        }
    }

    // Per-conversation `activeThinking` toggle — dispatched through
    // the virtual hook so each OpenAI-family subclass can inject the
    // shape its API expects (reasoning_effort for OpenAI o1/o3,
    // reasoning {effort} for OpenRouter, chat_template_kwargs for
    // self-hosted servers, etc.).  No-op when the subclass does not
    // override AND the active model does not accept the base
    // implementation's reasoning_effort parameter.
    applyReasoningConfig(root, req);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void OpenAIProvider::normalizeMessages(QJsonArray& /*messages*/) const {
    // No-op for the standard OpenAI family. OpenAI / OpenRouter / DeepSeek
    // / llama.cpp accept Verzeta's multi-agent history verbatim, so their
    // request body is unchanged. OpenAICompatProvider overrides this.
}

void OpenAIProvider::applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const {
    // OpenAI's `reasoning_effort` parameter is exclusive to the o1 /
    // o3 reasoning families.  Sending it to gpt-4o / gpt-3.5 etc.
    // returns HTTP 400 ("Unrecognized request argument: reasoning_effort")
    // — so the gate is mandatory, not advisory.  When the model
    // matches, the toggle maps `thinkingMode=true` → `"high"` and
    // `false` → `"low"`; `"low"` is sent rather than omitting the
    // field so the wire shape stays consistent across both toggle
    // states.
    const QString model = req.config.modelName.toLower();
    if (!isOpenAiReasoningModel(model))
        return;
    if (root.contains(QStringLiteral("tools")) &&
        (model.startsWith(QStringLiteral("gpt-6-sol")) ||
         model.startsWith(QStringLiteral("gpt-6-luna")))) {
        // These models take tools on Chat Completions only without reasoning.
        root[QStringLiteral("reasoning_effort")] = QStringLiteral("none");
        return;
    }
    root[QStringLiteral("reasoning_effort")] =
        req.config.thinkingMode ? QStringLiteral("high") : QStringLiteral("low");
    // Non-reasoning OpenAI models: no parameter to send.  Subclasses
    // override this method to add their own shape.
}

/**
 * @brief Parses a single SSE "data:" payload line from the OpenAI stream.
 * @param line Raw JSON bytes (the payload after "data: " prefix has been stripped).
 * @sideeffects Emits chunkReceived() for content/tool deltas.
 *              Emits requestFinished() when finish_reason is set.
 */
void OpenAIProvider::parseSseLine(const QByteArray& line) {
    if (line.isEmpty()) {
        return;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaLlm) << "OpenAI: failed to parse SSE line:" << err.errorString();
        return;
    }

    const QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("error"))) {
        // A mid-stream error object ends the response; report it instead
        // of finishing as if the reply were complete.
        const QJsonValue e = obj.value(QStringLiteral("error"));
        const QString msg =
            e.isObject() ? e.toObject().value(QStringLiteral("message")).toString() : e.toString();
        emit requestError(msg.isEmpty() ? QStringLiteral("The provider returned an error.") : msg);
        m_finishedEmitted = true;
        m_pendingToolCalls.clear();
        return;
    }
    const QJsonArray choices = obj[QStringLiteral("choices")].toArray();
    if (choices.isEmpty()) {
        // May be the final usage stats object — log token counts
        const QJsonObject usage = obj[QStringLiteral("usage")].toObject();
        if (!usage.isEmpty()) {
            const int total = usage[QStringLiteral("total_tokens")].toInt();
            qCDebug(verzetaLlm) << "OpenAI usage: total_tokens=" << total;
        }
        return;
    }

    const QJsonObject choice = choices[0].toObject();
    const QString finishReason = choice[QStringLiteral("finish_reason")].toString();
    const QJsonObject delta = choice[QStringLiteral("delta")].toObject();

    // Reasoning sidecar — three sources collapsed into one emission:
    //   1. `delta.reasoning`         — vLLM / Qwen / OpenRouter-normalised
    //   2. `delta.reasoning_content` — DeepSeek-R1 style
    //   3. Inline `<think>...</think>` blocks extracted from
    //      `delta.content` via the shared hoister.
    // Captured into a single thinking buffer + emitted on the
    // chunk's `thinkingDelta` field.  Empty on every non-reasoning
    // chunk — zero per-chunk overhead in the common case.
    const QString rawReasoning = delta[QStringLiteral("reasoning")].toString();
    const QString rawReasoningContent = delta[QStringLiteral("reasoning_content")].toString();

    // Text content delta — route through the hoister so inline
    // `<think>...</think>` blocks are pulled into the thinking
    // sidecar before the content reaches downstream consumers.
    QString content = delta[QStringLiteral("content")].toString();
    QString inlineHoisted;
    if (!content.isEmpty()) {
        const auto hoist = m_thinkingHoister.feed(content);
        content = hoist.content;
        inlineHoisted = hoist.thinking;
    }

    const QString thinkingForChunk = rawReasoning + rawReasoningContent + inlineHoisted;

    if (!content.isEmpty() || !thinkingForChunk.isEmpty()) {
        LlmChunk chunk;
        chunk.delta = content;
        chunk.thinkingDelta = thinkingForChunk;
        emit chunkReceived(chunk);
    }

    static const auto stripHarmonyTokens = [](QString name) {
        // Cut at the FIRST `<|` — every Harmony control token starts
        // with that prefix. Function names cannot legitimately contain
        // `<` per the OpenAI tool-schema spec
        // (^[a-zA-Z0-9_-]{1,64}$), so any `<|...|>`-leaked suffix is
        // unambiguously a chat-template artifact.
        const int idx = name.indexOf(QStringLiteral("<|"));
        if (idx >= 0)
            name.truncate(idx);
        return name.trimmed();
    };
    const QJsonArray toolCallsDelta = delta[QStringLiteral("tool_calls")].toArray();
    for (const QJsonValue& tcVal : toolCallsDelta) {
        const QJsonObject tc = tcVal.toObject();
        // `index` separates several calls streamed in one response.
        PendingToolCall& call = m_pendingToolCalls[tc[QStringLiteral("index")].toInt(0)];
        const QString tcId = tc[QStringLiteral("id")].toString();
        if (!tcId.isEmpty())
            call.id = tcId;
        const QJsonObject func = tc[QStringLiteral("function")].toObject();
        const QString rawName = func[QStringLiteral("name")].toString();
        const QString name = stripHarmonyTokens(rawName);
        if (!rawName.isEmpty() && name != rawName) {
            qCDebug(verzetaLlm) << "OpenAI: stripped Harmony control tokens from tool name —"
                                << "raw:" << rawName << "| sanitised:" << name;
        }
        if (!name.isEmpty())
            call.name = name;
        // Arguments arrive as streaming partial JSON strings
        call.args += func[QStringLiteral("arguments")].toString();
    }

    // Finish reason signals end of response
    if (!finishReason.isEmpty() && finishReason != QStringLiteral("null")) {
        // For tool_calls, the tool chunk is emitted from streamFinished handler
        if (finishReason != QStringLiteral("tool_calls")) {
            LlmChunk finalChunk;
            finalChunk.finishReason = finishReason;
            emit chunkReceived(finalChunk);
            emit requestFinished(finishReason, 0);
            m_finishedEmitted = true;
        }
    }
}

/**
 * @brief Converts a ToolSchema to OpenAI function definition format.
 *
 *        Stub: returns an empty object, and nothing currently calls
 *        it. ToolCallingSchema performs this conversion for all
 *        providers, including the OpenAI function definition form.
 * @param schema Tool schema to convert.
 * @return An empty object, always, while this stub stands in.
 */
QJsonObject OpenAIProvider::toolSchemaToOpenAI(const ToolSchema& /*schema*/) {
    // Unimplemented: see the note above.
    return {};
}

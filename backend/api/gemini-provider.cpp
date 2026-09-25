// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file gemini-provider.cpp
 * @brief Implementation of the Google Gemini LLM provider adapter.
 *        Handles Gemini's unique streaming SSE format, role mapping, and
 *        functionCall parsing.
 * @layer API
 * @dependencies HttpClient (Utility), Qt6::Core, Qt6::Network
 */

#include "gemini-provider.h"

#include "../utils/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QUuid>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the GeminiProvider.
 * @param http Reference to the shared HttpClient.
 * @param parent Optional Qt parent.
 */
GeminiProvider::GeminiProvider(HttpClient& http, QObject* parent)
    : ILLMProvider(parent), m_http(http) {
    connect(&m_http, &HttpClient::chunkReceived, this, &GeminiProvider::parseStreamChunk);
    connect(&m_http, &HttpClient::streamFinished, this, [this]() {
        if (!m_finishedEmitted) {
            qCWarning(verzetaLlm) << "Gemini: stream finished without a finishReason "
                                     "chunk — emitting synthetic stop so the cascade "
                                     "can finalize cleanly";
            emit requestFinished(QStringLiteral("stop"), 0);
            m_finishedEmitted = true;
        }
    });
    connect(&m_http, &HttpClient::errorOccurred, this, [this](const QString& msg) {
        emit requestError(msg);
    });
    connect(&m_modelsHttp, &HttpClient::errorOccurred, this, [](const QString& msg) {
        qCWarning(verzetaLlm) << "Gemini model list request failed:" << msg;
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
                        const QJsonArray arr = doc.object()[QStringLiteral("models")].toArray();
                        for (const QJsonValue& v : arr) {
                            const QJsonObject mo = v.toObject();
                            // Only chat-capable models; the list also carries
                            // embedding, image and audio models.
                            const QJsonArray methods =
                                mo[QStringLiteral("supportedGenerationMethods")].toArray();
                            if (!methods.isEmpty() &&
                                !methods.contains(QJsonValue(QStringLiteral("generateContent"))))
                                continue;
                            const QString name = mo[QStringLiteral("name")].toString();
                            // Strip "models/" prefix from Gemini API names
                            if (name.startsWith(QStringLiteral("models/"))) {
                                models.append(name.mid(7));
                            } else if (!name.isEmpty()) {
                                models.append(name);
                            }
                        }
                        if (!models.isEmpty()) {
                            m_models = models;
                            emit modelsRefreshed(m_models);
                        }
                    }
                } else {
                    qCWarning(verzetaLlm) << "Gemini model list returned status" << statusCode;
                }
            });
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void GeminiProvider::setBaseUrl(const QString& url) {
    m_baseUrl = url;
}

void GeminiProvider::setApiKey(const QString& key) {
    m_apiKey = key;
}

// ---------------------------------------------------------------------------
// ILLMProvider interface
// ---------------------------------------------------------------------------

QString GeminiProvider::providerId() const {
    return QStringLiteral("gemini");
}

QString GeminiProvider::displayName() const {
    return QStringLiteral("Google Gemini");
}

QStringList GeminiProvider::availableModels() {
    return m_models;
}

/**
 * @brief Fetches model list from GET /v1beta/models.
 * @sideeffects Requires API key. Emits modelsRefreshed() on success.
 */
void GeminiProvider::refreshModels() {
    if (m_apiKey.isEmpty()) {
        emit modelsRefreshed(m_models);  // Return cached static list
        return;
    }
    QUrl url(m_baseUrl + QStringLiteral("/models"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("key"), m_apiKey);
    query.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("1000"));
    url.setQuery(query);
    m_modelsHttp.get(url);
}

int GeminiProvider::contextWindowFor(const QString& model) {
    const QString m = model.toLower();
    if (m.contains(QStringLiteral("1.5")) || m.contains(QStringLiteral("2.0")) ||
        m.contains(QStringLiteral("2.5")) || m.contains(QStringLiteral("exp")) ||
        m.contains(QStringLiteral("flash")) || m.contains(QStringLiteral("pro"))) {
        return 1000000;
    }
    // Older / unrecognised Gemini family (e.g. gemini-1.0-pro is "pro" and
    // already handled above; this covers anything else) → conservative window.
    return 32768;
}

bool GeminiProvider::supportsStreaming() const {
    return true;
}

bool GeminiProvider::supportsToolCalling() const {
    return true;
}

bool GeminiProvider::supportsVision() const {
    return true;
}

/*
 * @brief Sends a chat completion request to Gemini via SSE streaming.
 * @param req Complete LLM request payload.
 * @sideeffects Initiates HTTPS streaming request. API key injected as query param.
 */
void GeminiProvider::sendRequest(const LlmRequest& req) {
    if (m_apiKey.isEmpty()) {
        emit requestError(QStringLiteral("Google Gemini API key not configured"));
        return;
    }

    m_finishedEmitted = false;  // fresh request, no terminal signal yet
    m_sawFunctionCall = false;

    const QByteArray body = buildRequestBody(req);

    // Gemini endpoint: /v1beta/models/{model}:streamGenerateContent?key=...&alt=sse
    QUrl url(m_baseUrl + QStringLiteral("/models/") + req.config.modelName +
             QStringLiteral(":streamGenerateContent"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("key"), m_apiKey);
    query.addQueryItem(QStringLiteral("alt"), QStringLiteral("sse"));
    url.setQuery(query);

    // No Authorization header — auth via query param
    qCDebug(verzetaLlm) << "Gemini request to" << url.host() << "model:" << req.config.modelName;
    m_http.postStream(url, body, {});
}

/**
 * @brief Cancels the in-progress request.
 */
void GeminiProvider::cancelRequest() {
    m_http.cancel();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Builds the JSON body for POST .../streamGenerateContent.
 * @param req LLM request payload.
 * @return Compact JSON bytes.
 */
namespace {
/** @brief Function name in the form the API accepts: letters, digits, _ . -; MCP "server:tool"
 * becomes "server__tool". */
QString apiToolName(const QString& name) {
    QString out;
    for (const QChar c : name) {
        if (c == QLatin1Char(':'))
            out += QStringLiteral("__");
        else if ((c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('_') ||
                 c == QLatin1Char('.') || c == QLatin1Char('-'))
            out += c;
        else
            out += QLatin1Char('_');
    }
    return out.left(64);
}
constexpr const char* kSkipSignature = "skip_thought_signature_validator";
}  // namespace

QByteArray GeminiProvider::buildRequestBody(const LlmRequest& req) {
    QJsonObject root;

    // System instruction (top-level, separate from conversation messages)
    if (!req.systemPrompt.isEmpty()) {
        root[QStringLiteral("systemInstruction")] =
            QJsonObject{{QStringLiteral("parts"),
                         QJsonArray{QJsonObject{{QStringLiteral("text"), req.systemPrompt}}}}};
    }

    // Function names by tool call id, so a tool result can be labelled with
    // the function it answers (Gemini matches results by name).
    QHash<QString, QString> toolNames;
    for (const LlmMessage& msg : req.messages) {
        for (const QJsonValue& v : msg.toolCallsJson) {
            const QJsonObject tc = v.toObject();
            const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
            const QString name = fn.isEmpty() ? tc.value(QStringLiteral("name")).toString()
                                              : fn.value(QStringLiteral("name")).toString();
            toolNames.insert(tc.value(QStringLiteral("id")).toString(), name);
        }
    }

    // Build contents array — Gemini uses "user" and "model" roles
    QJsonArray contents;
    // Every functionResponse for one model turn goes in the same user
    // content, so consecutive tool results are merged.
    auto appendFunctionResponse = [&contents](const QJsonObject& part) {
        if (!contents.isEmpty()) {
            QJsonObject last = contents.last().toObject();
            QJsonArray lastParts = last.value(QStringLiteral("parts")).toArray();
            if (last.value(QStringLiteral("role")).toString() == QStringLiteral("user") &&
                !lastParts.isEmpty() &&
                lastParts.last().toObject().contains(QStringLiteral("functionResponse"))) {
                lastParts.append(part);
                last[QStringLiteral("parts")] = lastParts;
                contents[contents.size() - 1] = last;
                return;
            }
        }
        contents.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("parts"), QJsonArray{part}}});
    };
    for (const LlmMessage& msg : req.messages) {
        if (msg.role == QStringLiteral("tool") && !msg.toolCallId.isEmpty()) {
            appendFunctionResponse(QJsonObject{
                {QStringLiteral("functionResponse"),
                 QJsonObject{{QStringLiteral("name"),
                              apiToolName(toolNames.value(msg.toolCallId, msg.toolCallId))},
                             {QStringLiteral("response"),
                              QJsonObject{{QStringLiteral("content"), msg.content}}}}}});
            continue;
        }

        QJsonObject contentObj;
        // The contents array carries only user and model turns; a system
        // note in the conversation is passed as user text.
        contentObj[QStringLiteral("role")] =
            msg.role == QStringLiteral("system") ? QStringLiteral("user") : toGeminiRole(msg.role);

        QJsonArray parts;
        for (const LlmImageData& img : msg.images) {
            parts.append(QJsonObject{
                {QStringLiteral("inlineData"),
                 QJsonObject{{QStringLiteral("mimeType"), img.mimeType},
                             {QStringLiteral("data"), QString::fromLatin1(img.base64)}}}});
        }
        if (!msg.content.isEmpty()) {
            parts.append(QJsonObject{{QStringLiteral("text"), msg.content}});
        }

        if (!msg.toolCallsJson.isEmpty()) {
            for (const QJsonValue& v : msg.toolCallsJson) {
                // History keeps the OpenAI shape; Gemini wants {name, args}.
                const QJsonObject tc = v.toObject();
                const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
                const QString name = fn.isEmpty() ? tc.value(QStringLiteral("name")).toString()
                                                  : fn.value(QStringLiteral("name")).toString();
                const QJsonValue rawArgs = fn.isEmpty() ? tc.value(QStringLiteral("arguments"))
                                                        : fn.value(QStringLiteral("arguments"));
                QJsonObject args;
                if (rawArgs.isObject()) {
                    args = rawArgs.toObject();
                } else if (rawArgs.isString()) {
                    const QJsonDocument d = QJsonDocument::fromJson(rawArgs.toString().toUtf8());
                    if (d.isObject())
                        args = d.object();
                }
                QJsonObject part{{QStringLiteral("functionCall"),
                                  QJsonObject{{QStringLiteral("name"), apiToolName(name)},
                                              {QStringLiteral("args"), args}}}};
                // Gemini 3 checks a signature on every replayed function
                // call. Calls this provider made carry their own; calls
                // made by another provider or model get the documented
                // value that skips the check.
                const QString sig =
                    m_thoughtSignatures.value(tc.value(QStringLiteral("id")).toString());
                part[QStringLiteral("thoughtSignature")] =
                    sig.isEmpty() ? QString::fromLatin1(kSkipSignature) : sig;
                parts.append(part);
            }
        }

        // A content with no parts is rejected on every later request.
        if (parts.isEmpty())
            continue;
        contentObj[QStringLiteral("parts")] = parts;
        contents.append(contentObj);
    }
    root[QStringLiteral("contents")] = contents;

    // Generation config — LlmConfig::temperature defaults to -1 (the
    // "use model default" sentinel in llm-config.h); only forward it
    // when the user has actually set a value, so Gemini falls back to
    // its own model default rather than 400-ing on a fresh conv.
    QJsonObject genConfig;
    if (req.config.temperature >= 0) {
        genConfig[QStringLiteral("temperature")] = req.config.temperature;
    }
    if (req.config.presencePenalty >= 0) {
        genConfig[QStringLiteral("presencePenalty")] = req.config.presencePenalty;
    }
    if (req.config.frequencyPenalty >= 0) {
        genConfig[QStringLiteral("frequencyPenalty")] = req.config.frequencyPenalty;
    }
    if (req.config.maxTokens > 0) {
        genConfig[QStringLiteral("maxOutputTokens")] = req.config.maxTokens;
    }

    // Per-conversation `activeThinking` toggle. Gemini 3 uses thinkingLevel
    // (below). The Gemini 2.5 family (flash + pro + flash-lite) exposes it via
    // `generationConfig.thinkingConfig.thinkingBudget`:
    //   -1 → dynamic budget, model decides how many tokens to spend
    //    0 → thinking disabled
    // Older Gemini families (1.5, 2.0) reject the field, so it is
    // gated on the `gemini-2.5*` model name prefix.  Companion
    // response-parser change skips `parts[*].thought == true` items
    // so reasoning content is silently dropped (matches the universal
    // rule that thinking visibility is a separate UI plan).
    if (req.config.modelName.startsWith(QStringLiteral("gemini-2.5"))) {
        // gemini-2.5-pro cannot turn thinking off (budget 0 is rejected);
        // it keeps its default when the toggle is off.
        const bool pro = req.config.modelName.startsWith(QStringLiteral("gemini-2.5-pro"));
        if (req.config.thinkingMode || !pro) {
            genConfig[QStringLiteral("thinkingConfig")] =
                QJsonObject{{QStringLiteral("thinkingBudget"), req.config.thinkingMode ? -1 : 0}};
        }
    } else if (req.config.modelName.startsWith(QStringLiteral("gemini-3"))) {
        // Gemini 3 always thinks; its depth is set by thinkingLevel.
        genConfig[QStringLiteral("thinkingConfig")] =
            QJsonObject{{QStringLiteral("thinkingLevel"),
                         req.config.thinkingMode ? QStringLiteral("high") : QStringLiteral("low")}};
    }

    root[QStringLiteral("generationConfig")] = genConfig;

    if (!req.availableTools.isEmpty() && supportsToolCalling()) {
        QJsonArray fnDecls;
        const bool haveWhitelist = !req.allowedTools.isEmpty();
        for (const ToolSchema& tool : req.availableTools) {
            if (haveWhitelist && !req.allowedTools.contains(tool.name)) {
                continue;
            }
            QJsonObject decl = tool.toGeminiFunction();
            const QString apiName = apiToolName(tool.name);
            decl[QStringLiteral("name")] = apiName;
            m_toolNames.insert(apiName, tool.name);
            fnDecls.append(decl);
        }
        if (!fnDecls.isEmpty()) {
            QJsonObject toolsObj;
            toolsObj[QStringLiteral("functionDeclarations")] = fnDecls;
            root[QStringLiteral("tools")] = QJsonArray{toolsObj};
        }
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

/**
 * @brief Parses a single SSE data payload from the Gemini stream.
 * @param data JSON bytes (the payload after "data: " has been stripped by HttpClient).
 * @sideeffects Emits chunkReceived() for text/tool; emits requestFinished() on stop.
 */
void GeminiProvider::parseStreamChunk(const QByteArray& data) {
    if (data.isEmpty()) {
        return;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaLlm) << "Gemini: failed to parse stream chunk:" << err.errorString();
        return;
    }

    const QJsonObject obj = doc.object();
    const QJsonArray candidates = obj[QStringLiteral("candidates")].toArray();
    if (candidates.isEmpty()) {
        // May be a promptFeedback safety block
        const QJsonObject feedback = obj[QStringLiteral("promptFeedback")].toObject();
        if (!feedback.isEmpty()) {
            const QString blockReason = feedback[QStringLiteral("blockReason")].toString();
            if (!blockReason.isEmpty()) {
                qCWarning(verzetaLlm) << "Gemini prompt blocked:" << blockReason;
                emit requestError(QStringLiteral("Content blocked: %1").arg(blockReason));
            }
        }
        return;
    }

    const QJsonObject candidate = candidates[0].toObject();
    const QString finishReason = candidate[QStringLiteral("finishReason")].toString();
    const QJsonObject content = candidate[QStringLiteral("content")].toObject();
    const QJsonArray parts = content[QStringLiteral("parts")].toArray();

    for (const QJsonValue& partVal : parts) {
        const QJsonObject part = partVal.toObject();

        // Gemini 2.5 thinking parts carry `thought: true` alongside
        // the reasoning text.  Route the text into the dedicated
        // sidecar so the message-bubble disclosure renders it and
        // the request side stays content-only (per the L3 invariant
        // pinned by test-request-builder-no-thinking-leak).
        // Function-calling and ordinary text parts have no `thought`
        // field or `thought: false`, so they fall through to the
        // existing content / function-call extraction below.
        if (part.value(QStringLiteral("thought")).toBool(false)) {
            const QString thinking = part[QStringLiteral("text")].toString();
            if (!thinking.isEmpty()) {
                LlmChunk chunk;
                chunk.thinkingDelta = thinking;
                emit chunkReceived(chunk);
            }
            continue;
        }

        // Text delta
        const QString text = part[QStringLiteral("text")].toString();
        if (!text.isEmpty()) {
            LlmChunk chunk;
            chunk.delta = text;
            emit chunkReceived(chunk);
        }

        const QJsonObject funcCall = part[QStringLiteral("functionCall")].toObject();
        if (!funcCall.isEmpty()) {
            const QString name = funcCall[QStringLiteral("name")].toString();
            const QJsonObject argsObj = funcCall[QStringLiteral("args")].toObject();
            // Gemini sends no call id. A fresh one per call keeps two
            // identical calls in one conversation apart in the tool log.
            const QString callId =
                QStringLiteral("call_gemini_%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(12));
            const QString sig = part[QStringLiteral("thoughtSignature")].toString();
            if (!sig.isEmpty()) {
                if (!m_thoughtSignatures.contains(callId))
                    m_thoughtSignatureOrder.append(callId);
                m_thoughtSignatures.insert(callId, sig);
                while (m_thoughtSignatureOrder.size() > 256) {
                    m_thoughtSignatures.remove(m_thoughtSignatureOrder.takeFirst());
                }
            }
            m_sawFunctionCall = true;
            LlmChunk toolChunk;
            toolChunk.toolCallJson =
                QJsonObject{{QStringLiteral("id"), callId},
                            {QStringLiteral("name"), m_toolNames.value(name, name)},
                            {QStringLiteral("arguments"), argsObj}};
            toolChunk.finishReason = QStringLiteral("tool_calls");
            emit chunkReceived(toolChunk);
        }
    }

    // Map Gemini finish reasons to canonical values
    if (!finishReason.isEmpty() && finishReason != QStringLiteral("FINISH_REASON_UNSPECIFIED")) {
        QString mappedReason;
        if (m_sawFunctionCall && finishReason == QStringLiteral("STOP")) {
            // A function-call turn ends with STOP; the app runs tools only
            // on "tool_calls".
            mappedReason = QStringLiteral("tool_calls");
        } else if (finishReason == QStringLiteral("STOP")) {
            mappedReason = QStringLiteral("stop");
        } else if (finishReason == QStringLiteral("MAX_TOKENS")) {
            mappedReason = QStringLiteral("length");
        } else if (finishReason == QStringLiteral("SAFETY") ||
                   finishReason == QStringLiteral("RECITATION")) {
            mappedReason = QStringLiteral("content_filter");
        } else {
            mappedReason = finishReason.toLower();
        }

        // Get usage if available
        const QJsonObject usageMetadata = obj[QStringLiteral("usageMetadata")].toObject();
        const int totalTokens = usageMetadata[QStringLiteral("totalTokenCount")].toInt(0);

        LlmChunk finalChunk;
        finalChunk.finishReason = mappedReason;
        finalChunk.tokenCountEstimate = totalTokens;
        emit chunkReceived(finalChunk);
        emit requestFinished(mappedReason, totalTokens);
        m_finishedEmitted = true;
    }
}

/**
 * @brief Converts a ToolSchema to Gemini function declaration format.
 *
 *        Stub: returns an empty object, and nothing currently calls
 *        it. ToolCallingSchema performs this conversion for all
 *        providers, including the Gemini function declaration form.
 * @param schema Tool schema to convert.
 * @return An empty object, always, while this stub stands in.
 */
QJsonObject GeminiProvider::toolSchemaToGemini(const ToolSchema& /*schema*/) {
    // Unimplemented: see the note above.
    return {};
}

/**
 * @brief Converts an LLM role string to Gemini's role vocabulary.
 * @param role "user"|"assistant"|"tool"|"system"
 * @return "user" or "model"
 */
QString GeminiProvider::toGeminiRole(const QString& role) {
    if (role == QStringLiteral("assistant")) {
        return QStringLiteral("model");
    }
    // "user", "tool" results → "user"
    return QStringLiteral("user");
}

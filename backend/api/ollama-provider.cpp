// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ollama-provider.cpp
 * @brief Implementation of the Ollama LLM provider adapter.
 *        Handles NDJSON streaming responses and Ollama-specific API formats.
 * @layer API
 * @dependencies HttpClient (Utility), Qt6::Core, Qt6::Network
 */

#include "ollama-provider.h"

#include "../utils/logger.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

namespace {
inline bool streamTraceEnabled() {
    static const bool s_enabled = qEnvironmentVariableIntValue("VERZETA_STREAM_TRACE") > 0;
    return s_enabled;
}
}  // namespace


// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the OllamaProvider.
 * @param http Reference to the shared HttpClient.
 * @param parent Optional Qt parent.
 */
OllamaProvider::OllamaProvider(HttpClient& http, QObject* parent)
    : ILLMProvider(parent), m_http(http), m_ctxNam(this) {
    // Forward HTTP chunks to our parsing method
    connect(&m_http, &HttpClient::chunkReceived, this, &OllamaProvider::parseStreamChunk);
    // Fallback: only emit requestFinished if done:true was never parsed
    // (e.g. connection dropped before final chunk)
    connect(&m_http, &HttpClient::streamFinished, this, [this]() {
        if (!m_doneReceived) {
            emit requestFinished(QStringLiteral("stop"), 0);
        }
        m_doneReceived = false;
        m_hasPendingToolCall = false;
    });
    connect(&m_http, &HttpClient::errorOccurred, this, [this](const QString& msg) {
        emit requestError(msg);
    });
    connect(&m_http,
            &HttpClient::responseReceived,
            this,
            [this](int statusCode, const QByteArray& body) {
                if (statusCode == 200) {
                    const QStringList models = parseModelList(body);
                    m_models = models;
                    emit modelsRefreshed(m_models);
                } else {
                    qCWarning(verzetaLlm) << "Ollama model list returned status" << statusCode;
                }
            });
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void OllamaProvider::setBaseUrl(const QString& url) {
    // Strip trailing slash to avoid double-slash in API paths
    m_baseUrl = url.trimmed();
    while (m_baseUrl.endsWith(QLatin1Char('/'))) {
        m_baseUrl.chop(1);
    }
}

QString OllamaProvider::baseUrl() const {
    return m_baseUrl;
}

// ---------------------------------------------------------------------------
// ILLMProvider interface
// ---------------------------------------------------------------------------

QString OllamaProvider::providerId() const {
    return QStringLiteral("ollama");
}

QString OllamaProvider::displayName() const {
    return QStringLiteral("Ollama (Local)");
}

QStringList OllamaProvider::availableModels() {
    return m_models;
}

/**
 * @brief Fetches available models from GET /api/tags.
 * @sideeffects Initiates GET request; emits modelsRefreshed() on success.
 */
void OllamaProvider::refreshModels() {
    m_http.get(QUrl(m_baseUrl + QStringLiteral("/api/tags")));
}

int OllamaProvider::contextWindowFor(const QString& model) {
    if (model.isEmpty()) {
        return 0;
    }
    const auto it = m_ctxCache.constFind(model);
    if (it != m_ctxCache.constEnd()) {
        return it.value();
    }
    // Cache miss — warm it for next time (single-flight) and report unknown
    // now. Ollama has no published-family fallback, so the cold first turn
    // resolves to 0 and RequestBuilder keeps its default window.
    warmContextWindow(model);
    return 0;
}

void OllamaProvider::warmContextWindow(const QString& model) {
    if (model.isEmpty() || m_ctxInFlight.contains(model)) {
        return;  // single-flight: a probe for this model is already running
    }
    m_ctxInFlight.insert(model);

    QNetworkRequest request{QUrl(m_baseUrl + QStringLiteral("/api/show"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    const QByteArray body =
        QJsonDocument(QJsonObject{{QStringLiteral("model"), model}}).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_ctxNam.post(request, body);
    // Parse on the finished signal — fully async, no blocking, no timers.
    connect(reply, &QNetworkReply::finished, this, [this, reply, model]() {
        reply->deleteLater();
        m_ctxInFlight.remove(model);
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(verzetaLlm) << "Ollama /api/show failed for" << model << ":"
                                  << reply->errorString();
            return;
        }
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            return;
        }
        const QJsonObject info = doc.object().value(QStringLiteral("model_info")).toObject();
        // The architecture-specific key ends in ".context_length"
        // (e.g. "gemma4.context_length"). There is exactly one such key.
        for (auto kit = info.constBegin(); kit != info.constEnd(); ++kit) {
            if (kit.key().endsWith(QStringLiteral(".context_length"))) {
                const int ctx = kit.value().toInt(0);
                if (ctx > 0) {
                    m_ctxCache.insert(model, ctx);
                    qCDebug(verzetaLlm) << "Ollama resolved context window for" << model << "="
                                        << ctx << "(via" << kit.key() << ")";
                }
                return;
            }
        }
    });
}

bool OllamaProvider::supportsStreaming() const {
    return true;
}

bool OllamaProvider::supportsToolCalling() const {
    return true;
}

bool OllamaProvider::supportsVision() const {
    return true;
}

/*
 * @brief Sends a chat request to Ollama via POST /api/chat.
 * @param req Complete LLM request payload.
 * @sideeffects Initiates streaming HTTP request.
 */
void OllamaProvider::sendRequest(const LlmRequest& req) {
    m_hasPendingToolCall = false;
    m_doneReceived = false;
    m_inbandToolBuf.clear();
    m_thinkingHoister.reset();
    m_thinkingCharsSeen = 0;
    m_contentCharsSeen = 0;
    m_streamContentFramesSeen = 0;
    m_lastUnconsumedFrameExcerpt.clear();

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    // Activated only when VERZETA_STREAM_TRACE=1 is set. APPENDS a per-turn
    // separator to the raw-response dump (it does NOT truncate per request)
    // so a WHOLE session — every turn's raw NDJSON chunks, in order — is
    // captured in one file. That lets a mid-run pathology (e.g. an empty
    // finish=stop turn where eval_count>0 but no content/thinking reached us)
    // be inspected after the fact instead of being overwritten by the next
    // turn. Delete the file before a session to start clean. When the env
    // var is unset, no file is touched and no log line is emitted.
    if (streamTraceEnabled()) {
        const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!cacheDir.isEmpty()) {
            QDir().mkpath(cacheDir);
            QFile rf(cacheDir + QStringLiteral("/last-ollama-response.jsonl"));
            if (rf.open(QIODevice::WriteOnly | QIODevice::Append)) {
                rf.write(QStringLiteral("===== STREAM-BEGIN model=%1 thinking=%2 "
                                        "messages=%3 =====\n")
                             .arg(req.config.modelName,
                                  req.config.thinkingMode ? QStringLiteral("true")
                                                          : QStringLiteral("false"),
                                  QString::number(req.messages.size()))
                             .toUtf8());
                rf.close();
            }
        }
        qCInfo(verzetaLlm) << "TRACE: STREAM-BEGIN provider=ollama model=" << req.config.modelName
                           << "thinking=" << req.config.thinkingMode
                           << "messages=" << req.messages.size();
    }
    // === END STREAM-TRACE DEBUG ===

    const QByteArray body = buildRequestBody(req);
    const QUrl url(m_baseUrl + QStringLiteral("/api/chat"));

    // Diagnostic — prints the exact tool count + system prompt size + first
    // 200 chars of the body so you can SEE on stderr whether tools are
    // making it into the wire request. This was added during a group-chat
    // tool-calling regression where the model never emitted real tool_calls
    // and we needed to verify the `tools` array was present in the body.
    int toolCount = 0;
    bool toolsKeyPresent = false;
    {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            if (obj.contains(QStringLiteral("tools"))) {
                toolsKeyPresent = true;
                toolCount = obj[QStringLiteral("tools")].toArray().size();
            }
        }
    }
    qCDebug(verzetaLlm) << "Ollama request →" << url.toString()
                        << "| model:" << req.config.modelName << "| stream:" << req.config.stream
                        << "| body bytes:" << body.size()
                        << "| tools key present:" << toolsKeyPresent << "| tool count:" << toolCount
                        << "| messages in req:" << req.messages.size()
                        << "| system prompt chars:" << req.systemPrompt.length()
                        << "| availableTools (pre-build):" << req.availableTools.size();

    {
        const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!cacheDir.isEmpty()) {
            QDir().mkpath(cacheDir);
            const QString path = cacheDir + QStringLiteral("/last-ollama-request.json");
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(body);
                f.close();
                qCDebug(verzetaLlm) << "Ollama request body dumped to" << path;
            }
        }
    }

    m_http.postStream(url, body, {});
}

/**
 * @brief Cancels the in-progress Ollama request.
 * @sideeffects Aborts HTTP connection.
 */
void OllamaProvider::cancelRequest() {
    m_http.cancel();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Builds the Ollama /api/chat request body.
 * @param req LLM request.
 * @return Compact JSON bytes.
 */
QByteArray OllamaProvider::buildRequestBody(const LlmRequest& req) {
    QJsonObject root;
    root[QStringLiteral("model")] = req.config.modelName;
    root[QStringLiteral("stream")] = req.config.stream;

    // Per-conversation `activeThinking` toggle — drives Ollama 0.5+'s
    // reasoning channel for models that support it (qwen3-family,
    // deepseek-r1, gpt-oss-thinking, etc.).  `think: true` enables the
    // model's thinking pass before content emission; `think: false`
    // keeps the model in plain-content mode regardless of its default
    // (reasoning models default to thinking and emit content into a
    // separate `message.thinking` field, leaving `message.content`
    // empty unless this flag suppresses thinking).  Older Ollama
    // versions ignore this field safely — same contract documented
    // on the equivalent `body["think"] = false` line in
    // `remote-ragp-backend.cpp` for the RAGP classification path.
    //
    // Response-side: `message.thinking` is NOT currently parsed or
    // surfaced to the UI — the toggle's effect for now is purely to
    // drive the model's internal reasoning so it produces better
    // final content.  Wiring the thinking channel into a dedicated
    // UI affordance is a follow-up plan; the in-conversation message
    // body must NOT be polluted with raw thinking text until that
    // surface exists.
    root[QStringLiteral("think")] = req.config.thinkingMode;

    // Build messages array — prepend system message if provided
    QJsonArray messages;
    if (!req.systemPrompt.isEmpty()) {
        QJsonObject sysMsg;
        sysMsg[QStringLiteral("role")] = QStringLiteral("system");
        sysMsg[QStringLiteral("content")] = req.systemPrompt;
        messages.append(sysMsg);
    }

    for (const LlmMessage& msg : req.messages) {
        QJsonObject m;
        m[QStringLiteral("role")] = msg.role;
        m[QStringLiteral("content")] = msg.content;
        if (!msg.toolCallsJson.isEmpty()) {
            m[QStringLiteral("tool_calls")] = msg.toolCallsJson;
        }
        if (!msg.speakerName.isEmpty()) {
            m[QStringLiteral("name")] = msg.speakerName;
        }
        // For role=tool messages, emit the tool_call_id linking back
        // to the assistant turn's tool_calls array entry. Without this
        // the transcript is malformed per the OpenAI contract and small
        // local models (qwen3-class) emit a single stop token on their
        // next turn. ChatController::assembleLlmHistory populates
        // msg.toolCallId during history rebuild from the authoritative
        // tool_calls side table.
        if (msg.role == QStringLiteral("tool") && !msg.toolCallId.isEmpty()) {
            m[QStringLiteral("tool_call_id")] = msg.toolCallId;
        }
        // Vision: attach base64 images (Ollama format: "images": ["base64..."])
        if (!msg.images.isEmpty()) {
            QJsonArray imagesArr;
            for (const LlmImageData& img : msg.images) {
                imagesArr.append(QString::fromLatin1(img.base64));
            }
            m[QStringLiteral("images")] = imagesArr;
        }
        messages.append(m);
    }
    root[QStringLiteral("messages")] = messages;

    // Generation options.
    // num_ctx: Ollama's input context window. We MUST pass this explicitly —
    // Ollama otherwise defaults to 2048 tokens regardless of the underlying
    // model's real capacity, silently truncating the system prompt and
    // history from the beginning. That causes small local models to lose
    // their agent role + contract and hallucinate fake dialog transcripts.
    // Generation options. Only send parameters the user has explicitly
    // configured (-1 = use model default). This lets Ollama use the
    // model's own tuned parameters from its Modelfile — critical for
    // models like qwen3.5 that ship with specific repeat_penalty,
    // top_k, top_p, and temperature settings. Overriding with our
    // generic defaults (e.g. temperature=0.7) would replace the
    // model's carefully tuned values.
    QJsonObject options;
    if (req.config.temperature >= 0) {
        options[QStringLiteral("temperature")] = req.config.temperature;
    }
    if (req.config.topK >= 0) {
        options[QStringLiteral("top_k")] = static_cast<int>(req.config.topK);
    }
    if (req.config.topP >= 0) {
        options[QStringLiteral("top_p")] = req.config.topP;
    }
    if (req.config.repeatPenalty >= 0) {
        options[QStringLiteral("repeat_penalty")] = req.config.repeatPenalty;
    }
    if (req.config.presencePenalty >= 0) {
        options[QStringLiteral("presence_penalty")] = req.config.presencePenalty;
    }
    if (req.config.frequencyPenalty >= 0) {
        options[QStringLiteral("frequency_penalty")] = req.config.frequencyPenalty;
    }
    options[QStringLiteral("num_predict")] = req.config.maxTokens > 0 ? req.config.maxTokens : -1;
    if (req.config.contextWindow > 0) {
        options[QStringLiteral("num_ctx")] = req.config.contextWindow;
    }
    root[QStringLiteral("options")] = options;

    if (!req.availableTools.isEmpty() && supportsToolCalling()) {
        QJsonArray toolsArr;
        const bool haveWhitelist = !req.allowedTools.isEmpty();
        for (const ToolSchema& tool : req.availableTools) {
            if (haveWhitelist && !req.allowedTools.contains(tool.name)) {
                continue;
            }
            toolsArr.append(tool.toOpenAIFunction());
        }
        if (!toolsArr.isEmpty()) {
            root[QStringLiteral("tools")] = toolsArr;
        }
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

/**
 * @brief Parses a single NDJSON line from the Ollama streaming response.
 * @param line Raw JSON bytes.
 * @sideeffects Emits chunkReceived() for content tokens; emits requestFinished() when done.
 */
void OllamaProvider::parseStreamChunk(const QByteArray& line) {
    if (line.isEmpty()) {
        return;
    }

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    // Append the RAW NDJSON chunk from Ollama, byte-identical to what
    // arrived on the wire, to last-ollama-response.jsonl. One chunk per
    // line. Lets the operator diff raw provider output vs persisted DB
    // content to confirm whether truncation is at provider/network
    // level or downstream (sanitizer / cascade / streaming-manager).
    if (streamTraceEnabled()) {
        const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!cacheDir.isEmpty()) {
            QFile rf(cacheDir + QStringLiteral("/last-ollama-response.jsonl"));
            if (rf.open(QIODevice::WriteOnly | QIODevice::Append)) {
                rf.write(line);
                if (!line.endsWith('\n'))
                    rf.write("\n");
                rf.close();
            }
        }
    }
    // === END STREAM-TRACE DEBUG ===

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaLlm) << "Ollama: failed to parse chunk:" << err.errorString();
        return;
    }

    const QJsonObject obj = doc.object();
    const bool done = obj[QStringLiteral("done")].toBool(false);

    if (done) {
        m_doneReceived = true;
        // Flush any residual buffered content (e.g. orphaned partial-
        // tag prefix without a matching close) so it doesn't get lost.
        if (!m_inbandToolBuf.isEmpty()) {
            LlmChunk tail;
            tail.delta = m_inbandToolBuf;
            m_inbandToolBuf.clear();
            emit chunkReceived(tail);
        }
        const int evalCount = obj[QStringLiteral("eval_count")].toInt(0);
        const QString reason =
            m_hasPendingToolCall ? QStringLiteral("tool_calls") : QStringLiteral("stop");

        // Diagnostic for empty-content pathologies on finish=stop.
        // Two distinct cases share the same gate (reason=="stop" +
        // m_contentCharsSeen==0 + evalCount>0) but have different
        // root causes and different log messages:
        //
        //   A. Qwen-class continuation-after-tool-call misroute.
        //      thinking>0 + content==0 + finish=stop means the model
        //      emitted its USER-FACING REPLY into message.thinking
        //      instead of message.content.  Reproducible via plain
        //      curl against the same Ollama host with the identical
        //      cached request body: ~40 % of replays exhibit the
        //      misroute on qwen3.5:9b with think:true + tools + a
        //      tool_call → tool_result history.  Not a Verzeta
        //      parser bug; an Ollama / qwen chat-template misrouting.
        //      The captured text is still surfaced to the user via
        //      the message bubble's reasoning disclosure
        //      (AssistantThinkingDisclosure.qml).
        //
        //   B. Truly empty stream.  Both buffers zero with evalCount>0
        //      means the model produced tokens that landed in a
        //      field this parser does not consume.  Original
        //      pathology this diagnostic was written for; kept as a
        //      backstop for any new Ollama response shape.
        //
        // Both fire only on the failure-mode gate — silent on the
        // healthy path where content was non-empty.
        if (reason == QStringLiteral("stop") && evalCount > 0 && m_contentCharsSeen == 0) {
            if (m_thinkingCharsSeen > 0) {
                qCWarning(verzetaLlm)
                    << "Ollama: finish=stop with empty message.content but" << m_thinkingCharsSeen
                    << "chars captured from message.thinking — known "
                       "qwen-class continuation-after-tool-call misroute "
                       "(Ollama / qwen chat-template returned the "
                       "user-facing reply in the thinking channel; not a "
                       "Verzeta parser bug; reproducible via curl outside "
                       "this process).  The captured text is surfaced in "
                       "the assistant bubble's reasoning disclosure.";
            } else if (m_streamContentFramesSeen == 0) {
                // OLLAMA EMPTY STREAM (Cluster D): no content/thinking frame
                // arrived at all — Ollama returned effectively a single done
                // frame with content:"" while eval_count>0. The model
                // generated tokens but Ollama emitted none; this correlates
                // with gemma thinking mode and reproduces via curl against
                // the same host with the cached request body. It is an
                // Ollama/model-side drop, NOT a Verzeta parse error. The
                // empty-reply retry recovers it (usually on the next attempt).
                qCWarning(verzetaLlm).noquote()
                    << "Ollama: finish=stop with" << evalCount
                    << "eval tokens but NO content stream — 0 content/thinking"
                       " frames arrived. Ollama returned an empty stream"
                       " (single done frame, content:\"\"); the model"
                       " generated tokens but Ollama emitted none. This is an"
                       " Ollama/model-side drop (correlates with gemma"
                       " thinking mode; reproduce via curl with"
                       " last-ollama-request.json), not a Verzeta parse error."
                       " Done frame message:"
                    << QString::fromUtf8(QJsonDocument(obj[QStringLiteral("message")].toObject())
                                             .toJson(QJsonDocument::Compact));
            } else {
                // Content frames DID arrive but consumed to zero chars — the
                // tokens landed in a field this parser does not read. Surface
                // the offending frame so the field is identifiable.
                qCWarning(verzetaLlm)
                    << "Ollama: finish=stop with" << evalCount
                    << "eval tokens but zero chars in both message.content "
                       "and message.thinking after"
                    << m_streamContentFramesSeen
                    << "content frame(s) — tokens went to a field this parser "
                       "does not read.  Last done payload object keys:"
                    << obj.keys().join(QStringLiteral(", "))
                    << "| last unconsumed frame message excerpt:"
                    << (m_lastUnconsumedFrameExcerpt.isEmpty() ? QStringLiteral("(none captured)")
                                                               : m_lastUnconsumedFrameExcerpt);
            }
        }

        m_hasPendingToolCall = false;
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        if (streamTraceEnabled()) {
            qCInfo(verzetaLlm) << "TRACE: STREAM-DONE finish=" << reason
                               << " contentCharsSeen=" << m_contentCharsSeen
                               << " thinkingCharsSeen=" << m_thinkingCharsSeen
                               << " evalCount=" << evalCount
                               << " hasPendingToolCall=" << m_hasPendingToolCall
                               << " inbandToolBufResidual=" << m_inbandToolBuf.size();
        }
        // === END STREAM-TRACE DEBUG ===
        emit requestFinished(reason, evalCount);
        return;
    }

    const QJsonObject message = obj[QStringLiteral("message")].toObject();
    QString rawDelta = message[QStringLiteral("content")].toString();
    const QString rawThinking = message[QStringLiteral("thinking")].toString();
    m_thinkingCharsSeen += rawThinking.size();
    // Count frames that actually carried streamed content/thinking. Zero of
    // these on a finish=stop with eval_count>0 means Ollama returned an
    // empty stream (a single done frame, content:"") — the model generated
    // tokens but Ollama emitted none, not a field this parser missed.
    if (!rawDelta.isEmpty() || !rawThinking.isEmpty()) {
        ++m_streamContentFramesSeen;
    }

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    // Per-chunk RAW deltas BEFORE the inline-think hoister runs. Lets
    // the operator see whether the model emitted content into
    // message.thinking (Qwen continuation-after-tool-call misroute) or
    // message.content, and whether the hoister later re-routed content
    // into the thinking sidecar via `<think>` tag detection.
    if (streamTraceEnabled() && (!rawDelta.isEmpty() || !rawThinking.isEmpty())) {
        qCInfo(verzetaLlm) << "TRACE: CHUNK rawContent=" << rawDelta.size()
                           << " rawThinking=" << rawThinking.size()
                           << " hoisterInsideThink(before)="
                           << "(opaque)";
    }
    // === END STREAM-TRACE DEBUG ===

    // Inline `<think>...</think>` extraction.  Qwen 2.5 / 3 (and other
    // reasoning-capable models) emit thinking inline in `message.content`
    // when the chat template doesn't suppress it.  Route the content
    // through the shared hoister BEFORE the tool-call hoister sees it
    // so the latter operates on think-tag-free text and the captured
    // inner text lands in the same thinking sidecar as the separate
    // `message.thinking` channel.
    const auto hoist = m_thinkingHoister.feed(rawDelta);
    rawDelta = hoist.content;
    m_contentCharsSeen += rawDelta.size();

    // Anomaly forensics for the "tokens went to a field this parser does
    // not read" diagnostic below: remember a bounded excerpt of the most
    // recent frame whose message object carried data but contributed
    // nothing this parser consumes (no content, no thinking, no
    // tool_calls). When the zero-chars-with-eval-tokens warning fires,
    // the excerpt identifies the offending field instead of leaving the
    // investigation blind. Cheap: only evaluated on frames that consumed
    // nothing; cleared per request.
    if (rawDelta.isEmpty() && rawThinking.isEmpty() && !message.isEmpty() &&
        !message.contains(QStringLiteral("tool_calls"))) {
        const QStringList msgKeys = message.keys();
        if (msgKeys.size() > 2 || (!msgKeys.contains(QStringLiteral("content")) &&
                                   !msgKeys.contains(QStringLiteral("role")))) {
            m_lastUnconsumedFrameExcerpt =
                QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)).left(400);
        }
    }

    // Emit captured thinking — separate-channel + inline-hoisted —
    // as a thinking-only chunk so ChatController's StreamingManager
    // accumulates it into the parallel buffer.  Skip when both are
    // empty so non-reasoning models don't fire a spurious chunk.
    const QString thinkingForChunk = rawThinking + hoist.thinking;
    if (!thinkingForChunk.isEmpty()) {
        LlmChunk thinkingChunk;
        thinkingChunk.thinkingDelta = thinkingForChunk;
        emit chunkReceived(thinkingChunk);
    }

    // In-band tool-call extraction. Qwen-family models, when their
    // function-calling state slips under a dense system prompt, emit
    // tool intent as text inside the message body using qwen's
    // chat-template tool tags:
    //   <|tool_call|>name=write_file arguments={"...":"..."}</|tool_call|>
    // Some variants also emit:
    //   <|tool_call|>{"name":"x","arguments":{...}}</|tool_call|>
    // Either form must be hoisted into a structured tool_calls chunk
    // so the tool dispatcher actually runs the call (instead of the
    // user just seeing JSON blobs in chat). We buffer the partial
    // tail across chunks because the closing tag may arrive in a
    // later chunk than the opening.
    if (!rawDelta.isEmpty()) {
        m_inbandToolBuf.append(rawDelta);
        QString flushed;
        while (true) {
            const int openIdx = m_inbandToolBuf.indexOf(QStringLiteral("<|tool_call|>"));
            if (openIdx < 0) {
                // No open tag in buffer — but possible PARTIAL tag at
                // the tail ("<|tool", "<|tool_c", etc.). Hold back any
                // suffix that could be the start of a tag, flush rest.
                int safe = m_inbandToolBuf.size();
                static const QStringList kPrefixes{
                    QStringLiteral("<"),
                    QStringLiteral("<|"),
                    QStringLiteral("<|t"),
                    QStringLiteral("<|to"),
                    QStringLiteral("<|too"),
                    QStringLiteral("<|tool"),
                    QStringLiteral("<|tool_"),
                    QStringLiteral("<|tool_c"),
                    QStringLiteral("<|tool_ca"),
                    QStringLiteral("<|tool_cal"),
                    QStringLiteral("<|tool_call"),
                    QStringLiteral("<|tool_call|"),
                };
                for (const QString& p : kPrefixes) {
                    if (m_inbandToolBuf.endsWith(p)) {
                        safe = m_inbandToolBuf.size() - p.size();
                        break;
                    }
                }
                flushed.append(m_inbandToolBuf.left(safe));
                m_inbandToolBuf.remove(0, safe);
                break;
            }
            // Flush any text before the open tag.
            if (openIdx > 0) {
                flushed.append(m_inbandToolBuf.left(openIdx));
                m_inbandToolBuf.remove(0, openIdx);
            }
            const int closeIdx = m_inbandToolBuf.indexOf(QStringLiteral("</|tool_call|>"));
            if (closeIdx < 0) {
                // Open tag present, close not yet — keep buffering.
                break;
            }
            // Extract the body between tags, drop the tags.
            const int bodyStart = QStringLiteral("<|tool_call|>").size();
            const QString body = m_inbandToolBuf.mid(bodyStart, closeIdx - bodyStart).trimmed();
            m_inbandToolBuf.remove(0, closeIdx + QStringLiteral("</|tool_call|>").size());

            // Parse the body. Two shapes:
            //   1. {"name":"x","arguments":{...}}
            //   2. name=x arguments={...}
            QString toolName;
            QJsonValue toolArgs;
            const QString trimmed = body.trimmed();
            if (trimmed.startsWith(QLatin1Char('{'))) {
                const auto d = QJsonDocument::fromJson(trimmed.toUtf8());
                if (d.isObject()) {
                    const QJsonObject o = d.object();
                    toolName = o.value(QStringLiteral("name")).toString();
                    toolArgs = o.value(QStringLiteral("arguments"));
                }
            } else {
                static const QRegularExpression rx(QStringLiteral(
                    R"(name\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s+arguments\s*=\s*(\{[\s\S]*\}))"));
                const auto m = rx.match(trimmed);
                if (m.hasMatch()) {
                    toolName = m.captured(1);
                    const auto d = QJsonDocument::fromJson(m.captured(2).toUtf8());
                    if (d.isObject())
                        toolArgs = d.object();
                }
            }
            if (!toolName.isEmpty()) {
                m_hasPendingToolCall = true;
                QJsonObject toolCallFlat;
                toolCallFlat[QStringLiteral("id")] =
                    QStringLiteral("call_inband_%1").arg(QString::number(qHash(body), 16));
                toolCallFlat[QStringLiteral("name")] = toolName;
                toolCallFlat[QStringLiteral("arguments")] = toolArgs;
                LlmChunk toolChunk;
                toolChunk.toolCallJson = toolCallFlat;
                emit chunkReceived(toolChunk);
                qCDebug(verzetaLlm) << "Ollama: hoisted in-band tool call:" << toolName;
            } else {
                qCWarning(verzetaLlm) << "Ollama: in-band tool tag with unparseable body, "
                                         "dropping content";
            }
        }
        if (!flushed.isEmpty()) {
            LlmChunk chunk;
            chunk.delta = flushed;
            emit chunkReceived(chunk);
        }
    }

    const QJsonArray toolCalls = message[QStringLiteral("tool_calls")].toArray();
    if (!toolCalls.isEmpty()) {
        m_hasPendingToolCall = true;
        for (int i = 0; i < toolCalls.size(); ++i) {
            const QJsonObject call = toolCalls.at(i).toObject();
            const QJsonObject funcObj = call[QStringLiteral("function")].toObject();

            // Build a flat object with id, name, arguments at top level.
            QJsonObject toolCallFlat;
            toolCallFlat[QStringLiteral("id")] = call.contains(QStringLiteral("id"))
                                                     ? call[QStringLiteral("id")]
                                                     : QJsonValue(QStringLiteral("call_%1").arg(i));
            toolCallFlat[QStringLiteral("name")] = funcObj[QStringLiteral("name")];
            toolCallFlat[QStringLiteral("arguments")] = funcObj[QStringLiteral("arguments")];

            LlmChunk toolChunk;
            toolChunk.toolCallJson = toolCallFlat;
            emit chunkReceived(toolChunk);
        }
    }
}

/**
 * @brief Parses the /api/tags response into a model name list.
 * @param body JSON response body.
 * @return List of model name strings.
 */
QStringList OllamaProvider::parseModelList(const QByteArray& body) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaLlm) << "Ollama: failed to parse model list:" << err.errorString();
        return {};
    }

    QStringList models;
    const QJsonArray modelsArr = doc.object()[QStringLiteral("models")].toArray();
    for (const QJsonValue& m : modelsArr) {
        const QString name = m.toObject()[QStringLiteral("name")].toString();
        if (!name.isEmpty()) {
            models.append(name);
        }
    }

    qCInfo(verzetaLlm) << "Ollama models refreshed:" << models.size() << "models";
    return models;
}

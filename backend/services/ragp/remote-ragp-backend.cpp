// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file remote-ragp-backend.cpp
 * @brief Fully async implementation of the remote (provider-
 *        backed) RAGP classifier. No nested event loops, no blocking.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Network
 *
 * Async + lifetime design:
 *
 *   classifyOllamaAsync() spawns a QNetworkReply and a global
 *   QTimer::singleShot. Both share a std::shared_ptr<QPromise> and a
 *   std::shared_ptr<bool> "completed" flag. Whichever fires first
 *   resolves the promise exactly once; the other is a no-op.
 *
 *   The reply's finished-signal lambda is connected WITH `this`
 *   (RemoteBackend) as the QObject context. If RemoteBackend is
 *   destroyed before the reply completes, Qt auto-disconnects the
 *   lambda. The reply is then aborted (child of m_net, which is a
 *   child of this).
 *
 *   The timeout lambda is connected via QTimer::singleShot(nullptr, ...)
 *   with no QObject context, so it fires GUARANTEED after the timeout
 *   regardless of any object destruction. It captures only
 *   shared_ptr members (no raw pointers), so it's safe even if
 *   everything else is gone.
 *
 *   This design guarantees the promise always resolves within
 *   `timeoutMs` ms, and never touches destroyed objects.
 */

#include "remote-ragp-backend.h"

#include "../../api/llm-interface.h"
#include "../../services/model-router.h"
#include "../../services/settings-service.h"
#include "../../utils/contention-ledger.h"
#include "../../utils/http-client.h"
#include "../../utils/logger.h"

#include <QTimer>

#include <memory>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPromise>

namespace Ragp {

RemoteBackend::RemoteBackend(ModelRouter& router, SettingsService& settingsService, QObject* parent)
    : QObject(parent)
    , m_router(router)
    , m_settings(settingsService)
    , m_net(new QNetworkAccessManager(this)) {
    m_net->setProxy(QNetworkProxy::NoProxy);
}

RemoteBackend::~RemoteBackend() = default;
// m_net is a child QObject — Qt deletes it and all its child replies
// via the QObject parent chain. Pending timeout lambdas capture only
// shared_ptr members (no raw `this`) and remain safe after destruction.

QString RemoteBackend::backendName() const {
    return QStringLiteral("remote:%1").arg(m_router.activeProviderId());
}

bool RemoteBackend::isAvailable() const {
    // A configured model is required regardless of provider — the
    // classifier reuses the conversation's active model.
    if (m_router.activeModelName().isEmpty()) {
        return false;
    }
    const QString providerId = m_router.activeProviderId();
    if (providerId.compare(QStringLiteral("ollama"), Qt::CaseInsensitive) == 0) {
        // Ollama keeps its tuned raw-HTTP path; availability is just
        // "base URL configured".
        return !m_settings.ollamaBaseUrl().isEmpty();
    }
    // Every other provider routes through the classifier-owned
    // ILLMProvider instance. Availability mirrors what the factory
    // can construct: API key present for cloud providers, base URL
    // present for custom servers, etc. The factory logs the precise
    // reason at construction time; here we only answer yes/no.
    QString whyNot;
    const bool ok = classifierProviderAvailable(providerId, m_settings, &whyNot);
    if (!ok) {
        qCDebug(verzetaUi).noquote()
            << "RAGP remote: unavailable for" << providerId << "—" << whyNot;
    }
    return ok;
}

QFuture<Classification> RemoteBackend::makeUnknownFuture(const QString& source) const {
    auto promise = std::make_shared<QPromise<Classification>>();
    promise->start();
    Classification c;
    c.source = source;
    c.confidence = 0.0;
    promise->addResult(c);
    promise->finish();
    return promise->future();
}

QFuture<Classification> RemoteBackend::classifyAsync(const Request& req) {
    const QString provider = m_router.activeProviderId();
    if (provider.compare(QStringLiteral("ollama"), Qt::CaseInsensitive) == 0) {
        return classifyOllamaAsync(req);
    }
    // Generic path — classifier-owned ILLMProvider instance for the
    // active provider (openai / anthropic / gemini / openrouter /
    // deepseek / llamacpp_remote / custom.<slug>).
    return classifyViaProviderAsync(req);
}

bool RemoteBackend::ensureClassifierProvider(QString* whyNot) {
    const QString activeId = m_router.activeProviderId();
    if (m_cls.valid() && m_cls.providerId == activeId) {
        return true;  // Cached instance still matches the active provider.
    }
    // Active provider changed (or first use): rebuild. A stale bundle
    // is destroyed here — its provider first, then its HttpClient,
    // per ClassifierProvider's member declaration order. If a request
    // were somehow in flight on the stale instance (m_clsBusy guards
    // against this at the call site), destruction aborts it safely
    // because providers own no heap state beyond their HttpClient ref.
    m_cls = makeClassifierProvider(activeId, m_settings, this, whyNot);
    return m_cls.valid();
}

QFuture<Classification> RemoteBackend::classifyViaProviderAsync(const Request& req) {
    const qint64 enterMs = QDateTime::currentMSecsSinceEpoch();
    const QString providerId = m_router.activeProviderId();
    const QString model = m_router.activeModelName();
    const QString sourceTag = QStringLiteral("remote:%1").arg(providerId);

    if (model.isEmpty()) {
        qCWarning(verzetaUi) << "RAGP remote: no active model — returning UNKNOWN";
        return makeUnknownFuture(sourceTag + QStringLiteral(":unconfigured"));
    }
    if (m_clsBusy) {
        // ILLMProvider instances are single-request. Cascade
        // classifications are serialized in practice, so this guard
        // fires only on pathological re-entry. Resolve immediately
        // with UNKNOWN so the caller's Tier-1 fallback applies.
        qCWarning(verzetaUi) << "RAGP remote: classifier busy — returning UNKNOWN";
        return makeUnknownFuture(sourceTag + QStringLiteral(":busy"));
    }
    QString whyNot;
    if (!ensureClassifierProvider(&whyNot)) {
        qCWarning(verzetaUi).noquote()
            << "RAGP remote: cannot construct classifier provider:" << whyNot;
        return makeUnknownFuture(sourceTag + QStringLiteral(":unavailable"));
    }

    // Build a one-shot request. The provider classes own all wire-
    // format details; we only fill the canonical LlmRequest.
    LlmRequest llmReq;
    llmReq.requestId = 0;  // Not router-dispatched; relay ids unused.
    llmReq.conversationId = QStringLiteral("ragp-classifier");
    llmReq.systemPrompt.clear();
    LlmMessage userMsg;
    userMsg.role = QStringLiteral("user");
    userMsg.content = buildPrompt(req);
    llmReq.messages = {userMsg};
    llmReq.config.providerId = providerId;
    llmReq.config.modelName = model;
    llmReq.config.temperature = 0.0;
    llmReq.config.maxTokens = 512;
    llmReq.config.stream = true;         // All providers stream; we accumulate.
    llmReq.config.thinkingMode = false;  // Classification is pattern-match, not reasoning.
    // The classifier request must NOT inherit any per-model sampling
    // profile side effects — RequestBuilder is not involved here, so
    // the config above is exactly what reaches buildRequestBody().

    // Shared completion state — same exactly-once + guaranteed-timeout
    // design as classifyOllamaAsync. The accumulator lives in a
    // shared_ptr so the chunk lambda can append after `this` is gone
    // (it can't be invoked after destruction thanks to the QObject
    // context, but the timeout lambda has no context by design).
    auto promise = std::make_shared<QPromise<Classification>>();
    auto completed = std::make_shared<bool>(false);
    auto buffer = std::make_shared<QString>();
    promise->start();

    ILLMProvider* provider = m_cls.provider.get();
    m_clsBusy = true;

    // Per-request connections, stored so the finalize path can sever
    // them. shared_ptr container because finalize is called from
    // lambdas that outlive this stack frame.
    auto conns = std::make_shared<QList<QMetaObject::Connection>>();

    QPointer<RemoteBackend> selfGuard(this);
    const quint64 ledgerId =
        ContentionLedger::begin(ContentionLedger::CallClass::Ragp, providerId, model);
    auto finalize = [promise, completed, conns, selfGuard, enterMs, sourceTag, ledgerId](
                        const Classification& result) {
        if (*completed)
            return;
        *completed = true;
        ContentionLedger::end(ledgerId, result.confidence > 0.0, result.source);
        for (const auto& c : *conns)
            QObject::disconnect(c);
        conns->clear();
        if (selfGuard)
            selfGuard->m_clsBusy = false;
        const qint64 resolveMs = QDateTime::currentMSecsSinceEpoch();
        qCInfo(verzetaUi) << "RAGP remote(provider-path): future resolving after"
                          << (resolveMs - enterMs) << "ms total"
                          << "| source:" << result.source << "| targets:" << result.targets.size();
        promise->addResult(result);
        promise->finish();
    };

    // Chunk accumulation. Context `this`: auto-disconnected if the
    // backend is destroyed mid-stream; the no-context timeout below
    // still guarantees promise resolution.
    conns->append(QObject::connect(
        provider, &ILLMProvider::chunkReceived, this, [buffer](const LlmChunk& chunk) {
            if (!chunk.delta.isEmpty()) {
                buffer->append(chunk.delta);
            }
        }));

    conns->append(QObject::connect(
        provider,
        &ILLMProvider::requestFinished,
        this,
        [buffer, finalize, sourceTag](const QString& finishReason, int totalTokens) {
            Q_UNUSED(finishReason);
            Q_UNUSED(totalTokens);
            finalize(parseProviderContent(*buffer, sourceTag));
        }));

    conns->append(QObject::connect(provider,
                                   &ILLMProvider::requestError,
                                   this,
                                   [finalize, sourceTag](const QString& errorMessage) {
                                       qCWarning(verzetaUi).noquote()
                                           << "RAGP remote(provider-path): provider error:"
                                           << errorMessage;
                                       Classification c;
                                       c.source = sourceTag + QStringLiteral(":provider-error");
                                       c.confidence = 0.0;
                                       finalize(c);
                                   }));

    // Guaranteed timeout — no QObject context, mirrors the Ollama
    // path. Captures only shared_ptrs + the QPointer guard; safe
    // after any destruction order. Cancels the provider request if
    // the backend (and thus the provider instance) is still alive.
    const int timeoutMs = m_timeoutMs;
    QTimer::singleShot(
        timeoutMs, nullptr, [finalize, completed, selfGuard, sourceTag, timeoutMs]() {
            if (*completed)
                return;
            qCWarning(verzetaUi) << "RAGP remote(provider-path): timeout after" << timeoutMs
                                 << "ms";
            if (selfGuard && selfGuard->m_cls.valid()) {
                selfGuard->m_cls.provider->cancelRequest();
            }
            Classification c;
            c.source = sourceTag + QStringLiteral(":timeout");
            c.confidence = 0.0;
            finalize(c);
        });

    qCDebug(verzetaUi).noquote() << "RAGP remote(provider-path): dispatching to" << providerId
                                 << "model" << model;
    ContentionLedger::markDispatched(ledgerId);
    provider->sendRequest(llmReq);

    return promise->future();
}

QFuture<QString> RemoteBackend::oneShotCompleteAsync(const QString& prompt) {
    const QString providerId = m_router.activeProviderId();
    const QString model = m_router.activeModelName();

    auto readyEmpty = []() {
        QPromise<QString> p;
        p.start();
        p.addResult(QString());
        p.finish();
        return p.future();
    };

    if (model.isEmpty() || m_clsBusy) {
        return readyEmpty();
    }
    QString whyNot;
    if (!ensureClassifierProvider(&whyNot)) {
        qCWarning(verzetaUi).noquote()
            << "RAGP oneShotCompleteAsync: classifier provider unavailable:" << whyNot;
        return readyEmpty();
    }

    // One-shot request on the SAME classifier provider @-mention routing
    // uses (the provider RAGP is configured with). Raw text is returned;
    // the caller owns interpretation.
    LlmRequest llmReq;
    llmReq.requestId = 0;
    llmReq.conversationId = QStringLiteral("ragp-intent-confirm");
    llmReq.systemPrompt.clear();
    LlmMessage userMsg;
    userMsg.role = QStringLiteral("user");
    userMsg.content = prompt;
    llmReq.messages = {userMsg};
    llmReq.config.providerId = providerId;
    llmReq.config.modelName = model;
    llmReq.config.temperature = 0.0;
    llmReq.config.maxTokens = 16;  // a one-word YES/NO answer
    llmReq.config.stream = true;
    llmReq.config.thinkingMode = false;  // detection, not reasoning

    auto promise = std::make_shared<QPromise<QString>>();
    auto completed = std::make_shared<bool>(false);
    auto buffer = std::make_shared<QString>();
    promise->start();

    ILLMProvider* provider = m_cls.provider.get();
    m_clsBusy = true;

    auto conns = std::make_shared<QList<QMetaObject::Connection>>();
    QPointer<RemoteBackend> selfGuard(this);
    const quint64 ledgerId =
        ContentionLedger::begin(ContentionLedger::CallClass::Confirm, providerId, model);
    auto finalize = [promise, completed, conns, selfGuard, ledgerId](const QString& text) {
        if (*completed)
            return;
        *completed = true;
        // Empty text only ever means error/timeout/unavailable — a real
        // answer is at least one token ("YES"/"NO").
        ContentionLedger::end(ledgerId, !text.isEmpty());
        for (const auto& c : *conns)
            QObject::disconnect(c);
        conns->clear();
        if (selfGuard)
            selfGuard->m_clsBusy = false;
        promise->addResult(text);
        promise->finish();
    };

    conns->append(QObject::connect(
        provider, &ILLMProvider::chunkReceived, this, [buffer](const LlmChunk& chunk) {
            if (!chunk.delta.isEmpty())
                buffer->append(chunk.delta);
        }));
    conns->append(QObject::connect(
        provider, &ILLMProvider::requestFinished, this, [buffer, finalize](const QString&, int) {
            finalize(*buffer);
        }));
    conns->append(QObject::connect(
        provider, &ILLMProvider::requestError, this, [finalize](const QString& err) {
            qCWarning(verzetaUi).noquote() << "RAGP oneShotCompleteAsync: provider error:" << err;
            finalize(QString());  // empty == could not determine
        }));

    const int timeoutMs = m_timeoutMs;
    QTimer::singleShot(timeoutMs, nullptr, [finalize, completed, selfGuard, timeoutMs]() {
        if (*completed)
            return;
        qCWarning(verzetaUi) << "RAGP oneShotCompleteAsync: timeout after" << timeoutMs << "ms";
        if (selfGuard && selfGuard->m_cls.valid()) {
            selfGuard->m_cls.provider->cancelRequest();
        }
        finalize(QString());
    });

    ContentionLedger::markDispatched(ledgerId);
    provider->sendRequest(llmReq);
    return promise->future();
}

Classification RemoteBackend::parseProviderContent(const QString& accumulatedContent,
                                                   const QString& sourceTag) {
    Classification result;
    result.source = sourceTag;
    result.confidence = 0.0;

    // Extract the outermost JSON object. Models occasionally wrap the
    // contract JSON in markdown fences or pre/post prose despite the
    // OUTPUT-ONLY instruction; the braces-window extraction tolerates
    // both without loosening the schema parse itself.
    const int firstBrace = accumulatedContent.indexOf(QLatin1Char('{'));
    const int lastBrace = accumulatedContent.lastIndexOf(QLatin1Char('}'));
    if (firstBrace < 0 || lastBrace <= firstBrace) {
        qCWarning(verzetaUi) << "RAGP remote(provider-path): no JSON object in content;"
                             << "first 200 chars:" << accumulatedContent.left(200);
        return result;
    }
    const QString inner = accumulatedContent.mid(firstBrace, lastBrace - firstBrace + 1);

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(inner.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(verzetaUi) << "RAGP remote(provider-path): JSON parse failed:"
                             << err.errorString() << "| body:" << inner.left(200);
        return result;
    }

    fillClassificationFromRoot(doc.object(), result);
    return result;
}

void RemoteBackend::fillClassificationFromRoot(const QJsonObject& root, Classification& result) {
    const QJsonArray targetsArr = root.value(QStringLiteral("targets")).toArray();
    for (const QJsonValue& tv : targetsArr) {
        const QJsonObject tobj = tv.toObject();
        const QString alias = tobj.value(QStringLiteral("alias")).toString();
        if (alias.isEmpty())
            continue;

        Target t;
        t.alias = alias;
        t.intent = intentFromString(tobj.value(QStringLiteral("intent")).toString());
        t.context = tobj.value(QStringLiteral("context")).toString();
        result.targets.append(t);
    }

    // Schema-tolerant confidence: small models occasionally emit the
    // number as a JSON string ("0.9"); QJsonValue::toDouble() reads a
    // string as 0.0, which would wrongly demote a confident answer.
    const QJsonValue confVal = root.value(QStringLiteral("confidence"));
    result.confidence = confVal.isString() ? confVal.toString().toDouble() : confVal.toDouble();
    if (result.confidence < 0.0)
        result.confidence = 0.0;
    if (result.confidence > 1.0)
        result.confidence = 1.0;

    const QJsonObject pending = root.value(QStringLiteral("pending_action")).toObject();
    const QString who = pending.value(QStringLiteral("who")).toString().trimmed();
    result.selfPendingAction = who.compare(QStringLiteral("self"), Qt::CaseInsensitive) == 0;
    const bool delegated = who.compare(QStringLiteral("other"), Qt::CaseInsensitive) == 0;
    if (delegated) {
        // Tolerate a leading @ sigil (models mirror chat syntax).
        QString target = pending.value(QStringLiteral("target")).toString().trimmed();
        if (target.startsWith(QLatin1Char('@')))
            target.remove(0, 1);
        result.pendingDelegateAlias = target;
    }
    result.pendingActionHint = (result.selfPendingAction || !result.pendingDelegateAlias.isEmpty())
                                   ? pending.value(QStringLiteral("what")).toString().trimmed()
                                   : QString();
}

QString RemoteBackend::buildPrompt(const Request& req) {
    QString roster;
    for (int i = 0; i < req.rosterAliases.size(); ++i) {
        if (i > 0)
            roster += QStringLiteral(", ");
        roster += req.rosterAliases.at(i);
    }

    const QString executedLine = req.executedToolsSummary.trimmed().isEmpty()
                                     ? QStringLiteral("none — no tool ran in this turn")
                                     : req.executedToolsSummary.trimmed();

    return QStringLiteral("You are classifying one group-chat reply. You make TWO "
                          "independent decisions and output ONE JSON object. No prose.\n"
                          "\n"
                          "DECISION 1 — routing targets (@-mentions):\n"
                          "An @-mention is a ROUTING TARGET when the author wants that "
                          "agent to act next — respond, contribute, answer, take a turn, "
                          "perform a task, clarify, correct, or take over. It is NOT a "
                          "routing target when the author merely names, cites, thanks, "
                          "quotes, or asks ABOUT the agent. Judge the WHOLE MESSAGE, not "
                          "just the sentence holding the mention: if ANY part of the "
                          "reply asks the mentioned agent to act next, the mention IS a "
                          "routing target — even when the @-mention itself sits in a "
                          "thanks or praise sentence. Author self-mentions are never "
                          "targets. Judge the SEMANTIC intent in whatever language the "
                          "message is written; never match keywords.\n"
                          "Intent categories (routing): DELEGATE_RESPONSE, DELEGATE_TASK, "
                          "PLURAL_ADDRESS, BROADCAST_REQUEST, CLARIFICATION_REQUEST, "
                          "CORRECTION_REQUEST, STATUS_CHECK, HANDOFF_COMPLETE.\n"
                          "Intent categories (non-routing — prefer empty targets): "
                          "REFERENCE, ACKNOWLEDGMENT, SUMMARY_LIST, QUESTION_ABOUT, "
                          "QUOTING, GREETING_FAREWELL, UNKNOWN.\n"
                          "\n"
                          "DECISION 2 — pending_action:\n"
                          "FIRST decide the reply's STANCE. If the author is WAITING for a "
                          "reply before proceeding — asking the user or a teammate a question, "
                          "requesting permission or approval, proposing or offering an action "
                          "to be approved, or presenting options to choose among — then "
                          "who=\"none\", EVEN IF a tool, file, or action is named (\"Shall I "
                          "create pricing.md?\", \"Would it be best if we add X?\" are WAITING, "
                          "not commitments). A proposal, an offer, or a request for permission "
                          "is never a commitment. Only when the author is ACTING — doing "
                          "something itself/next regardless of any reply — do the who rules "
                          "below apply.\n"
                          "who=\"self\" ONLY when the author committed to performing a "
                          "CONCRETE action THEMSELVES — writing/reading/editing a file, "
                          "opening or editing the canvas, running a search, command or "
                          "test, OR delivering announced content (a draft, copy, list, "
                          "code) that does NOT appear in the message (e.g. the message "
                          "ends at \"Here is the draft:\" with nothing after it) — and "
                          "it is NOT covered by the executed-tools line "
                          "below. who=\"other\" when the author asked ONE teammate to "
                          "perform a concrete action next (a tool call, a file, a "
                          "command) — put that teammate's Roster alias in `target`, even "
                          "if their mention appears only in a thanks sentence. "
                          "who=\"none\" when the reply merely quotes/RE-SENDS/recaps "
                          "earlier messages, DESCRIBES work as already DONE (past tense) or "
                          "summarises/concludes, when the work already executed, or when "
                          "there is no concrete pending action. `what` is one short "
                          "line when who is \"self\" or \"other\", else \"\"; `target` "
                          "is a Roster alias when who=\"other\", else \"\".\n"
                          "\n"
                          "Output schema (exactly this shape):\n"
                          "{\"targets\":[{\"alias\":\"…\",\"intent\":\"…\","
                          "\"context\":\"\"}],\"confidence\":0.0,"
                          "\"pending_action\":{\"who\":\"none\",\"what\":\"\","
                          "\"target\":\"\"}}\n"
                          "confidence is YOUR certainty; use < 0.5 if unsure.\n"
                          "\n"
                          "Compact examples (aliases Zoe/Amy/Tom are ILLUSTRATIVE ONLY — "
                          "your targets MUST use aliases from the Roster line below, "
                          "never these):\n"
                          "\"thanks @Zoe, good catch.\" → {\"targets\":[],"
                          "\"confidence\":0.9,\"pending_action\":{\"who\":\"none\","
                          "\"what\":\"\",\"target\":\"\"}}\n"
                          "\"@Amy — write the deploy script.\" → {\"targets\":"
                          "[{\"alias\":\"Amy\",\"intent\":\"DELEGATE_TASK\","
                          "\"context\":\"deploy script\"}],\"confidence\":0.9,"
                          "\"pending_action\":{\"who\":\"other\",\"what\":\"write the "
                          "deploy script\",\"target\":\"Amy\"}}\n"
                          "\"I'll draft pricing.md now and share it here.\" → "
                          "{\"targets\":[],\"confidence\":0.9,\"pending_action\":"
                          "{\"who\":\"self\",\"what\":\"draft pricing.md\","
                          "\"target\":\"\"}}\n"
                          "\"Would it be best if we create pricing.md now, or search for the "
                          "old one first?\" (asking the user to choose) → {\"targets\":[],"
                          "\"confidence\":0.9,\"pending_action\":{\"who\":\"none\","
                          "\"what\":\"\",\"target\":\"\"}}\n"
                          "\"Tone is locked. Here is the initial copy draft:\" (message "
                          "ENDS there — the draft never appears) → {\"targets\":[],"
                          "\"confidence\":0.9,\"pending_action\":{\"who\":\"self\","
                          "\"what\":\"deliver the announced copy draft\","
                          "\"target\":\"\"}}\n"
                          "\"Thanks @Tom, great summary! Please go ahead and run the "
                          "final tests now.\" → {\"targets\":[{\"alias\":\"Tom\","
                          "\"intent\":\"DELEGATE_TASK\",\"context\":\"run final "
                          "tests\"}],\"confidence\":0.9,\"pending_action\":"
                          "{\"who\":\"other\",\"what\":\"run the final tests\","
                          "\"target\":\"Tom\"}}\n"
                          "\n"
                          "NOW CLASSIFY THIS INPUT (targets may ONLY come from this "
                          "Roster):\n"
                          "Roster: %1\n"
                          "Author: %2\n"
                          "Tools the author ALREADY executed this turn (fact): %4\n"
                          "Message:\n\"\"\"\n%3\n\"\"\"\n"
                          "\n"
                          "OUTPUT ONLY THE JSON object. Nothing else.\n")
        .arg(roster, req.authorAlias, req.content, executedLine);
}

Classification RemoteBackend::parseResponse(const QByteArray& responseJson) {
    Classification result;
    result.source = QStringLiteral("remote:ollama");
    result.confidence = 0.0;

    QJsonParseError err;
    const QJsonDocument outer = QJsonDocument::fromJson(responseJson, &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaUi) << "RAGP remote: outer JSON parse failed:" << err.errorString();
        return result;
    }

    const QJsonObject outerObj = outer.object();
    const QJsonObject messageObj = outerObj.value(QStringLiteral("message")).toObject();
    QString inner = messageObj.value(QStringLiteral("content")).toString();
    if (inner.isEmpty()) {
        const QString thinking = messageObj.value(QStringLiteral("thinking")).toString();
        if (!thinking.isEmpty()) {
            qCDebug(verzetaUi) << "RAGP remote: message.content empty, falling back to "
                                  "message.thinking (reasoning model compatibility)";
            inner = thinking;
        }
    }
    if (inner.isEmpty()) {
        qCWarning(verzetaUi) << "RAGP remote: both message.content and message.thinking empty";
        return result;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(inner.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaUi) << "RAGP remote: inner JSON parse failed:" << err.errorString()
                             << "| body:" << inner.left(200);
        return result;
    }

    fillClassificationFromRoot(doc.object(), result);
    return result;
}

QFuture<Classification> RemoteBackend::classifyOllamaAsync(const Request& req) {
    const qint64 enterMs = QDateTime::currentMSecsSinceEpoch();

    const QString baseUrl = m_settings.ollamaBaseUrl().trimmed();
    const QString model = m_router.activeModelName();
    if (baseUrl.isEmpty() || model.isEmpty()) {
        qCWarning(verzetaUi) << "RAGP remote: missing baseUrl or model — returning UNKNOWN";
        return makeUnknownFuture(QStringLiteral("remote:ollama:unconfigured"));
    }

    QJsonObject userMessage;
    userMessage[QStringLiteral("role")] = QStringLiteral("user");
    userMessage[QStringLiteral("content")] = buildPrompt(req);
    QJsonArray messages;
    messages.append(userMessage);

    QJsonObject body;
    body[QStringLiteral("model")] = model;
    body[QStringLiteral("messages")] = messages;
    body[QStringLiteral("stream")] = false;
    body[QStringLiteral("format")] = QStringLiteral("json");
    body[QStringLiteral("think")] = false;
    // keep_alive hint: keep the chat session warm between RAGP calls so
    // a second classifier turn doesn't pay model-reload cost even if
    // the inter-call gap is longer than Ollama's default 5-minute TTL.
    body[QStringLiteral("keep_alive")] = QStringLiteral("30m");
    QJsonObject options;
    options[QStringLiteral("temperature")] = 0.0;
    options[QStringLiteral("num_predict")] = 512;
    body[QStringLiteral("options")] = options;

    QString url = baseUrl;
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    url += QStringLiteral("/api/chat");

    QNetworkRequest httpReq((QUrl(url)));
    httpReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    httpReq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    httpReq.setRawHeader(QByteArrayLiteral("Connection"), QByteArrayLiteral("keep-alive"));
    httpReq.setTransferTimeout(10000);

    // Shared state — survives ANY destruction order:
    //   - promise: held by reply-lambda + timeout-lambda. Last one
    //     standing resolves it (or QPromise destructor cancels it if
    //     nobody resolves — which shouldn't happen thanks to the
    //     guaranteed global timeout).
    //   - completed: single-fire guard against double-complete when
    //     reply.finished and timer.timeout race.
    //   - replyPtr: QPointer guard so the timeout lambda can abort
    //     a still-pending reply without touching a destroyed object.
    auto promise = std::make_shared<QPromise<Classification>>();
    auto completed = std::make_shared<bool>(false);
    promise->start();

    const quint64 ledgerId = ContentionLedger::begin(ContentionLedger::CallClass::Ragp,
                                                     QUrl(baseUrl).host() + QLatin1Char(':') +
                                                         QString::number(QUrl(baseUrl).port(11434)),
                                                     model);
    ContentionLedger::markDispatched(ledgerId);
    QNetworkReply* reply = m_net->post(httpReq, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QPointer<QNetworkReply> replyGuard(reply);

    const qint64 postMs = QDateTime::currentMSecsSinceEpoch();
    qCDebug(verzetaUi) << "RAGP remote: post issued after" << (postMs - enterMs) << "ms of prep"
                       << "| url:" << url;

    // Finalize helper — exactly-once completion. Logs the resolution
    // timestamp so the calling site sees the true settle point (after
    // all promise bookkeeping); the caller's own latencyMs already
    // covers Tier 1+2+3 so we just stamp the reply→resolved segment.
    auto finalize = [promise, completed, enterMs, ledgerId](const Classification& result) {
        if (*completed)
            return;
        *completed = true;
        ContentionLedger::end(ledgerId, result.confidence > 0.0, result.source);
        const qint64 resolveMs = QDateTime::currentMSecsSinceEpoch();
        qCInfo(verzetaUi) << "RAGP remote: future resolving after" << (resolveMs - enterMs)
                          << "ms total"
                          << "| source:" << result.source << "| targets:" << result.targets.size();
        promise->addResult(result);
        promise->finish();
    };

    // Reply finished — runs on main thread.
    // Context `this` (QObject) → if RemoteBackend is destroyed before
    // the reply completes, Qt disconnects this lambda automatically.
    // The timeout lambda (below) is our safety net that guarantees
    // the promise resolves even if this connection is severed.
    QObject::connect(
        reply, &QNetworkReply::finished, this, [replyGuard, finalize, enterMs, postMs]() {
            const qint64 replyMs = QDateTime::currentMSecsSinceEpoch();
            qCInfo(verzetaUi) << "RAGP remote: reply.finished after" << (replyMs - enterMs)
                              << "ms total"
                              << "(post→reply:" << (replyMs - postMs) << "ms)";
            if (!replyGuard) {
                // Reply already destroyed somehow. Nothing to do —
                // the timeout lambda will handle the finalize.
                return;
            }
            const QNetworkReply::NetworkError err = replyGuard->error();
            const QByteArray respBody = replyGuard->readAll();
            replyGuard->deleteLater();

            if (err == QNetworkReply::OperationCanceledError) {
                // Aborted by timeout lambda. Its finalize() already
                // ran (or is about to); our finalize is a no-op thanks
                // to `completed`.
                return;
            }

            if (err != QNetworkReply::NoError) {
                qCWarning(verzetaUi) << "RAGP remote: network error:" << err;
                Classification c;
                c.source = QStringLiteral("remote:ollama:net-error");
                c.confidence = 0.0;
                finalize(c);
                return;
            }

            finalize(parseResponse(respBody));
        });

    // Guaranteed timeout — no QObject context (nullptr) means this
    // fires after m_timeoutMs NO MATTER WHAT. Even if RemoteBackend
    // is destroyed, the lambda runs, sees completed=false, and
    // finalizes with UNKNOWN-timeout. The replyGuard is a QPointer so
    // aborting a destroyed reply is a safe no-op.
    //
    // Note: `finalize` internally checks the shared `completed` flag,
    // so this is a no-op if the reply already finished first.
    const int timeoutMs = m_timeoutMs;
    QTimer::singleShot(timeoutMs, nullptr, [replyGuard, finalize, completed, timeoutMs]() {
        if (*completed) {
            // Reply already resolved — nothing to do.
            return;
        }

        qCWarning(verzetaUi) << "RAGP remote: timeout after" << timeoutMs << "ms";

        // Abort the reply if it's still pending and its owner
        // (m_net) hasn't already destroyed it.
        if (replyGuard) {
            replyGuard->abort();
            replyGuard->deleteLater();
        }

        Classification c;
        c.source = QStringLiteral("remote:ollama:timeout");
        c.confidence = 0.0;
        finalize(c);
    });

    return promise->future();
}

}  // namespace Ragp

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-summarizer.cpp
 * @brief Implementation of the dynamic-compaction service. See the
 *        header for the architectural contract.
 * @layer Service
 * @dependencies DbManager, MessageService, ConversationService,
 *               ModelRouter, SettingsService, ILLMProvider,
 *               Qt6::Core, Qt6::Sql.
 */

#include "conversation-summarizer.h"

#include "../api/llm-interface.h"
#include "../models/db-manager.h"
#include "../models/message.h"
#include "../utils/contention-ledger.h"
#include "../utils/http-client.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "conversation-service.h"
#include "message-service.h"
#include "model-router.h"
#include "settings-service.h"

#include <QTimer>

#include <memory>
#include <QDateTime>
#include <QLoggingCategory>
#include <QPointer>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

/** Receipts inserted by compaction are excluded from coverage. */
bool isCompactionReceipt(const Message& m) {
    return m.role == QStringLiteral("system") &&
           m.content.startsWith(QStringLiteral("~Dynamic Compact Performed"));
}

/** ~4 chars/token estimate, the same heuristic HistoryBudgeter uses. */
int estimateTokens(const QString& s) {
    return static_cast<int>(s.toUtf8().size() / 4);
}

}  // namespace

ConversationSummarizer::ConversationSummarizer(DbManager& db,
                                               MessageService& msgSvc,
                                               ConversationService& convSvc,
                                               ModelRouter& router,
                                               SettingsService& settings,
                                               QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_msgSvc(msgSvc)
    , m_convSvc(convSvc)
    , m_router(router)
    , m_settings(settings) {
    qCInfo(verzetaUi) << "ConversationSummarizer initialized";
}

ConversationSummarizer::~ConversationSummarizer() {
    qCInfo(verzetaUi) << "ConversationSummarizer destroyed";
}
// m_prov's members are unique_ptrs; destruction aborts any in-flight
// request via the provider's own teardown. Pending timeout lambdas
// capture only shared_ptrs + a QPointer to `this` and null-check.

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

std::optional<ConversationSummary> ConversationSummarizer::summaryFor(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return std::nullopt;

    const auto it = m_cache.constFind(convId);
    if (it != m_cache.constEnd())
        return *it;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT summary_text, last_message_id, last_message_idx, "
                             "       covered_count, generated_at_ms, token_count, "
                             "       model_used, trigger_reason, invalidation_reason "
                             "  FROM conversation_summaries WHERE conversation_id = ?"));
    q.addBindValue(convId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "summaryFor: SELECT failed:" << q.lastError().text();
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;

    ConversationSummary row;
    row.conversationId = convId;
    row.summaryText = q.value(0).toString();
    row.lastMessageId = q.value(1).toString();
    row.lastMessageIdx = q.value(2).toInt();
    row.coveredCount = q.value(3).toInt();
    row.generatedAtMs = q.value(4).toLongLong();
    row.tokenCount = q.value(5).toInt();
    row.modelUsed = q.value(6).toString();
    row.triggerReason = q.value(7).toString();
    row.invalidationReason = q.value(8).toString();
    m_cache.insert(convId, row);
    return row;
}

bool ConversationSummarizer::shouldSummarize(const QString& convId,
                                             int totalMessages,
                                             int droppedCount,
                                             bool compactEnabled,
                                             int fillPercent,
                                             int compactEveryTurns) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!compactEnabled)
        return false;

    // Context-pressure paths. REACTIVE = the budgeter is already
    // dropping history (the original rule, kept as the backstop).
    // PROACTIVE = measured fill reached the amber threshold — compact
    // BEFORE anything is dropped, so the model never hits a cliff
    // where uncovered history silently vanishes. Both share the
    // freshness/staleness gate below: a fresh summary on a
    // still-full window must NOT re-trigger every turn.
    const bool reactivePressure =
        totalMessages > kMinSummarizableMessages && droppedCount > kMinDroppedMessages;
    const bool proactivePressure =
        fillPercent >= kProactiveFillPercent && totalMessages > kMinSummarizableMessages;

    if (reactivePressure || proactivePressure) {
        const auto existing = summaryFor(convId);
        if (!existing.has_value() || !existing->fresh()) {
            return true;  // Nothing usable — generate.
        }
        // Fresh summary exists: regenerate only when enough NEW
        // ASSISTANT turns have accumulated beyond what it covers.
        //
        // Counted in assistant turns via assistantTurnsAfter (role=system
        // compaction receipts are NOT assistant rows, so they can never
        // inflate this). The previous test compared `coveredCount` (which
        // EXCLUDES receipts) against `totalMessages - kKeepLastN` (a raw
        // row count that INCLUDES them): every compaction inserts a
        // receipt, so the gap grew purely from receipts until the summary
        // read as permanently "stale" and re-compacted every turn — a
        // self-reinforcing loop that worsened as receipts piled up. Keying
        // off real assistant progress removes the loop entirely.
        if (assistantTurnsAfter(convId, existing->lastMessageId) >= kStaleAfterNewMessages) {
            return true;
        }
        // Not stale → fall through to the cadence check (a fresh
        // summary does not exempt a conversation from attention
        // freshening once enough NEW turns accumulated).
    }

    // CADENCE path — per-conversation, in ASSISTANT turns (one group
    // exchange produces many tool/system rows; turns are what the
    // user reasons in). 0 disables. Requires enough rows that
    // coverage is non-empty (buildPrompt covers nothing below
    // kKeepLastN rows).
    if (compactEveryTurns > 0 && totalMessages > kKeepLastN) {
        const auto existing = summaryFor(convId);
        const QString afterId =
            (existing.has_value() && existing->fresh()) ? existing->lastMessageId : QString();
        return assistantTurnsAfter(convId, afterId) >= compactEveryTurns;
    }
    return false;
}

int ConversationSummarizer::cadenceTurnsUsed(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Same anchor the CADENCE trigger uses: count assistant turns AFTER a
    // fresh summary's coverage, or the whole conversation when there isn't
    // one yet. (Compaction receipts are role=system → never counted.)
    const auto existing = summaryFor(convId);
    const QString afterId =
        (existing.has_value() && existing->fresh()) ? existing->lastMessageId : QString();
    return assistantTurnsAfter(convId, afterId);
}

int ConversationSummarizer::assistantTurnsAfter(const QString& convId, const QString& afterMsgId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    // COALESCE: unknown/empty anchor counts the whole conversation.
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM messages "
                             "WHERE conversation_id = ? AND role = 'assistant' "
                             "AND created_at > COALESCE((SELECT created_at FROM messages "
                             "                            WHERE id = ?), 0)"));
    q.addBindValue(convId);
    q.addBindValue(afterMsgId.isNull() ? QStringLiteral("") : afterMsgId);
    if (!q.exec() || !q.next()) {
        qCWarning(verzetaUi) << "Summarizer: assistantTurnsAfter query failed:"
                             << q.lastError().text();
        return 0;
    }
    return q.value(0).toInt();
}

// ---------------------------------------------------------------------------
// Invalidation / clearing (L9)
// ---------------------------------------------------------------------------

void ConversationSummarizer::invalidate(const QString& convId, const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE conversation_summaries SET invalidation_reason = ? "
                             " WHERE conversation_id = ?"));
    q.addBindValue(reason);
    q.addBindValue(convId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "invalidate: UPDATE failed:" << q.lastError().text();
        return;
    }
    m_cache.remove(convId);
    if (q.numRowsAffected() > 0) {
        qCInfo(verzetaUi) << "Summary invalidated for conv" << convId.left(8)
                          << "reason:" << reason;
    }
}

void ConversationSummarizer::clearFor(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM conversation_summaries WHERE conversation_id = ?"));
    q.addBindValue(convId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "clearFor: DELETE failed:" << q.lastError().text();
        return;
    }
    m_cache.remove(convId);
}

// ---------------------------------------------------------------------------
// Generation (L5 + L10 + L11-refined)
// ---------------------------------------------------------------------------

bool ConversationSummarizer::ensureProvider(const QString& providerId, QString* whyNot) {
    if (m_prov.valid() && m_prov.providerId == providerId) {
        return true;
    }
    m_prov = Ragp::makeClassifierProvider(providerId, m_settings, this, whyNot);
    return m_prov.valid();
}

QString ConversationSummarizer::buildPrompt(const QString& convId,
                                            QString* outLastMsgId,
                                            int* outLastIdx,
                                            int* outCoveredCount) {
    // Pull the conversation. 2000 rows is far beyond any context
    // window's verbatim reach; the token budget below bounds the prompt
    // regardless of row count.
    const QList<Message> all = m_msgSvc.getRecentMessages(convId, 2000);
    if (all.isEmpty())
        return QString();

    // Coverage = everything except the kKeepLastN newest rows and
    // compaction receipts (L4). getRecentMessages returns
    // chronological order (same contract the cascade detectors rely
    // on).
    int end = all.size() - kKeepLastN;
    if (end <= 0)
        return QString();

    // Never split an assistant(tool_calls) + role=tool result group
    // at the coverage boundary: if the first KEPT row is a tool
    // result, walk the boundary back so the whole group stays in the
    // verbatim tail. Otherwise the kept tail starts with orphan tool
    // rows that assembleHistory must drop — losing context right
    // after a compact.
    while (end > 0 && all[end].role == QStringLiteral("tool")) {
        --end;
    }
    if (end <= 0)
        return QString();

    // ROLLING coverage. When a fresh prior summary exists, only the
    // messages PAST its coverage are summarised and the prior summary is
    // carried in as context, so the new summary EXTENDS it. This keeps the
    // per-call input bounded (prior summary + recent delta) and coverage
    // CUMULATIVE — instead of re-sending the whole history every time,
    // which overran the summariser model's context window and produced an
    // empty summary.
    const std::optional<ConversationSummary> prior = summaryFor(convId);
    QString priorSummaryText;
    int priorCovered = 0;
    int startIdx = 0;
    if (prior.has_value() && prior->fresh() && !prior->lastMessageId.isEmpty()) {
        for (int i = 0; i < end; ++i) {
            if (all[i].id == prior->lastMessageId) {
                startIdx = i + 1;  // summarise strictly past prior coverage
                break;
            }
        }
        priorSummaryText = prior->summaryText;
        priorCovered = prior->coveredCount;
    }

    // Candidate covered rows in [startIdx, end), in chronological order.
    /** @brief One prompt line plus the source message's id and 1-based
     *         index, so the most-recent-within-budget window can be taken
     *         while still reporting truthful coverage bookkeeping. */
    struct CoveredRow {
        QString line;
        QString id;
        int idx;
    };
    QList<CoveredRow> rows;
    for (int i = startIdx; i < end; ++i) {
        const Message& m = all[i];
        if (isCompactionReceipt(m))
            continue;
        QString body = m.content.trimmed();
        if (body.isEmpty())
            continue;
        const QString who = m.memberAlias.isEmpty() ? m.role : m.memberAlias;
        if (body.size() > kPerMessageExcerptChars) {
            body = body.left(kPerMessageExcerptChars) + QStringLiteral(" […truncated]");
        }
        rows.append({QStringLiteral("[%1] %2").arg(who, body), m.id, i + 1});
    }
    if (rows.isEmpty()) {
        return QString();  // nothing new past prior coverage → no regen
    }

    // Keep the MOST RECENT rows that fit the input token budget (drop the
    // oldest of the delta first), accounting for the carried-over prior
    // summary. kSummaryContextWindow on the request is sized to hold this
    // budget + the prior summary + the output, so the prompt is never
    // truncated to an empty result.
    QStringList lines;
    int covered = 0;
    int tokens = estimateTokens(priorSummaryText);
    int firstKept = rows.size();
    for (int i = rows.size() - 1; i >= 0; --i) {
        const int t = estimateTokens(rows[i].line);
        if (covered > 0 && tokens + t > kSummaryInputTokenBudget) {
            break;
        }
        tokens += t;
        ++covered;
        firstKept = i;
    }
    for (int i = firstKept; i < rows.size(); ++i) {
        lines.append(rows[i].line);
    }

    *outLastMsgId = rows.last().id;
    *outLastIdx = rows.last().idx;
    *outCoveredCount = priorCovered + covered;

    const QString priorBlock =
        priorSummaryText.isEmpty()
            ? QString()
            : QStringLiteral("Summary so far (EXTEND it with the new messages below — keep "
                             "everything still relevant, fold in the new content; do not "
                             "repeat it verbatim):\n%1\n\n")
                  .arg(priorSummaryText);

    // L10 — single hardcoded template.
    return QStringLiteral("You are summarising an in-progress group chat. The chat will "
                          "continue; your summary will become the only record of these "
                          "messages in future LLM context. Preserve:\n"
                          "\n"
                          "1. Decisions and their rationale (\"We decided X because Y\").\n"
                          "2. Open questions and unresolved items.\n"
                          "3. Per-speaker attribution when it matters for decisions "
                          "(\"Alice proposed X; Rob countered with Y\").\n"
                          "4. Tool calls executed and their outcomes (which files "
                          "written — keep exact filenames, which polls opened, what "
                          "was searched).\n"
                          "5. Active task plans and their current state, and WHO OWNS "
                          "which deliverable next (explicit assignments must survive "
                          "verbatim).\n"
                          "6. Sub-agent runs that were spawned: their run_id, status, "
                          "and what they were asked to do (a pending run's report will "
                          "arrive after this summary is in use).\n"
                          "7. User directives that should bind future behaviour.\n"
                          "8. Compaction-system-message events.\n"
                          "\n"
                          "Discard: conversational filler, repeated paraphrases, verbose "
                          "acknowledgements, tool argument detail unless "
                          "decision-relevant.\n"
                          "\n"
                          "Target length: 400-800 tokens. Use bullet structure. Do NOT "
                          "include chat-template markers.\n"
                          "\n"
                          "%1Conversation to summarise:\n%2\n")
        .arg(priorBlock, lines.join(QLatin1Char('\n')));
}

void ConversationSummarizer::generateAsync(const QString& convId, const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return;

    if (m_busy) {
        // L5 self-healing: the trigger heuristic re-fires on the next
        // qualifying turn; no queue needed, no work lost silently —
        // this log is the audit trail.
        qCInfo(verzetaUi) << "Summarizer busy — dropping generate request for conv"
                          << convId.left(8) << "(reason:" << reason
                          << "); trigger will re-fire next turn";
        return;
    }

    // Resolve provider/model: explicit summarization override first,
    // active foreground pair otherwise (L11).
    QString providerId = m_settings.summarizationProvider().trimmed();
    QString modelName = m_settings.summarizationModel().trimmed();
    if (providerId.isEmpty())
        providerId = m_router.activeProviderId();
    if (modelName.isEmpty())
        modelName = m_router.activeModelName();
    if (providerId.isEmpty() || modelName.isEmpty()) {
        const QString err = QStringLiteral("no provider/model configured for summarization");
        qCWarning(verzetaUi) << "Summarizer:" << err;
        emit summaryFailed(convId, err);
        return;
    }

    QString whyNot;
    if (!ensureProvider(providerId, &whyNot)) {
        qCWarning(verzetaUi).noquote() << "Summarizer: cannot construct provider:" << whyNot;
        emit summaryFailed(convId, whyNot);
        return;
    }

    QString lastMsgId;
    int lastIdx = 0;
    int coveredCount = 0;
    const QString prompt = buildPrompt(convId, &lastMsgId, &lastIdx, &coveredCount);
    if (prompt.isEmpty()) {
        const QString err = QStringLiteral("nothing to summarise (too few messages)");
        qCInfo(verzetaUi) << "Summarizer:" << err << "conv" << convId.left(8);
        emit summaryFailed(convId, err);
        return;
    }

    LlmRequest req;
    req.requestId = 0;
    req.conversationId = QStringLiteral("summarizer:%1").arg(convId);
    LlmMessage userMsg;
    userMsg.role = QStringLiteral("user");
    userMsg.content = prompt;
    req.messages = {userMsg};
    req.config.providerId = providerId;
    req.config.modelName = modelName;
    req.config.temperature = 0.3;  // Faithful, lightly-varied prose.
    req.config.maxTokens = kSummaryMaxTokens;
    req.config.stream = true;  // Accumulate chunks; all providers stream.
    req.config.thinkingMode = false;
    // Explicit num_ctx — without it Ollama defaults to 2048 and truncates a
    // large summarisation prompt to nothing (empty summary). buildPrompt
    // bounds the input to kSummaryInputTokenBudget so this always fits.
    req.config.contextWindow = kSummaryContextWindow;

    m_busy = true;
    emit compactingChanged(convId, true);

    // Exactly-once completion + guaranteed timeout — same lifetime
    // design as RemoteBackend::classifyViaProviderAsync (rules 6/9/10).
    auto completed = std::make_shared<bool>(false);
    auto buffer = std::make_shared<QString>();
    auto conns = std::make_shared<QList<QMetaObject::Connection>>();
    QPointer<ConversationSummarizer> selfGuard(this);
    const QString modelUsedTag = QStringLiteral("%1/%2").arg(providerId, modelName);

    const quint64 ledgerId =
        ContentionLedger::begin(ContentionLedger::CallClass::Summary, providerId, modelName);
    auto finalize = [selfGuard,
                     completed,
                     conns,
                     convId,
                     reason,
                     modelUsedTag,
                     lastMsgId,
                     lastIdx,
                     coveredCount,
                     ledgerId](const QString& text, const QString& error) {
        if (*completed)
            return;
        *completed = true;
        ContentionLedger::end(
            ledgerId, error.isEmpty(), error.isEmpty() ? QString() : QStringLiteral("failed"));
        for (const auto& c : *conns)
            QObject::disconnect(c);
        conns->clear();
        if (!selfGuard)
            return;  // Service died; nothing to persist.
        selfGuard->m_busy = false;
        emit selfGuard->compactingChanged(convId, false);

        const QString trimmed = text.trimmed();
        if (!error.isEmpty() || trimmed.isEmpty()) {
            const QString err = error.isEmpty() ? QStringLiteral("empty summary output") : error;
            qCWarning(verzetaUi).noquote()
                << "Summarizer: generation failed for conv" << convId.left(8) << "—" << err;
            emit selfGuard->summaryFailed(convId, err);
            return;
        }

        ConversationSummary row;
        row.conversationId = convId;
        row.summaryText = trimmed;
        row.lastMessageId = lastMsgId;
        row.lastMessageIdx = lastIdx;
        row.coveredCount = coveredCount;
        row.generatedAtMs = QDateTime::currentMSecsSinceEpoch();
        row.tokenCount = estimateTokens(trimmed);
        row.modelUsed = modelUsedTag;
        row.triggerReason = reason;
        row.invalidationReason = QStringLiteral("none");

        if (!selfGuard->persist(row)) {
            emit selfGuard->summaryFailed(convId, QStringLiteral("failed to persist summary"));
            return;
        }
        selfGuard->m_cache.insert(convId, row);
        // L4 — receipt only after a successful persist; the receipt
        // is the user-visible proof the compaction actually happened.
        selfGuard->insertReceipt(convId, reason);
        qCInfo(verzetaUi) << "Summary generated for conv" << convId.left(8) << "— covered"
                          << coveredCount << "messages," << row.tokenCount
                          << "tokens, reason:" << reason;
        emit selfGuard->summaryReady(convId, reason);
    };

    ILLMProvider* provider = m_prov.provider.get();
    conns->append(QObject::connect(
        provider, &ILLMProvider::chunkReceived, this, [buffer](const LlmChunk& chunk) {
            if (!chunk.delta.isEmpty())
                buffer->append(chunk.delta);
        }));
    conns->append(QObject::connect(
        provider, &ILLMProvider::requestFinished, this, [buffer, finalize](const QString&, int) {
            finalize(*buffer, QString());
        }));
    conns->append(QObject::connect(
        provider, &ILLMProvider::requestError, this, [finalize](const QString& errorMessage) {
            finalize(QString(), errorMessage);
        }));

    // Guaranteed timeout — no QObject context; safe after any
    // destruction order (captures shared_ptrs + QPointer only).
    QTimer::singleShot(kGenerateTimeoutMs, nullptr, [finalize, completed, selfGuard]() {
        if (*completed)
            return;
        if (selfGuard && selfGuard->m_prov.valid()) {
            selfGuard->m_prov.provider->cancelRequest();
        }
        finalize(QString(), QStringLiteral("summarization timed out"));
    });

    qCInfo(verzetaUi) << "Summarizer: dispatching generation for conv" << convId.left(8) << "via"
                      << modelUsedTag << "reason:" << reason;
    ContentionLedger::markDispatched(ledgerId);
    provider->sendRequest(req);
}

// ---------------------------------------------------------------------------
// Persistence helpers
// ---------------------------------------------------------------------------

bool ConversationSummarizer::persist(const ConversationSummary& row) {
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO conversation_summaries "
                             "  (conversation_id, summary_text, last_message_id, "
                             "   last_message_idx, covered_count, generated_at_ms, "
                             "   token_count, model_used, trigger_reason, "
                             "   invalidation_reason) "
                             "VALUES (?,?,?,?,?,?,?,?,?,?) "
                             "ON CONFLICT(conversation_id) DO UPDATE SET "
                             "  summary_text=excluded.summary_text, "
                             "  last_message_id=excluded.last_message_id, "
                             "  last_message_idx=excluded.last_message_idx, "
                             "  covered_count=excluded.covered_count, "
                             "  generated_at_ms=excluded.generated_at_ms, "
                             "  token_count=excluded.token_count, "
                             "  model_used=excluded.model_used, "
                             "  trigger_reason=excluded.trigger_reason, "
                             "  invalidation_reason=excluded.invalidation_reason"));
    q.addBindValue(row.conversationId);
    q.addBindValue(row.summaryText);
    q.addBindValue(row.lastMessageId);
    q.addBindValue(row.lastMessageIdx);
    q.addBindValue(row.coveredCount);
    q.addBindValue(row.generatedAtMs);
    q.addBindValue(row.tokenCount);
    q.addBindValue(row.modelUsed);
    q.addBindValue(row.triggerReason);
    q.addBindValue(row.invalidationReason);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "Summarizer persist failed:" << q.lastError().text();
        return false;
    }
    return true;
}

void ConversationSummarizer::insertReceipt(const QString& convId, const QString& reason) {
    Message receipt;
    receipt.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    receipt.conversationId = convId;
    receipt.role = QStringLiteral("system");
    receipt.content = QStringLiteral("~Dynamic Compact Performed, Reason: %1~")
                          .arg(reason == QStringLiteral("manual") ? QStringLiteral("Manual")
                                                                  : QStringLiteral("Auto"));
    receipt.createdAt = QDateTime::currentDateTimeUtc();
    const QString id = m_msgSvc.addMessage(receipt);
    if (id.isEmpty()) {
        qCWarning(verzetaUi) << "Summarizer: receipt insert failed for conv" << convId.left(8);
    }
}

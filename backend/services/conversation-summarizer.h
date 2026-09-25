// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-summarizer.h
 * @brief Dynamic-compaction service: generates, persists, injects-on-
 *        demand and invalidates per-conversation summaries so long
 *        group chats keep their decision history in context after the
 *        HistoryBudgeter would otherwise drop it.
 * @layer Service
 * @dependencies DbManager, MessageService, ConversationService,
 * ModelRouter, SettingsService, Ragp classifier
 *               provider factory (owned-provider construction),
 *               Qt6::Core.
 *
 * Architecture:
 *   - One summary row per conversation (schema v20); new generations
 *     overwrite it.
 *   - Every compaction inserts a first-class role=system receipt
 *     message ("~Dynamic Compact Performed, Reason: X~").
 *   - Generation is asynchronous. The triggering turn falls back to
 *     plain budgeting and the NEXT turn benefits.
 *   - Trigger heuristic (size + dropped-count + freshness + per-conv
 *     enable flag), evaluated by RequestBuilder and computed here in
 *     shouldSummarize().
 *   - Invalidation on member or folder change, /flashmemory, manual
 *     regenerate, and a stale threshold.
 *   - A single hardcoded summarisation prompt template.
 *   - Generation dispatches on a summarizer-OWNED ILLMProvider instance
 *     built by the classifier provider factory, NOT on ModelRouter's
 *     BACKGROUND slot. That slot is single-in-flight and owned by the
 *     heartbeat dispatch lifecycle, so sharing it would let a heartbeat
 *     fire orphan an in-flight summarisation (or vice versa). The owned
 *     instance collides with neither foreground nor heartbeat traffic.
 *     Provider and model resolve from SettingsService
 *     summarization_provider/_model, defaulting to the active
 *     foreground pair when unset.
 *
 * Threading: strictly main-thread orchestration (asserts at every
 * public entry). Provider I/O is async signal-driven; the guaranteed
 * no-context QTimer timeout + QPointer guards mirror the
 * RemoteBackend::classifyViaProviderAsync lifetime design.
 *
 * Concurrency: ONE generation in flight at a time. A trigger that
 * arrives while busy is dropped with a log; the trigger heuristic
 * re-fires on the next turn, so the system self-heals without a queue.
 */

#pragma once

#include "../models/conversation-summary.h"
#include "ragp/classifier-provider-factory.h"

#include <optional>
#include <QHash>
#include <QObject>
#include <QString>

class ConversationService;
class DbManager;
class MessageService;
class ModelRouter;
class SettingsService;

/**
 * @brief Dynamic-compaction service: owns summary generation,
 *        persistence, freshness/invalidation, and the trigger
 *        heuristic. One instance app-wide (AppController-owned);
 *        strictly main-thread orchestration with async provider I/O.
 */
class ConversationSummarizer : public QObject {
    Q_OBJECT

  public:
    /** Minimum total DB messages before a conv is summarizable. */
    static constexpr int kMinSummarizableMessages = 30;

    /** Minimum messages the budgeter must be dropping before the
     *  auto-trigger fires; below this, plain budgeting is fine. */
    static constexpr int kMinDroppedMessages = 10;

    /** Newest messages NEVER covered by a summary. They stay verbatim
     *  in context, so summarising them would duplicate content. */
    static constexpr int kKeepLastN = 15;

    /** Under context pressure, a fresh summary is regenerated only once
     *  this many NEW **assistant turns** have accumulated beyond the
     *  message it last covered (the under-pressure backstop above the
     *  per-conversation CADENCE, which defaults to 20).
     *
     *  Counted in ASSISTANT TURNS via assistantTurnsAfter(), NOT raw
     *  rows. This is load-bearing: the per-compaction `role=system`
     *  receipt is not an assistant row, so it can never inflate this
     *  count. An earlier version measured "rows behind" by comparing the
     *  receipt-EXCLUDING `coveredCount` against a receipt-INCLUDING raw
     *  total; accumulated receipts then drove the gap past the threshold
     *  and the summary re-compacted every turn (each adding another
     *  receipt), a runaway that worsened with conversation age. Keep
     *  this metric receipt-immune. */
    static constexpr int kStaleAfterNewMessages = 30;

    /** Context-fill percentage at which the PROACTIVE trigger fires:
     *  a summary is generated BEFORE the budgeter drops anything, so
     *  the model never experiences a cliff where uncovered history
     *  silently vanishes. Deliberately equal to the UI gauge's amber
     *  threshold: the algorithm and the indicator tell one story. */
    static constexpr int kProactiveFillPercent = 70;

    /** Hard cap on the summarisation request's output tokens. The
     *  prompt targets 400-800 tokens; 1024 leaves headroom
     *  without letting a runaway model burn the window. */
    static constexpr int kSummaryMaxTokens = 1024;

    /** Context window (num_ctx) requested for the summarisation call.
     *  MUST be set explicitly: providers fall back to a small default
     *  (Ollama: 2048) which TRUNCATES a large summarisation prompt to
     *  nothing → an empty summary → all compacted history lost. Sized to
     *  hold the bounded input + a carried-over prior summary + the output. */
    static constexpr int kSummaryContextWindow = 32768;

    /** Upper bound on conversation-content tokens fed to ONE summarisation
     *  call. Rolling coverage (a fresh prior summary is carried in and only
     *  NEW messages past its coverage are summarised) keeps the per-call
     *  delta small; this caps the first/large-gap call so the prompt always
     *  fits kSummaryContextWindow. */
    static constexpr int kSummaryInputTokenBudget = 24000;

    /** Per-message excerpt cap when building the summarisation prompt.
     *  Bounds the prompt for conversations with very long messages.
     *  Decision-relevant content fits well under this. */
    static constexpr int kPerMessageExcerptChars = 700;

    /** Generation timeout (ms). Summaries are not latency-critical
     *  (they land for the NEXT turn) but must resolve deterministically. */
    static constexpr int kGenerateTimeoutMs = 120000;

    /**
     * @param db       Engine layer (non-owning); summary row CRUD.
     * @param msgSvc   Message reads (prompt assembly) + system-receipt
     *                 insertion (non-owning).
     * @param convSvc  Conversation metadata + llm_config reads
     *                 (non-owning).
     * @param router   Active provider/model resolution (non-owning).
     * @param settings Summarization provider/model overrides
     *                 (non-owning).
     * @param parent   Qt parent (AppController).
     */
    ConversationSummarizer(DbManager& db,
                           MessageService& msgSvc,
                           ConversationService& convSvc,
                           ModelRouter& router,
                           SettingsService& settings,
                           QObject* parent = nullptr);
    ~ConversationSummarizer() override;

    /**
     * @brief Loads the conversation's summary row (cache-first).
     * @param convId Conversation UUID.
     * @returns The row, or nullopt when none exists. Callers must
     *          check .fresh() before injecting.
     */
    std::optional<ConversationSummary> summaryFor(const QString& convId);

    /**
     * @brief Trigger heuristic with three independent paths, all gated
     *        by freshness/staleness so a fresh summary can never
     *        re-trigger every turn:
     *          REACTIVE:  the budgeter is already dropping history
     *                      (the original behaviour; now the backstop).
     *          PROACTIVE: measured context fill reached the amber
     *                      threshold (kProactiveFillPercent), so compact
     *                      BEFORE anything is dropped.
     *          CADENCE:   `compactEveryTurns` assistant replies
     *                      accumulated beyond current coverage
     *                      (configurable per conversation; 0 = off),
     *                      for attention freshening independent of fill.
     *        Read-mostly: the cadence path runs one COUNT query.
     * @param convId        Conversation UUID.
     * @param totalMessages Current total DB message count for the conv.
     * @param droppedCount  Messages the budgeter is dropping THIS turn.
     * @param compactEnabled Per-conv dynamic_compact_enabled flag
     *                       (resolved by the caller from LlmConfig).
     * @param fillPercent   Measured context fill (0-100) for THIS
     *                      request build; 0 when unknown.
     * @param compactEveryTurns Per-conv cadence in assistant turns;
     *                      0 disables the cadence path.
     * @returns true when a (re)generation should be enqueued.
     */
    bool shouldSummarize(const QString& convId,
                         int totalMessages,
                         int droppedCount,
                         bool compactEnabled,
                         int fillPercent = 0,
                         int compactEveryTurns = 0);

    /**
     * @brief Starts an asynchronous summary generation. Fire-and-forget
     *        for callers; completion lands via summaryReady (success)
     *        or summaryFailed.
     *
     *        Drops the request (with a log) when a generation is
     *        already in flight; the trigger heuristic re-fires next
     *        turn, so the system self-heals. Inserts the system receipt
     *        message only AFTER the summary persists successfully.
     * @param convId Conversation UUID.
     * @param reason "auto" | "manual"; persisted + shown in the receipt.
     */
    void generateAsync(const QString& convId, const QString& reason);

    /**
     * @brief Invalidates the stored summary: marks it non-fresh so it
     *        stops being injected; the next qualifying turn regenerates.
     * @param convId Conversation UUID.
     * @param reason One of the schema's invalidation_reason values
     *               (member_changed / folder_changed / flashmemory /
     *               user_regenerated / stale).
     */
    void invalidate(const QString& convId, const QString& reason);

    /**
     * @brief Deletes the conversation's summary row outright
     *        (/flashmemory wipe path).
     * @param convId Conversation UUID.
     */
    void clearFor(const QString& convId);

    /**
     * @brief Assistant turns accumulated toward the NEXT cadence compaction,
     *        i.e. turns since the current fresh summary's coverage (or since
     *        the start when there is no fresh summary). This is the same count
     *        the CADENCE trigger in shouldSummarize() compares against
     *        `compactEveryTurns`, exposed so the chat-input gauge can show
     *        progress toward the next memory refresh.
     * @param convId Conversation UUID.
     * @returns Assistant turns since coverage (0 when none).
     */
    int cadenceTurnsUsed(const QString& convId);

    /**
     * @brief True while a generation is in flight (any conversation).
     * @returns The single-flight guard state.
     */
    bool busy() const { return m_busy; }

    /**
     * @brief TEST-ONLY seam: pre-seeds the owned provider bundle with
     *        a caller-supplied provider (typically a stub emitting
     *        canned chunks) bound to `providerId`, so generateAsync's
     *        full async flow is testable without network or model.
     *        ensureProvider() keeps the seeded bundle as long as the
     *        resolved provider id matches.
     * @param providerId Provider id the stub impersonates.
     * @param provider   Owned stub instance.
     */
    void setProviderForTest(const QString& providerId, std::unique_ptr<ILLMProvider> provider) {
        m_prov.http.reset();
        m_prov.provider = std::move(provider);
        m_prov.providerId = providerId;
    }

  signals:
    /**
     * @brief A summary generated + persisted + receipt inserted.
     * @param convId Conversation UUID.
     * @param reason "auto" | "manual".
     */
    void summaryReady(const QString& convId, const QString& reason);

    /**
     * @brief Generation failed (provider error / timeout / empty
     *        output). No receipt is inserted; any prior summary row is
     *        left untouched.
     * @param convId Conversation UUID.
     * @param error  Human-readable cause.
     */
    void summaryFailed(const QString& convId, const QString& error);

    /**
     * @brief Emitted when the single-flight busy state flips, carrying
     *        the conversation whose summary is being generated. `busy` is
     *        true the moment generation starts and false when it finishes
     *        (whether it succeeded or failed). Drives the "compacting…"
     *        UI indicator.
     * @param convId Conversation UUID being compacted.
     * @param busy   True at start, false on completion.
     */
    void compactingChanged(const QString& convId, bool busy);

  private:
    /** Builds the summarisation prompt from the conversation's messages,
     *  excluding compaction receipts and the kKeepLastN tail. Returns
     *  empty when there is nothing worth summarising. Out-params
     *  capture the coverage bookkeeping persisted with the row. */
    QString buildPrompt(const QString& convId,
                        QString* outLastMsgId,
                        int* outLastIdx,
                        int* outCoveredCount);

    /** Counts the conversation's role=assistant rows created after
     *  the given message (all of them when afterMsgId is empty or
     *  unknown). One indexed COUNT query, which is the cadence path's cost. */
    int assistantTurnsAfter(const QString& convId, const QString& afterMsgId);

    /** Persists (UPSERTs) the summary row. Returns false on SQL error. */
    bool persist(const ConversationSummary& row);

    /** Inserts the role=system compaction receipt message. */
    void insertReceipt(const QString& convId, const QString& reason);

    /** (Re)builds m_prov for the resolved provider when missing or
     *  stale. False (with whyNot) when construction is impossible. */
    bool ensureProvider(const QString& providerId, QString* whyNot);

    DbManager& m_db;
    MessageService& m_msgSvc;
    ConversationService& m_convSvc;
    ModelRouter& m_router;
    SettingsService& m_settings;

    /** Cache: convId → summary row (mirrors DB; dropped on invalidate). */
    QHash<QString, ConversationSummary> m_cache;

    /** Summarizer-owned provider bundle (see the provider note in the
     *  file header). */
    Ragp::ClassifierProvider m_prov;

    /** Single-flight guard. */
    bool m_busy = false;
};

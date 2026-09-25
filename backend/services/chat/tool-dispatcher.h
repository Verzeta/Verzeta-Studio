// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file tool-dispatcher.h
 * @brief ChatController collaborator that owns the serial tool-call
 *        batch dispatch state machine.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core, MessageService, ConversationService
 *               (non-owning references), models/tool-call.h,
 *               api/llm-interface.h (via ToolService), and (via the
 *               ToolBatchInputs struct) non-owning pointers to
 *               ToolService, PlanService, TaskRunner, TaskObserver,
 *               FileService, MembershipService.
 *
 * State machine:
 *   - pending-calls FIFO queue (populated during chunk streaming)
 *   - shared parent assistant-row id for the batch
 *   - iteration counter (capped by kMaxToolIterations to stop
 *     runaway tool loops)
 *
 * The batch-drain → continuation-turn tail-call uses
 *   QTimer::singleShot(0, weak.data(), [weak]{ dispatchNext(); })
 * instead of a direct self-call. This is not ornamental: with
 * N≥2 parallel tool calls completing synchronously, a direct
 * recursive call blows the stack on long batches. The
 * QTimer-and-QPointer form posts the next dispatch to the next
 * event-loop tick and bails gracefully if the dispatcher is
 * destroyed in between.
 *
 * Ownership: constructed in ChatController's constructor via
 *   m_toolDispatcher = std::make_unique<Chat::ToolDispatcher>(
 *       m_msgSvc, m_convSvc, this);
 * Declared AFTER StreamingManager in chat-controller.h so reverse-
 * order destruction runs ToolDispatcher → StreamingManager →
 * RequestBuilder → services, which is dependency-correct.
 *
 * Threading: strictly main-thread. Every public method begins with
 *   VERZETA_ASSERT_MAIN_THREAD();
 * The tool handler itself runs on a QtConcurrent worker thread for
 * tools NOT in the main-thread subset (SQLite-touching tools run
 * inline). The result is delivered back on the main thread via
 * QFutureWatcher.
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

class AuditService;
class ConversationService;
class FileService;
class MembershipService;
class MessageService;
class PlanService;
class TaskObserver;
class TaskRunner;
class ToolService;

namespace Chat {

/**
 * @brief Per-batch inputs that ChatController snapshots at enqueue
 *        time and hands to the dispatcher. Everything the async
 *        completion callback needs to run without reaching back into
 *        ChatController's member state.
 *
 * Nullable service pointers match ChatController's own nullable
 * semantics. If a service isn't attached when the batch fires, the
 * dispatcher gracefully skips the corresponding side effect.
 */
struct ToolBatchInputs {
    /** Assistant row id that emitted the tool_calls finish. Every
     *  tool_calls side-table row shares this parent. */
    QString parentAssistantMsgId;

    /** Conversation snapshot, stable across batch even if user
     *  switches chats mid-batch. */
    QString inflightConvId;

    /** Monotonic request id at dispatch time. Any callback whose
     *  own `requestIdAtDispatch` capture diverges from the
     *  dispatcher's recorded batch id is stale and its result is
     *  dropped. */
    quint64 requestId = 0;

    /** Identity of the responder that emitted the batch, used for
     *  tool-result attribution and for the continuation turn's
     *  streaming begin(). */
    QString responseMemberAlias;
    QString responseMemberAgentId;  ///< Agent id of the same responder.

    /** Current active-task anchor. Read by the TaskObserver hook +
     *  by AUTO-ANCHOR decision. */
    QString activeTaskPlanId;

    /** Last user text, used by AUTO-ANCHOR to synthesize a plan
     *  goal when a substantive tool fires without start_task. */
    QString lastUserText;

    /** Folder id the calling conv resolves to (the
     *  first `isProject()` row in
     *  `ConversationService::folderChainForConversation`, or the
     *  conv's direct folder when no project ancestor). Computed once
     *  per batch by `ToolDispatcher` during the existing chain walk
     *  it does for `setActiveProjectContext`; threaded into
     *  `ToolService::invokeTool` so FileService's virtual-FS router
     *  can resolve mount routing for tool-initiated file I/O. Empty
     *  when the conv has no folder chain. */
    QString resolvedCallerFolderId;

    // Services (non-owning, nullable). Caller snapshots pointers
    // from ChatController's m_* fields at enqueue time.
    ToolService* toolService = nullptr;    ///< Runs each tool call.
    PlanService* planService = nullptr;    ///< Needed, with taskRunner, for AUTO-ANCHOR.
    TaskRunner* taskRunner = nullptr;      ///< Starts AUTO-ANCHOR plans; notified after tool runs.
    TaskObserver* taskObserver = nullptr;  ///< Records tool calls and files for the active task.
    FileService* fileService = nullptr;    ///< Receives the project and conversation context.
    MembershipService* membershipService = nullptr;  ///< Resolves request_turn targets to members.
};

/**
 * @brief Main-thread-only collaborator owned by ChatController.
 *        Does NOT expose Q_PROPERTY / Q_INVOKABLE; all QML-facing
 *        surface stays on ChatController (facade-preservation
 *        invariant).
 *
 * Usage contract:
 *
 *   1. During streaming, ChatController::onChunkReceived calls
 *      appendPendingCall(chunk.toolCallJson) for every chunk whose
 *      toolCallJson is non-empty. Order preserved.
 *
 *   2. When the provider finishes with finishReason=="tool_calls",
 *      ChatController calls enqueueBatch(inputs). The dispatcher
 *      takes ownership of the pending-calls queue from step 1,
 *      records the batch parent id + request id, and dispatches
 *      the first call.
 *
 *   3. Each tool completion either:
 *      - dispatches the next call in the batch (via the
 *        QTimer::singleShot tail-call described in the file
 *        header), OR
 *      - emits batchCompletedContinueTurn when the queue drains,
 *        so ChatController can re-open streaming and re-dispatch
 *        the LLM for the continuation turn.
 *
 *   4. At any cleanup boundary (user cancel, provider error,
 *      conversation switch, turn end, retry) ChatController calls
 *      resetState(). This clears all batch state synchronously;
 *      any in-flight async callback sees its requestId no longer
 *      matches and drops its result.
 */
class ToolDispatcher : public QObject {
    Q_OBJECT

  public:
    /** Default per-member-turn cap on chained tool iterations: a
     *  runaway-loop backstop, NOT a work budget. 10 was too low for
     *  legitimate multi-file work (an engineer building a component library
     *  across ~15 files hit it mid-task and the round died), so the default
     *  is 25. The EFFECTIVE cap is per-instance and user-configurable via
     *  SettingsService::toolIterationCap; see maxToolIterations(). */
    static constexpr int kDefaultToolIterations = 25;

    /** @brief The effective per-turn tool-iteration cap.
     *  @returns The cap; 0 means UNLIMITED (no backstop). Defaults to
     *           kDefaultToolIterations. */
    int maxToolIterations() const { return m_maxToolIterations; }

    /** @brief Set the effective per-turn tool-iteration cap.
     *  @param cap Maximum chained tool iterations; 0 = unlimited. Negative
     *             values are clamped to the default. */
    void setMaxToolIterations(int cap) {
        m_maxToolIterations = cap < 0 ? kDefaultToolIterations : cap;
    }

    /** @brief Whether the iteration budget has been reached (the single
     *         source for the budget-cap decision).
     *  @param iterations The current chained-tool iteration count.
     *  @returns true when @p iterations has reached the cap; always false
     *           when the cap is 0 (unlimited). */
    bool iterationCapReached(int iterations) const {
        return m_maxToolIterations > 0 && iterations >= m_maxToolIterations;
    }

    /** Consecutive failed tool calls that trip the runaway-FAILURE stop.
     *  Any success resets the count. Distinct from the iteration budget:
     *  this catches a stuck loop of FAILING tools (a broken tool retried
     *  forever) regardless of the budget, and is always in force (a small
     *  fixed safety value, not user-tunable). */
    static constexpr int kMaxConsecutiveToolFailures = 5;

    /** @brief The current consecutive-failure streak.
     *  @returns Number of tool calls that have failed in a row (reset on
     *           any success or a fresh turn). */
    int consecutiveFailures() const { return m_consecutiveFailures; }

    /** @brief Whether the consecutive-failure stop threshold was reached.
     *  @returns true when consecutive failures reached
     *           kMaxConsecutiveToolFailures, signalling a stuck loop of failing tools
     *           that must be interrupted. */
    bool failureCapReached() const { return m_consecutiveFailures >= kMaxConsecutiveToolFailures; }

    /**
     * @param msgSvc  Non-owning; must outlive this dispatcher.
     *                ChatController owns both.
     * @param convSvc Non-owning; used for folder-chain lookups when
     *                routing file-producing tools to the right
     *                project/conversation artifact directory.
     * @param parent  Qt parent (ChatController), providing backup destruction
     *                via the parent-chain behind std::unique_ptr
     *                member cleanup.
     */
    explicit ToolDispatcher(MessageService& msgSvc,
                            ConversationService& convSvc,
                            QObject* parent = nullptr);
    ~ToolDispatcher() override;

    /**
     * @brief Attach an AuditService so every tool completion records
     *        a `tool_invoked` event, and successful `write_file` /
     *        `edit_canvas` invocations additionally record
     *        `file_written` / `canvas_edited` events using the
     *        result-JSON fields the tools return. AppController wires
     *        this once during initialize(). When unset, the audit
     *        hooks are no-ops.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setAuditService(AuditService* svc);

    // -----------------------------------------------------------------
    // State queries (main-thread only)
    // -----------------------------------------------------------------

    /**
     * @brief Whether a batch is currently in flight.
     * @returns true iff the queue has not fully drained yet.
     */
    bool hasPendingBatch() const;

    /**
     * @brief Tool-chain echo breaker. Called by ChatController once
     *        per `tool_calls` finish BEFORE enqueueing the
     *        continuation batch. Compares the new assistant narration
     *        against the PREVIOUS tool-chain turn's narration (same
     *        in-flight chain) using the cascade's content
     *        fingerprint + Jaccard similarity.
     *
     *        Returns true when the narration is a near-verbatim
     *        repeat (>= CascadeController::kCascadeNoveltyThreshold),
     *        the signature of the runaway loop where an agent
     *        repeats "let me try a different approach" + the same
     *        tool call forever. The caller then force-finalizes the
     *        chain (same machinery as the iteration cap) instead of
     *        burning further iterations.
     *
     *        Always updates the stored fingerprint to the current
     *        narration, so consecutive calls compare successive
     *        turns. Cleared by resetState() with the rest of the
     *        chain state.
     *
     *        Empty/whitespace narration returns false, because empty
     *        tool-call turns are legitimate (model went straight to
     *        the tool) and are covered by the iteration cap.
     *
     * @param narration The assistant content of the just-finished
     *                  `tool_calls` turn.
     * @returns true iff this narration echoes the previous tool-chain
     *          turn and the chain should be force-finalized.
     */
    bool continuationIsEcho(const QString& narration);

    /**
     * @brief STRUCTURAL candidate gate for the deferred-action
     *        continuation. It replaces the retired English
     *        verb-list detectors, which could not survive
     *        multi-language narration and were rejected as a decision
     *        mechanism. A candidate is any narration containing a
     *        language-neutral concrete token: a REGISTERED tool name
     *        (live registry, never a hardcoded list), a
     *        `filename.ext` token with a curated technical extension,
     *        the canvas surface noun, or the DANGLING-PAYLOAD shape,
     *        where the narration ends with a colon (ASCII or fullwidth),
     *        the typographic marker of announced content that never
     *        followed. A match only means "ask the
     *        LLM decider"; the semantic verdict (self-kept pending
     *        action vs delegation vs nothing) is ALWAYS the LLM's:
     *        the classify pending_action field in groups, the intent
     *        confirmer in 1:1.
     * @param narration           Terminal assistant reply text.
     * @param registeredToolNames Tool names from the live ToolService
     *                            snapshot (empty list disables the
     *                            tool-name path only).
     * @returns "deferred_action" when a concrete token is present;
     *          else empty.
     */
    static QString narrationDeferredActionKind(const QString& narration,
                                               const QStringList& registeredToolNames);

    /**
     * @brief Extract the concrete targets a narration announces acting on.
     *
     * Returns the union of (a) registered tool names spoken in the
     * prose, (b) `filename.ext` tokens (curated extension set),
     * normalized to the lowercase basename, and (c) the synthetic
     * "canvas" target when the canvas surface is named. Shares its
     * token vocabulary with narrationDeferredActionKind so the gate
     * and the fire-awareness suppression can never disagree.
     *
     * @param narration           Terminal assistant reply text.
     * @param registeredToolNames Tool names from the live ToolService
     *                            snapshot.
     * @returns Lowercased target set; empty when nothing concrete is named.
     */
    static QSet<QString> announcedActionTargets(const QString& narration,
                                                const QStringList& registeredToolNames);

    /**
     * @brief Decide whether a narration announces ONLY work that already
     *        executed in the current turn (the "do not re-nudge an agent
     *        that just did the work" guard).
     *
     * A reply summarising completed calls ("I've updated guide.md" right
     * after edit_canvas ran on guide.md) must NEVER re-enter the
     * deferred-action pipeline; a reply announcing at least one target
     * with no matching executed call is genuinely pending work and stays
     * a candidate. A target-less narration is treated as backed when ANY
     * call executed this turn.
     *
     * @param narration             Terminal assistant reply text.
     * @param registeredToolNames   Tool names from the live ToolService
     *                              snapshot.
     * @param executedToolNames     Lowercased tool names executed in the
     *                              current turn-chain.
     * @param executedFileBasenames Lowercased basenames of files named in
     *                              those calls' arguments.
     * @returns true when everything announced is already covered (caller
     *          suppresses candidacy); false when new work is announced.
     */
    static bool announcesOnlyExecutedWork(const QString& narration,
                                          const QStringList& registeredToolNames,
                                          const QSet<QString>& executedToolNames,
                                          const QSet<QString>& executedFileBasenames);

    /**
     * @brief Number of pending calls still queued for dispatch.
     * @returns Count of queued-but-not-yet-dispatched tool calls.
     */
    int pendingCount() const;

    /**
     * @brief Current iteration counter. Each dispatched call
     *        increments it. ChatController's onRequestFinished reads
     *        this to honour the cap check BEFORE handing a fresh
     *        batch to enqueueBatch.
     * @returns Cumulative tool-call iteration count for the current
     *          turn.
     */
    int iterationCount() const;

    /**
     * @brief Parent assistant row id of the current (or last) batch.
     *        Useful for diagnostics only.
     * @returns Parent row UUID, or empty string when no batch is in
     *          flight.
     */
    QString currentBatchParentMsgId() const;

    // -----------------------------------------------------------------
    // State mutations (main-thread only)
    // -----------------------------------------------------------------

    /**
     * @brief Append a tool-call JSON object to the pending-calls
     *        queue. Called from ChatController::onChunkReceived for
     *        every streaming chunk that carries a non-empty
     *        toolCallJson.
     *
     * Providers can emit N≥2 tool calls in one response (OpenAI /
     * Anthropic parallel tools, qwen3.5 parallel tool-use), and each
     * chunk carries one call. APPEND, never overwrite: a regression
     * that replaces instead of appends silently loses every call
     * except the last.
     *
     * @param call Tool-call object in {id, name, arguments} shape.
     *             Empty objects are ignored (caller bug; logged).
     */
    void appendPendingCall(const QJsonObject& call);

    /**
     * @brief Begin a new dispatch batch. Moves the pending-calls
     *        queue into the dispatcher's in-flight state and
     *        dispatches the first call.
     *
     * Preconditions:
     *   - At least one pending call has been appended since the last
     *     batch completed or was reset.
     *   - inputs.parentAssistantMsgId is non-empty (the
     *     tool_calls-bearing assistant row has been persisted to DB).
     *   - inputs.requestId is non-zero (the inflight request the
     *     tool calls originated from).
     *
     * Post: a batch is in flight. When the queue fully drains, the
     * dispatcher emits batchCompletedContinueTurn and
     * ChatController re-opens streaming + re-dispatches the LLM.
     *
     * Emits batchFailed + clears state on any precondition miss so
     * no silent orphan tool calls leak into the DB.
     *
     * @param inputs Fully-populated ToolBatchInputs snapshot.
     */
    void enqueueBatch(const ToolBatchInputs& inputs);

    /**
     * @brief Clear ALL batch state without emitting any signals.
     *        Used on user cancel, provider error, conversation
     *        switch, turn end, retry: any path where ChatController
     *        wants to discard the in-flight batch cleanly.
     *
     * After resetState() returns:
     *   - hasPendingBatch() == false
     *   - iterationCount() == 0
     *   - currentBatchParentMsgId() == ""
     *   - m_batchRequestId = 0, so any in-flight QFutureWatcher
     *     callback sees its dispatch-time captured requestId no
     *     longer matches and drops its result silently.
     */
    void resetState();

  private:
    /** Fingerprint of the previous tool-chain turn's narration for
     *  the echo breaker (see continuationIsEcho). Cleared by
     *  resetState(). Declared in a dedicated private block adjacent
     *  to its API for locality; the class's main private section
     *  follows further below. */
    QSet<QString> m_lastChainNarrationFp;

  public:
  signals:
    /**
     * @brief Mirrored onto ChatController's existing public signal of
     *        the same name via a signal-to-signal connect. QML binds
     *        to ChatController, never to the dispatcher.
     * @param toolName Name of the tool being invoked.
     * @param argsJson Tool arguments as a JSON-encoded string.
     */
    void toolCallStarted(const QString& toolName, const QString& argsJson);

    /**
     * @brief Mirrored onto ChatController's existing public signal of
     *        the same name via a signal-to-signal connect.
     * @param toolName   Name of the tool that completed.
     * @param resultJson Tool result payload as a JSON-encoded string.
     */
    void toolCallCompleted(const QString& toolName, const QString& resultJson);

    /**
     * @brief Emitted when the pending-calls queue fully drains after
     *        the last tool in the batch completes. ChatController's
     *        slot re-opens a streaming placeholder and feeds the tool
     *        results back to the LLM for its continuation turn.
     *
     *        This signal is the ONLY path from "batch drained" back
     *        to a new LLM request; the dispatcher never calls
     *        buildAndSendRequest itself.
     * @param convId  Conversation UUID the continuation turn targets.
     * @param agentId Agent id whose tools just completed.
     * @param alias   Member alias whose tools just completed.
     */
    void
    batchCompletedContinueTurn(const QString& convId, const QString& agentId, const QString& alias);

    /**
     * @brief Emitted for every member alias the request_turn tool's
     *        result names as the next responder. ChatController's
     *        slot appends the alias to m_agentCascadeQueue (dedup +
     *        self-skip handled there, since the star-topology rule says
     *        cross-collaborator state stays owned by the facade).
     * @param alias Member alias request_turn names as the next
     *              responder.
     */
    void requestTurnTargetQueued(const QString& alias);

    /**
     * @brief Emitted when the AUTO-ANCHOR rule creates a plan row
     *        retroactively (a substantive tool fired without an
     *        explicit start_task and no active task was anchored
     *        before). ChatController's slot sets m_activeTaskPlanId
     *        so the next prompt-build picks up the ACTIVE TASK
     *        framing.
     * @param planId UUID of the newly-anchored plan row.
     */
    void activeTaskAutoAnchored(const QString& planId);

    /**
     * @brief Unhappy-path signal: the batch could not be dispatched
     *        or completed for a reason that's not "natural drain"
     *        (missing ToolService, missing parent msgId, etc.).
     *        ChatController's slot surfaces this via its existing
     *        errorOccurred signal so QML sees one unified error
     *        channel.
     * @param reason Human-readable failure description.
     */
    void batchFailed(const QString& reason);

  private:
    /**
     * @brief Pop the front of the queue and run its tool via the
     *        ToolService captured in m_batchInputs. The async
     *        completion path either dispatches the next call (queue
     *        not empty) or emits batchCompletedContinueTurn (queue
     *        drained). The QTimer::singleShot tail-call described
     *        in the file header guards against deep recursion on
     *        very long synchronous batches.
     */
    void dispatchNext();

    MessageService& m_msgSvc;                // non-owning
    ConversationService& m_convSvc;          // non-owning
    AuditService* m_auditService = nullptr;  // non-owning; optional

    QList<QJsonObject> m_pendingCalls;
    QString m_batchParentMsgId;
    int m_iterations = 0;
    /** Effective per-turn cap; 0 = unlimited. User-configurable via
     *  SettingsService::toolIterationCap (see setMaxToolIterations). */
    int m_maxToolIterations = kDefaultToolIterations;
    /** Consecutive failed tool calls (reset on any success / resetState). */
    int m_consecutiveFailures = 0;

    /** The inflight request id this batch belongs to. Used by the
     *  async callback to detect staleness: if ChatController calls
     *  resetState() between dispatch and completion, this is set
     *  back to 0 and the comparison `m_batchRequestId ==
     *  requestIdAtDispatch` becomes false. */
    quint64 m_batchRequestId = 0;

    /** Per-batch inputs snapshot: everything the async callback
     *  needs without reaching into ChatController state. */
    ToolBatchInputs m_batchInputs;
};

}  // namespace Chat

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file cascade-controller.h
 * @brief ChatController collaborator that owns the group-chat
 *        agent-cascade state machine (queue, per-member turn caps,
 *        global cap, current responder identity) AND the RAGP
 *        classification pipeline (Ragp::Service unique_ptr, backend
 *        swap, deferred auto-fallback).
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core, ConversationService, MembershipService,
 *               ModelRouter (non-owning references), Ragp::Service
 *               (owned), ragp types, models/member.h,
 *               SettingsService (pointer, for RAGP backend
 *               configuration).
 *
 * Owns:
 *   - m_queue              cascade queue (ordered aliases)
 *   - m_memberTurns        per-alias turn counter (cap defence)
 *   - m_iterations         global cascade cap counter
 *   - m_responderAlias /   current responder identity used for
 *     m_responderAgentId   attribution + @-mention parsing
 *   - m_ragpService        unique_ptr<Ragp::Service>
 *   - public static constants kMaxAgentCascade + kMaxTurnsPerMember
 *
 * The cascade-end path (cap warning, queue clear, state reset) lives
 * here as emitCascadeCompleteAndClear; the remaining end-of-turn work
 * (active-task declared-status / implicit completion / streaming
 * reset / turn-end cleanup) stays on ChatController and runs in its
 * onCascadeComplete slot.
 *
 * Deferred-fallback for local RAGP backend load failure:
 * LocalLlamaBackend::loadFailed is wired to a
 * QTimer::singleShot(0, this, ...) that swaps in the RemoteBackend
 * on the next main-thread tick. The defer is not cosmetic: without
 * it the signal emission would delete the sender mid-emit. The
 * QPointer<SettingsService> guard keeps the deferred call safe if
 * settings are destroyed in between. test-ragp-backend-swap.cpp is
 * the regression guard.
 *
 * Ownership: constructed in ChatController's constructor via
 *   m_cascade = std::make_unique<Chat::CascadeController>(
 *       m_convSvc, m_router, this);
 * MembershipService is attached later via setMembershipService
 * because AppController wires it after ChatController construction.
 * The owned Ragp::Service starts with a StubBackend; configureBackend
 * swaps in remote/local at AppController wiring time.
 *
 * Threading: strictly main-thread. Every public method asserts via
 * VERZETA_ASSERT_MAIN_THREAD(). The Tier 3 RAGP backend may run
 * on a worker thread internally; results arrive on the main thread
 * via QFutureWatcher.
 */

#pragma once

#include "../../models/member.h"
#include "../ragp/ragp-types.h"
#include "voice-turn-gate.h"

#include <QTimer>

#include <memory>
#include <QFuture>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

class ConversationService;
class MembershipService;
class ModelRouter;
class SettingsService;

namespace Ragp {
class Service;
class IRagpBackend;
}  // namespace Ragp

namespace Verzeta::Infer {
class InferenceSidecarHost;
}

namespace Chat {

/**
 * @brief Per-turn inputs for routeOrFinalize. ChatController snapshots
 *        these at the end of onRequestFinished's persistence +
 *        sanitisation block and hands them in one call; the cascade
 *        controller decides whether to dispatch another member turn
 *        or signal completion.
 */
struct CascadeRouteInputs {
    /** The in-flight conversation id (m_inflightConvId on
     *  ChatController). Used for membership lookups, RAGP request
     *  envelope, and passthrough to the user-mention signal. */
    QString convId;

    /** Captured pre-finalize responder content. StreamingManager::
     *  finalize clears m_content internally, so downstream consumers
     *  (RAGP classification + user-mention notification payload)
     *  must capture the value BEFORE the caller invokes finalize. */
    QString content;

    /** Monotonic request id snapshot. Only used by the async Tier 3
     *  path to detect stale completions. */
    quint64 requestId = 0;

    /** Persisted message id of the responder's reply (captured by
     *  ChatController BEFORE m_streaming->finalize since finalize
     *  clears msgId). Used by the echo-retry path to delete the
     *  bad message before re-dispatching the same member. May be
     *  empty if the reply was an empty assistant turn that was
     *  never persisted. */
    QString responderMsgId;

    /** Passthrough fields. CascadeController does not interpret
     *  these; it copies them into cascadeComplete's arg list when
     *  the cascade ends so ChatController's slot can drive the
     *  active-plan state transitions + turn-end cleanup. */
    QString declaredTaskStatus;
    QString finishReason;  ///< Provider finish reason, or an internal stop reason.
    int totalTokens = 0;   ///< Tokens used by the turn.
    qint64 elapsedMs = 0;  ///< Wall-clock duration of the turn.


    /** Alias of the teammate a LEADING attribution prefix named when it
     *  differed from the seated responder (ContentSanitizer's
     *  identity-classified strip), meaning the model wrote the reply in
     *  another member's voice. Non-empty routes the reply through the bounded
     *  delete-and-retry path (reason "impersonation"): persisting the
     *  imposter body under the responder poisons every later request's
     *  history. Empty for clean replies and 1:1 chats. */
    QString impersonatedAlias;

    /** Human-readable list of the tool calls the responder already
     *  executed in this turn-chain. It is forwarded into the classify
     *  prompt's executed-tools FACT line so completed work is never
     *  judged as a pending action. */
    QString executedToolsSummary;

    /** True when the reply contains language-neutral structural action
     *  tokens (registered tool name / filename.ext / canvas) that are
     *  NOT covered by executed calls. Widens the classify precheck so
     *  a mention-less candidate reply still gets the LLM's
     *  pending-action verdict (mention-bearing replies classify
     *  regardless). */
    bool hasStructuralActionSignals = false;

    /** True when the responder's per-alias deferred-continuation budget
     *  for this user round is spent. The pending-action verdict is
     *  then informational only (no continuation is requested). */
    bool deferredBudgetExhausted = false;
};

/**
 * @brief Main-thread-only collaborator owned by ChatController.
 *        No Q_PROPERTY / Q_INVOKABLE; the QML-facing surface stays on
 *        ChatController (facade-preservation invariant).
 */
class CascadeController : public QObject {
    Q_OBJECT

  public:
    /** Upper bound on total cascade turns per user message
     *  (was ChatController::kMaxAgentCascade). */
    static constexpr int kMaxAgentCascade = 10;

    /** Upper bound on turns per individual agent alias. Set to 3 to
     *  accommodate chain-handoff patterns (an agent can legitimately
     *  return to the conversation twice after handing off and
     *  receiving a follow-up) while still ruling out runaway
     *  ping-pong. Lowering this to 1 forbids the natural "A → B → A"
     *  loop; see test-cascade-controller per-member-cap sub-test. */
    static constexpr int kMaxTurnsPerMember = 3;

    /** Number of recent responder-content fingerprints retained per
     *  cascade for the saturation check. Sized to cover an entire
     *  kMaxAgentCascade run so that an echo at turn N is still
     *  detectable against turn 0. A window of 3 missed
     *  cross-cascade echoes once a 5+ agent group fanned out. */
    static constexpr int kCascadeFingerprintWindow = kMaxAgentCascade;

    /** Jaccard similarity ceiling (0.0–1.0). When a new responder
     *  reply's fingerprint matches ANY recent fingerprint at this
     *  similarity or higher, the responder's reply is treated as a
     *  near-verbatim copy of a teammate (or a recent self-turn). The
     *  bad reply is discarded and the responder is given up to
     *  kMaxEchoRetries internal attempts to produce an original
     *  reply. 0.95 catches genuine copies (paraphrase / verbatim
     *  echo) without flagging legitimate chain-handoffs that share
     *  some vocabulary with a prior turn. */
    static constexpr double kCascadeNoveltyThreshold = 0.95;

    /** Maximum number of internal retries given to a responder whose
     *  initial reply was flagged as an echo (or empty). Each retry
     *  is invisible to the user: the bad reply is deleted, a
     *  progressively stronger anti-echo nudge is appended to the
     *  prompt, and the same member is re-dispatched. After this many
     *  failures the cascade either keeps the (still-flagged) reply
     *  with an "echo-failed" marker (if non-empty) or drops it
     *  entirely (if empty), then continues to the next member. */
    static constexpr int kMaxEchoRetries = 3;

    /** Autonomous ROUND continuation. When a cascade exhausts its turn caps
     *  while the classifier still has a valid refused-by-cap target AND the
     *  round showed real progress (a tool fire, a corrective retry, OR a
     *  genuinely novel (non-echoed) conversational turn), the cascade starts a
     *  new round: per-member + iteration counters reset, work continues. The
     *  novelty signal is what lets agents collaborate via @-mentions WITHOUT
     *  tool calls (a normal discussion) across rounds instead of pausing for a
     *  user nudge every few turns; the echo/saturation detector keeps a true
     *  loop (no novel content) from continuing. The per-ROUND turn caps remain
     *  the runaway brake; this bound caps autonomous rounds per user message.
     *  Raised 3 -> 6 so a real collaboration runs to a natural stop. */
    static constexpr int kMaxAutoRounds = 6;

    /** Absolute safety backstop on autonomous rounds, applied even when a
     *  conversation sets maxAutoRounds = 0 (unbounded / "run until the team
     *  stagnates"). Normal stagnation (a round with no novel or tool progress)
     *  pauses the cascade far sooner; this only stops a pathological model
     *  that keeps emitting genuinely-new content forever from looping without
     *  bound. Also the clamp ceiling for an explicit per-conversation value. */
    static constexpr int kAutoRoundsHardCeiling = 50;

    /**
     * @brief Tokenizes a responder reply into a normalised word-set
     *        for similarity comparison. Strips @-mentions and
     *        punctuation, lower-cases, and skips short
     *        stop-token-class words (length < 3) to reduce noise from
     *        common articles and prepositions.
     *
     * PUBLIC because ToolDispatcher reuses the same fingerprinting
     * for its tool-chain echo breaker (an agent that repeats the
     * same narration + same tool call across continuation
     * iterations). Pure static utility with no CascadeController
     * state involved.
     * @param content Raw reply text to fingerprint.
     * @returns Normalised word-set; empty for empty/noise-only input.
     */
    static QSet<QString> contentFingerprint(const QString& content);

    /**
     * @brief Jaccard similarity |A ∩ B| / |A ∪ B| between two
     *        fingerprints. Public for the same ToolDispatcher reuse.
     * @param a First word-set.
     * @param b Second word-set.
     * @returns Similarity in [0, 1]; 0.0 when either side is empty
     *          (no comparable signal; see implementation rationale).
     */
    static double jaccardSimilarity(const QSet<QString>& a, const QSet<QString>& b);

    /**
     * @param convSvc  Non-owning; folderChain lookups + conversation
     *                 metadata.
     * @param router   Non-owning; used by configureBackend to
     *                 construct the RemoteBackend.
     * @param parent   Qt parent (ChatController).
     *
     * MembershipService is attached LATER via setMembershipService()
     * because AppController wires it after ChatController construction
     * (same pattern ChatController itself uses). The initial backend
     * is Ragp::StubBackend; configureBackend swaps in remote/local.
     */
    explicit CascadeController(ConversationService& convSvc,
                               ModelRouter& router,
                               QObject* parent = nullptr);
    ~CascadeController() override;

    /**
     * @brief Attach the membership service. Must be called before
     *        the first user message is sent to a group conversation.
     * @param membership Non-owning pointer; null is rejected before
     *                   group cascades fire (logged as a setup bug).
     */
    void setMembershipService(MembershipService* membership);

    // -----------------------------------------------------------------
    // Responder identity (was m_responseMemberAlias / ...AgentId)
    // -----------------------------------------------------------------

    /**
     * @brief Stamp the current responder identity. Set at sendMessage
     *        @-mention parse time and at every cascade dispatch.
     *        Cleared at turn end via resetTurnState.
     * @param alias   Responder alias (group member's display alias).
     * @param agentId Responder agent UUID (matches `agents.id`).
     */
    void setCurrentResponder(const QString& alias, const QString& agentId);

    /**
     * @brief The alias of the current responder.
     * @returns Alias string, or empty when no responder is set.
     */
    QString currentResponderAlias() const;

    /**
     * @brief The agent UUID of the current responder.
     * @returns Agent UUID, or empty when no responder is set.
     */
    QString currentResponderAgentId() const;

    // -----------------------------------------------------------------
    // Cascade queue (was m_agentCascadeQueue append)
    // -----------------------------------------------------------------

    /**
     * @brief Append an alias to the IMMEDIATE cascade queue with
     *        dedup + self-skip + cap-skip filtering. Drained FIRST
     *        by tryDispatchNext. Used by RAGP's applyClassification
     *        to queue routing targets identified by the classifier
     *        on the current responder's reply.
     * @param alias Routing target alias (case-insensitive).
     * @returns true iff the alias was added (false on dedup, self-
     *          skip, or cap-skip filtering).
     */
    bool enqueueTarget(const QString& alias);

    /**
     * @brief Append an alias to the DEFERRED cascade queue. The
     *        deferred queue runs AFTER the immediate queue is drained,
     *        once the classifier-driven sub-cascade has fully
     *        settled and there are no more routing targets to
     *        dispatch from the immediate queue. Used for
     *        user-pre-seeded mentions whose work semantically depends
     *        on the immediate sub-cascade completing. The classic
     *        example:
     *          User: "@Alice ask everyone's color, @Writer write their
     *                 answers to a file"
     *        → Alice = primary responder; Writer = deferred. Alice's
     *        reply drives an immediate cascade through the other
     *        agents who answer the colour question. Once that
     *        immediate cascade ends, Writer fires and sees the full
     *        Q&A in conversation history before doing its job.
     *
     *        Same dedup + self-skip + cap-skip semantics as
     *        enqueueTarget, AND deduplicates against the immediate
     *        queue too, since an alias the classifier already queued
     *        doesn't need a deferred slot.
     * @param alias Routing target alias (case-insensitive).
     * @returns true iff the alias was added.
     */
    bool enqueueDeferredTarget(const QString& alias);

    /**
     * @brief Current size of the immediate cascade queue.
     * @returns Number of pending routing targets in the immediate
     *          queue (does not include the deferred queue).
     */
    int queueSize() const;

    /**
     * @brief Whether BOTH the immediate AND deferred queues are
     *        empty.
     * @returns true iff there is nothing left to dispatch.
     */
    bool isQueueEmpty() const;

    /**
     * @brief Honour a `request_turn`-style yield after a tool batch.
     *
     *        Pop one queued target (immediate-queue first, deferred
     *        second) whose membership still resolves AND we're still
     *        under kMaxAgentCascade, increment the iteration counter,
     *        set the new responder, and emit `memberTurnStarted` so
     *        ChatController's slot opens streaming + rebuilds the
     *        request for that member. Returns true iff a turn was
     *        actually dispatched.
     *
     *        Designed to be called from
     *        `ChatController::onToolBatchCompleted` WHEN a
     *        `request_turn` was invoked in the just-finished batch.
     *        Without this path, an agent whose every turn finishes
     *        with finish=tool_calls (e.g. a coordinator chaining
     *        start_task / request_turn / start_poll / close_poll /
     *        request_turn / …) never yields, because the cascade-queue-drain
     *        path lives inside `routeOrFinalize`, which only runs
     *        when finish != tool_calls.
     *
     *        Same body as the private `tryDispatchNext`; this is the
     *        public entry-point for the tool-batch yield path.
     * @param convId The in-flight conversation id (passed through to
     *               membership lookups for the popped target).
     * @returns true iff a turn was actually dispatched.
     */
    bool yieldToQueuedAfterToolBatch(const QString& convId);

    // -----------------------------------------------------------------
    // Turn counters (read-only externally)
    // -----------------------------------------------------------------

    /**
     * @brief Current total number of cascade turns dispatched within
     *        the active cascade (bounded by kMaxAgentCascade).
     * @returns Iteration counter; 0 outside any active cascade.
     */
    int cascadeIterations() const;

    /**
     * @brief Number of turns the given member has taken in the
     *        active cascade (bounded by kMaxTurnsPerMember).
     * @param alias Member alias (case-insensitive lookup).
     * @returns Per-member turn count; 0 for any alias that has not
     *          yet been dispatched within the active cascade.
     */
    int memberTurnCount(const QString& alias) const;

    // -----------------------------------------------------------------
    // RAGP backend management
    // -----------------------------------------------------------------

    /**
     * @brief The current backend's human-readable identifier.
     *        Exposed so ChatController's ragpBackendName() delegates
     *        here.
     * @returns One of `"stub"`, `"remote:ollama"`, `"local:<file>"`,
     *          or `"(none)"` if no backend is set.
     */
    QString ragpBackendName() const;

    /**
     * @brief Access to the underlying Ragp::Service.
     *
     * Primary consumer is the integration test suite, which uses
     * this to inject scripted IRagpBackend implementations and
     * pin the tier-1/tier-2/tier-3 classification flow. Production
     * code goes through configureBackend() instead.
     *
     * @return Non-owning pointer; the service is owned by this
     *         collaborator and must not be deleted through the
     *         returned pointer.
     */
    Ragp::Service* ragpService() const;

    /**
     * @brief Configure the Tier 3 RAGP backend from SettingsService.
     *        Decides between local llama.cpp and remote Ollama per
     *        the user's setting + availability of the local model
     *        file, then atomically replaces Ragp::Service's backend
     *        (the Tier-2 cache is cleared by setBackend).
     *
     *        Emits ragpBackendChanged(name) when the swap completes.
     *        LocalLlamaBackend::loadFailed is wired to a
     *        QTimer::singleShot deferred fallback to RemoteBackend
     *        (see file header for the race-safety rationale).
     * @param settings Live settings service (used both immediately
     *                 and captured as a QPointer for the deferred-
     *                 fallback path).
     */
    void configureBackend(SettingsService& settings);

    /**
     * @brief Attaches the inference-sidecar host the local RAGP
     *        bridge backend rides through. Non-owning; AppController-
     *        owned and destroyed after every consumer. Call before
     *        configureBackend() selects the local engine.
     * @param host Sidecar host; nullptr keeps local selection but the
     *             backend's warm fails into the remote auto-fallback.
     */
    void setInferenceSidecarHost(Verzeta::Infer::InferenceSidecarHost* host) {
        m_inferenceHost = host;
    }

    // -----------------------------------------------------------------
    // Turn-state lifecycle
    // -----------------------------------------------------------------

    /**
     * @brief Clear queue + per-member turns + iterations + responder.
     *        Called on new user turn, stopGeneration, onRequestError,
     *        and retry. Does NOT clear the cross-cascade fingerprint
     *        window; only clearCrossCascadeFingerprints does that.
     */
    void resetTurnState();

    /**
     * @brief Single entry point for cascade routing at the end of
     *        onRequestFinished. The controller decides (based on
     *        isGroupChat + content non-emptiness + RAGP pipeline)
     *        to either dispatch another member turn
     *        (memberTurnStarted signal) or complete the cascade
     *        (cascadeComplete signal with the passthrough fields).
     *
     *        Handles BOTH the sync-Tier-1/2 path and the async-Tier-3
     *        QFutureWatcher deferral internally. ChatController never
     *        sees the intermediate classification.
     * @param inputs Per-turn snapshot built by ChatController at the
     *               end of onRequestFinished's persistence +
     *               sanitisation block.
     */
    void routeOrFinalize(const CascadeRouteInputs& inputs);

    /**
     * @brief Installs (or clears) the voice pacing gate.
     *
     * The gate is consulted at the top of routeOrFinalize and nowhere
     * else. It is NULL unless a voice call is wired in, and a null gate
     * makes routeOrFinalize behave exactly as it does today.
     * @param gate The gate, or nullptr to remove it. Not owned; must
     *             outlive this controller or be cleared first.
     */
    void setVoiceTurnGate(IVoiceTurnGate* gate);

    /**
     * @brief Resumes a turn that the voice gate held.
     * @param msgId The message whose speech finished. Ignored when it
     *              does not match the held turn (a late or duplicate
     *              release is harmless).
     */
    void releaseVoiceHold(const QString& msgId);

    /**
     * @brief Force-end the cascade with a deterministic finishReason,
     *        bypassing all echo-retry, saturation and classification
     *        logic in `routeOrFinalize`. Used when ChatController
     *        hits a hard kill-switch (e.g. `kMaxToolIterations` cap)
     *        and the cascade MUST end cleanly regardless of the
     *        responder's content shape.
     *
     *        Without this path, the kMaxToolIterations cap was a
     *        soft-kill: after the cap, `routeOrFinalize` would treat
     *        an empty-content tool_calls response as an "echo / empty
     *        reply" and emit `echoDetectedRequestRetry`, which
     *        re-dispatched the same agent after calling
     *        `ToolDispatcher::resetState()` (zeroing m_iterations).
     *        The cap was effectively bypassed, and if the LLM kept
     *        returning empty + tool_calls, the cascade could hang
     *        indefinitely.
     *
     *        Emits `cascadeComplete` with the supplied
     *        `inputs.finishReason`; clears cascade queue + member-
     *        turn counters + responder; same downstream contract as
     *        the natural `routeOrFinalize` completion path.
     * @param inputs Per-turn snapshot (only the passthrough fields
     *               finishReason, totalTokens, elapsedMs and
     *               declaredTaskStatus are read; the rest are
     *               accepted for signature parity with
     *               routeOrFinalize).
     * @sideeffects Emits `cascadeComplete`; clears `m_queue`,
     *              `m_deferredQueue`, `m_memberTurns`, `m_memberRetryCount`,
     *              `m_responderAlias`, `m_responderAgentId`,
     *              `m_iterations`, `m_recentContentFingerprints`
     *              (via `emitCascadeCompleteAndClear`).
     */
    void forceCascadeComplete(const CascadeRouteInputs& inputs);

    /**
     * @brief Clear the cross-cascade self-echo window
     *        (`m_priorCascadeFingerprints`).
     *
     *        Must be called by ChatController on any boundary where
     *        the cross-cascade window MUST NOT carry over:
     *          - Conversation switch (cross-conv fingerprint pollution
     *            would otherwise be possible since CascadeController
     *            doesn't track convId per-fingerprint).
     *          - stopGeneration (user interrupt, fresh slate semantic).
     *
     *        This is INTENTIONALLY a separate entry point from
     *        `resetTurnState`: the latter fires at every cascade
     *        boundary (each user message ends one cascade, the next
     *        starts a new one) and MUST NOT clear the cross-cascade
     *        window, since that would defeat its purpose. Only explicit
     *        cross-cascade-boundary signals (conv switch, interrupt)
     *        warrant clearing.
     */
    void clearCrossCascadeFingerprints();

    /**
     * @brief Round-continuation progress feed: called by
     *        ChatController whenever a tool batch completes with at
     *        least one real (named) call. Progress is the gate that
     *        lets a cap-ended round continue autonomously. A
     *        narration-only round does NOT earn continuation (the
     *        corrective-retry counter is the second gate; see
     *        kMaxAutoRounds docs).
     */
    void noteToolFireThisRound() { ++m_roundToolFires; }

    /**
     * @brief Round-continuation progress feed for CORRECTIVE work: called
     *        by ConversationRun when a confirmed deferred-action
     *        continuation is dispatched for a group responder. A
     *        continuation that converts a narrated action into real work
     *        is progress in the same sense as an echo-retry, so the round
     *        must not pause mid-correction.
     */
    void noteCorrectiveContinuationThisRound() { ++m_roundRetries; }

    /* Exposed publicly for unit tests (pure predicate — no state
     * beyond the seated responder alias). */
    /**
     * @brief Whether a classify result should trigger the group
     *        deferred-action continuation: the verdict says self-kept
     *        pending work, confidence is usable, and the responder's
     *        continuation budget remains. Routing precedence is NOT
     *        checked here. routeOrFinalize consults this predicate
     *        only AFTER tryDispatchNext dispatched nobody (real
     *        routing always wins by call order).
     * @param result Classification to evaluate.
     * @param inputs The turn's route inputs (budget snapshot).
     * @returns true when deferredContinuationRequested should fire.
     */
    bool shouldRequestDeferredContinuation(const Ragp::Classification& result,
                                           const CascadeRouteInputs& inputs) const;

  signals:
    /** Dispatch a fresh cascade member turn. ChatController opens a
     *  streaming placeholder and defers buildAndSendRequest. */
    void memberTurnStarted(const QString& alias, const QString& agentId);

    /**
     * @brief Cascade is drained. ChatController runs active-plan
     *        state transitions + turn-end cleanup (streaming reset,
     *        tool reset, inflight clear, isGenerating=false).
     *        Cascade-internal state has ALREADY been cleared by the
     *        time this fires.
     * @param declaredTaskStatus The captured `\<task_state\>` marker
     *                             value (only populated when an
     *                             active plan was anchored; see
     *                             ContentSanitizer stage 6).
     * @param finishReason         The terminal LLM finish reason
     *                             (`stop`, `length`, `tool_calls`,
     *                             `error`, etc.).
     * @param totalTokens          Aggregated token count for the
     *                             cascade's final turn.
     * @param elapsedMs            Wall-clock latency for the cascade
     *                             in milliseconds.
     * @param roundTurns           Dispatched agent turns in the final
     *                             round (drives the visible
     *                             round-settled note; a natural
     *                             multi-turn settle was previously
     *                             indistinguishable from a stall in
     *                             the UI).
     * @param pauseNoted           True when cascadePausedAtCap already
     *                             surfaced this ending (no second
     *                             note).
     */
    void cascadeComplete(const QString& declaredTaskStatus,
                         const QString& finishReason,
                         int totalTokens,
                         qint64 elapsedMs,
                         int roundTurns = 0,
                         bool pauseNoted = false);

    /**
     * @brief User was mentioned in the responder's reply.
     *        Signal-to-signal forwarded onto
     *        ChatController::userMentionedInGroup.
     * @param conversationId Conversation UUID.
     * @param agentAlias     Alias of the agent that produced the
     *                       mention.
     * @param messageText    The full text of the message containing
     *                       the @-user mention.
     */
    void userMentionedInGroup(const QString& conversationId,
                              const QString& agentAlias,
                              const QString& messageText);

    /**
     * @brief A new autonomous round started after the previous round
     *        hit its turn caps with work still queued. ChatController
     *        inserts the visible "continuing" system note.
     * @param conversationId Conversation UUID.
     * @param round          1-based round number just started (2..N).
     * @param nextAlias      The cap-refused alias now being served.
     */
    void roundContinued(const QString& conversationId, int round, const QString& nextAlias);

    /**
     * @brief The cascade ended at its turn caps WITHOUT continuation
     *        (no progress this round, or kMaxAutoRounds exhausted)
     *        while a classified target was still waiting.
     *        ChatController inserts the visible "paused" system note
     *        so the stall is NEVER silent.
     * @param conversationId Conversation UUID.
     * @param refusedAlias   The alias that was refused at the cap.
     * @param reason         "no_progress" | "rounds_exhausted".
     */
    void cascadePausedAtCap(const QString& conversationId,
                            const QString& refusedAlias,
                            const QString& reason);

    /**
     * @brief Classified @-mention targets were present but dropped
     *        below the dispatch confidence floor, so routing was
     *        suppressed. Forwarded through ChatController so the UI
     *        can show WHY a group chat went quiet instead of
     *        stalling silently.
     * @param conversationId Conversation UUID.
     * @param responderAlias Alias whose reply contained the mentions.
     * @param targetAliases  The would-have-been dispatch targets.
     * @param confidence     Classifier confidence that fell below the
     *                       floor.
     * @param source         Classifier source tag, for example
     *                       "rule-fallback-after:remote:openai", so a
     *                       reader can tell WHY confidence was low:
     *                       the backend being down is a different
     *                       problem from a genuinely ambiguous reply.
     */
    void mentionRoutingSuppressed(const QString& conversationId,
                                  const QString& responderAlias,
                                  const QStringList& targetAliases,
                                  double confidence,
                                  const QString& source);

    /**
     * @brief RAGP backend swapped. Signal-to-signal forwarded onto
     *        ChatController::ragpBackendChanged.
     * @param backendName The new backend's identifier (matches
     *                    ragpBackendName()).
     */
    void ragpBackendChanged(const QString& backendName);

    /**
     * @brief Responder's reply was flagged as a near-verbatim copy
     *        of a recent reply (Jaccard >= kCascadeNoveltyThreshold)
     *        OR was empty. The cascade has not yet exhausted the
     *        responder's retry budget. ChatController's slot deletes
     *        `badMsgId` from the message store, opens a fresh
     *        streaming placeholder for the same alias/agent, and
     *        re-dispatches buildAndSendRequest with `retryAttempt`
     *        propagated into BuildRequestInputs so the request
     *        builder can inject a progressively stronger anti-echo
     *        nudge. The cascade ITERATION counter is NOT advanced by
     *        a retry; the same `m_iterations` slot serves the retry.
     * @param convId       In-flight conversation UUID.
     * @param badMsgId     UUID of the offending message row to
     *                     delete before re-dispatch (may be empty
     *                     when the reply was empty and never
     *                     persisted).
     * @param alias        Responder alias to re-dispatch.
     * @param agentId      Responder agent UUID to re-dispatch.
     * @param retryAttempt 1-based retry counter (1, 2, 3 on
     *                     successive retries; feeds the anti-echo
     *                     prompt nudge intensity).
     * @param reason       Bad-reply class: "echo" (near-verbatim copy)
     *                     or "empty" (no content). Selects which
     *                     corrective nudge RequestBuilder appends on
     *                     the retry. (The former "claims_no_fire" class
     *                     was retired; announced-but-undone actions now
     *                     route through the NON-DESTRUCTIVE
     *                     deferred-action continuation instead of a
     *                     delete-retry.)
     */
    void echoDetectedRequestRetry(const QString& convId,
                                  const QString& badMsgId,
                                  const QString& alias,
                                  const QString& agentId,
                                  int retryAttempt,
                                  const QString& reason);

    /**
     * @brief The classify verdict says the responder announced a
     *        concrete action they kept for THEMSELVES and did not
     *        execute (Classification::selfPendingAction).
     *        Emitted only when NO member was actually dispatched this
     *        turn (real routing always wins, because routeOrFinalize tries
     *        tryDispatchNext first), no user route, confidence ≥ 0.5,
     *        and the responder's continuation budget is not
     *        exhausted. The consumer (ConversationRun) runs the
     *        non-destructive continuation, or force-completes the
     *        cascade if a user message preempted meanwhile. The
     *        cascade iteration index does NOT advance (continuations
     *        are corrections, like retries).
     * @param convId  Conversation the verdict belongs to.
     * @param alias   Seated responder alias.
     * @param agentId Responder agent template id.
     * @param hint    Classifier's one-line pending-action description.
     */
    void deferredContinuationRequested(const QString& convId,
                                       const QString& alias,
                                       const QString& agentId,
                                       const QString& hint);

    /**
     * @brief Responder exhausted kMaxEchoRetries attempts and the
     *        final reply was still flagged as echo or empty.
     *        ChatController's slot decides what to keep: empty
     *        content → delete the row; non-empty content → mark the
     *        row's metadata so the UI can render an "echo-failed"
     *        badge but the user can still see what was produced.
     *        After the slot runs, the cascade has ALREADY advanced
     *        via tryDispatchNext (or completed via cascadeComplete).
     *        This signal is purely for the bad-reply bookkeeping.
     * @param convId    In-flight conversation UUID.
     * @param badMsgId  UUID of the final exhausted-retry message
     *                  row (may be empty when the reply was empty).
     * @param wasEmpty  true iff the final retry's reply was
     *                  content-empty; false when it was non-empty
     *                  but still echoed.
     */
    void echoMaxRetriesExhausted(const QString& convId, const QString& badMsgId, bool wasEmpty);

  private:
    /** Apply a RAGP classification to the cascade queue. Same body as
     *  the pre-extraction ChatController::applyRagpClassification,
     *  minus reads of m_streaming (uses capturedContent explicitly). */
    void applyClassification(const Ragp::Classification& result,
                             const QList<Member>& members,
                             bool isFirstResponder,
                             const QString& capturedContent,
                             const QString& convId);


    /** Pop one member off the queue, resolve it via MembershipService,
     *  increment the cascade iteration counter, set the new responder,
     *  and emit memberTurnStarted. Returns true if a turn was
     *  dispatched, false if queue empty / cap hit / no resolvable
     *  member. Mirrors ChatController::dispatchNextCascadeMember. */
    bool tryDispatchNext(const QString& convId);

    /** Dispatch the classify verdict's DELEGATE (pending_action
     *  who="other") when no classified target dispatched. This is the
     *  handoff arm of the LLM-decided continuation. Consulted by
     *  routeOrFinalize strictly AFTER tryDispatchNext returned false
     *  (real routing wins by call order, mirroring the self-pending
     *  nudge). Enqueues through enqueueTarget, so the per-member cap
     *  (with its visible round-continuation/pause machinery), dedup
     *  and self-skip all apply unchanged. Returns true when a turn
     *  was dispatched. */
    bool maybeDispatchDelegate(const Ragp::Classification& result,
                               const QList<Member>& members,
                               const QString& convId);

    /** Terminal-turn visibility for classified-but-undispatched
     *  targets: when the cascade is about to complete while the
     *  classify result still holds targets (every one was skipped by
     *  the intent gate / self-skip / resolution), log the per-target
     *  alias+intent detail and emit mentionRoutingSuppressed so the
     *  turn never ends silently with unconsumed routing information
     *  (such a dead-end previously ended with no log output at all).
     *  Cap-refused targets are excluded, because the round-continuation
     *  machinery already surfaces those. */
    void noteUndispatchedTargets(const Ragp::Classification& result, const QString& convId);

    /** Clear cascade-internal state (queue, member turns, iterations,
     *  responder) and emit cascadeComplete with the supplied
     *  passthrough fields. */
    void emitCascadeCompleteAndClear(const CascadeRouteInputs& inputs);

    ConversationService& m_convSvc;  // non-owning
    MembershipService* m_membership = nullptr;
    ModelRouter& m_router;  // non-owning
    /** Non-owning sidecar host for the local RAGP bridge (may be null). */
    Verzeta::Infer::InferenceSidecarHost* m_inferenceHost = nullptr;

    std::unique_ptr<Ragp::Service> m_ragpService;

    /** IMMEDIATE cascade queue, drained FIFO before the deferred
     *  queue. Populated by RAGP's classifier-driven applyClassification
     *  and (historically) by sendMessage's multi-mention parse.
     *  sendMessage's secondary mentions are now rewired to the
     *  deferred queue instead. */
    /** Voice pacing gate; NULL in every run without a live voice call.
     *  Consulted only at the top of routeOrFinalize. */
    IVoiceTurnGate* m_voiceGate = nullptr;
    /** Turn held by the gate, replayed verbatim on release. */
    CascadeRouteInputs m_voiceHeldInputs;
    /** True while m_voiceHeldInputs is waiting to be replayed. */
    bool m_voiceHoldActive = false;
    /** Safety deadline so a release that never arrives cannot wedge the
     *  cascade; single-shot, cancelled by the real release. */
    QTimer* m_voiceHoldDeadline = nullptr;
    /** Replies already held once. A turn is paced AT MOST ONCE, so the
     *  replay after a release cannot re-hold it and spin. Bounded. */
    QSet<QString> m_voiceReleasedMsgIds;

    QStringList m_queue;

    /** DEFERRED cascade queue, drained AFTER m_queue is empty AND
     *  no more classifier targets are arriving. Populated by
     *  enqueueDeferredTarget (currently only sendMessage's secondary
     *  mentions). When tryDispatchNext finds m_queue empty, it pops
     *  one alias from m_deferredQueue, sets it as the new responder,
     *  and starts a fresh sub-cascade for that member; if THAT
     *  sub-cascade adds more immediate targets they drain BEFORE the
     *  next deferred member fires. */
    QStringList m_deferredQueue;

    QMap<QString, int> m_memberTurns;
    int m_iterations = 0;
    QString m_responderAlias;
    QString m_responderAgentId;

    /** Sliding window of the last kCascadeFingerprintWindow responder-
     *  content fingerprints (token-set hashes) within the current
     *  cascade. Cleared by resetTurnState(). Used by the saturation
     *  detector to catch loops where agents paraphrase or echo each
     *  other's substance. RAGP's per-turn classifier cannot see
     *  conversation-level repetition because each classify call sees
     *  only the latest reply in isolation. */
    QList<QSet<QString>> m_recentContentFingerprints;

    /** Cross-cascade self-echo window.
     *
     *  Fingerprints from cascades PRIOR to the current one within the
     *  same conversation. Motivated by a multi-provider stress run that
     *  produced 3 coordinator self-echo pairs (Jaccard 1.00), where the
     *  coordinator produced identical persona-intro
     *  content across two separate cascades within the same
     *  conversation. The within-cascade detector
     *  (m_recentContentFingerprints) correctly returned zero pairs
     *  for each individual cascade, but across cascades the agent
     *  repeats itself verbatim and the user-visible artifact is a
     *  runaway-feeling chat.
     *
     *  Lifecycle:
     *    - Populated at resetTurnState(): the just-finished cascade's
     *      m_recentContentFingerprints are appended here, then the
     *      list is trimmed to `kCrossCascadeFingerprintMaxEntries`
     *      (= K cascades × kCascadeFingerprintWindow entries; K=2 by
     *      design, a conservative bound that catches that pattern
     *      without over-triggering on legitimate role re-intros across
     *      3+ cascades).
     *    - Consulted by isCascadeSaturated() ALONGSIDE
     *      m_recentContentFingerprints. The two lists are disjoint by
     *      construction (within-cascade vs prior-cascades).
     *    - Cleared by clearCrossCascadeFingerprints(), which is called by
     *      ChatController on conversation switch and stopGeneration
     *      so cross-conv pollution is structurally impossible. */
    QList<QSet<QString>> m_priorCascadeFingerprints;

    /** Maximum entries in m_priorCascadeFingerprints. Sized as K *
     *  kCascadeFingerprintWindow where K=2 (the user's selected
     *  conservative window; see comment on m_priorCascadeFingerprints
     *  for the trade-off rationale). */
    static constexpr int kCrossCascadeFingerprintMaxEntries = 2 * kCascadeFingerprintWindow;

    /** Per-alias retry counter for the echo-recovery loop. Lowercase
     *  alias key → number of retries already consumed for THIS
     *  responder's CURRENT cascade slot. Reset to zero when the
     *  responder produces a successful (non-echoed, non-empty)
     *  reply, when retries are exhausted, or on resetTurnState. */
    QHash<QString, int> m_memberRetryCount;

    /** Round-continuation state (cleared by resetTurnState):
     *  the most recent alias refused by the per-member cap, the count
     *  of real tool fires this round (fed by ChatController via
     *  noteToolFireThisRound), corrective retries fired this round,
     *  and the current 1-based round number. */
    QString m_capRefusedAlias;
    int m_roundToolFires = 0;
    int m_roundRetries = 0;
    /** Accepted, non-echoed conversational turns this round. These count as
     *  progress so a tool-less collaboration round continues (see
     *  kMaxAutoRounds). Reset per round and per cascade. */
    int m_roundNovelTurns = 0;
    int m_round = 1;

    /** True iff `candidate` matches ANY fingerprint in
     *  m_recentContentFingerprints at >= kCascadeNoveltyThreshold. */
    bool isCascadeSaturated(const QSet<QString>& candidate) const;
};

}  // namespace Chat

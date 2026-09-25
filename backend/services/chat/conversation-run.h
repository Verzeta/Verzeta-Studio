// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file conversation-run.h
 * @brief Owns the complete per-turn execution state and send →
 *        stream → tools → cascade → finalize pipeline for one
 *        conversation's in-flight turn. Extracted out of the
 *        ChatController god class so per-turn state lives in one
 *        cohesive unit rather than as flat members on the QML facade.
 * @layer Service (Chat subsystem)
 * @dependencies ModelRouter, ConversationService, MessageService,
 *               MessageListModel, Chat::RequestBuilder,
 *               Chat::StreamingManager, Chat::ToolDispatcher,
 *               Chat::CascadeController, Chat::ContentSanitizer, plus
 *               optional services (ToolService, RagService,
 *               AgentService, FileService, AgentRegistry,
 *               MembershipService, PlanService, TaskRunner,
 *               TaskObserver, TaskGateService, SlashCommandService,
 *               CanvasService, AuditService, SessionRouter,
 *               ConversationSummarizer, SkillService,
 *               HeartbeatConfigService, PollService) attached via
 *               set*() forwarders after construction.
 *
 * Architecture: ChatController owns exactly one ConversationRun via
 * std::unique_ptr (with ChatController as the QObject parent). The run
 * owns the per-turn collaborators (RequestBuilder, StreamingManager,
 * ToolDispatcher, CascadeController, ContentSanitizer) and carries all
 * the per-turn scalars (request id, inflight conv id, dispatched
 * provider/model, queued-user state, retry state, context-fill, the
 * in-flight generating flag, …). ChatController keeps the stable
 * QML-facing Q_PROPERTY / Q_INVOKABLE / signal surface and DELEGATES
 * to the run; the run emits change signals that ChatController
 * re-emits so QML behaviour is unchanged.
 *
 * Threading: strictly main-thread. Every public entry point begins
 * with VERZETA_ASSERT_MAIN_THREAD(). The desired concurrency model is
 * concurrent async I/O on the main thread (Qt event loop + per-provider
 * network access managers), not worker threads, so there are no data
 * races to guard.
 */


#pragma once

#include "../../api/llm-interface.h"
#include "../agent-service.h"  // AgentPattern enum, AgentConfig
#include "../task-runner.h"    // TaskDispatchRequest value member

#include <memory>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// Forward declarations — shared services attached via the constructor
// or set*() forwarders. ConversationRun holds non-owning pointers to
// them (AppController owns the real instances).
class AgentRegistry;
class AgentService;
class AuditService;
class CanvasService;
class ConversationService;
class ConversationSummarizer;
class FileService;
class HeartbeatConfigService;
class MembershipService;
class Message;
class MessageService;
class MessageListModel;
class ModelRouter;
class PlanService;
class PollService;
class RagService;
struct RagChunk;
class SettingsService;
class SkillService;
class SlashCommandService;
class TaskGateService;
class TaskObserver;
class TaskRunner;
class ToolService;

namespace Verzeta::Session {
class SessionRouter;
}

namespace Ragp {
class Service;
}

namespace Verzeta::Infer {
class InferenceSidecarHost;
}

namespace Chat {

class RequestBuilder;
struct BuildRequestInputs;
struct BuildResult;
class StreamingManager;
class ToolDispatcher;
class CascadeController;
struct CascadeRouteInputs;
class ContentSanitizer;
class ProviderScheduler;
class MemoryRetriever;
class ActionIntentConfirmer;

/**
 * @brief The complete chat engine for one conversation's turns.
 *
 * Owns the per-turn collaborators and state; runs the send →
 * stream → tools → cascade → finalize lifecycle. Emits change
 * signals that the parent ChatController re-emits onto its QML
 * facade, so the run carries no Q_PROPERTY / Q_INVOKABLE of its own.
 *
 * Lifecycle: constructed in ChatController's constructor and destroyed
 * before the shared services it references (reverse-declaration order
 * keeps the dependency invariant). Currently a single run represents
 * the active/in-flight conversation.
 */
class ConversationRun : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the run, creates its per-turn collaborators,
     *        and wires all of their signal/slot connections plus the
     *        ModelRouter streaming relay.
     * @param router    Shared ModelRouter (non-owning reference).
     * @param convSvc   ConversationService (non-owning reference).
     * @param msgSvc    MessageService (non-owning reference).
     * @param msgModel  The QML-facing MessageListModel owned by
     *                  ChatController (non-owning pointer).
     * @param parent    Qt parent (ChatController).
     */
    explicit ConversationRun(ModelRouter& router,
                             ConversationService& convSvc,
                             MessageService& msgSvc,
                             MessageListModel* msgModel,
                             QObject* parent = nullptr);
    ~ConversationRun() override;

    // -----------------------------------------------------------------
    // Optional-service attachment (forwarded by ChatController setters).
    // Each takes a non-owning pointer; passing nullptr detaches.
    // -----------------------------------------------------------------

    /**
     * @brief Attaches the tool-calling service.
     * @param svc Non-owning; null disables tools.
     */
    void setToolService(ToolService* svc);
    /**
     * @brief Attaches the RAG service.
     * @param svc Non-owning; null disables RAG.
     */
    void setRagService(RagService* svc);
    /**
     * @brief Attaches the unified pre-turn memory-recall coordinator. When set,
     *        the turn's recall (RAG + AIM + ACN, per their gates) runs through it
     *        instead of RAG-only prefetch. Non-owning; null disables pre-turn
     *        recall entirely (turns dispatch un-augmented).
     * @param r Non-owning app-level coordinator; null detaches.
     */
    void setMemoryRetriever(MemoryRetriever* r);
    /**
     * @brief Attaches the file service.
     * @param svc Non-owning; null detaches.
     */
    void setFileService(FileService* svc);
    /**
     * @brief Attaches the agent registry.
     * @param registry Non-owning; null detaches.
     */
    void setAgentRegistry(AgentRegistry* registry);
    /**
     * @brief Attaches the membership service (also forwarded to the cascade).
     * @param svc Non-owning; null detaches.
     */
    void setMembershipService(MembershipService* svc);
    /**
     * @brief Attaches the plan service.
     * @param svc Non-owning; null detaches.
     */
    void setPlanService(PlanService* svc);
    /**
     * @brief Attaches the task runner.
     * @param runner Non-owning; null detaches.
     */
    void setTaskRunner(TaskRunner* runner);
    /**
     * @brief Attaches the passive task observer.
     * @param observer Non-owning; null detaches.
     */
    void setTaskObserver(TaskObserver* observer);
    /**
     * @brief Attaches the skill service.
     * @param svc Non-owning; null disables skills.
     */
    void setSkillService(SkillService* svc);
    /**
     * @brief Attaches the heartbeat-config service.
     * @param svc Non-owning; null detaches.
     */
    void setHeartbeatConfigService(HeartbeatConfigService* svc);
    /**
     * @brief Attaches the poll service.
     * @param svc Non-owning; null detaches.
     */
    void setPollService(PollService* svc);
    /**
     * @brief Attaches the audit service (also forwarded to the tool dispatcher).
     * @param svc Non-owning; null detaches.
     */
    void setAuditService(AuditService* svc);
    /**
     * @brief Attaches the cross-instance session router.
     * @param router Non-owning; null disables cross-instance coordination.
     */
    void setSessionRouter(Verzeta::Session::SessionRouter* router);
    /**
     * @brief Attaches the task-gate service (active-plan anchor).
     * @param svc Non-owning; null detaches.
     */
    void setTaskGateService(TaskGateService* svc);
    /**
     * @brief Attaches the slash-command service used by sendMessage.
     * @param svc Non-owning; null detaches.
     */
    void setSlashCommandService(SlashCommandService* svc);
    /**
     * @brief Attaches the canvas service for the submit_result auto-promote path.
     * @param svc Non-owning; null disables auto-promote.
     */
    void setCanvasService(CanvasService* svc);
    /**
     * @brief Attaches the agent service and wires its chunk/finish/error signals.
     * @param agentService Non-owning; null disables agent patterns.
     */
    void setAgentService(AgentService* agentService);
    /**
     * @brief Attaches the compaction summarizer and mirrors its busy state.
     * @param summarizer Non-owning; null detaches.
     */
    void setSummarizer(ConversationSummarizer* summarizer);

    /**
     * @brief Attaches the app-level per-provider dispatch scheduler.
     *
     * The scheduler is the single point of cross-conversation /
     * cross-session provider serialization. With it
     * attached, each turn acquires the target provider's slot before
     * dispatch and releases it on the turn's terminal signal. Turns
     * to different providers run in parallel, turns to the same
     * provider serialize. With it null, the run dispatches immediately
     * (used by the legacy single-instance tests that never set one).
     *
     * @param scheduler Non-owning; AppController owns the real instance.
     *                  Shared by every run across every ChatController.
     */
    void setProviderScheduler(Chat::ProviderScheduler* scheduler);

    /**
     * @brief Forwards the inference-sidecar host to this run's
     *        CascadeController (local RAGP bridge engine). Mirrors
     *        setProviderScheduler's wiring pattern.
     * @param host Non-owning sidecar host (may be null).
     */
    void setInferenceSidecarHost(Verzeta::Infer::InferenceSidecarHost* host);

    // -----------------------------------------------------------------
    // Active-conversation view state (the run is the source of truth;
    // ChatController delegates its activeConversationId/Title accessors
    // here). Currently the single run represents the active conversation.
    // -----------------------------------------------------------------

    /**
     * @brief The active conversation UUID.
     * @returns Conversation UUID, or empty string when none is active.
     */
    QString activeConvId() const { return m_activeConvId; }

    /**
     * @brief The active conversation display title.
     * @returns Cached display title, or empty string when none is active.
     */
    QString activeConvTitle() const { return m_activeConvTitle; }

    /**
     * @brief Switches the active conversation (loads its messages,
     *        restores its provider/model for the UI selector, keeps any
     *        in-flight cascade on its original conv).
     * @param id Conversation UUID, or empty to clear.
     */
    void switchConversation(const QString& id);

    /**
     * @brief Refreshes the cached active-conversation title from the
     *        DB when the active conversation's row was updated
     *        elsewhere (rename, model change).
     * @param id Conversation UUID that was updated.
     * @returns true iff the updated conversation is the active one (so
     *          the caller re-emits the facade signals).
     */
    bool onConversationUpdated(const QString& id);

    // -----------------------------------------------------------------
    // Turn entry points (delegated from ChatController's QML surface).
    // -----------------------------------------------------------------

    /**
     * @brief Sends a user message and starts the turn (slash short-
     *        circuit, queue-if-busy, \@mention routing, persist, stream).
     * @param text The user's message text; empty/whitespace is a no-op.
     */
    void sendMessage(const QString& text);

    /**
     * @brief Sends a message with inline file attachment content.
     * @param text            User message text.
     * @param attachmentPaths Absolute file paths; image bytes are
     *                        base64-encoded for vision, text files are
     *                        appended to the LLM context only.
     */
    void sendMessageWithAttachments(const QString& text, const QStringList& attachmentPaths);

    /** @brief Cancels the current generation, or discards a queued send if nothing is in flight. */
    void stopGeneration();

    /** @brief Retries the last user message by deleting it and re-sending the same text. */
    void retryLastMessage();

    /** @brief Manually triggers compaction of the active conversation (same semantics as /compact).
     */
    void compactNow();

    // -----------------------------------------------------------------
    // Cross-collaborator coordination (called by ChatController slots
    // wired to external services).
    // -----------------------------------------------------------------

    /**
     * @brief Queues a task-gated dispatch and drains it when idle.
     * @param req Dispatch request from TaskRunner.
     */
    void enqueueTaskDispatch(const TaskDispatchRequest& req);

    /** @brief Drains the (legacy) task-dispatch queue; logs and clears any stragglers. */
    void drainTaskDispatchQueue();

    /**
     * @brief Dispatches (or queues) a requester agent's reaction to a delivered sub-agent report.
     * @param convId         Parent conversation UUID.
     * @param requesterAlias Alias of the agent that spawned the run; empty falls back to the
     * primary agent.
     */
    void dispatchSubagentReaction(const QString& convId, const QString& requesterAlias);

    /** @brief Fires the oldest queued sub-agent reaction once no generation is in flight. */
    void drainPendingSubagentReactions();

    /** @brief Re-attempts the queued user send if the global ModelRouter slot is now free. */
    void drainQueuedSendIfPossible();

    /** @brief SessionRouter slotAvailable entry point: drains the queued user send then any queued
     * reactions. */
    void onSlotAvailable();

    /**
     * @brief Refreshes the active-canvas metadata cache used by the prompt builder.
     * @param filename   Canvas filename.
     * @param language   Canvas language tag.
     * @param revision   Current revision number.
     * @param lineCount  Total line count.
     * @param byteSize   Total byte size.
     */
    void onActiveCanvasMetadataRefresh(const QString& filename,
                                       const QString& language,
                                       int revision,
                                       int lineCount,
                                       qint64 byteSize);

    /**
     * @brief Mirrors the agent-pattern setting into the per-turn cache (gated while generating).
     * @param patternName Canonical pattern name from AgentSettings.
     */
    void onExternalAgentPatternChanged(const QString& patternName);

    /**
     * @brief Mirrors the require-confirmation toggle (gated while generating).
     * @param require New flag value.
     */
    void onExternalRequireConfirmationChanged(bool require);

    /**
     * @brief Mirrors the tools-enabled toggle (gated while generating).
     * @param enabled New flag value.
     */
    void onExternalToolsEnabledChanged(bool enabled);

    /**
     * @brief Coordinates a newly-created task in the active, idle conversation.
     * @param planId       Plan UUID just created.
     * @param convId       Conversation UUID the task lives in.
     * @param ownerAlias   Owner agent's alias.
     * @param ownerAgentId Owner agent template id.
     */
    void onExternalTaskStarted(const QString& planId,
                               const QString& convId,
                               const QString& ownerAlias,
                               const QString& ownerAgentId);

    /**
     * @brief Stops generation + clears the anchor when the active plan was stopped.
     * @param planId Plan UUID that was stopped.
     */
    void onExternalPlanStopped(const QString& planId);

    /**
     * @brief Aborts the in-flight cascade and/or clears the UI plan anchor when all of a
     * conversation's plans were stopped.
     * @param convId Conversation whose plans were all stopped.
     */
    void onExternalAllPlansStoppedInConversation(const QString& convId);

    /**
     * @brief Cancels any in-flight request targeting a conversation about to be deleted.
     * @param id Conversation UUID about to be deleted.
     */
    void onExternalConversationAboutToBeDeleted(const QString& id);

    /**
     * @brief Clears the active-session state when the active conversation was deleted.
     * @param id Conversation UUID that was deleted.
     */
    void onExternalConversationDeleted(const QString& id);

    /**
     * @brief Posts a plan artifact as an assistant message and (optionally) auto-promotes it to
     * canvas.
     * @param convId          Conversation UUID.
     * @param agentId         Producing agent template id.
     * @param alias           Producing agent's alias.
     * @param artifactContent Full artifact text.
     * @param summary         Short artifact summary.
     * @param stepId          Owning plan step id.
     * @param planId          Owning plan id.
     */
    void onTaskArtifactReady(const QString& convId,
                             const QString& agentId,
                             const QString& alias,
                             const QString& artifactContent,
                             const QString& summary,
                             const QString& stepId,
                             const QString& planId);

    // -----------------------------------------------------------------
    // State reads (delegated from ChatController's Q_PROPERTY getters
    // and C++-only accessors).
    // -----------------------------------------------------------------

    /**
     * @brief Active-conv-aware generation state (drives QML isGenerating).
     * @returns True iff the in-flight request belongs to the active conversation.
     */
    bool isGenerating() const;

    /**
     * @brief Raw per-instance generating flag (unmasked by active view).
     * @returns True while any cascade is in flight on this run.
     */
    bool isAnyConvGenerating() const { return m_isGenerating; }

    /**
     * @brief Whether a given conversation is the active one and generating.
     * @param convId Conversation UUID to test.
     * @returns True iff convId is the active conversation AND generating.
     */
    bool isConversationGenerating(const QString& convId) const;

    /**
     * @brief Whether a given conversation is the one actually mid-stream.
     * @param convId Conversation UUID to test.
     * @returns True iff convId equals the in-flight conversation id.
     */
    bool hasInflightStreamFor(const QString& convId) const;

    /**
     * @brief Context-window fill measured at the most recent build.
     * @returns Percentage 0-100; 0 before the first build in the active conv.
     */
    int contextFillPercent() const { return m_contextFillPercent; }

    /**
     * @brief True while an automatic memory refresh is due but is
     *        deliberately WAITING for the current round's idle gap
     *        (a compaction summary never interrupts a live reply, since
     *        it would contend with the foreground turn's provider).
     *        Drives the gauge's "waiting" hint so an overdue outer
     *        ring reads as intentional, not broken.
     * @returns True when a deferred summary is pending dispatch.
     */
    bool summaryDeferredPending() const { return m_summaryWantedThisCascade; }

    /**
     * @brief Assistant turns toward the next cadence compaction (gauge
     *        outer ring), measured at the most recent build.
     * @returns Turns since the current summary's coverage; 0 when none.
     */
    int compactionTurnsUsed() const { return m_compactionTurnsUsed; }
    /**
     * @brief The conversation's compactEveryTurns at the most recent build.
     * @returns The cadence denominator; 0 = cadence off.
     */
    int compactionTurnsTotal() const { return m_compactionTurnsTotal; }

    /**
     * @brief Whether the summarizer is compacting the foreground conversation.
     * @returns True iff a summary is generating for the active conversation.
     */
    bool isCompacting() const;

    /**
     * @brief The user text queued behind an in-flight cascade.
     * @returns Queued user text, or empty when nothing is queued.
     */
    QString queuedUserText() const { return m_queuedUserText; }

    /**
     * @brief Cumulative input+output tokens for the session.
     * @returns Running token total since construction.
     */
    int totalTokens() const { return m_totalTokens; }

    /**
     * @brief Estimated USD cost of the session (priced where available).
     * @returns Running cost estimate.
     */
    double estimatedCostUsd() const { return m_estimatedCostUsd; }

    /**
     * @brief Wall-clock duration of the last completed turn.
     * @returns Elapsed milliseconds of the most recent reply.
     */
    int lastResponseTimeMs() const { return m_lastResponseTimeMs; }

    /**
     * @brief The plan id anchored to the active conversation.
     * @returns Plan UUID, or empty when no task is active.
     */
    QString activeTaskPlanId() const;

    // -----------------------------------------------------------------
    // Internal-collaborator accessors (AppController wiring + tests).
    // -----------------------------------------------------------------

    /**
     * @brief Non-owning accessor for the owned CascadeController.
     * @returns Cascade controller pointer (never null after construction).
     */
    Chat::CascadeController* cascadeInternal() const { return m_cascade.get(); }

    /**
     * @brief Non-owning accessor for the owned StreamingManager.
     * @returns Streaming manager pointer (never null after construction).
     */
    Chat::StreamingManager* streamingInternal() const { return m_streaming.get(); }

    /**
     * @brief Non-owning accessor for the owned RequestBuilder.
     * @returns Request builder pointer (never null after construction).
     */
    Chat::RequestBuilder* requestBuilderInternal() const { return m_requestBuilder.get(); }

    /**
     * @brief Access the RAGP classification pipeline (owned by the cascade).
     * @returns Non-owning Ragp::Service pointer.
     */
    Ragp::Service* ragpService() const;

    /**
     * @brief Non-owning accessor for the deferred-action intent confirmer,
     *        building it lazily first (same path the turn flow uses). Test
     *        seam: integration tests inject a synchronous decider via
     *        ActionIntentConfirmer::setTestDecider to drive the confirmed /
     *        rejected branches without an LLM.
     * @returns Confirmer pointer, or null when the cascade has no RAGP
     *          service yet (construction is retried on the next turn).
     */
    Chat::ActionIntentConfirmer* intentConfirmerInternal() {
        ensureIntentConfirmer();
        return m_intentConfirmer.get();
    }

    /**
     * @brief Name of the currently-installed RAGP backend.
     * @returns Backend name string (e.g. local:llama.cpp:... or remote:...).
     */
    QString ragpBackendName() const;

    /**
     * @brief (Re)configures the RAGP Tier-3 backend from settings.
     * @param settings Non-owning SettingsService pointer; null is a no-op.
     */
    void configureRagpBackend(SettingsService* settings);

  signals:
    // The run emits these; ChatController connects each to its
    // identically-named facade signal (or a re-emit slot) so the QML
    // contract is byte-for-byte preserved.

    /** @brief Emitted when LLM generation starts or stops. */
    void isGeneratingChanged();
    /** @brief Emitted when the context-fill percentage was recomputed. */
    void contextFillPercentChanged();
    /** @brief Emitted when the compaction-cadence progress was recomputed. */
    void compactionCadenceChanged();
    /** @brief Emitted when the compaction-in-progress state changes. */
    void isCompactingChanged();
    /** @brief Emitted when the deferred-summary waiting state changes. */
    void summaryDeferredPendingChanged();
    /** @brief Emitted when the queued-user-message state changes. */
    void queuedUserTextChanged();
    /** @brief Emitted when any statistics value changes. */
    void statsChanged();
    /** @brief Emitted when the tools-enabled toggle changes. */
    void toolsEnabledChanged();
    /**
     * @brief Emitted on a non-fatal error.
     * @param message Human-readable error string.
     */
    void errorOccurred(const QString& message);
    /**
     * @brief Emitted when a send is queued behind an in-flight turn.
     * @param text Queued text.
     */
    void userMessageQueued(const QString& text);
    /**
     * @brief Emitted when the LLM requests a tool call.
     * @param toolName Canonical tool name.
     * @param argsJson Encoded argument JSON.
     */
    void toolCallStarted(const QString& toolName, const QString& argsJson);
    /**
     * @brief Emitted when a tool invocation completes.
     * @param toolName Canonical tool name.
     * @param resultJson Encoded result JSON.
     */
    void toolCallCompleted(const QString& toolName, const QString& resultJson);
    /**
     * @brief Emitted when an agent \@mentions the user in a group chat.
     * @param conversationId Group chat UUID.
     * @param agentAlias     Alias of the agent that mentioned the user.
     * @param messageText    The full assistant message content.
     */
    void userMentionedInGroup(const QString& conversationId,
                              const QString& agentAlias,
                              const QString& messageText);
    /**
     * @brief Forwarded from the cascade: @-mention routing was suppressed below the dispatch floor.
     * @param conversationId Group chat UUID.
     * @param responderAlias Agent whose reply contained the mentions.
     * @param targetAliases  Suppressed target aliases.
     * @param confidence     Classifier confidence (below floor).
     * @param source         Classifier source tag for diagnostics.
     */
    void mentionRoutingSuppressed(const QString& conversationId,
                                  const QString& responderAlias,
                                  const QStringList& targetAliases,
                                  double confidence,
                                  const QString& source);
    /**
     * @brief Emitted when the RAGP backend changes.
     * @param backendName Value of ragpBackendName() after the swap.
     */
    void ragpBackendChanged(const QString& backendName);

    /**
     * @brief Emitted on any active-conversation transition (switch,
     *        autocreate, auto-rename, post-delete clear). ChatController
     *        re-emits both activeConversationChanged and
     *        activeConversationSettingsChanged in response.
     */
    void activeConversationChanged();

  private slots:
    /**
     * @brief Receives a streaming token chunk from ModelRouter.
     * @param requestId Monotonic id; gated against m_currentRequestId to drop stale signals.
     * @param chunk     Partial text / tool-call chunk.
     */
    void onChunkReceived(quint64 requestId, const LlmChunk& chunk);

    /**
     * @brief Called when the LLM request completes.
     * @param requestId    Monotonic id (see onChunkReceived).
     * @param finishReason "stop" | "tool_calls" | "length" | "user_interrupted".
     * @param totalTokens  Total token count (input + output) if provided.
     */
    void onRequestFinished(quint64 requestId, const QString& finishReason, int totalTokens);

    /**
     * @brief Tail of onRequestFinished: write the turn-audit row, honour a
     *        queued-user preemption, and hand the assembled inputs to the
     *        cascade's routeOrFinalize. Factored out so it can run either
     *        synchronously or after an async deferred-action confirmation.
     * @param cascadeInputs Fully-populated routing inputs for this turn.
     */
    void finishTurnRouting(const Chat::CascadeRouteInputs& cascadeInputs);

    /**
     * @brief Lazily construct m_intentConfirmer against the cascade's RAGP
     *        service (so it reuses the RAGP-configured provider). No-op once
     *        built or when no RAGP service exists yet.
     */
    void ensureIntentConfirmer();

    /**
     * @brief Called when the provider reports an error.
     * @param requestId    Monotonic id (see onChunkReceived).
     * @param errorMessage Human-readable error string.
     */
    void onRequestError(quint64 requestId, const QString& errorMessage);

    /**
     * @brief Slot: the dispatcher drained its tool batch; re-opens the placeholder and continues.
     * @param convId  Conversation id the batch belonged to.
     * @param agentId Responder agent template id for the continuation turn.
     * @param alias   Responder alias for the continuation turn.
     */
    void onToolBatchCompleted(const QString& convId, const QString& agentId, const QString& alias);

    /**
     * @brief Slot: the request_turn tool named an alias; enqueues it and arms the yield flag.
     * @param alias Alias the tool requested.
     */
    void onToolDispatcherRequestTurnTarget(const QString& alias);

    /**
     * @brief Slot: a substantive tool fired without start_task, auto-anchoring a plan.
     * @param planId Newly-created plan UUID.
     */
    void onToolDispatcherActiveTaskAutoAnchored(const QString& planId);

    /**
     * @brief Slot: the dispatcher could not proceed (expected flow-control event; logged only).
     * @param reason Human-readable reason logged for diagnostics.
     */
    void onToolDispatcherBatchFailed(const QString& reason);

    /**
     * @brief Slot: the cascade dispatched the next group member's turn.
     * @param alias   Next responder's alias.
     * @param agentId Next responder's agent template id.
     */
    void onCascadeMemberTurnStarted(const QString& alias, const QString& agentId);

    /**
     * @brief Slot: the cascade finished (queue drained or cap hit); runs turn-end cleanup.
     * @param declaredTaskStatus Declared status passthrough from the last responder.
     * @param finishReason         Final finish reason for the cascade.
     * @param totalTokens          Cumulative token count.
     * @param elapsedMs            Wall-clock time the cascade took.
     * @param roundTurns           Dispatched agent turns in the final
     *                             round (settle-note gate).
     * @param pauseNoted           True when a cap-pause note already
     *                             surfaced this ending.
     */
    void onCascadeComplete(const QString& declaredTaskStatus,
                           const QString& finishReason,
                           int totalTokens,
                           qint64 elapsedMs,
                           int roundTurns = 0,
                           bool pauseNoted = false);

    /**
     * @brief Slot: the cascade flagged an echoed/empty reply with retry budget remaining.
     * @param convId       Conversation UUID.
     * @param badMsgId     Persisted id of the failed reply.
     * @param alias        Responder alias to retry.
     * @param agentId      Responder agent template id.
     * @param retryAttempt 1-based retry counter for diagnostics.
     * @param reason       Why the retry fired: "echo" | "empty". (The
     *                     former "claims_no_fire" class was retired;
     *                     announced-but-undone actions now use the
     *                     non-destructive deferred-action continuation.)
     */
    void onCascadeEchoDetectedRequestRetry(const QString& convId,
                                           const QString& badMsgId,
                                           const QString& alias,
                                           const QString& agentId,
                                           int retryAttempt,
                                           const QString& reason);

    /**
     * @brief Slot: the cascade exhausted the retry budget for a bad reply.
     * @param convId   Conversation UUID.
     * @param badMsgId Persisted id of the still-failed reply.
     * @param wasEmpty True when the bad reply produced zero content.
     */
    void
    onCascadeEchoMaxRetriesExhausted(const QString& convId, const QString& badMsgId, bool wasEmpty);

    /**
     * @brief Slot: the group classify verdict says the responder kept a
     *        concrete pending action for themselves
     *        (Classification::selfPendingAction). Runs the
     *        NON-DESTRUCTIVE continuation: keep the reply, bump the
     *        per-alias budget, insert the visible "↻" note, and
     *        dispatch ONE more turn at the same responder. A user
     *        message queued mid-classify wins, and the cascade is
     *        force-completed instead (preemption discipline).
     * @param convId  Conversation the verdict belongs to.
     * @param alias   Seated responder alias (the continuation target).
     * @param agentId Responder agent template id.
     * @param hint    Classifier's one-line description of the pending
     *                action (diagnostics).
     */
    void onCascadeDeferredContinuationRequested(const QString& convId,
                                                const QString& alias,
                                                const QString& agentId,
                                                const QString& hint);

  public:
    /**
     * @brief Resumes the conversation after an ASYNC image generation
     *        completed while the run was idle. Seats the requesting
     *        agent and dispatches one follow-up turn whose final
     *        user-role message carries the completion feedback and the
     *        generated image itself (vision input, this turn only).
     *        No-ops when a turn is live (the history row suffices), a
     *        user message is queued (preemption), the per-round budget
     *        is spent, or no requesting agent exists (user-driven
     *        generations). Without this every image generation ended
     *        in a silent stall: the tool returns "queued", the cascade
     *        finishes, and the completion row lands with no consumer.
     * @param convId    Conversation the image belongs to.
     * @param alias     Requesting member alias (empty in 1:1).
     * @param agentId   Requesting agent template id (empty = no resume).
     * @param imagePath Absolute path of the finished PNG.
     */
    void resumeAfterImageGenerated(const QString& convId,
                                   const QString& alias,
                                   const QString& agentId,
                                   const QString& imagePath);

    /**
     * @brief Reconstructs provider-ready LLM history from persisted
     *        messages (delegates to RequestBuilder::assembleHistory).
     *        Public so integration tests can reconstruct the exact
     *        on-the-wire payload.
     * @param dbMessages  Raw messages from MessageService::getMessages.
     * @param isGroupChat True for group chats (drives "(Alias said)" prefixing).
     * @param excludeMsgId UUID to skip (typically the unpersisted placeholder).
     * @returns Provider-ready history list.
     */
    QList<LlmMessage> assembleLlmHistory(const QList<Message>& dbMessages,
                                         bool isGroupChat,
                                         const QString& excludeMsgId);

  private:
    // -----------------------------------------------------------------
    // Pipeline helpers (moved verbatim from ChatController).
    // -----------------------------------------------------------------

    /**
     * @brief THE one BuildRequestInputs assembly, shared by the Direct
     *        and Agent dispatch paths, including the read-then-clear
     *        continuation flags (post-tool / deferred-action). Two
     *        hand-maintained copies previously drifted: the flags were
     *        added to the Agent copy only, so Direct-path continuations
     *        never carried their follow-through instruction to the
     *        model.
     * @returns Fully-populated inputs for RequestBuilder::buildRequest.
     */
    Chat::BuildRequestInputs makeBuildInputs();

    /** @brief Assembles the LlmRequest from history and routes it to the
     *  provider. When RAG is enabled it first kicks an ASYNC pre-fetch and
     *  returns; the build+dispatch resumes in onRetrievalReady. */
    void buildAndSendRequest();

    /** @brief The direct (non-agent) turn build + dispatch body (the former
     *  buildAndSendRequest tail). Called directly when no pre-fetch applies,
     *  or resumed from onMemoryRetrievalReady with m_pendingRagContext populated. */
    void buildAndDispatchDirect();

    /** @brief The agentic-pattern turn build + dispatch body (extracted from
     *  sendMessage). Called directly or resumed from onMemoryRetrievalReady. */
    void dispatchAgentTurn();

    /** @brief Which dispatch onMemoryRetrievalReady should resume after a pre-fetch. */
    enum class PendingTurn { None, Direct, Agent };

    /**
     * @brief Kicks the unified async pre-turn memory retrieval (RAG + AIM + ACN,
     *        per their independent gates) when there is query text; the turn
     *        resumes in onMemoryRetrievalReady. Signal-driven (MemoryRetriever::
     *        retrievalReady); never blocks, no timer. One query embed serves all
     *        enabled sources.
     * @param turn Which dispatch (Direct/Agent) to resume on completion.
     * @returns true if a retrieval was started (caller must return and wait);
     *          false if none applies (caller dispatches now, un-augmented).
     */
    bool maybeBeginMemoryRetrieve(PendingTurn turn);

    /**
     * @brief Resumes the pending turn with the pre-formatted injected context
     *        once the unified retrieval completes. Matched by requestId;
     *        stale/cancelled results (after stop/switch/new turn) are ignored.
     * @param requestId The id returned by the originating beginRetrieve().
     * @param injectedContext The merged, labelled context block (possibly empty
     *        → resume un-augmented).
     */
    void onMemoryRetrievalReady(quint64 requestId, const QString& injectedContext);

    /**
     * @brief Lazily creates this run's own AgentService executor and wires
     *        it, once ModelRouter, ToolService and RagService are all
     *        available.
     *
     * AgentService's constructor needs ToolService and RagService, which
     * arrive after construction via set*() forwarders, so the owned
     * executor cannot be built in the run's constructor. This helper is
     * called from setAgentService(), setToolService() and setRagService();
     * it constructs the executor exactly once (no-op thereafter), connects
     * its chunk / finished / errorOccurred signals to this run, and (if
     * the app-level facade is attached) registers the executor as a relay
     * source so the QML AgentService property and the remote bridge keep
     * observing agent progress.
     *
     * @returns true if the owned executor exists after the call (either
     *          already existed or was just created); false if a required
     *          dependency is still missing.
     */
    bool ensureOwnAgent();

    /**
     * @brief Acquires the target provider's scheduler slot, then routes
     *        the built request: immediately if the slot is free, or
     *        deferred (via the scheduler's onGranted callback) when the
     *        provider is busy. When no scheduler is attached, routes
     *        immediately (legacy single-instance behaviour).
     * @param request Fully-built LlmRequest whose config.providerId/
     *                modelName have already been resolved.
     */
    void dispatchThroughScheduler(const LlmRequest& request);

    /**
     * @brief Records that this run now holds a provider's slot and
     *        performs the actual ModelRouter::route(). Single recording
     *        point shared by the immediate-grant and queued-grant paths.
     * @param request The request to dispatch.
     */
    void doRoute(const LlmRequest& request);

    /**
     * @brief Releases this run's held provider slot if (and only if) it
     *        currently holds one, then clears the held marker. Idempotent
     *        and safe to call on every terminal path; a second call is a
     *        no-op. This is how the acquire/release pairing is kept exact
     *        (no double release, no lost release).
     */
    /**
     * @brief Opens the contention-ledger entry for a new provider
     *        request (closing any stale one), capturing the queue-wait
     *        start. Diagnostics only.
     * @param providerId Provider the request targets.
     * @param model      Model name of the request.
     */
    void ledgerBeginTurn(const QString& providerId, const QString& model);

    void releaseProviderSlotIfHeld();

    /**
     * @brief Cancels this run's still-queued scheduler waiter (if any)
     *        for a turn torn down before its grant fired. Distinct from
     *        releaseProviderSlotIfHeld(): cancel touches no in-flight
     *        count. Called from stop/abort/delete paths alongside the
     *        release helper so whichever state the run is in is cleaned.
     */
    void cancelQueuedSchedulerWaiter();

    /**
     * @brief Refreshes the per-turn agent-settings cache from a conversation's stored config.
     * @param convId Conversation UUID; empty/missing is a no-op.
     */
    void refreshAgentSettingsForConv(const QString& convId);

    /**
     * @brief Persists a user message and appends it to the model.
     * @param text Message content.
     * @returns Stored message UUID, or empty string on failure.
     */
    QString storeUserMessage(const QString& text);

    /**
     * @brief Single write path for the context-fill percentage.
     * @param percent New fill percentage (0-100; 0 = no measurement).
     */
    void updateContextFillPercent(int percent);
    void updateCompactionCadence(int used, int total);

    /**
     * @brief Records a turn's compaction decision from its build result.
     *
     * If the build met the compaction trigger, the summary is deferred to the
     * post-cascade idle gap (m_summaryWantedThisCascade) so it never competes
     * with the foreground turn for the provider, UNLESS context is already
     * critical, in which case it is dispatched immediately (the ceiling that
     * prevents an over-long cascade from overflowing the window).
     * @param br The turn's build result.
     */
    void noteSummaryDecision(const Chat::BuildResult& br);

    /**
     * @brief Sets or clears the queued user message, emitting on real change.
     * @param text        Queued text (empty clears the queue).
     * @param attachments Queued attachment paths.
     */
    void setQueuedUserMessage(const QString& text, const QStringList& attachments);

    /**
     * @brief Process-wide monotonic request id source (skips 0).
     *
     * Delegates to ModelRouter::nextRequestId(), the single source shared
     * across every ChatController / ConversationRun instance AND the agent
     * path (AgentService), so the requestId-gated stale-signal guards never
     * false-positive across concurrent in-flight turns.
     *
     * @returns A unique foreground request id (never 0).
     */
    static quint64 nextGlobalRequestId();

    // -----------------------------------------------------------------
    // Shared services (non-owning). Core four bound at construction;
    // optional ones attached via set*() forwarders.
    // -----------------------------------------------------------------
    ModelRouter& m_router;
    ConversationService& m_convSvc;
    MessageService& m_msgSvc;
    MessageListModel* m_msgModel = nullptr;  // owned by ChatController

    ToolService* m_toolService = nullptr;
    RagService* m_ragService = nullptr;
    MemoryRetriever* m_memoryRetriever = nullptr;  // app-level; unified pre-turn recall
    FileService* m_fileService = nullptr;
    AgentRegistry* m_agentRegistry = nullptr;
    MembershipService* m_membershipService = nullptr;
    PlanService* m_planService = nullptr;
    TaskRunner* m_taskRunner = nullptr;
    SkillService* m_skillService = nullptr;
    HeartbeatConfigService* m_heartbeatConfigService = nullptr;
    PollService* m_pollService = nullptr;
    AuditService* m_auditService = nullptr;
    QPointer<Verzeta::Session::SessionRouter> m_sessionRouter;
    TaskGateService* m_taskGate = nullptr;
    SlashCommandService* m_slashService = nullptr;
    CanvasService* m_canvasSvc = nullptr;
    /** Non-owning; source of the live tool-iteration budget. Stored in
     *  configureRagpBackend; read each turn so setting changes apply live. */
    SettingsService* m_settings = nullptr;
    TaskObserver* m_taskObserver = nullptr;
    AgentService* m_agentService = nullptr;
    ConversationSummarizer* m_summarizer = nullptr;
    QPointer<Chat::ProviderScheduler> m_providerScheduler;

    /** Set when a turn's build meets the compaction trigger but context is not
     *  yet critical: the summary is deferred and dispatched in the idle gap at
     *  onCascadeComplete so it never runs concurrently with a foreground turn
     *  (which saturated the single local provider). Consumed + cleared there. */
    bool m_summaryWantedThisCascade = false;

    /**
     * @brief Guarded writer for m_summaryWantedThisCascade. Emits
     *        summaryDeferredPendingChanged only on a real transition
     *        (the gauge binds to it).
     * @param wanted New deferred-summary state.
     */
    void setSummaryWantedThisCascade(bool wanted);

    // -----------------------------------------------------------------
    // Per-turn collaborators. Declaration order defines reverse-order
    // destruction: ContentSanitizer → CascadeController →
    // ToolDispatcher → StreamingManager → RequestBuilder → services.
    // -----------------------------------------------------------------
    std::unique_ptr<Chat::RequestBuilder> m_requestBuilder;
    std::unique_ptr<Chat::StreamingManager> m_streaming;
    std::unique_ptr<Chat::ToolDispatcher> m_toolDispatcher;
    std::unique_ptr<Chat::CascadeController> m_cascade;
    std::unique_ptr<Chat::ContentSanitizer> m_sanitizer;

    std::unique_ptr<Chat::ActionIntentConfirmer> m_intentConfirmer;

    std::unique_ptr<AgentService> m_ownAgent;

    // -----------------------------------------------------------------
    // Active-conversation view state (source of truth for this run).
    // -----------------------------------------------------------------
    QString m_activeConvId;
    QString m_activeConvTitle;

    // -----------------------------------------------------------------
    // Per-turn execution state.
    // -----------------------------------------------------------------
    bool m_isGenerating = false;
    QString m_inflightConvId;
    quint64 m_currentRequestId = 0;
    QString m_currentDispatchedModelName;
    QString m_currentDispatchedProviderId;

    QString m_schedulerWaiterId;
    QString m_schedulerHeldProviderId;

    /** Contention-ledger entry for the CURRENT provider request (0 =
     *  none open). One entry per dispatched request; begun at scheduler
     *  acquire (queue-wait phase), dispatched at doRoute / agent
     *  execute, first-token on the first stream chunk, ended on
     *  finish / error / slot release (idempotent). Diagnostics only,
     *  with no behavioral coupling. */
    quint64 m_ledgerTurnId = 0;
    bool m_ledgerFirstTokSeen = false;
    QString m_schedulerQueuedProviderId;
    QString m_lastUserMsgId;
    QString m_lastUserText;
    QElapsedTimer m_responseTimer;

    QString m_queuedUserText;
    QStringList m_queuedUserAttachments;

    QVector<QPair<QString, QString>> m_pendingSubagentReactions;
    QList<TaskDispatchRequest> m_taskDispatchQueue;

    QList<LlmImageData> m_pendingImages;
    QString m_pendingFileContext;

    quint64 m_pendingQueryId = 0;  ///< 0 = no async retrieval in flight
    PendingTurn m_pendingTurn = PendingTurn::None;
    QString m_pendingRagContext;  ///< pre-formatted block for the resumed turn

    AgentPattern m_agentPattern = AgentPattern::Direct;
    bool m_requireConfirmation = false;
    bool m_usingAgent = false;
    bool m_toolsEnabled = true;
    bool m_userCancelledInFlight = false;

    int m_currentEchoRetryAttempt = 0;
    QString m_currentRetryReason;
    bool m_yieldRequestedThisBatch = false;
    /// Set when the next build is the continuation dispatched right
    /// after a tool batch (onToolBatchCompleted); read-then-cleared in
    /// buildAndSendRequest so RequestBuilder can append a 1:1 keep-going nudge.
    bool m_postToolContinuationPending = false;

    /// 1:1 deferred-action continuation. Count of
    /// NON-DESTRUCTIVE continuations fired this user-message chain (reset on a
    /// fresh user send); bounds the announce-then-stop re-nudging. Pending flag
    /// is read-then-cleared in buildAndSendRequest so RequestBuilder appends
    /// the "you described an action; run the tool now, or say if you can't"
    /// nudge on the continuation turn.
    int m_deferredContinuationCount = 0;
    bool m_deferredContinuationPending = false;

    /// GROUP deferred-action continuations, PER ALIAS (lower-cased
    /// key) per user round. Each confirmed continuation for a seated group
    /// responder increments its alias's count; the candidate gate refuses
    /// further continuations for that alias once kMaxGroupDeferredPerAlias
    /// is reached. Cleared on every fresh user send alongside the 1:1
    /// counter, so one round's corrections never bleed into the next.
    QHash<QString, int> m_groupDeferredContinuations;

    /// Async-image follow-ups dispatched since the last user send
    /// (bounded by kMaxAsyncArtifactContinuations); pending flag is
    /// read-then-cleared in makeBuildInputs so RequestBuilder appends
    /// the image-ready follow-through exactly once.
    int m_asyncArtifactContinuations = 0;
    bool m_asyncArtifactPending = false;

    /// Fire-awareness record for the CURRENT turn-chain: lowercased names
    /// of every tool call dispatched since this chain began, plus the
    /// lowercased basenames of files named in their arguments. The
    /// deferred-action gate compares a reply's ANNOUNCED targets against
    /// these so a responder is never nudged for work it already executed
    /// ("I've updated guide.md" after edit_canvas ran on guide.md), and
    /// the confirm prompt receives a truthful executed-tools context.
    /// Cleared on a fresh user send and at every new member turn.
    QSet<QString> m_chainExecutedToolNames;
    QSet<QString> m_chainExecutedFiles;

    int m_contextFillPercent = 0;
    int m_compactionTurnsUsed = 0;
    int m_compactionTurnsTotal = 0;
    bool m_summarizerBusy = false;
    QString m_compactingConvId;

    // Session statistics.
    int m_totalTokens = 0;
    double m_estimatedCostUsd = 0.0;
    int m_lastResponseTimeMs = 0;

    QString m_activeCanvasFilename;
    QString m_activeCanvasLanguage;
    int m_activeCanvasRevision = 0;
    int m_activeCanvasLineCount = 0;
    qint64 m_activeCanvasByteSize = 0;
};

}  // namespace Chat

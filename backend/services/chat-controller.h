// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file chat-controller.h
 * @brief Central orchestrator bridging the backend LLM pipeline to
 *        the QML UI. Manages the send → stream → display lifecycle,
 *        conversation switching, per-conversation settings, model
 *        selection, conversation export, and exposes all chat state
 *        as QML-bindable properties. This is the primary C++ object
 *        exposed as a QML singleton "ChatController".
 * @layer Service (UI orchestration)
 * @dependencies ModelRouter, ConversationService, MessageService,
 *               ExportService, MessageListModel, MarkdownConverter,
 *               plus optional services (ToolService, RagService,
 *               AgentService, FileService, AgentRegistry,
 *               MembershipService, SearchService, PlanService,
 *               TaskRunner, TaskObserver) attached via set*()
 *               methods after construction.
 *
 * Architecture: ChatController is a COORDINATOR over a map of
 * per-conversation execution engines. It owns a
 * QHash<conversationId, Chat::ConversationRun> (one run per
 * conversation that has become active or holds a turn) and keeps the
 * QML-facing Q_PROPERTY / Q_INVOKABLE / signal surface as thin
 * forwarders that delegate to the ACTIVE conversation's run. Each
 * Chat::ConversationRun owns the per-turn collaborators
 * (RequestBuilder, StreamingManager, ToolDispatcher, CascadeController,
 * ContentSanitizer) for its conversation; ChatController never touches
 * per-turn state directly.
 *
 * Active vs. background runs: switching the active conversation
 * re-points the facade at that conversation's run (disconnecting the
 * previous active run's change-signal forwarding and connecting the
 * new one's), then re-emits the facade change signals so QML rebinds.
 * A turn started while a conversation was active keeps running on that
 * conversation's run even after the user switches away. The
 * background run streams and persists to its own conversation while
 * the foreground shows the newly-active conversation's state.
 *
 * Performance: tool invocations that do not touch SQLite run on a
 * QtConcurrent worker thread; UI remains responsive while tools
 * execute. Streaming updates dispatch via Qt model signals at
 * chunk-granularity.
 *
 * Security: tool loops capped at Chat::ToolDispatcher::kMaxToolIterations
 * (10) per user message; LLM cannot recurse indefinitely on
 * tool_calls. Shell execution is gated by ProcessSandbox's
 * allow-list.
 */

#pragma once

#include "../api/llm-interface.h"
#include "../models/message.h"
#include "agent-service.h"  // AgentPattern enum, AgentConfig, AgentService forward used below

#include <memory>
#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QHash>  // provides std::hash<QString> for m_runs
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <unordered_map>

// Forward declarations
class AgentRegistry;
class AgentService;
class CanvasService;
class ConversationService;
class ExportService;
class FileService;
class MembershipService;
class MessageService;
class MessageListModel;
class ModelRouter;
class PlanService;
class RagService;
class SlashCommandService;
class TaskGateService;
class TaskRunner;
class ToolService;

namespace Verzeta::Session {
class SessionRouter;
}

#include "../models/member.h"

// Ragp::Service is owned by Chat::CascadeController — ChatController
// references it only via the cascade controller and does not pull in
// Ragp types through this header.

// Chat subsystem collaborators. ChatController owns these via
// std::unique_ptr and delegates responsibilities behind its stable
// QML-facing facade.
namespace Verzeta::Infer {
class InferenceSidecarHost;
}

namespace Chat {
class ConversationRun;
class RequestBuilder;     // assembleHistory + buildRequest
class StreamingManager;   // in-flight-stream state machine
class ToolDispatcher;     // serial tool-call batch dispatch
class CascadeController;  // group-chat cascade + RAGP
class ContentSanitizer;   // six-stage content sanitisation pipeline
class ProviderScheduler;
class MemoryRetriever;
class ActionIntentConfirmer;
}  // namespace Chat

// RAGP classification pipeline — forward declared for the
// integration-test hook `ragpService()`. Full include is local to
// chat-controller.cpp.
namespace Ragp {
class Service;
}

#include "../services/task-runner.h"  // for TaskDispatchRequest value member

/**
 * @brief QML singleton that orchestrates the full chat loop.
 *
 * The send flow:
 *   1. storeUserMessage(): persist user message to DB, append to model
 *   2. appendStreamingMessage(): add placeholder assistant row
 *   3. buildAndSendRequest(): assemble context + route to provider
 *   4. onChunkReceived(): per-token updates via dataChanged (hot path)
 *   5. onRequestFinished(): persist final assistant message, compute HTML
 *
 * Key QML-facing items:
 *   - Per-conversation LLM settings (system prompt, temperature,
 *     max tokens, streaming, thinking mode) exposed as bindable
 *     Q_PROPERTY accessors.
 *   - Session statistics (totalTokens, estimatedCostUsd,
 *     lastResponseTimeMs, messageCount) as bindable Q_PROPERTYs.
 *   - Model selection via modelsForProvider() and setModel().
 *   - Export via exportConversation() (delegates to ExportService).
 *   - clearAllConversations(): danger zone operation.
 *   - renameConversation(): wired to the sidebar rename action.
 *
 * Registered as QML singleton in AppController::registerTypes() via
 * qmlRegisterSingletonInstance<ChatController>(...).
 *
 * QML access:
 *   ChatController.sendMessage("Hello")
 *   ChatController.isGenerating
 *   ChatController.messages           // MessageListModel*
 *   ChatController.activeConversationTitle
 *   ChatController.conversations      // QVariantList of {id, title, updatedAt}
 *   ChatController.availableProviders // QVariantList of {providerId, displayName}
 *   ChatController.activeSystemPrompt
 *   ChatController.activeTemperature
 *   ChatController.activeMaxTokens
 *   ChatController.activeStreaming
 *   ChatController.activeThinking
 *
 * Thread safety: Designed for main-thread use. Provider signals are connected
 * via direct connections (same thread). LlamaCppProvider uses Qt::QueuedConnection
 * internally for cross-thread delivery.
 */
class ChatController : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString activeConversationId READ activeConversationId WRITE setActiveConversationId
                   NOTIFY activeConversationChanged)

    Q_PROPERTY(QString activeConversationTitle READ activeConversationTitle NOTIFY
                   activeConversationChanged)

    /**
     * @brief Context-window fill (0-100) measured at the
     *        most recent request build: (system prompt + tool schemas
     *        + compaction summary + included history) / context
     *        window. Drives the chat-input compaction indicator.
     */
    Q_PROPERTY(int contextFillPercent READ contextFillPercent NOTIFY contextFillPercentChanged)

    /** Compaction-cadence progress (chat-input gauge OUTER ring): assistant
     *  turns accumulated toward the next memory refresh, and the
     *  conversation's compactEveryTurns (0 = cadence off). */
    Q_PROPERTY(int compactionTurnsUsed READ compactionTurnsUsed NOTIFY compactionCadenceChanged)
    Q_PROPERTY(int compactionTurnsTotal READ compactionTurnsTotal NOTIFY compactionCadenceChanged)

    Q_PROPERTY(bool isCompacting READ isCompacting NOTIFY isCompactingChanged)

    /** True while a memory refresh is DUE but deliberately waiting for
     *  the current round's idle gap (compaction never interrupts a
     *  live reply). The gauge shows a "waiting" hint so an overdue
     *  outer ring reads as intentional. */
    Q_PROPERTY(bool compactionWaitingForIdle READ compactionWaitingForIdle NOTIFY
                   compactionWaitingForIdleChanged)

    Q_PROPERTY(bool isGenerating READ isGenerating NOTIFY isGeneratingChanged)

    Q_PROPERTY(QString queuedUserText READ queuedUserText NOTIFY queuedUserTextChanged)

    Q_PROPERTY(MessageListModel* messages READ messages CONSTANT)

    // NOTE: The `conversations` Q_PROPERTY was deleted as part of the
    // model-layer refactor. QML now binds directly to the
    // ConversationListModel context property, which is fed by row-level
    // ConversationService signals. ChatController no longer exposes a
    // QVariantList shadow of the conversation list.

    // -----------------------------------------------------------------------
    // Session statistics
    // -----------------------------------------------------------------------

    Q_PROPERTY(int totalTokens READ totalTokens NOTIFY statsChanged)

    Q_PROPERTY(double estimatedCostUsd READ estimatedCostUsd NOTIFY statsChanged)

    Q_PROPERTY(int lastResponseTimeMs READ lastResponseTimeMs NOTIFY statsChanged)

    Q_PROPERTY(int messageCount READ messageCount NOTIFY statsChanged)

    // NOTE: `artifacts` and `toolCallLog` Q_PROPERTY QVariantList shadows
    // were deleted during the model-layer refactor. QML now binds to the
    // proper ArtifactsModel and ToolCallLogModel context properties —
    // both are push-based QAbstractListModels fed by MessageService
    // row-level signals. Every file artifact and tool call update is
    // delivered as a beginInsertRows / dataChanged, never as a full
    // QVariantList rebuild or refresh() pull.

    // -----------------------------------------------------------------------
    // Per-conversation settings
    // -----------------------------------------------------------------------

  public:
    /**
     * @brief Constructs the ChatController.
     * @param router       Reference to the ModelRouter for provider dispatch.
     * @param convSvc      Reference to ConversationService for conversation CRUD.
     * @param msgSvc       Reference to MessageService for message persistence.
     * @param exportSvc    Reference to ExportService for conversation export.
     * @param parent       Optional Qt parent.
     * @sideeffects Connects to ModelRouter signals for streaming.
     *              Connects ExportService signals for export result notification.
     */
    explicit ChatController(ModelRouter& router,
                            ConversationService& convSvc,
                            MessageService& msgSvc,
                            ExportService& exportSvc,
                            QObject* parent = nullptr);

    /**
     * @brief Destroys the controller.
     *        Disconnects from ModelRouter signals.
     */
    ~ChatController() override;

    // -----------------------------------------------------------------------
    // Q_INVOKABLE methods — callable from QML
    // -----------------------------------------------------------------------

    /**
     * @brief Sends a user message and triggers LLM response streaming.
     * @param text        The user's message text (must not be empty).
     * @param attachments Optional list of file paths to attach.
     * @sideeffects Stores user message in DB, appends to model, starts streaming.
     *              Emits isGeneratingChanged(). No-op if text is empty or already generating.
     */
    Q_INVOKABLE void sendMessage(const QString& text, const QVariantList& attachments = {});

    /**
     * @brief Cancels the current LLM generation.
     * @sideeffects Calls ModelRouter::cancelCurrent(). Finalizes the streaming message
     *              with finishReason="user_interrupted". Emits isGeneratingChanged().
     *              No-op if not generating.
     */
    Q_INVOKABLE void stopGeneration();

    /**
     * @brief Retries the last user message.
     *        Removes the last assistant response from DB and model, then re-sends.
     * @sideeffects Deletes last assistant message (if persisted), removes from model,
     *              re-invokes sendMessage() with the previous user text.
     *              No-op if generating or no messages present.
     */
    Q_INVOKABLE void retryLastMessage();

    /**
     * @brief Switches the active conversation.
     * @param id UUID of the conversation to activate.
     * @sideeffects Loads messages from DB into MessageListModel. Emits activeConversationChanged.
     *              If id is invalid or not found, emits errorOccurred().
     */
    Q_INVOKABLE void switchConversation(const QString& id);

    // -----------------------------------------------------------------------
    // Agent pattern selection and wiring
    // -----------------------------------------------------------------------

    /**
     * @brief Wires an AgentService for agent pattern execution.
     * @param agentService Pointer to AgentService (not owned). nullptr disables agents.
     * @sideeffects Connects agent signals to streaming/completion handlers.
     */
    void setAgentService(AgentService* agentService);

    /**
     * @brief Returns the content of the last assistant message in the active conversation.
     * @return Message content string, or empty if no assistant messages exist.
     * @note Used by the "Read Aloud" feature to get the text to synthesize.
     */
    Q_INVOKABLE QString lastAssistantMessage() const;

    // NOTE: artifacts() / toolCallLog() / clearArtifacts() / clearToolCallLog()
    // were removed. The frontend binds directly to ArtifactsModel and
    // ToolCallLogModel (context properties); deleting a row is done by
    // deleting the underlying Message via MessageService, which fires
    // messageDeleted → the model removes the row automatically.

    /**
     * @brief Returns the effective artifact directory for the active
     *        conversation.
     * @returns Absolute path. Per-project when the conversation lives
     *          inside a project/organization folder, otherwise a
     *          per-conversation subdirectory.
     */
    Q_INVOKABLE QString activeArtifactsPath() const;

    /** @brief Opens the active conversation's artifact directory in the OS file manager. */
    Q_INVOKABLE void openActiveArtifactsFolder();

    /**
     * @brief Opens a local file in the OS default application (text
     *        editor, image viewer, …). Used by the Artifacts overlay's
     *        per-row Open button. Only existing regular files are
     *        opened; the path is converted via QUrl::fromLocalFile so
     *        spaces and reserved characters in artifact paths are
     *        handled correctly (QML string URLs are not).
     * @param absolutePath Absolute local path of the file to open.
     * @returns true when the path was an existing file and the OS
     *          hand-off was issued; false otherwise.
     */
    Q_INVOKABLE bool openPathExternally(const QString& absolutePath);

    // -----------------------------------------------------------------------
    // Tool calling & RAG wiring
    // -----------------------------------------------------------------------

    /**
     * @brief Attach the dynamic-compaction service.
     *        Passed per-request into BuildRequestInputs; null detaches.
     * @param summarizer Non-owning pointer (AppController owns).
     */
    void setSummarizer(class ConversationSummarizer* summarizer);

    /**
     * @brief Reader for the contextFillPercent Q_PROPERTY.
     * @returns Estimated percentage (0-100) of the model's context
     *          window consumed at the most recent request build;
     *          0 before the first build in the active conversation.
     */
    int contextFillPercent() const;

    /**
     * @brief Reader for compactionTurnsUsed (gauge outer-ring numerator).
     * @returns Assistant turns accumulated toward the next cadence
     *          compaction at the most recent build; 0 when none/no active run.
     */
    int compactionTurnsUsed() const;
    /**
     * @brief Reader for compactionTurnsTotal (gauge outer-ring denominator).
     * @returns The active conversation's compactEveryTurns at the most recent
     *          build; 0 = cadence off (or no active run).
     */
    int compactionTurnsTotal() const;

    /**
     * @brief Reader for the isCompacting Q_PROPERTY.
     * @returns True iff the summarizer is generating a summary for the
     *          conversation currently in the foreground.
     */
    bool isCompacting() const;

    /**
     * @brief Reader for the compactionWaitingForIdle Q_PROPERTY.
     * @returns True while a due memory refresh is deferred to the
     *          current round's idle gap on the active conversation.
     */
    bool compactionWaitingForIdle() const;

    /**
     * @brief Manual compaction from the chat-input
     *        button (same semantics as /compact): invalidates the
     *        current summary and regenerates with reason "manual".
     *        No-op without an active conversation or summarizer.
     */
    Q_INVOKABLE void compactNow();

    /**
     * @brief Attaches a ToolService for LLM tool calling support.
     * @param toolService Pointer to ToolService (not owned). nullptr disables tools.
     */
    void setToolService(ToolService* toolService);

    /**
     * @brief Attaches a RagService for retrieval-augmented generation.
     * @param ragService Pointer to RagService (not owned). nullptr disables RAG.
     */
    void setRagService(RagService* ragService);

    /**
     * @brief Attaches the unified pre-turn memory-recall coordinator, relayed to
     *        every conversation run. Drives RAG + AIM (+ ACN) recall per their
     *        independent gates.
     * @param retriever Pointer to the app-level MemoryRetriever (not owned);
     *        nullptr disables pre-turn recall.
     */
    void setMemoryRetriever(Chat::MemoryRetriever* retriever);

    /**
     * @brief Configure the RAGP Tier-3 backend from the current
     *        SettingsService state. Chooses between local llama.cpp
     *        and remote Ollama based on:
     *
     *          ragpLocalEnabled  &&
     *          ragpDefaultModelFilename resolves to an existing file
     *              &&
     *          VERZETA_HAS_LLAMA is compiled in
     *
     *        Any one of these being false → RemoteBackend is used.
     *        A missing local model file at config time is NOT an
     *        error: the user's intent is "use local" but the file
     *        isn't there yet, so remote carries the session until they
     *        drop the GGUF in and re-configure.
     *
     *        When a LocalLlamaBackend is chosen, its `loadFailed`
     *        signal is wired to a DEFERRED swap to RemoteBackend via
     *        QTimer::singleShot(0, ...) so a load failure triggered
     *        by the backend's own signal emission does not destroy
     *        the sender mid-emit. The same class of mid-emit
     *        mutation crashes a direct swap.
     *
     *        Safe to call any time after construction, and any
     *        number of times; each call builds a fresh backend and
     *        clears the Tier-2 cache.
     *
     * @param settings Pointer to SettingsService (not owned). Must
     *                 be non-null and live for at least the duration
     *                 of the resulting backend.
     */
    void configureRagpBackend(class SettingsService* settings);

    /**
     * @brief Returns the name of the currently-installed RAGP backend.
     * @returns String in the form `local:llama.cpp:<file>.gguf` or
     *          `remote:<providerId>`, or empty when no backend is set.
     *
     * Safe to call from the main thread at any time; a concurrent
     * classify in flight does not race this getter because
     * Ragp::Service::backendName() only reads the unique_ptr's get()
     * result.
     */
    QString ragpBackendName() const;

    /**
     * @brief Access the underlying Ragp::Service (tier-1/2/3 RAGP
     *        classification pipeline). Primary consumer is the
     *        integration test suite, which uses this to inject
     *        scripted IRagpBackend implementations and drive the
     *        cascade path through controlled tier outcomes. Not
     *        exposed to QML: no invokable marker, no property.
     *
     * @return Non-owning pointer; ownership stays with
     *         Chat::CascadeController. Pointer is guaranteed
     *         non-null after construction, but tests should verify
     *         defensively.
     */
    Ragp::Service* ragpService() const;

    /**
     * @brief Attaches a FileService for reading file attachment content.
     * @param fileService Pointer to FileService (not owned).
     */
    void setFileService(FileService* fileService);

    /**
     * @brief Attaches the AgentRegistry so ChatController can resolve
     *        a conversation's primary_agent_id to a full Agent definition
     *        and compose the layered system prompt at send time.
     * @param registry Pointer to AgentRegistry (not owned).
     */
    void setAgentRegistry(AgentRegistry* registry);

    /**
     * @brief Attaches the MembershipService for group-chat routing.
     * @param svc  Non-owning pointer; pass nullptr to detach.
     */
    void setMembershipService(MembershipService* svc);

    // SearchService is owned by AppController and injected directly
    // into the memory-tool classes by Tools::registerAllBuiltInTools.
    // ChatController no longer holds a reference to it.
    //
    // The previous registerMemoryTools() entry point has been removed;
    // Tools::registerAllBuiltInTools (backend/tools/tool-registration.h)
    // now owns memory-tool registration.

    // -----------------------------------------------------------------------
    // Task-lifecycle wiring
    // -----------------------------------------------------------------------

    /**
     * @brief Attaches PlanService for task state persistence.
     * @param svc  Non-owning pointer; pass nullptr to detach.
     */
    void setPlanService(PlanService* svc);

    /**
     * @brief Attaches TaskRunner for dispatch enqueuing.
     * @param runner  Non-owning pointer; pass nullptr to detach.
     */
    void setTaskRunner(TaskRunner* runner);

    /**
     * @brief Attaches the passive TaskObserver used to record tool
     *        calls / files / drafts for the active plan.
     * @param observer  Non-owning pointer; pass nullptr to detach.
     */
    void setTaskObserver(class TaskObserver* observer);

    /**
     * @brief Attaches a SkillService so RequestBuilder can resolve
     *        preferred-skill summaries for the active conversation
     *        and `read_skill_file` is reachable from QML.
     * @param svc  Non-owning pointer; pass nullptr to disable skills.
     */
    void setSkillService(class SkillService* svc);

    /**
     * @brief Attaches HeartbeatConfigService so RequestBuilder can
     *        teach foreground agents that the self-config heartbeat
     *        tools exist and surface any existing schedules for the
     *        active conversation.
     * @param svc  Non-owning pointer; pass nullptr to detach.
     */
    void setHeartbeatConfigService(class HeartbeatConfigService* svc);

    /**
     * @brief Attaches PollService so RequestBuilder can surface open
     *        polls in the ACTIVE POLLS prompt layer.
     * @param svc  Non-owning pointer; null leaves the layer disabled.
     */
    void setPollService(class PollService* svc);

    /**
     * @brief Attaches an AuditService so every terminal-end of an
     *        agent's contribution records an `agent_turn` event with
     *        provider + model + finishReason + tokens + elapsed_ms in
     *        event_detail.
     * @param svc  Non-owning pointer; null disables audit recording.
     *
     * AppController wires this once at startup.
     */
    void setAuditService(class AuditService* svc);

    /**
     * @brief Attaches the per-frontend SessionRouter for cross-instance
     *        ModelRouter slot coordination.
     * @param router  Non-owning pointer; null disables cross-instance
     *                coordination (single-tenant mode).
     *
     *        With multiple ChatController instances (local + per-wire-
     *        session) sharing one ModelRouter, two simultaneous route()
     *        calls would clobber each other's m_currentRequestId on
     *        the shared provider. The earlier instance's chunks would be
     *        tagged with the latest request id and silently dropped by
     *        the earlier instance's stale-signal guard.
     *
     *        With a SessionRouter wired in, ChatController.sendMessage
     *        gates on SessionRouter::anySessionGenerating(). When
     *        another instance holds the slot, the new send is queued
     *        locally (existing m_queuedUserText path). When the other
     *        instance releases the slot (its own m_isGenerating goes
     *        false), SessionRouter emits slotAvailable and queued
     *        instances drain via drainQueuedSendIfPossible().
     *
     *        Passing nullptr is allowed. Test fixtures construct
     *        ChatController without a SessionRouter and behaviour
     *        falls back to single-instance gating on m_isGenerating
     *        only.
     */
    void setSessionRouter(class Verzeta::Session::SessionRouter* router);

    /**
     * @brief Attaches the app-level per-provider dispatch scheduler and
     *        forwards it to every owned (and future) ConversationRun.
     * @param scheduler Non-owning pointer; null disables scheduling
     *                  (runs dispatch immediately, the legacy single-
     *                  instance behaviour used by tests).
     *
     *        The scheduler is the single point of cross-conversation /
     *        cross-session per-provider serialization:
     *        turns to different providers run in parallel, turns to the
     *        same provider serialize. AppController owns one instance and
     *        passes the SAME pointer to the local ChatController and to
     *        every wire-session ChatController so all runs share one
     *        scheduler.
     */
    void setProviderScheduler(class Chat::ProviderScheduler* scheduler);

    /**
     * @brief Attaches the inference-sidecar host (local llama bridge
     *        engine), stored for future runs and forwarded to every
     *        existing run's cascade. Mirrors setProviderScheduler.
     * @param host Non-owning; AppController-owned, outlives all runs.
     */
    void setInferenceSidecarHost(Verzeta::Infer::InferenceSidecarHost* host);

    /**
     * @brief Queues a task-gated dispatch for the next turn.
     * @param req  Dispatch request from TaskRunner.
     *
     * Called by TaskRunner via its enqueue callback. If nothing is
     * in flight the dispatch runs immediately via the normal
     * buildAndSendRequest path with task-gated request fields.
     */
    void enqueueTaskDispatch(const TaskDispatchRequest& req);

    /**
     * @brief Dispatches the requester agent to react to its
     *        sub-agent's delivered report. Called by AppController's
     *        runFinished hook right after the 🤖 report message
     *        persists. Without this the report lands and the
     *        conversation goes silent: the agent that said "I'll
     *        wait for the report" never gets a turn to act on it.
     *
     *        If a generation is in flight the reaction is QUEUED
     *        (deduplicated) and fired at the next cascade boundary by
     *        drainPendingSubagentReactions, so a report never lands
     *        into silence, busy or not. A queued user message always
     *        wins priority over queued reactions. The dispatched turn
     *        is a fresh cascade kickoff governed by all normal turn
     *        caps, echo detection, and claims checking.
     * @param convId         Parent conversation UUID.
     * @param requesterAlias Alias of the agent that spawned the run;
     *                       empty (or unknown) falls back to the
     *                       conversation's primary agent.
     */
    void dispatchSubagentReaction(const QString& convId, const QString& requesterAlias);

    /**
     * @brief Fires the oldest queued sub-agent reaction once no
     *        generation is in flight. Called from onCascadeComplete
     *        after turn-end cleanup; each dispatched reaction drains
     *        the next entry at its own cascade completion.
     */
    void drainPendingSubagentReactions();

    /**
     * @brief Drains pending task dispatches from the queue. Called after
     *        each turn completes (from onRequestFinished) so task work
     *        resumes at natural turn boundaries.
     */
    void drainTaskDispatchQueue();

    /**
     * @brief Attaches the TaskGateService.
     * @param svc  Non-owning pointer.
     *
     * AppController owns the single instance; ChatController's
     * m_taskGate becomes a non-owning pointer the moment this setter
     * runs. Must be called before any chat send / cascade complete /
     * switch-conversation path that reads the active-plan anchor.
     * AppController wires it during initialize() right after
     * constructing the service.
     */
    void setTaskGateService(TaskGateService* svc);

    /**
     * @brief Attaches the SlashCommandService.
     * @param svc  Non-owning pointer.
     *
     * sendMessage uses the service to short-circuit slash-prefixed
     * inputs (returns true → ChatController stops; false → text
     * flows through normal LLM dispatch).
     */
    void setSlashCommandService(SlashCommandService* svc);

    /**
     * @brief Attaches CanvasService for the submit_result auto-promote path.
     * @param svc  Non-owning pointer; null leaves auto-promote disabled.
     *
     * AppController wires this once during initialize().
     */
    void setCanvasService(CanvasService* svc);

    /**
     * @brief Non-owning accessor for the internal `Chat::CascadeController`.
     * @returns Cascade controller pointer (never null after construction).
     *
     * Used by AppController to plumb the responder-identity reference
     * into TaskController::installTaskToolHandlers. CascadeController
     * stays in `Chat::`; `currentResponderAlias` /
     * `currentResponderAgentId` reads from the task-tool handlers go
     * through this ref.
     */
    Chat::CascadeController* cascadeInternal() const;

    /**
     * @brief Non-owning accessor for the internal `Chat::StreamingManager`.
     * @returns Streaming manager pointer (never null after construction).
     *
     * AppController uses this to wire
     * HeartbeatSubagentService::onStreamFinalized to the per-conv
     * stream-collision deferral path. The pointer is owned by
     * ChatController via std::unique_ptr; the heartbeat service holds
     * a QPointer to it so a ChatController teardown does not UAF.
     */
    Chat::StreamingManager* streamingInternal() const;

    /**
     * @brief Non-owning accessor for the in-flight/active run's
     *        deferred-action intent confirmer (built lazily). Test seam
     *        for the deferred-action continuation flows: integration
     *        tests install a synchronous decider via
     *        ActionIntentConfirmer::setTestDecider.
     * @returns Confirmer pointer, or null when no run / no RAGP service.
     */
    Chat::ActionIntentConfirmer* intentConfirmerInternal() const;

    /**
     * @brief Public slot wired to TaskController::taskArtifactReady.
     * @param convId           Conversation UUID.
     * @param agentId          Producing agent template id.
     * @param alias            Producing agent's alias in the project.
     * @param artifactContent  Full artifact text.
     * @param summary          Short artifact summary.
     * @param stepId           Owning plan step id.
     * @param planId           Owning plan id.
     *
     * The submit_result handler runs on a QtConcurrent worker thread;
     * this slot runs on the main thread (where MessageService writes
     * are safe). Posts the artifact as an assistant message in the
     * target conversation.
     */
    void onTaskArtifactReady(const QString& convId,
                             const QString& agentId,
                             const QString& alias,
                             const QString& artifactContent,
                             const QString& summary,
                             const QString& stepId,
                             const QString& planId);

    /**
     * @brief Routes an async image-generation completion to the owning
     *        conversation's run, which resumes the requesting agent
     *        with the finished image (see
     *        ConversationRun::resumeAfterImageGenerated). Wired by
     *        AppController to ImageService::imageReadyForFollowUp.
     * @param convId    Conversation the image belongs to.
     * @param alias     Requesting member alias (empty in 1:1).
     * @param agentId   Requesting agent template id.
     * @param imagePath Absolute path of the finished image.
     */
    void onImageReadyForFollowUp(const QString& convId,
                                 const QString& alias,
                                 const QString& agentId,
                                 const QString& imagePath);

    // NOTE: hasActivePlanInCurrentChat() was removed. PlansModel exposes
    // a signal-backed `hasActiveTask` property; QML binds to that directly.

    /**
     * @brief Returns a human-readable label describing where the
     *        active conversation lives in the folder tree.
     * @returns Examples: "in project: StackBroken", "in org: Acme",
     *          "standalone chat"; empty when no conversation is active.
     *
     * Used by the chat header to show users exactly which project /
     * org scope their chat belongs to.
     */
    Q_INVOKABLE QString activeConversationScopeLabel() const;

    /**
     * @brief Returns the raw folder id of the active conversation.
     * @returns Folder UUID, or empty when the active conversation lives
     *          at root or none is active.
     *
     * Diagnostic helper for sidebar / move-to-folder mismatches.
     */
    Q_INVOKABLE QString activeConversationFolderId() const;

    /**
     * @brief Reports whether a given conversation currently has an LLM
     *        turn in flight, in the FOREGROUND or in the BACKGROUND.
     * @param convId
     *        Conversation UUID to test. Empty returns false.
     * @returns True iff some owned ConversationRun is mid-turn and that
     *          turn targets `convId`, regardless of which conversation
     *          is selected in the foreground.
     *
     * Aggregates across every owned run rather than only the active one:
     * with concurrent multi-chat a turn may be running on a conversation
     * the user has switched away from, and that conversation must still
     * read as "generating". Drives the per-conversation sidebar
     * generating indicator and TaskRunner's heartbeat skip predicate
     * (which must not nudge a conversation that is already working in the
     * background). Pairs with generatingConversationsChanged(), which
     * fires whenever any run's generating state flips so QML bindings on
     * this accessor re-evaluate without polling.
     */
    Q_INVOKABLE bool isConversationGenerating(const QString& convId) const;

    /**
     * @brief Reports whether a given conversation is the one currently
     *        mid-stream, regardless of which conversation is selected
     *        in the foreground.
     * @param convId  Conversation UUID to test.
     * @returns True iff `m_inflightConvId == convId`.
     *
     * Distinct from isConversationGenerating(): that one tracks
     * `m_activeConvId` (the user-selected conv), while this tracks
     * `m_inflightConvId` (the conv whose request is actually in
     * flight). The two diverge when the user switches away mid-stream.
     *
     * HeartbeatSubagentService uses this to defer an auto-surface
     * message-insert into a conversation that is currently mid-stream
     * from a user-driven turn, preserving user-perceived message
     * ordering.
     */
    bool hasInflightStreamFor(const QString& convId) const;

    /**
     * @brief Returns the raw per-instance generation state.
     * @returns Raw `m_isGenerating` field, unconditional.
     *
     * The Q_PROPERTY `isGenerating()` is active-conv-aware (returns
     * false when inflight != active); SessionRouter needs the unmasked
     * truth to track "is THIS ChatController instance currently
     * holding the global ModelRouter foreground slot".
     *
     * Reading is main-thread; the field is mutated only from main, so
     * no synchronization is needed.
     */
    bool isAnyConvGenerating() const;

    // NOTE: activePlansForCurrentChat() was deleted. The push-based
    // PlansModel context property now owns plan-list exposure to QML.
    // PlansOverlay binds directly to PlansModel; it receives row-level
    // updates via PlanService signals and never pulls via Q_INVOKABLE.

    /**
     * @brief Returns the plan id currently anchored to this chat.
     * @returns Plan UUID, or empty when no task is active.
     *
     * Plain public accessor, not exposed to QML. Characterisation
     * tests and non-QML consumers read the active-task anchor through
     * this C++-only accessor. Forwards to
     * `m_taskGate->activePlanId()`.
     */
    QString activeTaskPlanId() const;

    /**
     * @brief Sends a message with file attachment content read inline.
     * @param text            User message text.
     * @param attachmentPaths Absolute paths to files; their content is appended.
     * @sideeffects Reads each file via FileService, appends to message text,
     *              then calls sendMessage() with the combined text.
     */
    Q_INVOKABLE void sendMessageWithAttachments(const QString& text,
                                                const QStringList& attachmentPaths);

    // -----------------------------------------------------------------------
    // Property accessors
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the currently-active conversation id.
     * @returns Conversation UUID, or empty when none is selected.
     */
    QString activeConversationId() const;

    /**
     * @brief Selects which conversation is the foreground chat.
     * @param id  Conversation UUID, or empty to clear the selection.
     */
    void setActiveConversationId(const QString& id);

    /**
     * @brief Returns the title of the active conversation.
     * @returns Display title, or empty when none is selected.
     */
    QString activeConversationTitle() const;

    /**
     * @brief Reports active-conv-aware generation state for the QML
     *        Q_PROPERTY `isGenerating`.
     * @returns True when the in-flight request belongs to the
     *          currently-selected conversation.
     */
    bool isGenerating() const;

    /**
     * @brief Returns the message list model bound to the active conversation.
     * @returns Non-owning pointer to the QML-facing MessageListModel.
     */
    MessageListModel* messages() const;

    /**
     * @brief Returns the user message currently queued behind an in-flight
     *        cascade (empty when nothing is queued).
     * @returns The queued user text, or empty QString.
     */
    QString queuedUserText() const;

    // Session statistics accessors

    /**
     * @brief Returns the running token total for the current session.
     * @returns Cumulative input + output tokens since the controller
     *          was constructed.
     */
    int totalTokens() const;

    /**
     * @brief Returns the estimated USD cost of the current session.
     * @returns Cumulative cost (provider-priced where available).
     */
    double estimatedCostUsd() const;

    /**
     * @brief Returns the elapsed milliseconds of the last completed turn.
     * @returns Wall-clock duration of the most recent reply.
     */
    int lastResponseTimeMs() const;

    /**
     * @brief Returns the number of messages displayed in the active
     *        conversation.
     * @returns Row count from the MessageListModel.
     */
    int messageCount() const;

    /**
     * @brief Number of per-conversation runs currently held in the map.
     * @returns Live run count (including the initial empty-conversation
     *          run before any conversation is selected).
     *
     * Test-only observability for the coordinator's run map. Not exposed
     * to QML.
     */
    int runCountForTest() const;

  signals:
    /** @brief Emitted when the active conversation changes or its title updates. */
    void activeConversationChanged();

    /** @brief Emitted when LLM generation starts or stops. */
    void isGeneratingChanged();

    /**
     * @brief Emitted whenever ANY owned conversation run's generating
     *        state flips (a turn starts or stops on any conversation,
     *        foreground or background).
     *
     * The set of currently-generating conversations is not exposed
     * directly; consumers query isConversationGenerating(convId) per row
     * and re-evaluate that binding on this signal. This is the
     * multi-chat sidebar affordance's change driver: no polling, no
     * timers. Fired from a slot connected to every run's
     * isGeneratingChanged at run-creation time.
     */
    void generatingConversationsChanged();

    /** @brief Emitted when the queued-user-message state changes (set on
     *         queue, cleared on dispatch / cancel). Drives the persistent
     *         "queued, sending next" composer chip. */
    void queuedUserTextChanged();

    /** @brief Internal mirror of AgentSettings.toolsEnabledChanged.
     *         Kept for legacy C++ subscribers; QML consumers should
     *         subscribe to AgentSettings directly. */
    void toolsEnabledChanged();

    /** @brief Emitted when any per-conversation setting changes
     *         (active-conversation switch, etc.). Internal-only now:
     *         QML subscribes to
     *         `AgentSettings.activeConversationSettingsChanged`. The
     *         signal stays because several internal call sites still
     *         emit it on active-conversation transitions. */
    void activeConversationSettingsChanged();

    /**
     * @brief Emitted when a non-fatal error occurs (e.g. provider error).
     * @param message  Human-readable error string.
     */
    void errorOccurred(const QString& message);

    /**
     * @brief Emitted when sendMessage is called while a response is in
     *        flight.
     * @param text  Queued text that will fire once the current turn
     *              (and any cascade behind it) finishes.
     *
     * QML uses this to surface a "queued" indicator + clear the input
     * field instead of silently retaining the typed text.
     */
    void userMessageQueued(const QString& text);

    /**
     * @brief Aggregated stream-finalize signal: re-emitted whenever ANY
     *        owned ConversationRun's StreamingManager finalizes a
     *        streaming message.
     *
     * With per-conversation runs each owning their own
     * StreamingManager, a consumer that needs "a foreground stream just
     * finalized" (HeartbeatSubagentService's Tier-2 drain pump) can no
     * longer connect to a single startup-pinned StreamingManager. It
     * connects here instead and gets every run's finalize, regardless of
     * which run is foreground.
     *
     * @param msgId        Finalized message UUID.
     * @param finishReason Final finish reason for the stream.
     * @param ok           Whether the finalize persisted successfully.
     */
    void streamFinalized(const QString& msgId, const QString& finishReason, bool ok);

    /** @brief Emitted when any statistics property changes. */
    void statsChanged();

    /**
     * @brief Emitted when the LLM requests a tool call.
     * @param toolName  Canonical tool name.
     * @param argsJson  Encoded argument JSON.
     */
    void toolCallStarted(const QString& toolName, const QString& argsJson);

    /**
     * @brief Emitted when a tool invocation completes.
     * @param toolName    Canonical tool name.
     * @param resultJson  Encoded result JSON.
     */
    void toolCallCompleted(const QString& toolName, const QString& resultJson);

    // artifactsChanged() and toolCallLogChanged() signals were removed.
    // ArtifactsModel and ToolCallLogModel emit their own row-level
    // Qt Model/View signals when MessageService data changes.

    /**
     * @brief Emitted when contextFillPercent was recomputed (a new
     *        request build measured the context window usage).
     */
    void contextFillPercentChanged();

    /** @brief Emitted when the compaction-cadence progress was recomputed. */
    void compactionCadenceChanged();

    /**
     * @brief Emitted when the compaction-in-progress state for the active
     *        conversation changes (start / completion, or a conversation
     *        switch into/out of the conversation being compacted).
     */
    void isCompactingChanged();

    /** @brief Emitted when the deferred-refresh waiting state changes. */
    void compactionWaitingForIdleChanged();

    /**
     * @brief Emitted when an agent in a group chat \@mentions the user
     *        (e.g. `\@owner`, `\@user`, `\@you`, `\@leader`). Also fires a
     *        desktop notification; this signal lets the QML UI show an
     *        in-app banner or badge.
     * @param conversationId Group chat UUID.
     * @param agentAlias     Alias of the agent that mentioned the user.
     * @param messageText    The full assistant message content.
     */
    void userMentionedInGroup(const QString& conversationId,
                              const QString& agentAlias,
                              const QString& messageText);

    /**
     * @brief Forwarded from CascadeController: @-mention routing was
     *        suppressed (classifier confidence below the dispatch
     *        floor while targets were present). QML surfaces a
     *        conversation badge so the user can see WHY a group chat
     *        stalled instead of guessing.
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
     * @brief Emitted whenever the RAGP backend changes, either via
     *        an explicit `configureRagpBackend` call (e.g. user
     *        toggled the setting) or via an auto-fallback triggered
     *        by LocalLlamaBackend::loadFailed. AppController
     *        listens to keep its `ragpBackendLive` Q_PROPERTY in
     *        sync so the Settings UI chip reflects the LIVE backend
     *        (which may differ from the CONFIGURED intent after an
     *        auto-fallback).
     * @param backendName Value of `ragpBackendName()` after the swap.
     *
     */
    void ragpBackendChanged(const QString& backendName);

  public slots:
    /**
     * @brief Cache-mirror slot for the agent-pattern setting.
     * @param patternName  Pattern name from
     *                     AgentSettingsController::agentPatternChanged.
     *
     * Keeps `m_agentPattern` in sync with the canonical name owned by
     * AgentSettings. Read synchronously in `sendMessage`.
     */
    void onExternalAgentPatternChanged(const QString& patternName);

    /**
     * @brief Cache-mirror slot for the require-confirmation toggle.
     * @param require  New flag value.
     *
     * Updates `m_requireConfirmation` from AgentSettingsController;
     * read synchronously when constructing AgentConfig.
     */
    void onExternalRequireConfirmationChanged(bool require);

    /**
     * @brief Cache-mirror slot for the tools-enabled toggle.
     * @param enabled  New flag value.
     *
     * Updates `m_toolsEnabled`; read synchronously in `sendMessage`
     * / `buildAndSendRequest` when building tool inputs.
     */
    void onExternalToolsEnabledChanged(bool enabled);

    /**
     * @brief Drains queued user text if the global ModelRouter slot is
     *        now available.
     *
     * Called when SessionRouter emits `slotAvailable` (some other
     * ChatController instance just released the slot). If this
     * instance has `m_queuedUserText` non-empty AND its own
     * `m_isGenerating` is false AND no other instance has re-acquired
     * the slot in the meantime, re-invokes sendMessage with the
     * queued text. Otherwise no-op (the text stays queued for the
     * next slotAvailable signal).
     *
     * Public so SessionRouter's lambda subscriber to slotAvailable
     * can call it. Runs on the main thread.
     */
    void drainQueuedSendIfPossible();

    /**
     * @brief SessionRouter slotAvailable entry point: drains the
     *        queued user send first (priority), then any queued
     *        sub-agent reactions. The reaction drain must live here
     *        and not only in onCascadeComplete: when ANOTHER
     *        session's controller held the foreground slot, the
     *        local onCascadeComplete never fires, and reactions
     *        queued behind that session would otherwise wait forever.
     */
    void onSlotAvailable();

    /**
     * @brief Refreshes the active-canvas metadata cache from CanvasService.
     * @param filename   Canvas filename.
     * @param language   Canvas language tag.
     * @param revision   Current revision number.
     * @param lineCount  Total line count.
     * @param byteSize   Total byte size.
     *
     * Wired by AppController to CanvasService::canvasOpened /
     * canvasUpdated / canvasClosed via a small adapter lambda. The
     * cache feeds buildAndSendRequest which feeds RequestBuilder's
     * metadata-only prompt layer.
     */
    void onActiveCanvasMetadataRefresh(const QString& filename,
                                       const QString& language,
                                       int revision,
                                       int lineCount,
                                       qint64 byteSize);

    /**
     * @brief Post-task-creation coordination slot.
     * @param planId        Plan UUID just created.
     * @param convId        Conversation UUID the task lives in.
     * @param ownerAlias    Owner agent's alias in the project.
     * @param ownerAgentId  Owner agent template id.
     *
     * Wired by AppController to TaskController::taskStarted. When the
     * newly-created task lives in the currently-active conversation
     * AND no request is in flight, applies the active-plan anchor
     * (via m_taskGate), sets the cascade responder, opens a streaming
     * placeholder for the owner's first reply, and kicks off the LLM
     * turn via buildAndSendRequest.
     */
    void onExternalTaskStarted(const QString& planId,
                               const QString& convId,
                               const QString& ownerAlias,
                               const QString& ownerAgentId);

    /**
     * @brief Single-plan stop coordination.
     * @param planId  Plan UUID that was stopped.
     *
     * Wired to TaskController::planStopped. If the stopped plan was
     * the active-conversation's anchored plan, stops generation +
     * resets cascade + clears the anchor.
     */
    void onExternalPlanStopped(const QString& planId);

    /**
     * @brief Stop-all coordination.
     * @param convId  Conversation whose plans were all stopped.
     *
     * Wired to TaskController::allPlansStoppedInConversation. If the
     * conversation is the active one, cancels the in-flight request,
     * resets cascade, and clears the active-plan anchor.
     */
    void onExternalAllPlansStoppedInConversation(const QString& convId);

    /**
     * @brief Pre-delete coordination slot.
     * @param id  Conversation UUID about to be deleted.
     *
     * Wired by AppController to
     * ConversationController::conversationAboutToBeDeleted. Cancels
     * any in-flight LLM request whose inflight / active conversation
     * id matches the row being deleted. Fires BEFORE the
     * conversation row is removed so the cancel path's DB writes
     * land on a still-valid FK.
     */
    void onExternalConversationAboutToBeDeleted(const QString& id);

    /**
     * @brief Post-delete coordination slot.
     * @param id  Conversation UUID that was deleted.
     *
     * Wired to ConversationController::conversationDeleted. Clears
     * the active-conversation state (and the last-user-message /
     * streaming caches) when the deleted id matches the currently-
     * active conversation.
     */
    void onExternalConversationDeleted(const QString& id);


  private:
    // Core services. References stay here because a handful of
    // ChatController-resident, non-per-turn methods read them
    // (activeArtifactsPath, activeConversationScopeLabel,
    // lastAssistantMessage, activeConvConfig). The same pointers are
    // forwarded into every run via the set*() forwarders.
    ModelRouter& m_router;
    ConversationService& m_convSvc;
    MessageService& m_msgSvc;
    ExportService& m_exportSvc;
    MessageListModel* m_msgModel;  ///< Owned by this controller (child QObject)

    // FileService is the one optional service read by a ChatController-
    // resident method (activeArtifactsPath) AND needed by every run, so we
    // keep a non-owning mirror here in addition to forwarding it.
    FileService* m_fileService = nullptr;

    /**
     * @brief Snapshot of every optional service attached via the set*()
     *        forwarders, so a lazily-created run can be wired with the
     *        exact same collaborators the existing runs have.
     *
     * All pointers are non-owning (AppController owns the instances); each
     * field mirrors one set*() forwarder. No lifecycle is managed here.
     */
    struct AttachedServices {
        Verzeta::Infer::InferenceSidecarHost* inferenceSidecarHost = nullptr;
        ToolService* toolService = nullptr;
        RagService* ragService = nullptr;
        Chat::MemoryRetriever* memoryRetriever = nullptr;
        FileService* fileService = nullptr;
        AgentRegistry* agentRegistry = nullptr;
        MembershipService* membershipService = nullptr;
        PlanService* planService = nullptr;
        TaskRunner* taskRunner = nullptr;
        class TaskObserver* taskObserver = nullptr;
        class SkillService* skillService = nullptr;
        class HeartbeatConfigService* heartbeatConfigService = nullptr;
        class PollService* pollService = nullptr;
        class AuditService* auditService = nullptr;
        Verzeta::Session::SessionRouter* sessionRouter = nullptr;
        TaskGateService* taskGate = nullptr;
        SlashCommandService* slashService = nullptr;
        CanvasService* canvasSvc = nullptr;
        AgentService* agentService = nullptr;
        class ConversationSummarizer* summarizer = nullptr;
        class SettingsService* ragpSettings = nullptr;  // last configureRagpBackend arg
        Chat::ProviderScheduler* providerScheduler = nullptr;
    };
    AttachedServices m_services;

    /**
     * @brief Per-conversation execution engines, keyed by conversation
     *        id. One run per conversation that has become active or holds
     *        a turn. Each run is a child QObject of this controller (Qt
     *        parent ownership) AND held by std::unique_ptr here, so it is
     *        destroyed deterministically when removed from the map or when
     *        ChatController is destroyed, before the shared service
     *        references it captured (those services outlive ChatController;
     *        AppController destroys them after it). Runs hold non-owning
     *        pointers to the shared services, never owning them.
     *
     * std::unordered_map (not QHash) because the value type is move-only;
     * <QHash> is included above purely to supply std::hash<QString>.
     */
    std::unordered_map<QString, std::unique_ptr<Chat::ConversationRun>> m_runs;

    /**
     * @brief The conversation id whose run currently backs the QML facade.
     *        Empty before any conversation is selected. switchConversation
     *        re-points this and re-wires the facade forwarding.
     */
    QString m_activeConvId;

    /**
     * @brief Non-owning pointer to the active conversation's run (the run
     *        m_runs[m_activeConvId] resolves to), or nullptr when no
     *        conversation is active. Cached so the hot facade getters do
     *        not re-hash on every call.
     */
    Chat::ConversationRun* m_activeRun = nullptr;

    /**
     * @brief Live facade-forwarding connections from the ACTIVE run to
     *        this controller's signals. Disconnected and rebuilt by
     *        repointFacade() on every active-run change so exactly one
     *        run drives the facade at a time.
     */
    QList<QMetaObject::Connection> m_facadeConns;

    /**
     * @brief Returns the run for a conversation, creating and fully
     *        wiring a new one when none exists yet.
     * @param convId         Conversation id to resolve.
     * @param createIfMissing
     *                       When true and no run exists, constructs one,
     *                       attaches every currently-known service, and
     *                       inserts it into the map; when false, returns
     *                       nullptr for an unknown conversation.
     * @returns The conversation's run, or nullptr when convId is empty or
     *          (createIfMissing == false) no run exists.
     */
    Chat::ConversationRun* runFor(const QString& convId, bool createIfMissing);

    /**
     * @brief Ensures a run backs the facade, creating one for an empty
     *        conversation when none is selected yet.
     * @returns The active run (never nullptr after a successful call).
     *
     * Used by the send entry points: with no conversation selected the
     * run autocreates its conversation on first send and re-keys the map
     * via its activeConversationChanged signal.
     */
    Chat::ConversationRun* ensureActiveRun();

    /**
     * @brief Returns the run backing the active view.
     * @returns m_activeRun (may be nullptr when no conversation is
     *          active).
     */
    Chat::ConversationRun* activeRun() const { return m_activeRun; }

    /**
     * @brief Returns the run that currently holds the in-flight turn, if
     *        any, else the active run.
     * @returns The generating run, or the active run, or nullptr.
     *
     * Dispatch is serialized, so at most one run is generating
     * at a time. The collaborator accessors (cascadeInternal /
     * streamingInternal) resolve through this so external consumers that
     * read responder identity DURING a turn see the run actually driving
     * it, even when the user has switched the foreground elsewhere.
     */
    Chat::ConversationRun* inflightOrActiveRun() const;

    /**
     * @brief Moves a run's entry in the map from one conversation key to
     *        another (no-op when the keys match or the slot does not hold
     *        the given run).
     * @param oldKey Current map key.
     * @param newKey Destination map key.
     * @param run    The run expected at oldKey (guards against clobbering).
     */
    void rekeyRun(const QString& oldKey, const QString& newKey, Chat::ConversationRun* run);

    /**
     * @brief Re-keys the active run's map slot when the run adopted a new
     *        conversation id (first-send autocreate) and updates
     *        m_activeConvId to match.
     */
    void rekeyActiveRunIfNeeded();

    /**
     * @brief Constructs and fully wires a brand-new run for a conversation.
     * @param convId Conversation id the run represents.
     * @returns Non-owning pointer to the inserted run.
     *
     * Wires the run's change signals are NOT done here (that is
     * repointFacade's job, and only for the active run); this attaches the
     * services and primes the run's view state to convId.
     */
    Chat::ConversationRun* createRun(const QString& convId);

    /**
     * @brief Points the facade at a conversation's run: tears down the
     *        previous active run's facade-forwarding connections, builds
     *        new ones for the target run, updates m_activeConvId /
     *        m_activeRun, and re-emits the facade change signals so QML
     *        rebinds to the newly-active run's state.
     * @param run    The run to make active (may be nullptr to detach,
     *               e.g. after the active conversation is deleted).
     * @param convId The conversation id the run represents (empty when
     *               run is nullptr).
     */
    void repointFacade(Chat::ConversationRun* run, const QString& convId);

    /**
     * @brief Builds the facade-forwarding connections from a run to this
     *        controller's signals.
     * @param run The run whose change signals drive the facade.
     *
     * Stores the connections in m_facadeConns so repointFacade can tear
     * them down when the active run changes. Member-function → signal
     * connects only (no Qt::UniqueConnection-with-lambda).
     */
    void connectFacade(Chat::ConversationRun* run);

  public:
    /**
     * @brief Rebuilds the in-flight LLM history from persisted messages,
     *        reconstructing well-formed tool_calls ↔ tool pairs so every
     *        `role=tool` message is preceded by an `assistant` message
     *        whose `tool_calls` array contains a matching `id`, exactly
     *        as required by the OpenAI-compatible tool calling contract
     *        that Ollama and every other provider inherits.
     *
     * Algorithm (single forward pass with look-ahead):
     *
     *   for each DB message m:
     *     - user/system/legacy-assistant → emit as-is (with group
     *       "(Alias said)\n" prefix for group-chat assistants)
     *     - assistant with finish=tool_calls:
     *         load tool_calls rows for m.id via MessageService.
     *         if rows exist, look ahead across the next contiguous
     *         run of role=tool messages and match each row by
     *         `metadata.tool_call_id`. If the full set pairs,
     *         emit one assistant LlmMessage with a populated
     *         toolCallsJson + one tool LlmMessage per paired
     *         response (with toolCallId set). Advance past the
     *         paired tool rows.
     *         If pairing is incomplete (orphan assistant or
     *         partial pairing), emit the assistant WITHOUT its
     *         tool_calls field and SKIP any unpaired tool rows.
     *         This preserves the assistant's visible content while
     *         preventing a malformed tool sequence from reaching
     *         the provider.
     *     - bare role=tool not preceded by a matching assistant:
     *         skip. Orphan tool rows would otherwise produce an
     *         unanchored sequence that small local models (qwen3,
     *         llama3-class) respond to by emitting a single stop
     *         token.
     *
     * @param dbMessages  Raw messages from MessageService::getMessages.
     * @param isGroupChat True if the conversation is a group chat; used
     *                    to decide whether to prefix assistant content
     *                    with "(Alias said)\n" for disambiguation.
     * @param excludeMsgId UUID to skip (typically the current streaming
     *                    placeholder which isn't persisted yet).
     * @return Provider-ready history list.
     *
     * All agent turns emit as role=assistant with "(Alias said)\n"
     * prefix in group chats (see request-builder.cpp). The
     * cascade-tail EOS problem is solved separately by appending a
     * synthetic role=system nudge before dispatch.
     *
     * Public so integration tests using a mock provider can
     * reconstruct the exact payload they'd receive on the wire.
     */
    QList<LlmMessage> assembleLlmHistory(const QList<Message>& dbMessages,
                                         bool isGroupChat,
                                         const QString& excludeMsgId);

  private:
    /**
     * @brief Loads the active conversation's LlmConfig from DB.
     * @return Current LlmConfig, or default-constructed if none is active.
     */
    LlmConfig activeConvConfig() const;
};

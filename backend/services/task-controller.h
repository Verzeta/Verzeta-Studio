// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file task-controller.h
 * @brief QML-exposed controller that owns the user-facing task-lifecycle
 *        API (start task, stop task, step interventions, primary-agent
 *        selection). Registered as the `Tasks` QML singleton by
 *        AppController, alongside `ChatController` (chat session) and
 *        `Conversations` (conversation CRUD).
 * @layer Service (UI orchestration)
 * @dependencies ConversationService, MessageService, PlanService,
 *               TaskRunner (non-owning refs); AgentRegistry,
 *               MembershipService (optional pointers attached via
 *               setters; AppController wires them after construction).
 *
 * Star-topology refusal: TaskController does NOT hold a
 * ChatController pointer. Cross-controller effects (dispatching a
 * fresh LLM turn after a task is created, stopping in-flight work
 * when the active plan is stopped, resetting cascade / anchor on
 * stop-all) run through signals the ChatController subscribes to
 * in AppController's wiring.
 *
 * TaskController also owns the task-tool handler installation
 * (`installTaskToolHandlers`: `start_task`, `submit_result`,
 * `report_blocked`, `update_plan_step`, `get_plan_status`,
 * `stop_task`) and the task-tool stub schema registration
 * (`registerTaskToolStubs`: the ten task tools registered with
 * `ToolKind::BuiltIn`). Handler bodies read the active-plan anchor
 * via TaskGateService and the current responder identity via
 * `Chat::CascadeController`, both passed in to
 * `installTaskToolHandlers` so the handler lambdas can capture
 * them by reference safely.
 *
 * Threading: strictly main-thread. Every public method asserts via
 * VERZETA_ASSERT_MAIN_THREAD().
 *
 * Ownership: constructed as `std::unique_ptr<TaskController>` on
 * AppController. AppController is the QObject parent. QML holds a
 * non-owning singleton pointer via `qmlRegisterSingletonInstance`.
 */
#pragma once

#include <functional>
#include <QObject>
#include <QString>

class AgentRegistry;
class ConversationService;
class FileService;
class MembershipService;
class MessageService;
class PlanService;
class TaskGateService;
class TaskRunner;
class ToolService;

namespace Chat {
class CascadeController;
}

/**
 * @brief QML singleton for user-facing task-lifecycle actions.
 */
class TaskController : public QObject {
    Q_OBJECT

  public:
    /**
     * @param convSvc    Non-owning reference; folder-chain lookups
     *                   for project / org scope resolution inside
     *                   userInitiatedStartTask, plus primary-agent
     *                   reads/writes for setPrimaryAgent /
     *                   primaryAgentId.
     * @param msgSvc     Non-owning reference; persists the goal
     *                   message + role=system task-event rows.
     * @param planSvc    Non-owning reference; active-plans-for-
     *                   conversation scan for stopAll.
     * @param taskRunner Non-owning reference; receives the
     *                   startTaskFromToolCall / handleStopTask /
     *                   userRetryStep / userOverrideStepAsDone /
     *                   userSkipStep calls the API drives.
     * @param parent     Qt parent (AppController).
     */
    explicit TaskController(ConversationService& convSvc,
                            MessageService& msgSvc,
                            PlanService& planSvc,
                            TaskRunner& taskRunner,
                            QObject* parent = nullptr);
    ~TaskController() override;

    /**
     * @brief Attach an optional AgentRegistry. Consumed by
     *        userInitiatedStartTask's fallback default-owner lookup
     *        in 1:1 chats with a primary agent. Null uses a generic
     *        "assistant" label.
     * @param registry Non-owning pointer; pass nullptr to detach.
     */
    void setAgentRegistry(AgentRegistry* registry);

    /**
     * @brief Attach an optional MembershipService. Consumed by
     *        userInitiatedStartTask's group-chat owner resolution
     *        (\@mention → roster canonicalisation → default
     *        responder). Null skips the group branch; 1:1 chats
     *        still work.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setMembershipService(MembershipService* svc);

    /**
     * @brief Attach an optional FileService. Consumed by
     *        `installTaskToolHandlers`'s `submit_result` body for
     *        the mirror markdown file write. Null skips the mirror
     *        write; the tool result simply omits the `artifact_path`
     *        key.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setFileService(FileService* svc);

    // -----------------------------------------------------------------
    // Task-tool handler installation
    //
    // Installs the six real task-lifecycle tool handlers
    // (`start_task`, `submit_result`, `report_blocked`,
    // `update_plan_step`, `get_plan_status`, `stop_task`) onto the
    // supplied ToolService via `replaceHandler`. Must be called
    // AFTER `registerTaskToolStubs` has registered the ten task-tool
    // stub schemas.
    //
    // Handlers READ active-plan anchor state from the supplied
    // `TaskGateService` reference and responder identity from the
    // supplied `Chat::CascadeController` reference. Both refs MUST
    // outlive every handler invocation; AppController declares both
    // services BEFORE m_toolService so reverse-declaration
    // destruction tears down the handler-owning ToolService first —
    // its handler lambdas die before the refs do.
    //
    // The submit_result handler emits `taskArtifactReady` (signal
    // below) instead of taking a write-back callback. AppController
    // wires the signal to `ChatController::onTaskArtifactReady` via
    // `Qt::QueuedConnection` so the artifact-as-message DB write
    // happens on the main thread (the handlers themselves run on a
    // QtConcurrent worker thread per the tool-dispatch path).
    /**
     * @brief Resolver returning the CascadeController of the run that is
     *        the right responder-identity source AT HANDLER-INVOKE TIME.
     *
     * With concurrent multi-chat, a task tool can fire on a
     * backgrounded run while a different conversation is foreground, so a
     * single startup-pinned CascadeController is wrong. The resolver
     * (ChatController::cascadeInternal, which routes through
     * inflightOrActiveRun()) returns the in-flight run's cascade per
     * call. May return nullptr if no run is resolvable; handlers degrade
     * gracefully (fall back to default owner / "assistant").
     */
    using CascadeResolver = std::function<Chat::CascadeController*()>;

    /**
     * @brief Install the real task-tool handlers, replacing the
     *        registered stubs.
     * @param toolSvc            Target tool registry. Required.
     * @param taskGate           Active-plan anchor ref. Required.
     * @param cascadeResolver    Returns the responder's cascade at
     *                           handler-invoke time; see CascadeResolver.
     * @param activeConvIdGetter Returns ChatController's
     *                           m_activeConvId at handler-invoke
     *                           time. Captured by value into each
     *                           handler lambda.
     */
    void installTaskToolHandlers(ToolService& toolSvc,
                                 TaskGateService& taskGate,
                                 CascadeResolver cascadeResolver,
                                 std::function<QString()> activeConvIdGetter);

    // -----------------------------------------------------------------
    // Task-tool schema registration
    //
    // Registers the six implemented task-tool schemas (start_task,
    // update_plan_step, get_plan_status, submit_result, report_blocked,
    // stop_task) with `ToolKind::BuiltIn` and main-thread residency so
    // they participate in the m_builtInNames invariant that
    // ToolService::addCustomTool / loadCustomTools use to reject
    // colliding custom-tool names. Each is registered with a placeholder
    // handler that `installTaskToolHandlers` replaces with the real body.
    //
    // The four earlier stub-only tools (delegate_task / add_plan_step /
    // approve_step / reject_step) were never implemented and are NOT
    // registered — advertising un-implemented tools to the model wasted
    // prompt tokens and produced "not yet implemented" errors.
    //
    // Init order (AppController::initialize): registerBuiltInTools
    // → m_taskController construction → registerTaskToolStubs
    // (this method) → loadCustomTools / McpService →
    // installTaskToolHandlers.
    /**
     * @brief Register the six implemented task-tool schemas with the tool
     *        registry (placeholder handlers, replaced by installTaskToolHandlers).
     * @param toolSvc Target tool registry. Required.
     */
    void registerTaskToolStubs(ToolService& toolSvc);

    // -----------------------------------------------------------------
    // Q_INVOKABLE — user-initiated task lifecycle
    // -----------------------------------------------------------------

    /**
     * @brief Starts a task in the supplied conversation. Persists
     *        the user's goal as a role=user message, resolves the
     *        owner (coordinator / primary-agent / \@mention
     *        target), creates a plan row via TaskRunner, then emits
     *        `taskStarted(planId, convId, ownerAlias, ownerAgentId)`
     *        so ChatController can dispatch the first LLM turn.
     *        ChatController's slot applies the active-plan anchor
     *        and kicks the streaming machinery.
     * @param convId   Target conversation. Caller passes
     *                 `ChatController.activeConversationId` typically.
     * @param goalText Free-text task goal.
     */
    Q_INVOKABLE void userInitiatedStartTask(const QString& convId, const QString& goalText);

    /**
     * @brief Marks a specific plan as stopped via TaskRunner. Emits
     *        `planStopped(planId)` on success so ChatController can
     *        clear the anchor and stop generation if the plan was
     *        the active one.
     * @param planId Plan UUID to stop.
     * @param reason Human-readable stop reason.
     * @returns true on success (plan transitioned to Failed /
     *          Stopped).
     */
    Q_INVOKABLE bool stopPlan(const QString& planId, const QString& reason);

    /**
     * @brief Marks every active plan in a given conversation as
     *        stopped. Emits
     *        `allPlansStoppedInConversation(convId)` when at least
     *        one plan was stopped, so ChatController can tear down
     *        in-flight work on that conversation if it's the active
     *        one.
     * @param convId Conversation UUID whose active plans should be
     *               stopped.
     * @returns Number of plans stopped.
     */
    Q_INVOKABLE int stopAllActivePlansInConversation(const QString& convId);

    /**
     * @brief Retry a step: TaskRunner resets counters, stores user
     *        note as context, re-dispatches.
     * @param stepId   Step UUID to retry.
     * @param userNote User-supplied corrective note (may be empty).
     * @returns true on success; false on unknown step.
     */
    Q_INVOKABLE bool retryStep(const QString& stepId, const QString& userNote);

    /**
     * @brief Force-mark a step as done without executor
     *        verification.
     * @param stepId Step UUID to force-complete.
     * @returns true on success; false on unknown step.
     */
    Q_INVOKABLE bool overrideStepAsDone(const QString& stepId);

    /**
     * @brief Skip a step: marks blocked, plan continues with
     *        remaining steps.
     * @param stepId Step UUID to skip.
     * @param reason Human-readable skip reason.
     * @returns true on success; false on unknown step.
     */
    Q_INVOKABLE bool skipStep(const QString& stepId, const QString& reason);

    /**
     * @brief Sets the primary agent for the given conversation. The
     *        ConversationService::conversationUpdated signal the
     *        underlying update emits triggers ChatController's
     *        cached-title / settings refresh for free.
     * @param convId  Conversation UUID.
     * @param agentId Agent UUID to pin as primary.
     */
    Q_INVOKABLE void setPrimaryAgent(const QString& convId, const QString& agentId);

    /**
     * @brief Returns the primary agent id of the supplied conv.
     * @param convId Conversation UUID.
     * @returns Agent UUID, or empty when there is no primary agent
     *          or the conversation is absent.
     */
    Q_INVOKABLE QString primaryAgentId(const QString& convId) const;

    // -----------------------------------------------------------------
    // Internal — TaskRunner → Tasks signal endpoint
    // -----------------------------------------------------------------

    /**
     * @brief Posts a role=system task-event message via
     *        MessageService. Wired directly to
     *        `TaskRunner::taskEventRequested` in AppController, so
     *        task-started / step-done / task-completed events appear
     *        inline in the chat. Not Q_INVOKABLE; no QML caller.
     * @param convId    Target conversation.
     * @param role      Message role; expected to be "system".
     * @param content   Rendered event text.
     * @param eventType Metadata key used by MessageBubble to style
     *                  the row (task_started / task_completed /
     *                  step_done / etc.).
     */
    void postTaskEventMessage(const QString& convId,
                              const QString& role,
                              const QString& content,
                              const QString& eventType);

  signals:
    /**
     * @brief Fired after a task has been created via
     *        userInitiatedStartTask. ChatController subscribes to
     *        dispatch the first LLM turn when the task lives in the
     *        active conversation.
     * @param planId       Newly-created plan UUID.
     * @param convId       Owning conversation UUID.
     * @param ownerAlias   Alias of the first-step owner.
     * @param ownerAgentId Resolved agent UUID for the first-step
     *                     owner.
     */
    void taskStarted(const QString& planId,
                     const QString& convId,
                     const QString& ownerAlias,
                     const QString& ownerAgentId);

    /**
     * @brief Fired after a single plan is stopped. ChatController
     *        subscribes to clear its active-plan anchor and (if the
     *        plan was the in-flight active task) stop generation.
     * @param planId UUID of the stopped plan.
     */
    void planStopped(const QString& planId);

    /**
     * @brief Fired after stopAllActivePlansInConversation completes
     *        with at least one stopped plan. ChatController
     *        subscribes to (if convId == active) stopGeneration +
     *        reset cascade + clear anchor.
     * @param convId Conversation UUID whose active plans were
     *               stopped.
     */
    void allPlansStoppedInConversation(const QString& convId);

    /**
     * @brief User-visible error channel for task-lifecycle failures.
     *        QML binds this alongside ChatController.errorOccurred
     *        and Conversations.errorOccurred on the same error
     *        banner.
     * @param message Human-readable error description.
     */
    void errorOccurred(const QString& message);

    /**
     * @brief Cross-thread bridge for the submit_result handler.
     *        AppController connects this to
     *        `ChatController::onTaskArtifactReady` via
     *        `Qt::QueuedConnection` so the body marshals from the
     *        QtConcurrent worker thread (where submit_result runs)
     *        to the main thread (where MessageService writes are
     *        safe).
     *
     * @param convId    Conversation that owns the plan + step.
     * @param agentId   Responder agent id (cascade currentResponder).
     * @param alias     Responder alias.
     * @param content   Raw artifact content.
     * @param summary   Short summary string.
     * @param stepId    Plan step id.
     * @param planId    Plan id.
     */
    void taskArtifactReady(const QString& convId,
                           const QString& agentId,
                           const QString& alias,
                           const QString& content,
                           const QString& summary,
                           const QString& stepId,
                           const QString& planId);

  private:
    // Per-tool handler installers — split for top-down readability,
    // mirroring the structure of the deleted
    // `Chat::TaskToolHandlers::register*Handler` private members.
    void registerStartTaskTool(ToolService& toolSvc,
                               TaskGateService& taskGate,
                               CascadeResolver cascadeResolver,
                               std::function<QString()> getter);
    void registerGetPlanStatusTool(ToolService& toolSvc,
                                   CascadeResolver cascadeResolver,
                                   std::function<QString()> getter);
    void registerStopTaskTool(ToolService& toolSvc, TaskGateService& taskGate);
    void registerCompleteTaskTool(ToolService& toolSvc,
                                  TaskGateService& taskGate,
                                  CascadeResolver cascadeResolver,
                                  std::function<QString()> activeConvIdGetter);

    ConversationService& m_convSvc;
    MessageService& m_msgSvc;
    PlanService& m_planSvc;
    TaskRunner& m_taskRunner;

    AgentRegistry* m_agentRegistry = nullptr;
    MembershipService* m_membershipService = nullptr;
    FileService* m_fileSvc = nullptr;
};

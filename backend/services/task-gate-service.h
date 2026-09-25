// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file task-gate-service.h
 * @brief Top-level service that owns the task-gated turn lifecycle.
 *        First-class service consumed by both ChatController and
 *        TaskController, not a chat-session internal.
 * @layer Service
 * @dependencies Qt6::Core, MessageService (non-owning reference),
 *               PlanService / TaskRunner / TaskObserver (non-owning
 *               pointers, attached via setters; AppController
 *               wires them AFTER service construction).
 *
 * Owns:
 *   - the active-plan anchor (the plan id the current conversation
 *     is "focused on", if any);
 *   - the PlanService::planStatusChanged listener that clears the
 *     anchor whenever the anchored plan reaches a terminal status
 *     (otherwise the next turn's system prompt would inject stale
 *     "ACTIVE TASK" framing for a completed plan);
 *   - the declared-status marker handling (agent emits
 *     \<task_state\>completed|blocked|…\</task_state\>);
 *   - the implicit-completion heuristic (active plan + tool call
 *     executed + finish=stop + no declared status = Completed);
 *   - the TaskObserver text-artifact hook (records substantive
 *     responder text as a Draft row on the active step);
 *   - the user-cancel plan-stop path;
 *   - the switch-conversation anchor clear + auto-resume from the
 *     destination conversation's most-recent active plan;
 *   - postEvent: the system-message insertion used both internally
 *     (declared / implicit-completion paths) and by callers like
 *     TaskController::postTaskEventMessage when TaskRunner asks to
 *     log "task started" / "task completed" events into the chat.
 *
 * Ownership: AppController owns the single instance via
 *   `std::unique_ptr<TaskGateService> m_taskGateService;`
 * declared BEFORE m_chatController so reverse-declaration
 * destruction orders Chat → ConversationController → TaskController
 * → AgentSettings → Export → TaskGateService → core services.
 * Both ChatController and TaskController hold non-owning pointers,
 * attached via setters during AppController::initialize() after
 * the service is constructed and its PlanService / TaskRunner /
 * TaskObserver dependencies are wired.
 *
 * Threading: strictly main-thread. Every public method begins with
 *   VERZETA_ASSERT_MAIN_THREAD();
 */

#pragma once

#include <QObject>
#include <QString>

class MessageService;
class PlanService;
class TaskObserver;
class TaskRunner;

/**
 * @brief Main-thread-only top-level service. Does NOT expose
 *        Q_PROPERTY / Q_INVOKABLE. The QML-facing surface for
 *        task lifecycle lives on TaskController (the `Tasks`
 *        singleton); this service is the C++-only state machine
 *        ChatController and TaskController coordinate through.
 */
class TaskGateService : public QObject {
    Q_OBJECT

  public:
    /**
     * @param msgSvc  Non-owning reference; used by postEvent() to
     *                INSERT role=system task_event message rows for
     *                terminal state transitions. Must outlive this
     *                service (AppController owns both via
     *                unique_ptr in dependency-correct declaration
     *                order; MessageService outlives TaskGateService
     *                because it's declared earlier in
     *                app-controller.h).
     * @param parent  Qt parent (AppController).
     */
    explicit TaskGateService(MessageService& msgSvc, QObject* parent = nullptr);
    ~TaskGateService() override;

    // -----------------------------------------------------------------
    // Service attachments (AppController wiring)
    // -----------------------------------------------------------------

    /**
     * @brief Attach the PlanService. Installs the
     *        planStatusChanged listener that clears the active-plan
     *        anchor on terminal transitions. Without this the next
     *        turn's system prompt could carry "ACTIVE TASK" framing
     *        for a completed plan, confusing the LLM into
     *        re-reporting finished work. Re-entry with a different
     *        pointer is unsupported and would leak the prior
     *        connection; AppController calls this exactly once at
     *        startup.
     * @param svc Non-owning pointer; null detaches (the listener
     *            is removed).
     */
    void setPlanService(PlanService* svc);

    /**
     * @brief Attach the TaskRunner used by handleUserCancel to stop
     *        the active plan when the user cancels in-flight work.
     * @param runner Non-owning pointer; pass nullptr to detach.
     */
    void setTaskRunner(TaskRunner* runner);

    /**
     * @brief Attach the TaskObserver used by
     *        recordTextArtifactIfApplicable to record responder
     *        text as a Draft artifact on the active step.
     * @param observer Non-owning pointer; pass nullptr to detach.
     */
    void setTaskObserver(TaskObserver* observer);

    // -----------------------------------------------------------------
    // Active-plan anchor
    // -----------------------------------------------------------------

    /**
     * @brief Set the active-plan id. Called by
     *        userInitiatedStartTask, the start_task tool handler,
     *        and the tool-dispatcher's auto-anchor feedback slot.
     *        Overwrites any prior value.
     * @param planId Plan UUID to anchor.
     */
    void setActivePlanId(const QString& planId);

    /**
     * @brief The current anchor. ChatController's public
     *        `activeTaskPlanId()` accessor reads through this
     *        service via its non-owning pointer.
     * @returns Plan UUID, or empty string when no task is active.
     */
    QString activePlanId() const;

    /**
     * @brief Equivalent to setActivePlanId(""). Called by paths
     *        that need to drop a stale anchor without waiting for
     *        the planStatusChanged listener (e.g. the report_blocked
     *        tool handler when the plan transitions to Blocked,
     *        which is not a terminal state, so the listener wouldn't fire).
     */
    void clearActivePlan();

    /**
     * @brief Whether an anchor is currently set.
     * @returns true iff a non-empty active-plan id is anchored.
     */
    bool hasActivePlan() const;

    // -----------------------------------------------------------------
    // Turn-end handlers
    // -----------------------------------------------------------------

    /**
     * @brief Apply the `\<task_state\>` marker captured from the
     *        responder's content during onRequestFinished
     *        sanitisation. Drives plan state on Completed / Blocked
     *        transitions, posts the "🏁 Task completed" /
     *        "⚠️ Task blocked" system event, and clears the anchor
     *        on terminal status. No-op if there's no active anchor
     *        or the marker is empty / "working" / "waiting".
     * @param declaredTaskStatus Captured marker value ("",
     *                             "working", "completed", "waiting",
     *                             or "blocked").
     * @param inflightConvId       In-flight conversation UUID (used
     *                             for the system-event post).
     */
    void applyDeclaredStatus(const QString& declaredTaskStatus, const QString& inflightConvId);

    /**
     * @brief Apply the implicit-completion heuristic. When an
     *        active task is anchored + at least one tool call
     *        executed during this turn + finishReason=stop + no
     *        declared status, the plan is marked Completed. This
     *        catches the common pattern where an LLM calls
     *        write_file and then stops (short of emitting a status
     *        marker). Without this, plans would remain Executing
     *        forever.
     * @param declaredTaskStatus Captured marker value (must be
     *                             empty for the heuristic to fire).
     * @param finishReason         Provider finish reason (must be
     *                             "stop" for the heuristic to
     *                             fire).
     * @param toolIterationCount   Tool calls dispatched in this
     *                             turn (must be >= 1 for the
     *                             heuristic to fire).
     * @param inflightConvId       In-flight conversation UUID (used
     *                             for the system-event post).
     */
    void applyImplicitCompletion(const QString& declaredTaskStatus,
                                 const QString& finishReason,
                                 int toolIterationCount,
                                 const QString& inflightConvId);

    /**
     * @brief Record the responder's substantive text as a Draft
     *        artifact row on the active plan's step, via
     *        TaskObserver. Gated on finishReason=stop + non-empty
     *        content + content length >= 30 chars after trimming.
     *
     *        NOT a part of the active-plan state machine itself;
     *        this is pure observability. The content parameter is
     *        the captured-pre-finalize content snapshot, because
     *        StreamingManager clears its own `m_content` before we
     *        get here, so the live getter returns empty.
     * @param finishReason    Provider finish reason (must be
     *                        "stop" to record).
     * @param capturedContent Pre-finalize content snapshot.
     * @param submitterAlias  Alias of the responder agent.
     */
    void recordTextArtifactIfApplicable(const QString& finishReason,
                                        const QString& capturedContent,
                                        const QString& submitterAlias);

    // -----------------------------------------------------------------
    // Cross-cutting lifecycle paths
    // -----------------------------------------------------------------

    /**
     * @brief Handle the userCancel path from onRequestFinished.
     *        Stops the active plan via TaskRunner::handleStopTask
     *        with reason "Cancelled by user", then clears the
     *        anchor. No-op if no anchor is set or TaskRunner is
     *        unattached.
     */
    void handleUserCancel();

    /**
     * @brief Handle conversation switch. The active-task tag is
     *        per-conversation: clear it, then scan the destination
     *        conversation's Executing/Planning plans and auto-resume
     *        the most-recently-updated one if any.
     * @param newConvId Destination conversation UUID.
     */
    void onConversationSwitched(const QString& newConvId);

    /**
     * @brief Post a role=system task_event message via
     *        MessageService. Used internally by applyDeclaredStatus
     *        / applyImplicitCompletion. Exposed publicly so callers
     *        like TaskController::postTaskEventMessage (the
     *        TaskRunner→system-message endpoint) can route
     *        task-started / task-completed events through the same
     *        implementation. No-op on empty convId / content.
     * @param convId    Conversation UUID to post into.
     * @param role      Message role (expected "system").
     * @param content   Rendered event text.
     * @param eventType Metadata tag (`task_started`,
     *                  `task_completed`, `task_blocked`,
     *                  `step_done`, etc.).
     */
    void postEvent(const QString& convId,
                   const QString& role,
                   const QString& content,
                   const QString& eventType);

  private:
    MessageService& m_msgSvc;  // non-owning
    PlanService* m_planService = nullptr;
    TaskRunner* m_taskRunner = nullptr;
    TaskObserver* m_taskObserver = nullptr;
    QString m_activePlanId;
};

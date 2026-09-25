// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file task-runner.h
 * @brief State machine orchestrator for the plan execution loop.
 *        Drives plan-step execution by enqueuing task-gated
 *        dispatches onto ChatController and reacting to tool-call
 *        results + validator hooks. Owns the per-plan budget
 *        counters. Does NOT directly call the model router; it
 *        routes everything through ChatController via an enqueue
 *        callback registered at startup.
 * @layer Service
 * @dependencies PlanService (data), ChatController (enqueue callback)
 */


#pragma once

#include "../models/agent-plan.h"

#include <QTimer>

#include <functional>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

class PlanService;

/**
 * @brief A dispatch that TaskRunner wants ChatController to service.
 *
 * These are enqueued via the callback `setEnqueueCallback` and popped by
 * ChatController between turns. Only TaskRunner ever constructs these;
 * conversational dispatches live in the existing cascade path.
 */
struct TaskDispatchRequest {
    QString convId;      ///< Home conversation of the plan
    QString ownerAlias;  ///< Alias of the step owner to dispatch
    QString agentId;     ///< Resolved agent template id
    QString planId;      ///< Current plan id
    QString stepId;      ///< Current step id
    QString reason;      ///< "initial" | "continuation" | "rework" | "heartbeat"
};

/**
 * @brief Callback type ChatController registers with TaskRunner so the
 *        runner can request a task-gated dispatch without having to know
 *        ChatController's internals. The callback is expected to run the
 *        dispatch asynchronously (or queue it behind any in-flight request)
 *        and eventually notify TaskRunner of the turn's outcome via the
 *        onToolCallExecuted / onTaskTurnFinishedWithText hooks.
 */
using TaskDispatchEnqueueFn = std::function<void(const TaskDispatchRequest&)>;

/**
 * @brief State-machine driver for the plan execution loop. Owns the
 *        per-plan budget counters and the 60-second heartbeat tick
 *        that nudges idle plans forward.
 */
class TaskRunner : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the task runner.
     * @param planSvc Non-owning reference to the plan data layer.
     * @param parent  Qt parent (AppController).
     */
    explicit TaskRunner(PlanService& planSvc, QObject* parent = nullptr);
    ~TaskRunner() override;

    // -----------------------------------------------------------------------
    // Wiring
    // -----------------------------------------------------------------------

    /**
     * @brief Registers the callback that ChatController uses to
     *        receive task-dispatch requests. Called once at
     *        AppController wiring time. TaskRunner never calls
     *        ChatController directly.
     * @param fn Callback ChatController supplies.
     */
    void setEnqueueCallback(TaskDispatchEnqueueFn fn);

    /**
     * @brief Registers a predicate the heartbeat tick uses to decide
     *        whether a conversation has an in-flight LLM request. The
     *        heartbeat suppresses nudges when the answer is true so we
     *        don't interrupt work that's already running.
     * @param fn Predicate taking a conversation id; returns true while
     *           that conversation has an LLM request in flight.
     */
    void setIsConversationGeneratingFn(std::function<bool(const QString&)> fn);

    /**
     * @brief Starts the 60-second heartbeat timer. Safe to call multiple
     *        times. Call once from AppController after wiring.
     */
    void startHeartbeat();

    /** @brief Stops the heartbeat timer (for shutdown or tests). */
    void stopHeartbeat();

    /**
     * @brief Hook called by ChatController when a conversation is
     *        deleted by the user. Drops all plans belonging to that
     *        conversation so the FK cascade cleanup is mirrored in
     *        the runner's in-memory state (last progress times, etc).
     * @param conversationId UUID of the deleted conversation.
     */
    void onConversationDeleted(const QString& conversationId);


    /**
     * @brief Creates a plan from a parsed `start_task` tool call and
     *        initiates execution by requesting a dispatch for the first
     *        step's owner.
     *
     * @param convId           Home conversation.
     * @param callerAlias      Alias of the agent calling start_task.
     * @param goal             Goal string from the tool args.
     * @param steps            Parsed step list (may be empty → creates a
     *                         single-step plan owned by callerAlias).
     * @param projectFolderId  Scope hint (may be empty).
     * @param orgFolderId      Scope hint (may be empty).
     * @return Plan UUID on success, empty on failure (invalid args, etc).
     */
    QString startTaskFromToolCall(const QString& convId,
                                  const QString& callerAlias,
                                  const QString& goal,
                                  const QList<PlanStep>& steps,
                                  const QString& projectFolderId,
                                  const QString& orgFolderId);

    /**
     * @brief Handles a submit_result tool call: persists the
     *        artifact, transitions the step to Submitted, then
     *        auto-approves the step and advances the plan.
     * @param stepId          UUID of the step being submitted.
     * @param type            Artifact type tag (text / canvas / file
     *                        / etc.).
     * @param content         Full artifact content.
     * @param summary         Short artifact summary.
     * @param submittingAlias Alias of the agent submitting.
     * @returns true on success; false on validation failure or
     *          missing step.
     */
    bool handleSubmitResult(const QString& stepId,
                            ArtifactType type,
                            const QString& content,
                            const QString& summary,
                            const QString& submittingAlias);

    /**
     * @brief Handles a report_blocked tool call: marks the step
     *        blocked and records the reason. Plan continues with
     *        other steps.
     * @param stepId UUID of the blocked step.
     * @param reason Human-readable blocked reason.
     * @returns true on success; false on unknown step.
     */
    bool handleReportBlocked(const QString& stepId, const QString& reason);

    /**
     * @brief Handles a stop_task tool call (or user Stop button).
     *        Marks the plan Failed and drops any pending dispatches.
     * @param planId UUID of the plan to stop.
     * @param reason Human-readable stop reason.
     * @returns true on success; false on unknown plan.
     */
    bool handleStopTask(const QString& planId, const QString& reason);

    /**
     * @brief Handles an update_plan_step tool call (progress note).
     *        Does not transition status; just stamps updated_at and
     *        emits a task event so the user sees the note in the
     *        chat.
     * @param stepId UUID of the step to annotate.
     * @param notes  Progress-note text.
     * @returns true on success; false on unknown step.
     */
    bool handleUpdatePlanStep(const QString& stepId, const QString& notes);


    /**
     * @brief User clicked "Retry" on a step. Resets the step's
     *        counters (rejection_count, tool_retry_count,
     *        executor_turns_used) back to zero, flips status to
     *        Pending, stores the user's note in
     *        last_rejection_reason so the next executor turn sees it
     *        as corrective context, then re-dispatches.
     * @param stepId   UUID of the step to retry.
     * @param userNote User-supplied corrective note (may be empty).
     * @returns true on success; false on unknown step.
     */
    bool userRetryStep(const QString& stepId, const QString& userNote);

    /**
     * @brief User clicked "Override as done". Force-completes the
     *        step without the owner actually producing an artifact.
     *        Used when the user sees the work was already good
     *        enough in an earlier attempt, or when they manually
     *        produced the result. Advances the plan to the next step
     *        (or to Completed).
     * @param stepId UUID of the step to force-complete.
     * @returns true on success; false on unknown step.
     */
    bool userOverrideStepAsDone(const QString& stepId);

    /**
     * @brief User clicked "Skip". Marks the step as Blocked with a
     *        "skipped by user" reason and advances the plan with the
     *        remaining steps. If the skipped step was the only one
     *        left, the plan transitions to Blocked.
     * @param stepId UUID of the step to skip.
     * @param reason Human-readable skip reason.
     * @returns true on success; false on unknown step.
     */
    bool userSkipStep(const QString& stepId, const QString& reason);

    // -----------------------------------------------------------------------
    // Validator hooks called by ChatController::onRequestFinished
    // -----------------------------------------------------------------------

    /**
     * @brief Called when a task-gated executor turn returned text
     *        instead of a tool call. TaskRunner increments the
     *        step's retry counter and either re-dispatches with a
     *        corrective nudge or blocks the step when the retry
     *        budget is exhausted.
     * @param convId       Home conversation UUID.
     * @param ownerAlias   Step owner alias.
     * @param agentId      Resolved agent template id.
     * @param planId       Plan UUID.
     * @param stepId       Step UUID.
     * @param producedText The (non-tool-call) text the agent
     *                     produced.
     */
    void onTaskTurnFinishedWithText(const QString& convId,
                                    const QString& ownerAlias,
                                    const QString& agentId,
                                    const QString& planId,
                                    const QString& stepId,
                                    const QString& producedText);

    /**
     * @brief Called when a task-gated executor turn produced a tool
     *        call that has now been executed (submit_result /
     *        report_blocked / stop_task are already handled by their
     *        dedicated handlers; this is for intermediate tools like
     *        read_file / search_web / update_plan_step where the
     *        step hasn't terminated yet). Triggers another executor
     *        turn for the same step unless the per-step executor
     *        turn cap is hit.
     * @param convId     Home conversation UUID.
     * @param ownerAlias Step owner alias.
     * @param agentId    Resolved agent template id.
     * @param planId     Plan UUID.
     * @param stepId     Step UUID.
     */
    void onIntermediateToolExecuted(const QString& convId,
                                    const QString& ownerAlias,
                                    const QString& agentId,
                                    const QString& planId,
                                    const QString& stepId);

    // -----------------------------------------------------------------------
    // Query API (used by ChatController at dispatch time + by prompt builder)
    // -----------------------------------------------------------------------

    /**
     * @brief Looks up the active step owned by `ownerAlias` in
     *        `convId`. Thin wrapper over PlanService for call-site
     *        convenience; also the integration seam where heartbeat /
     *        runner bookkeeping can inject state later.
     * @param convId     Home conversation UUID.
     * @param ownerAlias Step owner alias to look up.
     * @returns The active PlanStep when present; std::nullopt when
     *          the owner has no active step in the conversation.
     */
    std::optional<PlanStep> activeStepForMemberInConversation(const QString& convId,
                                                              const QString& ownerAlias) const;

    // -----------------------------------------------------------------------
    // Budgets
    // -----------------------------------------------------------------------

    /// Declared plan-wide LLM call budget. Not enforced.
    static constexpr int kMaxPlanLlmCalls = 60;
    /// Plan age limit. advancePlan() fails a plan older than this.
    static constexpr int kMaxPlanWallClockMs = 30 * 60 * 1000;  // 30 min
    /// Maximum steps; startTaskFromToolCall() drops any beyond this.
    static constexpr int kMaxStepsPerPlan = 15;
    /// Declared per-step executor turn budget. Not enforced.
    static constexpr int kMaxExecutorTurnsPerStep = 20;
    /// Declared per-step tool retry budget. Not enforced.
    static constexpr int kMaxToolRetriesPerStep = 3;
    /// Declared per-step Critic rejection budget. Not enforced.
    static constexpr int kMaxRejectionsPerStep = 3;
    /// Declared per-plan heartbeat budget. Not enforced.
    static constexpr int kMaxHeartbeatsPerPlan = 10;

  signals:
    /** @brief Emitted when a task event message should be posted to the chat. */
    void taskEventRequested(const QString& convId,
                            const QString& role,  // "system"
                            const QString& content,
                            const QString& eventType);  // started|step_done|...

    /**
     * @brief Emitted when a plan's internal state should be
     *        refreshed.
     * @param planId Plan UUID whose state changed.
     */
    void planUpdated(const QString& planId);

  private slots:
    /**
     * @brief Runs on the 60-second timer. Does nothing: a stalled task
     *        is not nudged, and the agent continues it when the user
     *        sends another message.
     */
    void onHeartbeatTick();

  private:
    PlanService& m_planService;
    TaskDispatchEnqueueFn m_enqueueFn;
    std::function<bool(const QString&)> m_isGeneratingFn;
    QTimer m_heartbeatTimer;
    QMap<QString, qint64> m_lastProgressMsByPlanId;
    QMap<QString, qint64> m_lastHeartbeatMsByPlanId;

    /**
     * @brief Resolves the agent id for a given alias in a conversation.
     *        Needed because TaskRunner must supply both alias + agent id
     *        to ChatController when enqueuing a dispatch. Implementation
     *        looks up MembershipService (injected via ChatController as
     *        part of the enqueue callback setup in AppController).
     *
     *        Currently unused: TaskRunner passes the alias only, and
     *        ChatController's dispatch slot does the lookup.
     */

    /** @brief Posts a task event message to the chat via signal. */
    void postEvent(const QString& convId, const QString& eventType, const QString& text);

    /** @brief Stamps last-progress time for a plan (tool call landed). */
    void recordProgress(const QString& planId);

    /**
     * @brief Advances a plan after a step transition. Called on
     *        completion, on block, and after intermediate tools when
     *        another executor turn is needed. Handles plan budget checks
     *        and state-machine transitions to Completed/Blocked/Failed.
     */
    void advancePlan(const QString& planId, const QString& convId);

    /**
     * @brief Requests a dispatch for the next pending/inprogress step.
     *        Returns true if a dispatch was enqueued, false if the plan
     *        has nothing left to run (caller should finalize).
     */
    bool
    dispatchNextPendingStep(const QString& planId, const QString& convId, const QString& reason);
};

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file task-runner.cpp
 * @brief State machine orchestrator that drives task execution.
 *
 *        Enqueues dispatches onto ChatController via a registered
 *        callback, tracks per-plan budgets, and applies state
 *        transitions based on tool-call results and validator hooks.
 * @layer Service
 * @dependencies PlanService, DbManager, Qt6::Core.
 */

#include "task-runner.h"

#include "../models/db-manager.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "plan-service.h"

#include <QDateTime>
#include <QUuid>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TaskRunner::TaskRunner(PlanService& planSvc, QObject* parent)
    : QObject(parent), m_planService(planSvc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_heartbeatTimer.setInterval(60 * 1000);  // 60s
    m_heartbeatTimer.setSingleShot(false);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &TaskRunner::onHeartbeatTick);
}

TaskRunner::~TaskRunner() = default;

void TaskRunner::setEnqueueCallback(TaskDispatchEnqueueFn fn) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_enqueueFn = std::move(fn);
}

void TaskRunner::setIsConversationGeneratingFn(std::function<bool(const QString&)> fn) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_isGeneratingFn = std::move(fn);
}

void TaskRunner::startHeartbeat() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_heartbeatTimer.isActive()) {
        m_heartbeatTimer.start();
        qCInfo(verzetaUi) << "TaskRunner: heartbeat started (60s interval)";
    }
}

void TaskRunner::stopHeartbeat() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_heartbeatTimer.isActive()) {
        m_heartbeatTimer.stop();
        qCInfo(verzetaUi) << "TaskRunner: heartbeat stopped";
    }
}

void TaskRunner::onConversationDeleted(const QString& conversationId) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Collect plan ids for cleanup of in-memory state BEFORE rows are gone
    const QList<AgentPlan> plans = m_planService.plansForConversation(conversationId);
    for (const AgentPlan& p : plans) {
        m_lastProgressMsByPlanId.remove(p.id);
        m_lastHeartbeatMsByPlanId.remove(p.id);
    }
    m_planService.deletePlansForConversation(conversationId);
    qCInfo(verzetaUi) << "TaskRunner: dropped" << plans.size() << "plan(s) for deleted conversation"
                      << conversationId.left(8);
}

void TaskRunner::recordProgress(const QString& planId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (planId.isEmpty())
        return;
    m_lastProgressMsByPlanId[planId] = QDateTime::currentMSecsSinceEpoch();
}

void TaskRunner::onHeartbeatTick() {
    VERZETA_ASSERT_MAIN_THREAD();
}

// ---------------------------------------------------------------------------
// Entry — start_task
// ---------------------------------------------------------------------------

QString TaskRunner::startTaskFromToolCall(const QString& convId,
                                          const QString& callerAlias,
                                          const QString& goal,
                                          const QList<PlanStep>& stepsIn,
                                          const QString& projectFolderId,
                                          const QString& orgFolderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty() || goal.trimmed().size() < 5) {
        qCWarning(verzetaUi) << "TaskRunner::startTaskFromToolCall: invalid args";
        return {};
    }

    // If the caller didn't supply steps, create a single-step plan owned by
    // the calling agent themselves. This is the simplest 1:1 case.
    QList<PlanStep> steps = stepsIn;
    if (steps.isEmpty()) {
        VERZETA_ASSERT_MAIN_THREAD();
        PlanStep s;
        s.title = goal.left(80);
        s.description = goal;
        s.ownerAlias = callerAlias.isEmpty() ? QStringLiteral("user") : callerAlias;
        s.acceptanceCriteria =
            QStringLiteral("Produce a concrete artifact that addresses: ") + goal.left(200);
        steps.append(s);
    }

    // Clamp to max steps
    if (steps.size() > kMaxStepsPerPlan) {
        steps = steps.mid(0, kMaxStepsPerPlan);
    }

    // Build and persist the plan
    AgentPlan plan;
    plan.conversationId = convId;
    plan.projectFolderId = projectFolderId;
    plan.organizationFolderId = orgFolderId;
    plan.goal = goal.trimmed();
    plan.startedBy = callerAlias.isEmpty() ? QStringLiteral("user") : callerAlias;
    plan.status = PlanStatus::Executing;

    const QString planId = m_planService.createPlan(plan, steps);
    if (planId.isEmpty()) {
        qCWarning(verzetaUi) << "TaskRunner: createPlan failed";
        return {};
    }

    qCInfo(verzetaUi) << "TaskRunner: plan" << planId << "started by" << callerAlias
                      << "goal:" << goal.left(60);

    // Fresh plan — initialize its progress stamp so the heartbeat doesn't
    // nudge immediately just because updated_at is slightly stale.
    recordProgress(planId);

    postEvent(
        convId, QStringLiteral("started"), QStringLiteral("🎯 Task started: %1").arg(plan.goal));

    return planId;
}

// ---------------------------------------------------------------------------
// Entry — submit_result
// ---------------------------------------------------------------------------

bool TaskRunner::handleSubmitResult(const QString& stepId,
                                    ArtifactType type,
                                    const QString& content,
                                    const QString& summary,
                                    const QString& submittingAlias) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto stepOpt = m_planService.getStep(stepId);
    if (!stepOpt.has_value()) {
        qCWarning(verzetaUi) << "handleSubmitResult: unknown step" << stepId;
        return false;
    }
    const PlanStep step = stepOpt.value();

    const auto planOpt = m_planService.getPlan(step.planId);
    if (!planOpt.has_value())
        return false;
    const AgentPlan plan = planOpt.value();

    recordProgress(plan.id);

    // Write artifact row
    StepArtifact artifact;
    artifact.stepId = stepId;
    artifact.type = type;
    artifact.content = content;
    artifact.summary = summary;
    artifact.submittedByAlias = submittingAlias;
    if (type == ArtifactType::File) {
        artifact.filePath = content;  // tool handler has already written the file
        artifact.content.clear();
    }
    const QString artifactId = m_planService.addArtifact(artifact);
    if (artifactId.isEmpty()) {
        qCWarning(verzetaUi) << "handleSubmitResult: addArtifact failed";
        return false;
    }

    m_planService.updateStepStatus(stepId, StepStatus::Submitted);
    m_planService.setArtifactApproval(artifactId, 1);
    m_planService.updateStepStatus(stepId, StepStatus::Done);

    // Mark the whole plan Completed — one logical step, one submit,
    // plan is done. The user can manually re-open it if they want
    // more work on the same task via a follow-up message.
    m_planService.updatePlanStatus(plan.id, PlanStatus::Completed);

    postEvent(plan.conversationId,
              QStringLiteral("step_done"),
              QStringLiteral("✓ @%1 completed step \"%2\": %3")
                  .arg(submittingAlias, step.title, summary));
    postEvent(plan.conversationId,
              QStringLiteral("plan_completed"),
              QStringLiteral("🏁 Task completed: %1").arg(plan.goal));
    return true;
}

// ---------------------------------------------------------------------------
// Entry — report_blocked
// ---------------------------------------------------------------------------

bool TaskRunner::handleReportBlocked(const QString& stepId, const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto stepOpt = m_planService.getStep(stepId);
    if (!stepOpt.has_value())
        return false;
    const PlanStep step = stepOpt.value();

    const auto planOpt = m_planService.getPlan(step.planId);
    if (!planOpt.has_value())
        return false;
    const AgentPlan plan = planOpt.value();

    m_planService.updateStepStatus(stepId, StepStatus::Blocked, reason);
    m_planService.updatePlanStatus(plan.id, PlanStatus::Blocked);

    postEvent(plan.conversationId,
              QStringLiteral("step_blocked"),
              QStringLiteral("⚠️ @%1 is blocked on \"%2\": %3. This step needs your input.")
                  .arg(step.ownerAlias, step.title, reason));
    return true;
}

// ---------------------------------------------------------------------------
// Entry — stop_task
// ---------------------------------------------------------------------------

bool TaskRunner::handleStopTask(const QString& planId, const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto planOpt = m_planService.getPlan(planId);
    if (!planOpt.has_value())
        return false;
    const AgentPlan plan = planOpt.value();

    m_planService.updatePlanStatus(planId, PlanStatus::Failed);
    postEvent(plan.conversationId,
              QStringLiteral("plan_failed"),
              QStringLiteral("🛑 Task stopped: %1 (reason: %2)").arg(plan.goal, reason));
    return true;
}

// ---------------------------------------------------------------------------
// Entry — update_plan_step (progress note)
// ---------------------------------------------------------------------------

bool TaskRunner::handleUpdatePlanStep(const QString& stepId, const QString& notes) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto stepOpt = m_planService.getStep(stepId);
    if (!stepOpt.has_value())
        return false;
    const PlanStep step = stepOpt.value();
    const auto planOpt = m_planService.getPlan(step.planId);
    if (!planOpt.has_value())
        return false;
    const AgentPlan plan = planOpt.value();

    // Just stamp updated_at by touching rejection reason to empty-then-note;
    // simpler: call incrementStepExecutorTurns as a no-op would be wrong.
    // Instead, use updateStepStatus with same status + reason to refresh
    // updated_at. PlanService will re-emit stepUpdated.
    m_planService.updateStepStatus(stepId, step.status, notes);

    postEvent(
        plan.conversationId,
        QStringLiteral("step_progress"),
        QStringLiteral("⏱ @%1 working on \"%2\": %3").arg(step.ownerAlias, step.title, notes));
    return true;
}


bool TaskRunner::userRetryStep(const QString& stepId, const QString& userNote) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto stepOpt = m_planService.getStep(stepId);
    if (!stepOpt.has_value())
        return false;
    const PlanStep step = stepOpt.value();
    const auto planOpt = m_planService.getPlan(step.planId);
    if (!planOpt.has_value())
        return false;
    const AgentPlan plan = planOpt.value();

    const QString noteForContext = userNote.trimmed().isEmpty()
                                       ? QStringLiteral("User asked to retry this step")
                                       : userNote.trimmed();

    // Reset counters + status → pending + stores the note as the
    // "last_rejection_reason" field, which the executor prompt builder
    // surfaces on the next dispatch.
    if (!m_planService.resetStepForRetry(stepId, noteForContext)) {
        return false;
    }

    // If the plan was Blocked, bring it back to Executing so the runner
    // will dispatch again.
    if (plan.status == PlanStatus::Blocked || plan.status == PlanStatus::Failed) {
        m_planService.updatePlanStatus(plan.id, PlanStatus::Executing);
    }

    // Refresh progress stamps so the heartbeat doesn't nudge prematurely.
    recordProgress(plan.id);

    postEvent(plan.conversationId,
              QStringLiteral("step_retry"),
              QStringLiteral("🔄 User asked @%1 to retry step \"%2\". Note: %3")
                  .arg(step.ownerAlias, step.title, noteForContext));

    // Re-dispatch the owner via the enqueue path.
    if (m_enqueueFn) {
        TaskDispatchRequest req;
        req.convId = plan.conversationId;
        req.ownerAlias = step.ownerAlias;
        req.planId = plan.id;
        req.stepId = step.id;
        req.reason = QStringLiteral("user_retry");
        m_enqueueFn(req);
    }
    return true;
}

bool TaskRunner::userOverrideStepAsDone(const QString& stepId) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto stepOpt = m_planService.getStep(stepId);
    if (!stepOpt.has_value())
        return false;
    const PlanStep step = stepOpt.value();
    const auto planOpt = m_planService.getPlan(step.planId);
    if (!planOpt.has_value())
        return false;
    const AgentPlan plan = planOpt.value();

    m_planService.updateStepStatus(
        stepId, StepStatus::Done, QStringLiteral("Manually marked done by user"));

    postEvent(plan.conversationId,
              QStringLiteral("step_override"),
              QStringLiteral("✋ User marked step \"%1\" as done (override)").arg(step.title));

    // Plan might now be complete or ready for the next step
    advancePlan(plan.id, plan.conversationId);
    return true;
}

bool TaskRunner::userSkipStep(const QString& stepId, const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto stepOpt = m_planService.getStep(stepId);
    if (!stepOpt.has_value())
        return false;
    const PlanStep step = stepOpt.value();
    const auto planOpt = m_planService.getPlan(step.planId);
    if (!planOpt.has_value())
        return false;
    const AgentPlan plan = planOpt.value();

    const QString skipReason = reason.trimmed().isEmpty()
                                   ? QStringLiteral("Skipped by user")
                                   : QStringLiteral("Skipped by user: ") + reason.trimmed();

    // CRITICAL: skipped steps must transition to Done, NOT Blocked.
    // Blocked is the "stuck, needs intervention" state — putting a skipped
    // step into Blocked left advancePlan in a loop where the same step kept
    // matching as needs-action and the plan could never complete. Done is
    // the right terminal state: skip means "we're not doing this, move on".
    // The skip reason is preserved in the rejection_reason column so the
    // UI can still distinguish skipped from genuinely completed.
    m_planService.updateStepStatus(stepId, StepStatus::Done, skipReason);

    // If the parent plan was Blocked because of this step, lift it back to
    // Executing so advancePlan can dispatch the next step or finalize.
    if (plan.status == PlanStatus::Blocked) {
        m_planService.updatePlanStatus(plan.id, PlanStatus::Executing);
    }

    postEvent(plan.conversationId,
              QStringLiteral("step_skipped"),
              QStringLiteral("⏭ %2 (step \"%1\")").arg(step.title, skipReason));

    // Check whether any other steps remain to be worked
    advancePlan(plan.id, plan.conversationId);
    return true;
}

// ---------------------------------------------------------------------------
// Validator hooks
// ---------------------------------------------------------------------------

void TaskRunner::onTaskTurnFinishedWithText(const QString& convId,
                                            const QString& ownerAlias,
                                            const QString& agentId,
                                            const QString& planId,
                                            const QString& stepId,
                                            const QString& producedText) {
    VERZETA_ASSERT_MAIN_THREAD();
    Q_UNUSED(convId);
    Q_UNUSED(ownerAlias);
    Q_UNUSED(agentId);
    Q_UNUSED(planId);
    Q_UNUSED(stepId);
    Q_UNUSED(producedText);
}

void TaskRunner::onIntermediateToolExecuted(const QString& convId,
                                            const QString& ownerAlias,
                                            const QString& agentId,
                                            const QString& planId,
                                            const QString& /*stepId*/) {
    VERZETA_ASSERT_MAIN_THREAD();
    Q_UNUSED(convId);
    Q_UNUSED(ownerAlias);
    Q_UNUSED(agentId);
    if (!planId.isEmpty()) {
        recordProgress(planId);
    }
}

// ---------------------------------------------------------------------------
// Query API
// ---------------------------------------------------------------------------

std::optional<PlanStep>
TaskRunner::activeStepForMemberInConversation(const QString& convId,
                                              const QString& ownerAlias) const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_planService.activeStepForOwnerInConversation(convId, ownerAlias);
}

// ---------------------------------------------------------------------------
// Plan advancement
// ---------------------------------------------------------------------------

void TaskRunner::advancePlan(const QString& planId, const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto planOpt = m_planService.getPlan(planId);
    if (!planOpt.has_value())
        return;

    // Budget check: wall clock
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 planAgeMs = now - planOpt->createdAt.toMSecsSinceEpoch();
    if (planAgeMs > kMaxPlanWallClockMs) {
        m_planService.updatePlanStatus(planId, PlanStatus::Failed);
        postEvent(convId,
                  QStringLiteral("plan_failed"),
                  QStringLiteral("🛑 Task failed: it ran past its time limit."));
        return;
    }

    // Try to dispatch the next pending/needs_rework step
    if (dispatchNextPendingStep(planId, convId, QStringLiteral("initial"))) {
        return;
    }

    // No more pending steps — determine terminal state. If every step is
    // Done, Completed. If any are Blocked, the plan is Blocked (some steps
    // done, some waiting for user unblock). If all are Blocked, still
    // Blocked — the user decides when to retry or drop.
    const QList<PlanStep> steps = m_planService.stepsForPlan(planId);
    bool anyBlocked = false;
    bool allDone = true;
    for (const PlanStep& s : steps) {
        if (s.status == StepStatus::Blocked)
            anyBlocked = true;
        if (s.status != StepStatus::Done)
            allDone = false;
    }

    if (allDone) {
        m_planService.updatePlanStatus(planId, PlanStatus::Completed);
        postEvent(convId,
                  QStringLiteral("plan_completed"),
                  QStringLiteral("🏁 Task completed: %1").arg(planOpt->goal));
    } else if (anyBlocked) {
        m_planService.updatePlanStatus(planId, PlanStatus::Blocked);
        postEvent(convId,
                  QStringLiteral("plan_blocked"),
                  QStringLiteral("⚠️ Task blocked on one or more steps. "
                                 "@owner, your input is needed."));
    } else {
        // Unexpected state — everything is in a weird mid-flight state.
        // Log and leave the plan as Executing so the next event will retry.
        qCWarning(verzetaUi) << "advancePlan: plan" << planId
                             << "has no pending steps but is not all done";
    }
}

bool TaskRunner::dispatchNextPendingStep(const QString& planId,
                                         const QString& convId,
                                         const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QList<PlanStep> steps = m_planService.stepsForPlan(planId);
    for (const PlanStep& s : steps) {
        if (s.status != StepStatus::Pending && s.status != StepStatus::InProgress &&
            s.status != StepStatus::NeedsRework) {
            continue;
        }

        // Reject user-owned steps. The runner has no dispatch path for
        // "user" and the older code posted a 'needs_user_input' event
        // every time advancePlan ran, spamming the chat and getting the
        // plan stuck. Mark the step Blocked so the user can use the
        // Plans Overlay's Mark Done / Skip / Stop buttons.
        if (s.ownerAlias.compare(QStringLiteral("user"), Qt::CaseInsensitive) == 0 ||
            s.ownerAlias.compare(QStringLiteral("owner"), Qt::CaseInsensitive) == 0) {
            const QString reason2 =
                QStringLiteral("The agent assigned this step to you. When you have "
                               "finished it, use Mark Done in the Plans overlay, or "
                               "use Skip to drop it.");
            m_planService.updateStepStatus(s.id, StepStatus::Blocked, reason2);
            postEvent(convId,
                      QStringLiteral("step_blocked"),
                      QStringLiteral("⚠️ Step \"%1\" requires you. Open "
                                     "the Plans overlay to Mark Done or "
                                     "Skip.")
                          .arg(s.title));
            // Continue scanning — there may be other steps that are
            // dispatchable by an actual agent.
            continue;
        }

        if (m_enqueueFn) {
            TaskDispatchRequest req;
            req.convId = convId;
            req.ownerAlias = s.ownerAlias;
            req.planId = planId;
            req.stepId = s.id;
            req.reason = reason;
            // ChatController resolves agentId from conversation members.
            m_enqueueFn(req);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Event posting
// ---------------------------------------------------------------------------

void TaskRunner::postEvent(const QString& convId, const QString& eventType, const QString& text) {
    VERZETA_ASSERT_MAIN_THREAD();
    emit taskEventRequested(convId, QStringLiteral("system"), text, eventType);
}

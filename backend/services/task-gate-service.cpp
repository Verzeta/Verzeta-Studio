// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file task-gate-service.cpp
 * @brief Implementation of TaskGateService. Contains the
 *        planStatusChanged listener, the declared-status and
 *        implicit-completion handlers, the TaskObserver text-artifact
 *        hook, the user-cancel plan-stop, the switch-conversation
 *        clear + auto-resume, and the postEvent system-message
 *        insertion helper.
 *
 *        See task-gate-service.h for the full contract.
 *
 * @layer Service
 * @dependencies MessageService, PlanService, TaskRunner, TaskObserver,
 *               models/agent-plan.h, models/message.h,
 *               utils/logger.h, utils/thread-discipline.h.
 */

#include "task-gate-service.h"

#include "../models/agent-plan.h"
#include "../models/message.h"
#include "../services/message-service.h"
#include "../services/plan-service.h"
#include "../services/task-observer.h"
#include "../services/task-runner.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QDateTime>
#include <QJsonObject>
#include <QUuid>

TaskGateService::TaskGateService(MessageService& msgSvc, QObject* parent)
    : QObject(parent), m_msgSvc(msgSvc) {}

TaskGateService::~TaskGateService() = default;

// ---------------------------------------------------------------------------
// Service attachments
// ---------------------------------------------------------------------------

void TaskGateService::setPlanService(PlanService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_planService = svc;

    // Listen for plan status transitions so the active-plan anchor
    // gets cleared whenever ITS plan reaches a terminal state, no
    // matter which code path triggered the transition. Every
    // completion path (TaskRunner::handleSubmitResult / handleStopTask,
    // critic rejection, heartbeat timeout, user cancel) routes
    // through PlanService::updatePlanStatus which fires this signal;
    // listening centrally here beats scattering anchor-clear calls
    // across every completion path and guarantees the prompt builder
    // never sees a stale anchor.
    //
    // Terminal states that clear: Completed, Failed. Intermediate
    // states (Planning, Executing, Critiquing, Blocked) leave the
    // anchor in place — blocked plans are stuck but not dead, and
    // keeping the anchor correctly surfaces the blocked work until
    // the user unblocks.
    if (m_planService) {
        connect(m_planService,
                &PlanService::planStatusChanged,
                this,
                [this](const QString& planId, PlanStatus newStatus) {
                    if (planId != m_activePlanId)
                        return;
                    if (newStatus == PlanStatus::Completed || newStatus == PlanStatus::Failed) {
                        qCInfo(verzetaUi)
                            << "TaskGateService: clearing active plan anchor" << planId.left(8)
                            << "— plan reached terminal state" << planStatusToString(newStatus)
                            << "via PlanService signal";
                        m_activePlanId.clear();
                    }
                });
    }
}

void TaskGateService::setTaskRunner(TaskRunner* runner) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_taskRunner = runner;
}

void TaskGateService::setTaskObserver(TaskObserver* observer) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_taskObserver = observer;
}

// ---------------------------------------------------------------------------
// Active-plan anchor
// ---------------------------------------------------------------------------

void TaskGateService::setActivePlanId(const QString& planId) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_activePlanId = planId;
}

QString TaskGateService::activePlanId() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_activePlanId;
}

void TaskGateService::clearActivePlan() {
    VERZETA_ASSERT_MAIN_THREAD();
    m_activePlanId.clear();
}

bool TaskGateService::hasActivePlan() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return !m_activePlanId.isEmpty();
}

// ---------------------------------------------------------------------------
// Declared-status marker handling
//
// Runs AFTER the turn is persisted and the cascade has drained.
// Updates the plan row's status from the LLM-emitted
// <task_state>…</task_state> marker, posts a matching system event
// into the chat, and clears the anchor on terminal transitions.
// ---------------------------------------------------------------------------
void TaskGateService::applyDeclaredStatus(const QString& declaredTaskStatus,
                                          const QString& inflightConvId) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (m_activePlanId.isEmpty() || declaredTaskStatus.isEmpty() || !m_taskRunner ||
        !m_planService) {
        return;
    }

    const QString planId = m_activePlanId;
    const auto pOpt = m_planService->getPlan(planId);
    if (!pOpt.has_value())
        return;

    const QList<PlanStep> steps = m_planService->stepsForPlan(planId);

    if (declaredTaskStatus == QStringLiteral("completed")) {
        if (!steps.isEmpty()) {
            m_planService->updateStepStatus(
                steps.first().id,
                StepStatus::Done,
                QStringLiteral("The model marked this step as completed."));
        }
        m_planService->updatePlanStatus(planId, PlanStatus::Completed);
        postEvent(inflightConvId,
                  QStringLiteral("system"),
                  QStringLiteral("🏁 Task completed: %1").arg(pOpt->goal),
                  QStringLiteral("plan_completed"));
        m_activePlanId.clear();
    } else if (declaredTaskStatus == QStringLiteral("blocked")) {
        if (!steps.isEmpty()) {
            m_planService->updateStepStatus(
                steps.first().id,
                StepStatus::Blocked,
                QStringLiteral("The model marked this step as blocked."));
        }
        m_planService->updatePlanStatus(planId, PlanStatus::Blocked);
        postEvent(inflightConvId,
                  QStringLiteral("system"),
                  QStringLiteral("⚠️ Task blocked: %1").arg(pOpt->goal),
                  QStringLiteral("plan_blocked"));
        m_activePlanId.clear();
    } else if (declaredTaskStatus == QStringLiteral("waiting")) {
        qCInfo(verzetaUi) << "TaskGateService: plan" << planId
                          << "in waiting state (model asked for clarification)";
    } else {
        qCInfo(verzetaUi) << "TaskGateService: plan" << planId
                          << "declared working — still in progress";
    }
}

// ---------------------------------------------------------------------------
// Implicit-completion heuristic
//
// Addresses a recurring failure mode: an agent calls write_file, the
// file is written successfully, the next turn returns reason=stop
// with zero chars (the model "thought" it was done after the tool
// call). No status marker ever arrives; without this heuristic the
// plan would sit in Executing forever from the user's perspective.
//
// Rule: active task + stop finish + at least one tool call executed
// + no declared status = implicit Completed.
// ---------------------------------------------------------------------------
void TaskGateService::applyImplicitCompletion(const QString& declaredTaskStatus,
                                              const QString& finishReason,
                                              int toolIterationCount,
                                              const QString& inflightConvId) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (m_activePlanId.isEmpty() || !declaredTaskStatus.isEmpty() ||
        finishReason != QStringLiteral("stop") || toolIterationCount <= 0 || !m_planService) {
        return;
    }

    const QString planId = m_activePlanId;
    const auto pOpt = m_planService->getPlan(planId);
    if (!pOpt.has_value() || pOpt->status == PlanStatus::Completed ||
        pOpt->status == PlanStatus::Failed) {
        return;
    }

    const QList<PlanStep> steps = m_planService->stepsForPlan(planId);
    if (!steps.isEmpty()) {
        m_planService->updateStepStatus(steps.first().id,
                                        StepStatus::Done,
                                        QStringLiteral("Marked done automatically: the tool calls "
                                                       "ran and no further reply followed"));
    }
    m_planService->updatePlanStatus(planId, PlanStatus::Completed);
    postEvent(inflightConvId,
              QStringLiteral("system"),
              QStringLiteral("🏁 Task completed: %1").arg(pOpt->goal),
              QStringLiteral("plan_completed"));
    qCInfo(verzetaUi) << "TaskGateService: implicit completion of plan" << planId << "after"
                      << toolIterationCount << "tool call(s) and empty stop";
    m_activePlanId.clear();
}

// ---------------------------------------------------------------------------
// TaskObserver text-artifact hook — body migrated verbatim from
// onRequestFinished persistence block.
// ---------------------------------------------------------------------------
void TaskGateService::recordTextArtifactIfApplicable(const QString& finishReason,
                                                     const QString& capturedContent,
                                                     const QString& submitterAlias) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (!m_taskObserver || m_activePlanId.isEmpty())
        return;
    if (finishReason != QStringLiteral("stop"))
        return;

    const QString trimmed = capturedContent.trimmed();
    if (trimmed.isEmpty() || trimmed.length() < 30)
        return;

    const QString alias = submitterAlias.isEmpty() ? QStringLiteral("assistant") : submitterAlias;

    // Summary = first line of content, capped at 120 chars.
    QString firstLine = trimmed.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (firstLine.length() > 120)
        firstLine = firstLine.left(120);

    m_taskObserver->recordTextArtifact(m_activePlanId, alias, capturedContent, firstLine);
}

// ---------------------------------------------------------------------------
// User-cancel plan stop — body migrated from onRequestFinished's
// userCancel branch.
// ---------------------------------------------------------------------------
void TaskGateService::handleUserCancel() {
    VERZETA_ASSERT_MAIN_THREAD();

    if (m_activePlanId.isEmpty() || !m_taskRunner)
        return;

    const QString planIdToStop = m_activePlanId;
    m_taskRunner->handleStopTask(planIdToStop, QStringLiteral("Cancelled by user"));
    m_activePlanId.clear();
}

// ---------------------------------------------------------------------------
// Switch-conversation clear + auto-resume.
//
// The active-task anchor is PER-CONVERSATION — each chat has its
// own. When the user switches conversations, drop the current
// anchor and scan the destination conversation's Executing / Planning
// plans; if any exist, resume from the most-recently-updated one
// (PlanService returns them ordered by updated_at DESC).
// ---------------------------------------------------------------------------
void TaskGateService::onConversationSwitched(const QString& newConvId) {
    VERZETA_ASSERT_MAIN_THREAD();

    m_activePlanId.clear();

    if (!m_planService || newConvId.isEmpty())
        return;

    const QList<AgentPlan> active = m_planService->activePlansForConversation(newConvId);
    for (const AgentPlan& p : active) {
        if (p.status == PlanStatus::Executing || p.status == PlanStatus::Planning) {
            m_activePlanId = p.id;
            break;  // list already ordered by updated_at DESC
        }
    }
}

// ---------------------------------------------------------------------------
// postEvent — shared helper for the system-message insertion body
// that ChatController::postTaskEventMessage (the Q_INVOKABLE
// ChatController entry point used by TaskRunner) now forwards to.
// ---------------------------------------------------------------------------
void TaskGateService::postEvent(const QString& convId,
                                const QString& role,
                                const QString& content,
                                const QString& eventType) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (convId.isEmpty() || content.isEmpty())
        return;

    Message msg;
    msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.conversationId = convId;
    msg.role = role.isEmpty() ? QStringLiteral("system") : role;
    msg.content = content;
    msg.createdAt = QDateTime::currentDateTimeUtc();
    msg.tokenCount = 0;
    msg.finishReason = QStringLiteral("task_event");
    msg.modelUsed = QString();
    msg.metadata = QJsonObject{
        {QStringLiteral("task_event"), true},
        {QStringLiteral("task_event_type"), eventType},
    };
    // messageAdded signal propagates to MessageListModel which
    // inserts the row if this conversation is currently shown.
    m_msgSvc.addMessage(msg);

    qCInfo(verzetaUi) << "Task event posted:" << eventType << "→" << content.left(80);
}

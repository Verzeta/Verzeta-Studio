// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file task-controller.cpp
 * @brief Implementation of the `Tasks` QML singleton. Owns the
 *        user-facing task-lifecycle API and the TaskRunner→system-
 *        message event writer.
 * @layer Service
 * @dependencies ConversationService, MessageService, PlanService,
 *               TaskRunner (references); AgentRegistry,
 *               MembershipService (optional pointers).
 */

#include "task-controller.h"

#include "../models/agent-plan.h"
#include "../models/agent.h"
#include "../models/conversation.h"
#include "../models/member.h"
#include "../models/message.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "agent-registry.h"
#include "chat/cascade-controller.h"
#include "conversation-service.h"
#include "file-service.h"
#include "membership-service.h"
#include "message-service.h"
#include "plan-service.h"
#include "task-gate-service.h"
#include "task-runner.h"
#include "tool-service.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>
#include <QUuid>

TaskController::TaskController(ConversationService& convSvc,
                               MessageService& msgSvc,
                               PlanService& planSvc,
                               TaskRunner& taskRunner,
                               QObject* parent)
    : QObject(parent)
    , m_convSvc(convSvc)
    , m_msgSvc(msgSvc)
    , m_planSvc(planSvc)
    , m_taskRunner(taskRunner) {
    qCInfo(verzetaUi) << "TaskController initialized";
}

TaskController::~TaskController() {
    qCInfo(verzetaUi) << "TaskController destroyed";
}

void TaskController::setAgentRegistry(AgentRegistry* registry) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_agentRegistry = registry;
    qCInfo(verzetaUi) << "TaskController: AgentRegistry" << (registry ? "attached" : "detached");
}

void TaskController::setMembershipService(MembershipService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_membershipService = svc;
    qCInfo(verzetaUi) << "TaskController: MembershipService" << (svc ? "attached" : "detached");
}

void TaskController::setFileService(FileService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_fileSvc = svc;
    qCInfo(verzetaUi) << "TaskController: FileService" << (svc ? "attached" : "detached");
}


namespace {

/**
 * @brief Temporary placeholder handler installed at schema-registration time and
 *        replaced with the real body by installTaskToolHandlers immediately after
 *        (so it is never reached in production). Returns a structured error if it
 *        somehow is, rather than silently no-op'ing.
 */
ToolHandler makeStubHandler(const QString& toolName) {
    return [toolName](const QJsonObject& /*args*/) -> QJsonValue {
        QJsonObject err;
        err[QStringLiteral("error")] =
            QStringLiteral("Tool '%1' is registered but its handler was not installed.")
                .arg(toolName);
        return err;
    };
}

ToolParameterSchema
makeParam(const QString& name, const QString& type, const QString& desc, bool required) {
    ToolParameterSchema p;
    p.name = name;
    p.type = type;
    p.description = desc;
    p.required = required;
    return p;
}

}  // namespace

void TaskController::registerTaskToolStubs(ToolService& toolSvc) {
    VERZETA_ASSERT_MAIN_THREAD();

    // ---- start_task ----------------------------------------------------
    {
        ToolSchema s;
        s.name = QStringLiteral("start_task");
        s.description =
            QStringLiteral("Create a tracked task plan in the current conversation. Call this "
                           "when the user (or another agent) assigns you concrete work. "
                           "Supply a clear goal and an ordered list of steps, each with a "
                           "title, the owner alias who will execute it (from the current "
                           "conversation roster), and concrete acceptance criteria. If you "
                           "omit steps you become the sole owner of a single step.");
        s.parameters.append(
            makeParam(QStringLiteral("goal"),
                      QStringLiteral("string"),
                      QStringLiteral("10-500 character description of the overall goal"),
                      true));
        s.parameters.append(
            makeParam(QStringLiteral("steps"),
                      QStringLiteral("array"),
                      QStringLiteral("Ordered list of step objects: "
                                     "{title, description?, owner_alias, acceptance_criteria}"),
                      false));
        toolSvc.registerTool(s, makeStubHandler(s.name), ToolKind::BuiltIn);
    }

    // ---- get_task_status -----------------------------------------------
    {
        ToolSchema s;
        s.name = QStringLiteral("get_task_status");
        s.description =
            QStringLiteral("Query the status of the active task(s) in this conversation. "
                           "Omit the argument to list the active task(s). Always safe to "
                           "call.");
        s.parameters.append(makeParam(QStringLiteral("task_id"),
                                      QStringLiteral("string"),
                                      QStringLiteral("Optional task id. Omit to list the active "
                                                     "task(s)."),
                                      false));
        toolSvc.registerTool(s, makeStubHandler(s.name), ToolKind::BuiltIn);
    }

    // ---- stop_task -----------------------------------------------------
    {
        ToolSchema s;
        s.name = QStringLiteral("stop_task");
        s.description =
            QStringLiteral("Stop an active plan. Use when the user cancels, or when the "
                           "plan's approach has proven to be a dead end. The plan transitions "
                           "to failed; any in-flight work is abandoned.");
        s.parameters.append(makeParam(QStringLiteral("plan_id"),
                                      QStringLiteral("string"),
                                      QStringLiteral("UUID of the plan to stop"),
                                      true));
        s.parameters.append(makeParam(QStringLiteral("reason"),
                                      QStringLiteral("string"),
                                      QStringLiteral("Short explanation for the stop"),
                                      true));
        toolSvc.registerTool(s, makeStubHandler(s.name), ToolKind::BuiltIn);
    }

    // ---- complete_task -------------------------------------------------
    {
        ToolSchema s;
        s.name = QStringLiteral("complete_task");
        s.description =
            QStringLiteral("Close the active tracked task as completed, once everything the "
                           "user asked for actually exists (files written, info gathered). A "
                           "task belongs to the whole conversation — ANYONE can close it, not "
                           "just whoever opened it. Prefer this over leaving a finished task "
                           "open.");
        s.parameters.append(
            makeParam(QStringLiteral("reason"),
                      QStringLiteral("string"),
                      QStringLiteral("Optional one-line note on what was delivered."),
                      false));
        toolSvc.registerTool(s, makeStubHandler(s.name), ToolKind::BuiltIn);
    }

    static const QStringList kTaskToolNames = {
        QStringLiteral("start_task"),
        QStringLiteral("complete_task"),
        QStringLiteral("get_task_status"),
        QStringLiteral("stop_task"),
    };
    for (const QString& name : kTaskToolNames) {
        toolSvc.setRunsOnMainThread(name, true);
    }

    qCInfo(verzetaUi) << "TaskController: registered" << kTaskToolNames.size()
                      << "task-tool schemas";
}


void TaskController::installTaskToolHandlers(ToolService& toolSvc,
                                             TaskGateService& taskGate,
                                             CascadeResolver cascadeResolver,
                                             std::function<QString()> activeConvIdGetter) {
    VERZETA_ASSERT_MAIN_THREAD();

    // The pre-fold task-tool-handlers.cpp guarded against null
    // PlanService / TaskRunner pointers (they were optional setters
    // on Chat::TaskToolHandlers). On TaskController both are
    // ctor-injected references, so the guard is structurally
    // unnecessary post-fold — they cannot be null.
    //
    // cascadeResolver replaces the startup-pinned
    // Chat::CascadeController&. Each handler resolves the in-flight run's
    // cascade per invocation so a task tool firing on a backgrounded run
    // (while another conversation is foreground) reads the RIGHT
    // responder identity.

    registerStartTaskTool(toolSvc, taskGate, cascadeResolver, activeConvIdGetter);
    registerGetPlanStatusTool(toolSvc, cascadeResolver, activeConvIdGetter);
    registerStopTaskTool(toolSvc, taskGate);
    registerCompleteTaskTool(toolSvc, taskGate, cascadeResolver, activeConvIdGetter);

    qCInfo(verzetaUi) << "TaskController: installed 4 task-lifecycle tool handlers "
                         "(start_task, complete_task, get_task_status, stop_task)";
}

// ---------------------------------------------------------------------------
// start_task — port of Chat::TaskToolHandlers::registerStartTaskHandler.
// ---------------------------------------------------------------------------

void TaskController::registerStartTaskTool(ToolService& toolSvc,
                                           TaskGateService& taskGate,
                                           CascadeResolver cascadeResolver,
                                           std::function<QString()> getter) {
    toolSvc.replaceHandler(
        QStringLiteral("start_task"),
        [this, &taskGate, cascadeResolver, getter](const QJsonObject& args) -> QJsonValue {
            // resolve the in-flight run's cascade per
            // invocation (null-safe: degrades to default-owner fallback).
            Chat::CascadeController* cascadePtr = cascadeResolver ? cascadeResolver() : nullptr;
            auto respAlias = [cascadePtr]() -> QString {
                return cascadePtr ? cascadePtr->currentResponderAlias() : QString();
            };
            const QString goal = args[QStringLiteral("goal")].toString().trimmed();
            if (goal.size() < 5) {
                return QJsonObject{
                    {QStringLiteral("error"), QStringLiteral("goal is required (min 5 chars)")}};
            }

            if (!taskGate.activePlanId().isEmpty()) {
                const auto existing = m_planSvc.getPlan(taskGate.activePlanId());
                if (existing.has_value() && existing->status != PlanStatus::Completed &&
                    existing->status != PlanStatus::Failed) {
                    qCWarning(verzetaUi)
                        << "start_task: idempotency guard — active plan" << taskGate.activePlanId()
                        << "already running, returning existing id instead"
                           " of creating a duplicate";
                    QJsonObject result;
                    result[QStringLiteral("plan_id")] = taskGate.activePlanId();
                    result[QStringLiteral("status")] = QStringLiteral("already_active");
                    result[QStringLiteral("goal")] = existing->goal;
                    result[QStringLiteral("instruction")] =
                        QStringLiteral("A task is already active for this conversation. "
                                       "The plan row is created — your job now is to "
                                       "actually DO the work described by the goal. Use "
                                       "whatever tools you need (write_file, search_web, "
                                       "read_file, etc.), and when you're ready, produce "
                                       "your answer as your normal chat reply. The goal "
                                       "to complete is: \"%1\". Proceed now — do NOT "
                                       "call start_task again.")
                            .arg(existing->goal);
                    return result;
                }
                taskGate.clearActivePlan();
            }

            // -- Build the valid-alias set for this conversation -----
            const QString activeConvId = getter();
            const auto convOpt = m_convSvc.getConversation(activeConvId);
            if (!convOpt.has_value()) {
                return QJsonObject{
                    {QStringLiteral("error"), QStringLiteral("no active conversation")}};
            }

            QStringList validAliases;
            QString defaultOwnerAlias;
            QString defaultOwnerAgentId;

            QHash<QString, QString> aliasNormalizedToCanonical;
            auto registerAlias = [&](const QString& canonical) {
                if (canonical.isEmpty())
                    return;
                validAliases.append(canonical);
                const QString norm =
                    canonical.trimmed().replace(QLatin1Char(' '), QLatin1Char('_')).toLower();
                aliasNormalizedToCanonical.insert(norm, canonical);
                aliasNormalizedToCanonical.insert(canonical.toLower(), canonical);
            };

            if (convOpt->isGroup && m_membershipService) {
                const QList<Member> members =
                    m_membershipService->conversationMembers(activeConvId);
                for (const Member& m : members) {
                    registerAlias(m.alias);
                }
                if (!respAlias().isEmpty() &&
                    validAliases.contains(respAlias(), Qt::CaseInsensitive)) {
                    defaultOwnerAlias = respAlias();
                } else {
                    const Member def =
                        m_membershipService->defaultResponderForConversation(activeConvId);
                    if (def.isValid())
                        defaultOwnerAlias = def.alias;
                }
            } else {
                defaultOwnerAgentId = convOpt->primaryAgentId;
                if (m_membershipService && !convOpt->folderId.isEmpty()) {
                    const QList<Member> proj =
                        m_membershipService->projectMembers(convOpt->folderId);
                    for (const Member& m : proj) {
                        if (m.agentId == defaultOwnerAgentId) {
                            defaultOwnerAlias = m.alias;
                            break;
                        }
                    }
                }
                if (defaultOwnerAlias.isEmpty() && m_agentRegistry &&
                    !defaultOwnerAgentId.isEmpty()) {
                    const Agent a = m_agentRegistry->getAgent(defaultOwnerAgentId);
                    if (a.isValid())
                        defaultOwnerAlias = a.name;
                }
                if (defaultOwnerAlias.isEmpty()) {
                    defaultOwnerAlias =
                        respAlias().isEmpty() ? QStringLiteral("assistant") : respAlias();
                }
                registerAlias(defaultOwnerAlias);
            }

            auto canonicalAlias = [&](const QString& raw) -> QString {
                if (raw.isEmpty())
                    return {};
                QString stripped = raw.trimmed();
                if (stripped.startsWith(QLatin1Char('@'))) {
                    stripped = stripped.mid(1);
                }
                const QString lower = stripped.toLower();
                auto it = aliasNormalizedToCanonical.constFind(lower);
                if (it != aliasNormalizedToCanonical.constEnd())
                    return it.value();
                const QString collapsed =
                    stripped.replace(QLatin1Char(' '), QLatin1Char('_')).toLower();
                it = aliasNormalizedToCanonical.constFind(collapsed);
                if (it != aliasNormalizedToCanonical.constEnd())
                    return it.value();
                return {};
            };

            QList<PlanStep> steps;
            QStringList rewroteOwners;
            const QJsonArray stepArr = args[QStringLiteral("steps")].toArray();
            for (const QJsonValue& v : stepArr) {
                const QJsonObject o = v.toObject();
                PlanStep s;
                s.title = o[QStringLiteral("title")].toString().trimmed();
                s.description = o[QStringLiteral("description")].toString();
                s.ownerAlias = o[QStringLiteral("owner_alias")].toString().trimmed();
                s.acceptanceCriteria =
                    o[QStringLiteral("acceptance_criteria")].toString().trimmed();
                if (s.title.isEmpty() || s.acceptanceCriteria.isEmpty()) {
                    return QJsonObject{
                        {QStringLiteral("error"),
                         QStringLiteral("each step needs title and acceptance_criteria")}};
                }
                // A task is GROUP-OWNED: owner_alias is informational
                // (display / attribution) only, never a gate. An empty,
                // "user"/"owner", or unrecognised owner silently falls
                // back to the conversation's default owner — start_task
                // must NEVER fail on ownership. (That ownership rejection
                // dead-ended plain 1:1 chats: call #1 was rejected for
                // owner "me", and the next turn's owner-aware framing then
                // forbade the sole participant from doing the work.)
                if (s.ownerAlias.startsWith(QLatin1Char('@'))) {
                    s.ownerAlias = s.ownerAlias.mid(1);
                }
                const QString canonical = canonicalAlias(s.ownerAlias);
                s.ownerAlias = canonical.isEmpty() ? defaultOwnerAlias : canonical;
                steps.append(s);
            }

            QString projectId, orgId;
            const QList<Folder> chain = m_convSvc.folderChainForConversation(activeConvId);
            for (const Folder& f : chain) {
                if (f.folderType == QStringLiteral("project") && projectId.isEmpty()) {
                    projectId = f.id;
                } else if (f.folderType == QStringLiteral("organization") && orgId.isEmpty()) {
                    orgId = f.id;
                }
            }

            const QString callerAlias = respAlias().isEmpty() ? defaultOwnerAlias : respAlias();

            const QString planId = m_taskRunner.startTaskFromToolCall(
                activeConvId, callerAlias, goal, steps, projectId, orgId);
            if (planId.isEmpty()) {
                return QJsonObject{
                    {QStringLiteral("error"), QStringLiteral("Failed to create plan")}};
            }

            taskGate.setActivePlanId(planId);
            qCInfo(verzetaUi) << "start_task (LLM-initiated): anchored conversation to plan"
                              << planId;

            QJsonObject result;
            result[QStringLiteral("plan_id")] = planId;
            result[QStringLiteral("status")] = QStringLiteral("created");
            result[QStringLiteral("steps")] = static_cast<int>(steps.size());
            result[QStringLiteral("note")] =
                QStringLiteral("Plan created and tagged to this conversation. Now do the "
                               "work — use tools as needed. Do NOT call start_task again "
                               "for this task.");
            if (!rewroteOwners.isEmpty()) {
                result[QStringLiteral("notes")] =
                    QStringLiteral("auto-assigned: ") + rewroteOwners.join(QStringLiteral("; "));
            }
            return result;
        });
}

// ---------------------------------------------------------------------------
// get_plan_status — port.
// ---------------------------------------------------------------------------

void TaskController::registerGetPlanStatusTool(ToolService& toolSvc,
                                               CascadeResolver cascadeResolver,
                                               std::function<QString()> getter) {
    toolSvc.replaceHandler(
        QStringLiteral("get_task_status"),
        [this, cascadeResolver, getter](const QJsonObject& args) -> QJsonValue {
            QString planId = args[QStringLiteral("task_id")].toString();
            if (planId.isEmpty()) {
                planId = args[QStringLiteral("plan_id")].toString();
            }
            QJsonArray results;

            if (!planId.isEmpty()) {
                const auto p = m_planSvc.getPlan(planId);
                if (!p.has_value()) {
                    return QJsonObject{{QStringLiteral("error"), QStringLiteral("plan not found")}};
                }
                QJsonObject planObj;
                planObj[QStringLiteral("id")] = p->id;
                planObj[QStringLiteral("goal")] = p->goal;
                planObj[QStringLiteral("status")] = planStatusToString(p->status);
                QJsonArray stepArr;
                for (const PlanStep& s : m_planSvc.stepsForPlan(planId)) {
                    QJsonObject so;
                    so[QStringLiteral("id")] = s.id;
                    so[QStringLiteral("title")] = s.title;
                    so[QStringLiteral("owner_alias")] = s.ownerAlias;
                    so[QStringLiteral("status")] = stepStatusToString(s.status);
                    stepArr.append(so);
                }
                planObj[QStringLiteral("steps")] = stepArr;
                results.append(planObj);
            } else {
                // resolve the in-flight run's cascade per
                // call (null-safe).
                Chat::CascadeController* cascadePtr = cascadeResolver ? cascadeResolver() : nullptr;
                const QString caller = cascadePtr ? cascadePtr->currentResponderAlias() : QString();
                QString projectId, orgId;
                const QString activeConvId = getter();
                if (!activeConvId.isEmpty()) {
                    const QList<Folder> chain = m_convSvc.folderChainForConversation(activeConvId);
                    for (const Folder& f : chain) {
                        if (f.folderType == QStringLiteral("project") && projectId.isEmpty())
                            projectId = f.id;
                        else if (f.folderType == QStringLiteral("organization") && orgId.isEmpty())
                            orgId = f.id;
                    }
                }
                const QList<AgentPlan> plans =
                    m_planSvc.activePlansForOwnerInScope(caller, projectId, orgId);
                for (const AgentPlan& p : plans) {
                    QJsonObject po;
                    po[QStringLiteral("id")] = p.id;
                    po[QStringLiteral("goal")] = p.goal;
                    po[QStringLiteral("status")] = planStatusToString(p.status);
                    po[QStringLiteral("home_conversation_id")] = p.conversationId;
                    results.append(po);
                }
            }
            return QJsonObject{{QStringLiteral("plans"), results}};
        });
}

// ---------------------------------------------------------------------------
// stop_task — port.
// ---------------------------------------------------------------------------

void TaskController::registerStopTaskTool(ToolService& toolSvc, TaskGateService& taskGate) {
    toolSvc.replaceHandler(
        QStringLiteral("stop_task"), [this, &taskGate](const QJsonObject& args) -> QJsonValue {
            QString planId = args[QStringLiteral("plan_id")].toString();
            if (!taskGate.activePlanId().isEmpty()) {
                if (planId != taskGate.activePlanId()) {
                    qCWarning(verzetaUi) << "stop_task: overriding stale plan_id" << planId << "→"
                                         << taskGate.activePlanId();
                    planId = taskGate.activePlanId();
                }
            } else if (!planId.isEmpty() && !m_planSvc.getPlan(planId).has_value()) {
                return QJsonObject{{QStringLiteral("error"),
                                    QStringLiteral("stop_task: plan_id %1 not found").arg(planId)}};
            }
            const QString reason = args[QStringLiteral("reason")].toString();
            if (!m_taskRunner.handleStopTask(planId, reason)) {
                return QJsonObject{{QStringLiteral("error"), QStringLiteral("plan not found")}};
            }
            if (taskGate.activePlanId() == planId)
                taskGate.clearActivePlan();
            return QJsonObject{{QStringLiteral("plan_id"), planId},
                               {QStringLiteral("status"), QStringLiteral("stopped")}};
        });
}

// ---------------------------------------------------------------------------
// complete_task — close the conversation's active task as completed.
//
// A task is GROUP-OWNED: ANY participant may close it (no owner check), and
// closing is attributed by passive capture of who called it (the responder
// alias) — shown back to the user via a visible system receipt. This replaces
// the old per-step `submit_result` → auto-approve "critic" gate as the simple,
// universal way to finish tracked work.
// ---------------------------------------------------------------------------

void TaskController::registerCompleteTaskTool(ToolService& toolSvc,
                                              TaskGateService& taskGate,
                                              CascadeResolver cascadeResolver,
                                              std::function<QString()> activeConvIdGetter) {
    toolSvc.replaceHandler(
        QStringLiteral("complete_task"),
        [this, &taskGate, cascadeResolver, activeConvIdGetter](
            const QJsonObject& args) -> QJsonValue {
            // Resolve the conversation's active task. The contract is
            // "close the CONVERSATION's open task" — the in-memory
            // anchor is an optimization, not the source of truth, so
            // an empty anchor falls back to the conversation's own
            // open (Executing/Planning) plan before the explicit
            // plan_id escape hatch. Depending on the anchor alone
            // made the tool error with "no active task" whenever the
            // anchor was lost while the DB still held the open plan.
            QString planId = taskGate.activePlanId();
            if (planId.isEmpty()) {
                QString convId = args[QStringLiteral("__caller_conv_id")].toString().trimmed();
                if (convId.isEmpty())
                    convId = activeConvIdGetter();
                if (!convId.isEmpty()) {
                    const QList<AgentPlan> open = m_planSvc.activePlansForConversation(convId);
                    for (const AgentPlan& p : open) {
                        if (p.status == PlanStatus::Executing || p.status == PlanStatus::Planning) {
                            planId = p.id;  // newest first (updated_at DESC)
                            break;
                        }
                    }
                }
            }
            if (planId.isEmpty()) {
                planId = args[QStringLiteral("plan_id")].toString().trimmed();
            }
            if (planId.isEmpty() || !m_planSvc.getPlan(planId).has_value()) {
                return QJsonObject{{QStringLiteral("error"),
                                    QStringLiteral("complete_task: no active task to "
                                                   "complete")}};
            }

            // Who is closing it — group-owned, so anyone; just capture it
            // for attribution. (Persisted completed_by + overlay display
            // land with the schema-v25 attribution change.)
            Chat::CascadeController* cascadePtr = cascadeResolver ? cascadeResolver() : nullptr;
            QString closer = cascadePtr ? cascadePtr->currentResponderAlias() : QString();
            if (closer.isEmpty())
                closer = QStringLiteral("assistant");

            if (!m_planSvc.updatePlanStatus(planId, PlanStatus::Completed)) {
                return QJsonObject{{QStringLiteral("error"),
                                    QStringLiteral("complete_task: failed to mark "
                                                   "completed")}};
            }
            // Closing the task closes its OUTLINE: the plan is
            // group-owned, so completion subsumes any step items that
            // were never individually ticked. Leaving them pending
            // made a completed task render a forever-partial progress
            // bar (doneSteps/totalSteps drives the fill) and a step
            // list of open items on a closed plan.
            const QList<PlanStep> outline = m_planSvc.stepsForPlan(planId);
            for (const PlanStep& st : outline) {
                if (st.status != StepStatus::Done) {
                    m_planSvc.updateStepStatus(
                        st.id,
                        StepStatus::Done,
                        QStringLiteral("Closed when the task was completed."));
                }
            }
            const auto plan = m_planSvc.getPlan(planId);
            const QString goal = plan.has_value() ? plan->goal : QString();
            if (taskGate.activePlanId() == planId)
                taskGate.clearActivePlan();

            const QString reason = args[QStringLiteral("reason")].toString().trimmed();
            const QString convId = activeConvIdGetter();
            if (!convId.isEmpty()) {
                // Use \u escapes, NOT raw \xNN byte escapes: inside a
                // QStringLiteral (a UTF-16 u"" literal) a \xNN run is read
                // as separate UTF-16 code units, not UTF-8 bytes, so the old
                // "\xE2\x9C\x85" mojibaked to U+00E2 + two control chars
                // (a-circumflex + boxes) in chat. \u2705 = check.
                QString msg = QStringLiteral("\u2705 Task completed by @%1").arg(closer);
                if (!goal.isEmpty())
                    msg += QStringLiteral(": %1").arg(goal);
                if (!reason.isEmpty()) {
                    msg += QStringLiteral(" (%1)").arg(reason);
                }
                m_msgSvc.addInterveningSystemMessage(convId, msg);
            }
            return QJsonObject{{QStringLiteral("plan_id"), planId},
                               {QStringLiteral("status"), QStringLiteral("completed")},
                               {QStringLiteral("completed_by"), closer}};
        });
}

// ---------------------------------------------------------------------------
// userInitiatedStartTask — owner resolution + plan creation + taskStarted emit
// ---------------------------------------------------------------------------

void TaskController::userInitiatedStartTask(const QString& convId, const QString& goalText) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty() || goalText.trimmed().isEmpty())
        return;

    // Persist the user's goal as a normal user message so it's
    // visible in the chat history and the model sees it as part of
    // the turn context.
    Message userMsg;
    userMsg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    userMsg.conversationId = convId;
    userMsg.role = QStringLiteral("user");
    userMsg.content = QStringLiteral("Start a task: %1").arg(goalText.trimmed());
    userMsg.createdAt = QDateTime::currentDateTimeUtc();
    userMsg.tokenCount = 0;
    if (m_msgSvc.addMessage(userMsg).isEmpty()) {
        emit errorOccurred(QStringLiteral("Failed to persist task goal"));
        return;
    }

    const auto convOpt = m_convSvc.getConversation(convId);
    if (!convOpt.has_value())
        return;

    // Resolve the owner alias + agent id. In group chats the user
    // may have @-mentioned a teammate; honor that first. Fall back
    // to the default responder (coordinator, else first member).
    // In 1:1 chats use the conversation's primary agent; fall back
    // to the agent template name via AgentRegistry or the generic
    // "assistant" label.
    QString ownerAlias;
    QString ownerAgentId;
    if (convOpt->isGroup && m_membershipService) {
        const QRegularExpression mentionRe(QStringLiteral("@([A-Za-z0-9_]+)"));
        const QRegularExpressionMatch firstMention = mentionRe.match(goalText);
        if (firstMention.hasMatch()) {
            const QString token = firstMention.captured(1);
            if (token.compare(QStringLiteral("user"), Qt::CaseInsensitive) != 0 &&
                token.compare(QStringLiteral("owner"), Qt::CaseInsensitive) != 0 &&
                token.compare(QStringLiteral("you"), Qt::CaseInsensitive) != 0 &&
                token.compare(QStringLiteral("leader"), Qt::CaseInsensitive) != 0 &&
                token.compare(QStringLiteral("all"), Qt::CaseInsensitive) != 0 &&
                token.compare(QStringLiteral("everyone"), Qt::CaseInsensitive) != 0) {
                const Member mentioned =
                    m_membershipService->findConversationMemberByAlias(convId, token);
                if (mentioned.isValid()) {
                    ownerAlias = mentioned.alias;
                    ownerAgentId = mentioned.agentId;
                    qCInfo(verzetaUi)
                        << "TaskController: routing task to @mention" << token << "→" << ownerAlias;
                }
            }
        }
        if (ownerAlias.isEmpty()) {
            const Member def = m_membershipService->defaultResponderForConversation(convId);
            if (def.isValid()) {
                ownerAlias = def.alias;
                ownerAgentId = def.agentId;
            }
        }
    } else {
        ownerAgentId = convOpt->primaryAgentId;
        if (!ownerAgentId.isEmpty()) {
            if (m_membershipService && !convOpt->folderId.isEmpty()) {
                const QList<Member> projMembers =
                    m_membershipService->projectMembers(convOpt->folderId);
                for (const Member& m : projMembers) {
                    if (m.agentId == ownerAgentId) {
                        ownerAlias = m.alias;
                        break;
                    }
                }
            }
            if (ownerAlias.isEmpty() && m_agentRegistry) {
                const Agent a = m_agentRegistry->getAgent(ownerAgentId);
                if (a.isValid())
                    ownerAlias = a.name;
            }
            if (ownerAlias.isEmpty()) {
                ownerAlias = QStringLiteral("assistant");
            }
        }
    }
    if (ownerAlias.isEmpty()) {
        // Legacy / no-agent chats: the task still runs (the
        // conversation's normal responder will reply to the goal
        // user message), just with a generic owner label on the
        // plan row.
        ownerAlias = QStringLiteral("assistant");
    }

    // Scope resolution — walk the folder chain for project / org ids.
    QString projectId, orgId;
    const QList<Folder> chain = m_convSvc.folderChainForConversation(convId);
    for (const Folder& f : chain) {
        if (f.folderType == QStringLiteral("project") && projectId.isEmpty())
            projectId = f.id;
        else if (f.folderType == QStringLiteral("organization") && orgId.isEmpty())
            orgId = f.id;
    }

    // Single-step plan — the LLM decides how to actually execute the
    // work; the plan row is just an anchor the PlansOverlay + ACTIVE
    // TASK framing attach to.
    QList<PlanStep> singleStep;
    {
        PlanStep s;
        s.title = goalText.trimmed().left(80);
        s.description = goalText.trimmed();
        s.ownerAlias = ownerAlias;
        s.acceptanceCriteria =
            QStringLiteral("Produce a concrete artifact (a complete draft, analysis, "
                           "decision, or file) that fully addresses: %1")
                .arg(goalText.trimmed().left(200));
        singleStep.append(s);
    }

    const QString planId = m_taskRunner.startTaskFromToolCall(
        convId, ownerAlias, goalText.trimmed(), singleStep, projectId, orgId);
    if (planId.isEmpty()) {
        emit errorOccurred(QStringLiteral("Failed to start task"));
        return;
    }

    qCInfo(verzetaUi) << "TaskController: user-initiated task started:" << planId
                      << "owner:" << ownerAlias << "goal:" << goalText.left(60);

    emit taskStarted(planId, convId, ownerAlias, ownerAgentId);
}

// ---------------------------------------------------------------------------
// stopPlan + stopAllActivePlansInConversation
// ---------------------------------------------------------------------------

bool TaskController::stopPlan(const QString& planId, const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    const bool ok = m_taskRunner.handleStopTask(
        planId, reason.isEmpty() ? QStringLiteral("stopped by user") : reason);
    if (!ok)
        return false;
    emit planStopped(planId);
    return true;
}

int TaskController::stopAllActivePlansInConversation(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return 0;

    const QList<AgentPlan> active = m_planSvc.activePlansForConversation(convId);
    int stopped = 0;
    for (const AgentPlan& p : active) {
        if (m_taskRunner.handleStopTask(
                p.id, QStringLiteral("stopped by user from the chat input bar"))) {
            ++stopped;
        }
    }
    if (stopped > 0) {
        emit allPlansStoppedInConversation(convId);
    }
    qCInfo(verzetaUi) << "TaskController: stopped" << stopped << "plan(s) in" << convId;
    return stopped;
}

// ---------------------------------------------------------------------------
// Step interventions (retry / override / skip) — TaskRunner owns state
// ---------------------------------------------------------------------------

bool TaskController::retryStep(const QString& stepId, const QString& userNote) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_taskRunner.userRetryStep(stepId, userNote);
}

bool TaskController::overrideStepAsDone(const QString& stepId) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_taskRunner.userOverrideStepAsDone(stepId);
}

bool TaskController::skipStep(const QString& stepId, const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_taskRunner.userSkipStep(stepId, reason);
}

// ---------------------------------------------------------------------------
// Primary agent
// ---------------------------------------------------------------------------

void TaskController::setPrimaryAgent(const QString& convId, const QString& agentId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return;
    // ConversationService::updatePrimaryAgent emits conversationUpdated;
    // ChatController's conversationUpdated listener refreshes the
    // active-session cached title + settings when convId matches.
    m_convSvc.updatePrimaryAgent(convId, agentId);
}

QString TaskController::primaryAgentId(const QString& convId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return {};
    const auto conv = m_convSvc.getConversation(convId);
    return conv.has_value() ? conv->primaryAgentId : QString{};
}

// ---------------------------------------------------------------------------
// postTaskEventMessage — TaskRunner → system-message writer
// ---------------------------------------------------------------------------

void TaskController::postTaskEventMessage(const QString& convId,
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
    msg.metadata = QJsonObject{
        {QStringLiteral("task_event"), true},
        {QStringLiteral("event_type"), eventType},
    };
    m_msgSvc.addMessage(msg);
}

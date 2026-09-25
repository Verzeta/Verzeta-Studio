// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file plan-service.cpp
 * @brief Implementation of PlanService: CRUD on the `agent_plans`,
 *        `plan_steps`, and `step_artifacts` tables.
 * @layer Service (Data Access)
 * @dependencies DbManager, Qt6::Core, Qt6::Sql.
 */

#include "plan-service.h"

#include "../models/db-manager.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

QString newUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QDateTime fromMsSinceEpoch(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC);
}

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PlanService::PlanService(DbManager& db, QObject* parent) : QObject(parent), m_db(db) {}

PlanService::~PlanService() = default;

// ---------------------------------------------------------------------------
// Row → struct converters
// ---------------------------------------------------------------------------

AgentPlan PlanService::plainPlanFromRow(const QSqlQuery& q) {
    VERZETA_ASSERT_MAIN_THREAD();
    AgentPlan p;
    p.id = q.value(QStringLiteral("id")).toString();
    p.conversationId = q.value(QStringLiteral("conversation_id")).toString();
    p.projectFolderId = q.value(QStringLiteral("project_folder_id")).toString();
    p.organizationFolderId = q.value(QStringLiteral("organization_folder_id")).toString();
    p.goal = q.value(QStringLiteral("goal")).toString();
    p.status = planStatusFromString(q.value(QStringLiteral("status")).toString());
    p.startedBy = q.value(QStringLiteral("started_by")).toString();
    p.createdAt = fromMsSinceEpoch(q.value(QStringLiteral("created_at")).toLongLong());
    p.updatedAt = fromMsSinceEpoch(q.value(QStringLiteral("updated_at")).toLongLong());
    p.turnsUsed = q.value(QStringLiteral("turns_used")).toInt();
    p.heartbeatsUsed = q.value(QStringLiteral("heartbeats_used")).toInt();
    return p;
}

PlanStep PlanService::plainStepFromRow(const QSqlQuery& q) {
    VERZETA_ASSERT_MAIN_THREAD();
    PlanStep s;
    s.id = q.value(QStringLiteral("id")).toString();
    s.planId = q.value(QStringLiteral("plan_id")).toString();
    s.ordering = q.value(QStringLiteral("ordering")).toInt();
    s.title = q.value(QStringLiteral("title")).toString();
    s.description = q.value(QStringLiteral("description")).toString();
    s.ownerAlias = q.value(QStringLiteral("owner_alias")).toString();
    s.acceptanceCriteria = q.value(QStringLiteral("acceptance_criteria")).toString();
    s.status = stepStatusFromString(q.value(QStringLiteral("status")).toString());
    s.rejectionCount = q.value(QStringLiteral("rejection_count")).toInt();
    s.toolRetryCount = q.value(QStringLiteral("tool_retry_count")).toInt();
    s.executorTurnsUsed = q.value(QStringLiteral("executor_turns_used")).toInt();
    s.lastRejectionReason = q.value(QStringLiteral("last_rejection_reason")).toString();
    s.createdAt = fromMsSinceEpoch(q.value(QStringLiteral("created_at")).toLongLong());
    s.updatedAt = fromMsSinceEpoch(q.value(QStringLiteral("updated_at")).toLongLong());
    return s;
}

StepArtifact PlanService::plainArtifactFromRow(const QSqlQuery& q) {
    VERZETA_ASSERT_MAIN_THREAD();
    StepArtifact a;
    a.id = q.value(QStringLiteral("id")).toString();
    a.stepId = q.value(QStringLiteral("step_id")).toString();
    a.type = artifactTypeFromString(q.value(QStringLiteral("artifact_type")).toString());
    a.filePath = q.value(QStringLiteral("file_path")).toString();
    a.content = q.value(QStringLiteral("content")).toString();
    a.summary = q.value(QStringLiteral("summary")).toString();
    a.submittedByAlias = q.value(QStringLiteral("submitted_by_alias")).toString();
    a.createdAt = fromMsSinceEpoch(q.value(QStringLiteral("created_at")).toLongLong());
    a.approved = q.value(QStringLiteral("approved")).toInt();

    const QString reasonsJson = q.value(QStringLiteral("critic_reasons")).toString();
    if (!reasonsJson.isEmpty()) {
        const QJsonArray arr = QJsonDocument::fromJson(reasonsJson.toUtf8()).array();
        for (const QJsonValue& v : arr)
            a.criticReasons.append(v.toString());
    }
    return a;
}

// ---------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------

QString PlanService::createPlan(AgentPlan plan, QList<PlanStep> steps) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (plan.conversationId.isEmpty() || plan.goal.isEmpty()) {
        qCWarning(verzetaDb) << "createPlan: conversationId and goal required";
        return {};
    }
    if (steps.isEmpty()) {
        qCWarning(verzetaDb) << "createPlan: at least one step required";
        return {};
    }

    if (!m_db.transaction()) {
        qCWarning(verzetaDb) << "createPlan: transaction start failed";
        return {};
    }

    const qint64 now = nowMs();
    if (plan.id.isEmpty())
        plan.id = newUuid();
    if (!plan.createdAt.isValid())
        plan.createdAt = fromMsSinceEpoch(now);
    plan.updatedAt = fromMsSinceEpoch(now);

    QSqlQuery ins(m_db.db());
    ins.prepare(QStringLiteral("INSERT INTO agent_plans (id, conversation_id, project_folder_id, "
                               "organization_folder_id, goal, status, started_by, created_at, "
                               "updated_at, turns_used, heartbeats_used) "
                               "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(plan.id);
    ins.addBindValue(plan.conversationId);
    ins.addBindValue(plan.projectFolderId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                                    : QVariant(plan.projectFolderId));
    ins.addBindValue(plan.organizationFolderId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                                         : QVariant(plan.organizationFolderId));
    ins.addBindValue(plan.goal);
    ins.addBindValue(planStatusToString(plan.status));
    ins.addBindValue(plan.startedBy);
    ins.addBindValue(plan.createdAt.toMSecsSinceEpoch());
    ins.addBindValue(plan.updatedAt.toMSecsSinceEpoch());
    ins.addBindValue(plan.turnsUsed);
    ins.addBindValue(plan.heartbeatsUsed);

    if (!ins.exec()) {
        qCWarning(verzetaDb) << "createPlan: insert plan failed:" << ins.lastError().text();
        m_db.rollback();
        return {};
    }

    // Insert steps
    for (int i = 0; i < steps.size(); ++i) {
        PlanStep& s = steps[i];
        if (s.id.isEmpty())
            s.id = newUuid();
        s.planId = plan.id;
        s.ordering = i;
        s.createdAt = fromMsSinceEpoch(now);
        s.updatedAt = fromMsSinceEpoch(now);
        if (s.status == StepStatus::Pending) {
            // default already
        }

        QSqlQuery sIns(m_db.db());
        sIns.prepare(
            QStringLiteral("INSERT INTO plan_steps (id, plan_id, ordering, title, description, "
                           "owner_alias, acceptance_criteria, status, rejection_count, "
                           "tool_retry_count, executor_turns_used, last_rejection_reason, "
                           "created_at, updated_at) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        sIns.addBindValue(s.id);
        sIns.addBindValue(s.planId);
        sIns.addBindValue(s.ordering);
        sIns.addBindValue(s.title);
        sIns.addBindValue(s.description);
        sIns.addBindValue(s.ownerAlias);
        sIns.addBindValue(s.acceptanceCriteria);
        sIns.addBindValue(stepStatusToString(s.status));
        sIns.addBindValue(s.rejectionCount);
        sIns.addBindValue(s.toolRetryCount);
        sIns.addBindValue(s.executorTurnsUsed);
        sIns.addBindValue(s.lastRejectionReason);
        sIns.addBindValue(s.createdAt.toMSecsSinceEpoch());
        sIns.addBindValue(s.updatedAt.toMSecsSinceEpoch());

        if (!sIns.exec()) {
            qCWarning(verzetaDb) << "createPlan: insert step failed:" << sIns.lastError().text();
            m_db.rollback();
            return {};
        }
    }

    if (!m_db.commit()) {
        qCWarning(verzetaDb) << "createPlan: commit failed";
        m_db.rollback();
        return {};
    }

    emit planCreated(plan.id);
    return plan.id;
}

std::optional<AgentPlan> PlanService::getPlan(const QString& planId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM agent_plans WHERE id = ?"));
    q.addBindValue(planId);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return plainPlanFromRow(q);
}

QList<AgentPlan> PlanService::plansForConversation(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<AgentPlan> out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM agent_plans WHERE conversation_id = ? "
                             "ORDER BY updated_at DESC"));
    q.addBindValue(conversationId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "plansForConversation failed:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(plainPlanFromRow(q));
    return out;
}

QList<AgentPlan> PlanService::activePlansForConversation(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<AgentPlan> out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM agent_plans "
                             "WHERE conversation_id = ? "
                             "AND status IN ('planning','executing','critiquing','blocked') "
                             "ORDER BY updated_at DESC"));
    q.addBindValue(conversationId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "activePlansForConversation failed:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(plainPlanFromRow(q));
    return out;
}

QList<AgentPlan> PlanService::plansByAgentIdAcrossChats(const QString& agentId,
                                                        const QStringList& statuses,
                                                        int limitDays) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<AgentPlan> out;
    if (agentId.isEmpty() || statuses.isEmpty())
        return out;

    // Build an IN-list of status placeholders
    QStringList placeholders;
    for (int i = 0; i < statuses.size(); ++i)
        placeholders << QStringLiteral("?");
    const QString statusIn = placeholders.join(QLatin1Char(','));

    QString sql = QStringLiteral("SELECT DISTINCT p.* "
                                 "FROM agent_plans p "
                                 "JOIN plan_steps s ON s.plan_id = p.id "
                                 "LEFT JOIN conversations c ON c.id = p.conversation_id "
                                 "WHERE p.status IN (%1) "
                                 "  AND ( "
                                 "    c.primary_agent_id = ? "
                                 "    OR EXISTS ( "
                                 "       SELECT 1 FROM conversation_members cm "
                                 "       WHERE cm.conversation_id = p.conversation_id "
                                 "         AND cm.agent_id = ? "
                                 "         AND cm.alias = s.owner_alias "
                                 "    ) "
                                 "    OR ( "
                                 "       p.project_folder_id IS NOT NULL "
                                 "       AND c.is_group = 0 "
                                 "       AND EXISTS ( "
                                 "           SELECT 1 FROM project_members pm "
                                 "           WHERE pm.folder_id = p.project_folder_id "
                                 "             AND pm.agent_id = ? "
                                 "       ) "
                                 "    ) "
                                 "  ) ")
                      .arg(statusIn);

    QList<QVariant> binds;
    for (const QString& s : statuses)
        binds.append(s);
    binds.append(agentId);  // 1:1 primary
    binds.append(agentId);  // group conversation_members
    binds.append(agentId);  // project_members roster

    if (limitDays > 0) {
        const qint64 cutoff =
            QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(limitDays) * 86400000LL;
        sql += QStringLiteral("AND p.updated_at >= ? ");
        binds.append(cutoff);
    }
    sql += QStringLiteral("ORDER BY p.updated_at DESC LIMIT 20");

    QSqlQuery q(m_db.db());
    q.prepare(sql);
    for (const QVariant& b : binds)
        q.addBindValue(b);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "plansByAgentIdAcrossChats failed:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(plainPlanFromRow(q));
    qCDebug(verzetaDb) << "plansByAgentIdAcrossChats: agent" << agentId << "statuses" << statuses
                       << "→" << out.size() << "plan(s)";
    return out;
}

QList<AgentPlan>
PlanService::recentlyCompletedPlansForOwnerInScope(const QString& ownerAlias,
                                                   const QString& projectFolderId,
                                                   const QString& organizationFolderId,
                                                   int limitDays) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<AgentPlan> out;
    if (ownerAlias.isEmpty())
        return out;

    const qint64 cutoffMs = QDateTime::currentMSecsSinceEpoch() -
                            static_cast<qint64>(limitDays) * 24LL * 3600LL * 1000LL;

    QString sql = QStringLiteral("SELECT DISTINCT p.* FROM agent_plans p "
                                 "JOIN plan_steps s ON s.plan_id = p.id "
                                 "WHERE s.owner_alias = ? "
                                 "AND p.status = 'completed' "
                                 "AND p.updated_at >= ? ");

    QList<QVariant> binds;
    binds.append(ownerAlias);
    binds.append(cutoffMs);

    if (!projectFolderId.isEmpty()) {
        sql += QStringLiteral("AND p.project_folder_id = ? ");
        binds.append(projectFolderId);
    }
    if (!organizationFolderId.isEmpty()) {
        sql += QStringLiteral("AND p.organization_folder_id = ? ");
        binds.append(organizationFolderId);
    }
    sql += QStringLiteral("ORDER BY p.updated_at DESC LIMIT 10");

    QSqlQuery q(m_db.db());
    q.prepare(sql);
    for (const QVariant& b : binds)
        q.addBindValue(b);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "recentlyCompletedPlansForOwnerInScope failed:"
                             << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(plainPlanFromRow(q));
    return out;
}

QList<AgentPlan>
PlanService::activePlansForOwnerInScope(const QString& ownerAlias,
                                        const QString& projectFolderId,
                                        const QString& organizationFolderId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<AgentPlan> out;
    if (ownerAlias.isEmpty())
        return out;

    // Join: find plans that have at least one active step owned by this alias.
    // Then apply scope filter: match project_folder_id / organization_folder_id
    // when either is supplied. If both scope filters are empty, we return all
    // plans the alias owns (loose-scope behavior).
    QString sql =
        QStringLiteral("SELECT DISTINCT p.* FROM agent_plans p "
                       "JOIN plan_steps s ON s.plan_id = p.id "
                       "WHERE s.owner_alias = ? "
                       "AND p.status IN ('planning','executing','critiquing','blocked') "
                       "AND s.status IN ('pending','in_progress','needs_rework','blocked') ");

    QList<QVariant> binds;
    binds.append(ownerAlias);

    if (!projectFolderId.isEmpty()) {
        sql += QStringLiteral("AND p.project_folder_id = ? ");
        binds.append(projectFolderId);
    }
    if (!organizationFolderId.isEmpty()) {
        sql += QStringLiteral("AND p.organization_folder_id = ? ");
        binds.append(organizationFolderId);
    }
    sql += QStringLiteral("ORDER BY p.updated_at DESC");

    QSqlQuery q(m_db.db());
    q.prepare(sql);
    for (const QVariant& b : binds)
        q.addBindValue(b);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "activePlansForOwnerInScope failed:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(plainPlanFromRow(q));
    return out;
}

bool PlanService::updatePlanStatus(const QString& planId, PlanStatus status) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE agent_plans SET status = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(planStatusToString(status));
    q.addBindValue(nowMs());
    q.addBindValue(planId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "updatePlanStatus failed:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0)
        return false;
    emit planStatusChanged(planId, status);
    emit planUpdated(planId);
    return true;
}

int PlanService::deletePlansForConversation(const QString& conversationId) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Collect IDs first so we can emit planDeleted for each
    QList<QString> ids;
    {
        QSqlQuery sel(m_db.db());
        sel.prepare(QStringLiteral("SELECT id FROM agent_plans WHERE conversation_id = ?"));
        sel.addBindValue(conversationId);
        if (!sel.exec())
            return 0;
        while (sel.next())
            ids.append(sel.value(0).toString());
    }
    if (ids.isEmpty())
        return 0;

    QSqlQuery del(m_db.db());
    del.prepare(QStringLiteral("DELETE FROM agent_plans WHERE conversation_id = ?"));
    del.addBindValue(conversationId);
    if (!del.exec()) {
        qCWarning(verzetaDb) << "deletePlansForConversation failed:" << del.lastError().text();
        return 0;
    }
    for (const QString& id : ids)
        emit planDeleted(id);
    return ids.size();
}

bool PlanService::deletePlan(const QString& planId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM agent_plans WHERE id = ?"));
    q.addBindValue(planId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "deletePlan failed:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0)
        return false;
    emit planDeleted(planId);
    return true;
}

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

std::optional<PlanStep> PlanService::getStep(const QString& stepId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM plan_steps WHERE id = ?"));
    q.addBindValue(stepId);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return plainStepFromRow(q);
}

QList<PlanStep> PlanService::stepsForPlan(const QString& planId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<PlanStep> out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM plan_steps WHERE plan_id = ? ORDER BY ordering ASC"));
    q.addBindValue(planId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "stepsForPlan failed:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(plainStepFromRow(q));
    return out;
}

QString PlanService::appendStepToPlan(const QString& planId, PlanStep step) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Compute next ordering
    int nextOrdering = 0;
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral(
            "SELECT COALESCE(MAX(ordering), -1) + 1 FROM plan_steps WHERE plan_id = ?"));
        q.addBindValue(planId);
        if (!q.exec() || !q.next()) {
            qCWarning(verzetaDb) << "appendStepToPlan: max ordering query failed";
            return {};
        }
        nextOrdering = q.value(0).toInt();
    }

    if (step.id.isEmpty())
        step.id = newUuid();
    step.planId = planId;
    step.ordering = nextOrdering;
    const qint64 now = nowMs();
    step.createdAt = fromMsSinceEpoch(now);
    step.updatedAt = fromMsSinceEpoch(now);

    QSqlQuery ins(m_db.db());
    ins.prepare(QStringLiteral("INSERT INTO plan_steps (id, plan_id, ordering, title, description, "
                               "owner_alias, acceptance_criteria, status, rejection_count, "
                               "tool_retry_count, executor_turns_used, last_rejection_reason, "
                               "created_at, updated_at) "
                               "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(step.id);
    ins.addBindValue(step.planId);
    ins.addBindValue(step.ordering);
    ins.addBindValue(step.title);
    ins.addBindValue(step.description);
    ins.addBindValue(step.ownerAlias);
    ins.addBindValue(step.acceptanceCriteria);
    ins.addBindValue(stepStatusToString(step.status));
    ins.addBindValue(step.rejectionCount);
    ins.addBindValue(step.toolRetryCount);
    ins.addBindValue(step.executorTurnsUsed);
    ins.addBindValue(step.lastRejectionReason);
    ins.addBindValue(step.createdAt.toMSecsSinceEpoch());
    ins.addBindValue(step.updatedAt.toMSecsSinceEpoch());

    if (!ins.exec()) {
        qCWarning(verzetaDb) << "appendStepToPlan insert failed:" << ins.lastError().text();
        return {};
    }
    emit planUpdated(planId);
    emit stepUpdated(step.id);
    return step.id;
}

bool PlanService::updateStepStatus(const QString& stepId,
                                   StepStatus status,
                                   const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    if (reason.isEmpty()) {
        q.prepare(QStringLiteral("UPDATE plan_steps SET status = ?, updated_at = ? WHERE id = ?"));
        q.addBindValue(stepStatusToString(status));
        q.addBindValue(nowMs());
        q.addBindValue(stepId);
    } else {
        q.prepare(QStringLiteral("UPDATE plan_steps SET status = ?, last_rejection_reason = ?, "
                                 "updated_at = ? WHERE id = ?"));
        q.addBindValue(stepStatusToString(status));
        q.addBindValue(reason);
        q.addBindValue(nowMs());
        q.addBindValue(stepId);
    }
    if (!q.exec() || q.numRowsAffected() == 0)
        return false;
    emit stepStatusChanged(stepId, status);
    emit stepUpdated(stepId);
    return true;
}

bool PlanService::updateStepOwner(const QString& stepId, const QString& newOwnerAlias) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE plan_steps SET owner_alias = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(newOwnerAlias);
    q.addBindValue(nowMs());
    q.addBindValue(stepId);
    if (!q.exec() || q.numRowsAffected() == 0)
        return false;
    emit stepUpdated(stepId);
    return true;
}

bool PlanService::incrementStepRejectionCount(const QString& stepId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE plan_steps SET rejection_count = rejection_count + 1, "
                             "updated_at = ? WHERE id = ?"));
    q.addBindValue(nowMs());
    q.addBindValue(stepId);
    if (!q.exec() || q.numRowsAffected() == 0)
        return false;
    emit stepUpdated(stepId);
    return true;
}

bool PlanService::incrementStepToolRetryCount(const QString& stepId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE plan_steps SET tool_retry_count = tool_retry_count + 1, "
                             "updated_at = ? WHERE id = ?"));
    q.addBindValue(nowMs());
    q.addBindValue(stepId);
    if (!q.exec() || q.numRowsAffected() == 0)
        return false;
    emit stepUpdated(stepId);
    return true;
}

bool PlanService::incrementStepExecutorTurns(const QString& stepId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE plan_steps SET executor_turns_used = executor_turns_used + 1, "
                             "updated_at = ? WHERE id = ?"));
    q.addBindValue(nowMs());
    q.addBindValue(stepId);
    if (!q.exec() || q.numRowsAffected() == 0)
        return false;
    emit stepUpdated(stepId);
    return true;
}

bool PlanService::resetStepForRetry(const QString& stepId, const QString& userNote) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE plan_steps SET status = 'pending', rejection_count = 0, "
                             "tool_retry_count = 0, executor_turns_used = 0, "
                             "last_rejection_reason = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(userNote);
    q.addBindValue(nowMs());
    q.addBindValue(stepId);
    if (!q.exec() || q.numRowsAffected() == 0)
        return false;
    emit stepStatusChanged(stepId, StepStatus::Pending);
    emit stepUpdated(stepId);
    return true;
}

std::optional<PlanStep>
PlanService::activeStepForOwnerInConversation(const QString& conversationId,
                                              const QString& ownerAlias) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT s.* FROM plan_steps s "
                             "JOIN agent_plans p ON p.id = s.plan_id "
                             "WHERE p.conversation_id = ? "
                             "AND s.owner_alias = ? "
                             "AND s.status IN ('pending','in_progress','needs_rework') "
                             "AND p.status IN ('planning','executing','critiquing') "
                             "ORDER BY s.updated_at ASC LIMIT 1"));
    q.addBindValue(conversationId);
    q.addBindValue(ownerAlias);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return plainStepFromRow(q);
}

// ---------------------------------------------------------------------------
// Artifacts
// ---------------------------------------------------------------------------

QString PlanService::addArtifact(StepArtifact artifact) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (artifact.stepId.isEmpty() || artifact.summary.isEmpty()) {
        qCWarning(verzetaDb) << "addArtifact: stepId and summary required";
        return {};
    }
    if (artifact.id.isEmpty())
        artifact.id = newUuid();
    if (!artifact.createdAt.isValid())
        artifact.createdAt = fromMsSinceEpoch(nowMs());

    QString reasonsJson;
    if (!artifact.criticReasons.isEmpty()) {
        QJsonArray arr;
        for (const QString& r : artifact.criticReasons)
            arr.append(r);
        reasonsJson = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT INTO step_artifacts (id, step_id, artifact_type, file_path, "
        "content, summary, submitted_by_alias, created_at, approved, critic_reasons) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(artifact.id);
    q.addBindValue(artifact.stepId);
    q.addBindValue(artifactTypeToString(artifact.type));
    q.addBindValue(artifact.filePath);
    q.addBindValue(artifact.content);
    q.addBindValue(artifact.summary);
    q.addBindValue(artifact.submittedByAlias);
    q.addBindValue(artifact.createdAt.toMSecsSinceEpoch());
    q.addBindValue(artifact.approved);
    q.addBindValue(reasonsJson);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "addArtifact failed:" << q.lastError().text();
        return {};
    }
    emit artifactAdded(artifact.id);
    return artifact.id;
}

QList<StepArtifact> PlanService::artifactsForStep(const QString& stepId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<StepArtifact> out;
    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("SELECT * FROM step_artifacts WHERE step_id = ? ORDER BY created_at DESC"));
    q.addBindValue(stepId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "artifactsForStep failed:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(plainArtifactFromRow(q));
    return out;
}

bool PlanService::setArtifactApproval(const QString& artifactId,
                                      int approved,
                                      const QStringList& reasons) {
    VERZETA_ASSERT_MAIN_THREAD();
    QString reasonsJson;
    if (!reasons.isEmpty()) {
        QJsonArray arr;
        for (const QString& r : reasons)
            arr.append(r);
        reasonsJson = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("UPDATE step_artifacts SET approved = ?, critic_reasons = ? WHERE id = ?"));
    q.addBindValue(approved);
    q.addBindValue(reasonsJson);
    q.addBindValue(artifactId);
    if (!q.exec() || q.numRowsAffected() == 0)
        return false;
    emit artifactApprovalChanged(artifactId, approved);
    return true;
}

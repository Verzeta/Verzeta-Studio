// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file plans-model.cpp
 * @brief Push-based plans model implementation. Subscribes to PlanService
 *        signals and translates every change into row-level Qt Model/View
 *        mutations. No polling, no refresh(), no timer.
 * @layer Service (Model)
 * @dependencies PlanService, Qt6::Core.
 */

#include "plans-model.h"

#include "../services/plan-service.h"
#include "../utils/logger.h"

#include <QDateTime>
#include <QVariantMap>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PlansModel::PlansModel(PlanService& svc, QObject* parent) : QAbstractListModel(parent), m_svc(svc) {
    connect(&m_svc, &PlanService::planCreated, this, &PlansModel::onPlanCreated);
    connect(&m_svc, &PlanService::planUpdated, this, &PlansModel::onPlanUpdated);
    connect(&m_svc, &PlanService::planStatusChanged, this, [this](const QString& id, PlanStatus) {
        onPlanUpdated(id);
    });
    connect(&m_svc, &PlanService::planDeleted, this, &PlansModel::onPlanDeleted);
    connect(&m_svc, &PlanService::stepUpdated, this, &PlansModel::onStepUpdated);
    connect(&m_svc,
            &PlanService::stepStatusChanged,
            this,
            [this](const QString& stepId, StepStatus) { onStepUpdated(stepId); });
    connect(&m_svc, &PlanService::artifactAdded, this, &PlansModel::onArtifactAdded);
}

// ---------------------------------------------------------------------------
// Active conversation binding
// ---------------------------------------------------------------------------

void PlansModel::setActiveConversation(const QString& convId) {
    if (convId == m_activeConvId)
        return;
    m_activeConvId = convId;
    reloadActiveConversation();
    emit activeConversationChanged();
}

QString PlansModel::activeConversationId() const {
    return m_activeConvId;
}

int PlansModel::count() const {
    return static_cast<int>(m_plans.size());
}

void PlansModel::onSourceConversationDeleted(const QString& convId) {
    // Walk our in-memory rows in reverse and remove any plans whose
    // conversationId matches. This handles the case where the DB
    // cascade already wiped the plan rows before PlanService had a
    // chance to emit planDeleted for them.
    for (int i = static_cast<int>(m_plans.size()) - 1; i >= 0; --i) {
        if (m_plans.at(i).conversationId != convId)
            continue;
        beginRemoveRows({}, i, i);
        m_plans.removeAt(i);
        endRemoveRows();
    }
    emit countChanged();
    recomputeDerivedState();
}

QStringList PlansModel::busyMembers() const {
    return m_busyMembers;
}

bool PlansModel::hasActiveTask() const {
    return m_hasActiveTask;
}

void PlansModel::recomputeDerivedState() {
    QStringList busy;
    bool anyActive = false;
    for (const AgentPlan& p : m_plans) {
        const bool planIsNonTerminal =
            p.status == PlanStatus::Planning || p.status == PlanStatus::Executing ||
            p.status == PlanStatus::Critiquing || p.status == PlanStatus::Blocked;
        if (planIsNonTerminal)
            anyActive = true;

        if (p.status != PlanStatus::Executing && p.status != PlanStatus::Critiquing)
            continue;

        const QList<PlanStep> steps = m_svc.stepsForPlan(p.id);
        for (const PlanStep& s : steps) {
            if (s.status == StepStatus::InProgress || s.status == StepStatus::Submitted) {
                const QString entry = QStringLiteral("@%1 on \"%2\"").arg(s.ownerAlias, s.title);
                if (!busy.contains(entry))
                    busy.append(entry);
            }
        }
    }

    if (busy != m_busyMembers) {
        m_busyMembers = busy;
        emit busyMembersChanged();
    }
    if (anyActive != m_hasActiveTask) {
        m_hasActiveTask = anyActive;
        emit hasActiveTaskChanged();
    }
}

// ---------------------------------------------------------------------------
// QAbstractListModel interface
// ---------------------------------------------------------------------------

int PlansModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_plans.size());
}

QVariant PlansModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_plans.size())) {
        return {};
    }
    const AgentPlan& p = m_plans.at(index.row());
    switch (role) {
        case IdRole:
            return p.id;
        case GoalRole:
            return p.goal;
        case StatusRole:
            return planStatusToString(p.status);
        case StartedByRole:
            return p.startedBy;
        case CreatedAtMsRole:
            return static_cast<qlonglong>(p.createdAt.isValid() ? p.createdAt.toMSecsSinceEpoch()
                                                                : 0);
        case UpdatedAtMsRole:
            return static_cast<qlonglong>(p.updatedAt.isValid() ? p.updatedAt.toMSecsSinceEpoch()
                                                                : 0);
        case StepsRole: {
            return buildStepsForPlan(p);
        }
        case TotalStepsRole: {
            return static_cast<int>(m_svc.stepsForPlan(p.id).size());
        }
        case DoneStepsRole: {
            const QList<PlanStep> steps = m_svc.stepsForPlan(p.id);
            int done = 0;
            for (const PlanStep& s : steps) {
                if (s.status == StepStatus::Done)
                    ++done;
            }
            return done;
        }
        default:
            return {};
    }
}

QHash<int, QByteArray> PlansModel::roleNames() const {
    return {
        {IdRole, QByteArrayLiteral("id")},
        {GoalRole, QByteArrayLiteral("goal")},
        {StatusRole, QByteArrayLiteral("status")},
        {StartedByRole, QByteArrayLiteral("startedBy")},
        {CreatedAtMsRole, QByteArrayLiteral("createdAtMs")},
        {UpdatedAtMsRole, QByteArrayLiteral("updatedAtMs")},
        {TotalStepsRole, QByteArrayLiteral("totalSteps")},
        {DoneStepsRole, QByteArrayLiteral("doneSteps")},
        {StepsRole, QByteArrayLiteral("steps")},
    };
}

// ---------------------------------------------------------------------------
// Signal handlers
// ---------------------------------------------------------------------------

void PlansModel::onPlanCreated(const QString& planId) {
    if (m_activeConvId.isEmpty())
        return;
    const auto planOpt = m_svc.getPlan(planId);
    if (!planOpt.has_value())
        return;
    if (!planBelongsToActiveConversation(*planOpt))
        return;

    // New plans get prepended (freshest first, matches DB order).
    const int insertRow = 0;
    beginInsertRows({}, insertRow, insertRow);
    m_plans.prepend(*planOpt);
    endInsertRows();
    emit countChanged();
    recomputeDerivedState();
}

void PlansModel::onPlanUpdated(const QString& planId) {
    const int row = findPlanRow(planId);
    if (row < 0) {
        // Not currently in our list — might be newly relevant.
        onPlanCreated(planId);
        return;
    }

    const auto planOpt = m_svc.getPlan(planId);
    if (!planOpt.has_value()) {
        onPlanDeleted(planId);
        return;
    }

    m_plans[row] = *planOpt;
    const QModelIndex idx = index(row);
    emit dataChanged(
        idx, idx, {StatusRole, UpdatedAtMsRole, TotalStepsRole, DoneStepsRole, StepsRole});
    recomputeDerivedState();
}

void PlansModel::onPlanDeleted(const QString& planId) {
    const int row = findPlanRow(planId);
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_plans.removeAt(row);
    endRemoveRows();
    emit countChanged();
    recomputeDerivedState();
}

void PlansModel::onStepUpdated(const QString& stepId) {
    // stepUpdated doesn't carry a planId — look it up.
    const auto stepOpt = m_svc.getStep(stepId);
    if (!stepOpt.has_value())
        return;
    const int row = findPlanRow(stepOpt->planId);
    if (row < 0)
        return;

    // A step change doesn't alter the plan row itself, but the step
    // list and doneSteps counter are nested — emit dataChanged for
    // the relevant roles so the QML delegate re-reads.
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {StepsRole, TotalStepsRole, DoneStepsRole});
    recomputeDerivedState();
}

void PlansModel::onArtifactAdded(const QString& /*artifactId*/) {
    // Artifact rows hang off a specific step which hangs off a specific
    // plan. PlanService's artifactAdded(artifactId) signal doesn't
    // carry the plan id, so we'd need a lookup to narrow the dataChanged
    // region. For the current UI (PlansOverlay shows status/steps but
    // not artifacts directly), no plan row fields are affected by an
    // artifact insert — the step list already reflects the latest step
    // state via stepUpdated. This slot is intentionally a no-op and
    // serves as a wiring point for a future ArtifactsModel that will
    // listen to the same signal directly. No broadcast dataChanged.
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

int PlansModel::findPlanRow(const QString& planId) const {
    for (int i = 0; i < m_plans.size(); ++i) {
        if (m_plans.at(i).id == planId)
            return i;
    }
    return -1;
}

bool PlansModel::planBelongsToActiveConversation(const AgentPlan& plan) const {
    return plan.conversationId == m_activeConvId;
}

void PlansModel::reloadActiveConversation() {
    beginResetModel();
    m_plans.clear();

    if (!m_activeConvId.isEmpty()) {
        // Active plans first (DB order is updated_at DESC).
        m_plans = m_svc.activePlansForConversation(m_activeConvId);

        // Append recently-completed plans in this conversation.
        const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - 7LL * 24 * 3600 * 1000;
        const QList<AgentPlan> all = m_svc.plansForConversation(m_activeConvId);
        for (const AgentPlan& p : all) {
            if ((p.status == PlanStatus::Completed || p.status == PlanStatus::Failed) &&
                p.updatedAt.toMSecsSinceEpoch() >= cutoff) {
                m_plans.append(p);
            }
        }
    }

    endResetModel();
    emit countChanged();
    recomputeDerivedState();
}

QVariantList PlansModel::buildStepsForPlan(const AgentPlan& plan) const {
    QVariantList out;
    const QList<PlanStep> steps = m_svc.stepsForPlan(plan.id);
    for (const PlanStep& s : steps) {
        QVariantMap sm;
        sm[QStringLiteral("id")] = s.id;
        sm[QStringLiteral("title")] = s.title;
        sm[QStringLiteral("description")] = s.description;
        sm[QStringLiteral("ownerAlias")] = s.ownerAlias;
        sm[QStringLiteral("status")] = stepStatusToString(s.status);
        sm[QStringLiteral("acceptance")] = s.acceptanceCriteria;
        sm[QStringLiteral("rejectionCount")] = s.rejectionCount;
        sm[QStringLiteral("toolRetryCount")] = s.toolRetryCount;
        sm[QStringLiteral("executorTurnsUsed")] = s.executorTurnsUsed;
        sm[QStringLiteral("lastRejectionReason")] = s.lastRejectionReason;
        const bool canIntervene =
            (plan.status == PlanStatus::Executing || plan.status == PlanStatus::Critiquing ||
             plan.status == PlanStatus::Blocked) &&
            (s.status != StepStatus::Done);
        sm[QStringLiteral("canIntervene")] = canIntervene;
        out.append(sm);
    }
    return out;
}

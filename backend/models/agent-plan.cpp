// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent-plan.cpp
 * @brief Enum/string conversion and JSON serialization for
 *        agent-plan data models. Stored-form strings are stable and used
 *        directly in SQL. Do not rename them without a schema migration.
 * @layer Data Access
 * @dependencies Qt6::Core
 */

#include "agent-plan.h"

#include <algorithm>
#include <QJsonDocument>

// ---------------------------------------------------------------------------
// PlanStatus
// ---------------------------------------------------------------------------

QString planStatusToString(PlanStatus s) {
    switch (s) {
        case PlanStatus::Planning:
            return QStringLiteral("planning");
        case PlanStatus::Executing:
            return QStringLiteral("executing");
        case PlanStatus::Critiquing:
            return QStringLiteral("critiquing");
        case PlanStatus::Blocked:
            return QStringLiteral("blocked");
        case PlanStatus::Completed:
            return QStringLiteral("completed");
        case PlanStatus::Failed:
            return QStringLiteral("failed");
    }
    return QStringLiteral("planning");
}

PlanStatus planStatusFromString(const QString& s) {
    if (s == QStringLiteral("planning"))
        return PlanStatus::Planning;
    if (s == QStringLiteral("executing"))
        return PlanStatus::Executing;
    if (s == QStringLiteral("critiquing"))
        return PlanStatus::Critiquing;
    if (s == QStringLiteral("blocked"))
        return PlanStatus::Blocked;
    if (s == QStringLiteral("completed"))
        return PlanStatus::Completed;
    if (s == QStringLiteral("failed"))
        return PlanStatus::Failed;
    return PlanStatus::Planning;
}

// ---------------------------------------------------------------------------
// StepStatus
// ---------------------------------------------------------------------------

QString stepStatusToString(StepStatus s) {
    switch (s) {
        case StepStatus::Pending:
            return QStringLiteral("pending");
        case StepStatus::InProgress:
            return QStringLiteral("in_progress");
        case StepStatus::Submitted:
            return QStringLiteral("submitted");
        case StepStatus::Done:
            return QStringLiteral("done");
        case StepStatus::NeedsRework:
            return QStringLiteral("needs_rework");
        case StepStatus::Blocked:
            return QStringLiteral("blocked");
    }
    return QStringLiteral("pending");
}

StepStatus stepStatusFromString(const QString& s) {
    if (s == QStringLiteral("pending"))
        return StepStatus::Pending;
    if (s == QStringLiteral("in_progress"))
        return StepStatus::InProgress;
    if (s == QStringLiteral("submitted"))
        return StepStatus::Submitted;
    if (s == QStringLiteral("done"))
        return StepStatus::Done;
    if (s == QStringLiteral("needs_rework"))
        return StepStatus::NeedsRework;
    if (s == QStringLiteral("blocked"))
        return StepStatus::Blocked;
    return StepStatus::Pending;
}

// ---------------------------------------------------------------------------
// ArtifactType
// ---------------------------------------------------------------------------

QString artifactTypeToString(ArtifactType t) {
    switch (t) {
        case ArtifactType::File:
            return QStringLiteral("file");
        case ArtifactType::Analysis:
            return QStringLiteral("analysis");
        case ArtifactType::Decision:
            return QStringLiteral("decision");
        case ArtifactType::Draft:
            return QStringLiteral("draft");
        case ArtifactType::Code:
            return QStringLiteral("code");
        case ArtifactType::ToolLog:
            return QStringLiteral("tool_log");
    }
    return QStringLiteral("draft");
}

ArtifactType artifactTypeFromString(const QString& s) {
    if (s == QStringLiteral("file"))
        return ArtifactType::File;
    if (s == QStringLiteral("analysis"))
        return ArtifactType::Analysis;
    if (s == QStringLiteral("decision"))
        return ArtifactType::Decision;
    if (s == QStringLiteral("draft"))
        return ArtifactType::Draft;
    if (s == QStringLiteral("code"))
        return ArtifactType::Code;
    if (s == QStringLiteral("tool_log"))
        return ArtifactType::ToolLog;
    return ArtifactType::Draft;
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

QJsonObject AgentPlan::toJson() const {
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("conversationId")] = conversationId;
    o[QStringLiteral("projectFolderId")] = projectFolderId;
    o[QStringLiteral("organizationFolderId")] = organizationFolderId;
    o[QStringLiteral("goal")] = goal;
    o[QStringLiteral("status")] = planStatusToString(status);
    o[QStringLiteral("startedBy")] = startedBy;
    o[QStringLiteral("createdAt")] = createdAt.toString(Qt::ISODate);
    o[QStringLiteral("updatedAt")] = updatedAt.toString(Qt::ISODate);
    o[QStringLiteral("turnsUsed")] = turnsUsed;
    o[QStringLiteral("heartbeatsUsed")] = heartbeatsUsed;
    return o;
}

QJsonObject PlanStep::toJson() const {
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("planId")] = planId;
    o[QStringLiteral("ordering")] = ordering;
    o[QStringLiteral("title")] = title;
    o[QStringLiteral("description")] = description;
    o[QStringLiteral("ownerAlias")] = ownerAlias;
    o[QStringLiteral("acceptanceCriteria")] = acceptanceCriteria;
    o[QStringLiteral("status")] = stepStatusToString(status);
    o[QStringLiteral("rejectionCount")] = rejectionCount;
    o[QStringLiteral("toolRetryCount")] = toolRetryCount;
    o[QStringLiteral("executorTurnsUsed")] = executorTurnsUsed;
    o[QStringLiteral("lastRejectionReason")] = lastRejectionReason;
    o[QStringLiteral("createdAt")] = createdAt.toString(Qt::ISODate);
    o[QStringLiteral("updatedAt")] = updatedAt.toString(Qt::ISODate);
    return o;
}

QJsonObject StepArtifact::toJson() const {
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("stepId")] = stepId;
    o[QStringLiteral("type")] = artifactTypeToString(type);
    o[QStringLiteral("filePath")] = filePath;
    o[QStringLiteral("content")] = content;
    o[QStringLiteral("summary")] = summary;
    o[QStringLiteral("submittedByAlias")] = submittedByAlias;
    o[QStringLiteral("createdAt")] = createdAt.toString(Qt::ISODate);
    o[QStringLiteral("approved")] = approved;
    QJsonArray reasons;
    for (const QString& r : criticReasons)
        reasons.append(r);
    o[QStringLiteral("criticReasons")] = reasons;
    return o;
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

QList<AgentPlan> plansExcludingConversation(QList<AgentPlan> plans, const QString& excludeConvId) {
    if (excludeConvId.isEmpty())
        return plans;
    plans.erase(std::remove_if(plans.begin(),
                               plans.end(),
                               [&excludeConvId](const AgentPlan& p) {
                                   return p.conversationId == excludeConvId;
                               }),
                plans.end());
    return plans;
}

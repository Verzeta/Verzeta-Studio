// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file task-observer.cpp
 * @brief TaskObserver implementation: passively writes
 *        `step_artifacts` rows for tool calls, files, and text
 *        replies that happen during an active-task turn.
 * @layer Service
 * @dependencies PlanService, agent-plan models, logger.
 */

#include "task-observer.h"

#include "../models/agent-plan.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "plan-service.h"

#include <QDateTime>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TaskObserver::TaskObserver(PlanService& plans, QObject* parent) : QObject(parent), m_plans(plans) {}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString TaskObserver::resolveActiveStepId(const QString& planId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (planId.isEmpty())
        return {};
    const QList<PlanStep> steps = m_plans.stepsForPlan(planId);
    if (steps.isEmpty())
        return {};
    return steps.first().id;
}

/**
 * @brief Shortens a string to at most @p cap characters.
 * @param s   Input text.
 * @param cap Maximum length, including the "..." suffix.
 * @returns @p s unchanged when it fits; otherwise its first cap - 3
 *          characters followed by "...".
 */
static QString truncateTo(const QString& s, int cap) {
    if (s.length() <= cap)
        return s;
    return s.left(cap - 3) + QStringLiteral("...");
}

// ---------------------------------------------------------------------------
// recordToolCall
// ---------------------------------------------------------------------------

QString TaskObserver::recordToolCall(const QString& planId,
                                     const QString& toolName,
                                     const QString& argsPreview,
                                     const QString& resultSummary,
                                     const QString& filePath,
                                     const QString& invokedByAlias) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString stepId = resolveActiveStepId(planId);
    if (stepId.isEmpty()) {
        qCWarning(verzetaUi) << "TaskObserver::recordToolCall: no step for plan" << planId;
        return {};
    }

    StepArtifact art;
    art.stepId = stepId;
    art.type = ArtifactType::ToolLog;
    art.filePath = filePath;  // may be empty
    art.content = truncateTo(argsPreview, kMaxArgsPreview);
    art.summary =
        QStringLiteral("%1: %2").arg(toolName, truncateTo(resultSummary, kMaxResultPreview));
    // step_artifacts.submitted_by_alias is NOT NULL in the DB schema.
    // For ToolLog rows there's no single responsible author — the model
    // picked the tool, not a human — so we fall back to "system" when
    // the caller doesn't pass an invoking alias. The previous version
    // left this empty, which hit the NOT NULL constraint and caused
    // every ToolLog write to silently fail (seen in debugging logs).
    art.submittedByAlias = invokedByAlias.isEmpty() ? QStringLiteral("system") : invokedByAlias;
    art.approved = 1;  // tool logs are informational, not pending review

    const QString id = m_plans.addArtifact(art);
    if (id.isEmpty()) {
        qCWarning(verzetaUi) << "TaskObserver::recordToolCall: addArtifact failed for" << toolName;
    } else {
        qCInfo(verzetaUi) << "TaskObserver: recorded tool call" << toolName << "on plan"
                          << planId.left(8) << "by" << art.submittedByAlias;
    }
    return id;
}

// ---------------------------------------------------------------------------
// recordFile
// ---------------------------------------------------------------------------

QString TaskObserver::recordFile(const QString& planId,
                                 const QString& filePath,
                                 const QString& producedByAlias,
                                 const QString& toolName) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString stepId = resolveActiveStepId(planId);
    if (stepId.isEmpty() || filePath.isEmpty())
        return {};

    StepArtifact art;
    art.stepId = stepId;
    art.type = ArtifactType::File;
    art.filePath = filePath;
    art.content.clear();  // file type has no inline content
    art.summary =
        QStringLiteral("wrote %1 via %2").arg(filePath.section(QLatin1Char('/'), -1), toolName);
    // NOT NULL constraint fallback — identical reasoning to recordToolCall.
    art.submittedByAlias = producedByAlias.isEmpty() ? QStringLiteral("system") : producedByAlias;
    art.approved = 1;  // observer-recorded file is not critic-gated

    const QString id = m_plans.addArtifact(art);
    if (!id.isEmpty()) {
        qCInfo(verzetaUi) << "TaskObserver: recorded file artifact" << filePath << "on plan"
                          << planId.left(8);
    }
    return id;
}

// ---------------------------------------------------------------------------
// recordTextArtifact
// ---------------------------------------------------------------------------

QString TaskObserver::recordTextArtifact(const QString& planId,
                                         const QString& submittedByAlias,
                                         const QString& content,
                                         const QString& summary) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString stepId = resolveActiveStepId(planId);
    if (stepId.isEmpty() || content.trimmed().isEmpty())
        return {};

    StepArtifact art;
    art.stepId = stepId;
    art.type = ArtifactType::Draft;
    art.filePath.clear();
    art.content = truncateTo(content.trimmed(), kMaxDraftLength);
    art.summary = summary.isEmpty()
                      ? QStringLiteral("text reply (%1 chars)").arg(content.trimmed().length())
                      : summary;
    // NOT NULL constraint fallback — identical reasoning to recordToolCall.
    art.submittedByAlias = submittedByAlias.isEmpty() ? QStringLiteral("system") : submittedByAlias;
    art.approved = 1;  // observer-recorded draft is not critic-gated

    const QString id = m_plans.addArtifact(art);
    if (!id.isEmpty()) {
        qCInfo(verzetaUi) << "TaskObserver: recorded text artifact" << content.trimmed().length()
                          << "chars on plan" << planId.left(8);
    }
    return id;
}

// ---------------------------------------------------------------------------
// recentActivity
// ---------------------------------------------------------------------------

QStringList TaskObserver::recentActivity(const QString& planId, int max) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QStringList out;
    if (planId.isEmpty() || max <= 0)
        return out;

    const QString stepId = resolveActiveStepId(planId);
    if (stepId.isEmpty())
        return out;

    // artifactsForStep returns newest first already.
    const QList<StepArtifact> arts = m_plans.artifactsForStep(stepId);
    for (const StepArtifact& a : arts) {
        if (out.size() >= max)
            break;

        switch (a.type) {
            case ArtifactType::ToolLog:
                out.append(QStringLiteral("tool: %1").arg(a.summary));
                break;
            case ArtifactType::File:
                out.append(
                    QStringLiteral("file: %1").arg(a.filePath.section(QLatin1Char('/'), -1)));
                break;
            case ArtifactType::Draft:
                out.append(QStringLiteral("draft: %1").arg(truncateTo(a.summary, 80)));
                break;
            case ArtifactType::Analysis:
                out.append(QStringLiteral("analysis: %1").arg(truncateTo(a.summary, 80)));
                break;
            case ArtifactType::Decision:
                out.append(QStringLiteral("decision: %1").arg(truncateTo(a.summary, 80)));
                break;
            case ArtifactType::Code:
                out.append(QStringLiteral("code: %1").arg(truncateTo(a.summary, 80)));
                break;
        }
    }
    return out;
}

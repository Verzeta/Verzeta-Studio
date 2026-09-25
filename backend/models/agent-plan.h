// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent-plan.h
 * @brief Data models for the state-driven execution loop: plans,
 *        steps, and artifacts. A plan is a tracked unit of work created by
 *        an agent calling start_task; it has a home conversation (where
 *        events post), a list of steps (each owned by an alias), and a set
 *        of artifacts submitted against those steps.
 * @layer Data Access
 * @dependencies Qt6::Core
 */


#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/**
 * @brief Lifecycle status of an AgentPlan.
 *
 * - Planning:   plan row exists, first owner dispatch queued or awaiting
 *               create_plan (if started via fallback planner path).
 * - Executing:  at least one step has been dispatched and is running.
 * - Critiquing: a submit_result has landed; CriticService is reviewing.
 * - Blocked:    one or more steps hit a cap; waiting for user unblock.
 * - Completed:  every step status == Done.
 * - Failed:     hard budget exceeded or user stopped the plan.
 */
enum class PlanStatus {
    Planning,
    Executing,
    Critiquing,
    Blocked,
    Completed,
    Failed,
};

/**
 * @brief Lifecycle status of a single plan step.
 *
 * - Pending:     not yet dispatched.
 * - InProgress:  executor turn(s) underway.
 * - Submitted:   submit_result called; awaiting Critic.
 * - Done:        Critic approved.
 * - NeedsRework: Critic rejected; re-dispatch pending.
 * - Blocked:     hit a per-step cap; waiting for user unblock.
 */
enum class StepStatus {
    Pending,
    InProgress,
    Submitted,
    Done,
    NeedsRework,
    Blocked,
};

/**
 * @brief Kind of artifact that a step produced.
 *
 * - File:     written to the plan's artifact root directory (file_path set).
 * - Analysis: inline text: research findings, data interpretation.
 * - Decision: inline text: a concrete choice with rationale.
 * - Draft:    inline text: copy, plan, outline.
 * - Code:     inline text: source snippet or patch.
 */
enum class ArtifactType {
    File,
    Analysis,
    Decision,
    Draft,
    Code,
    /// A row written by TaskObserver for every tool call made during an
    /// active-task turn. `summary` holds the tool name, `content` holds a
    /// truncated result preview, and `filePath` is set when the tool
    /// produced a file. Observer-only: submit_result cannot create one.
    ToolLog,
};

// ---------------------------------------------------------------------------
// Enum ↔ string conversion (for SQL storage and JSON serialization)
// ---------------------------------------------------------------------------

/**
 * @brief Converts a plan status to its stored lowercase name.
 * @param s Status to convert.
 * @returns "planning", "executing", "critiquing", "blocked", "completed"
 *          or "failed".
 */
QString planStatusToString(PlanStatus s);
/**
 * @brief Parses a stored plan status name.
 * @param s Name as written by planStatusToString().
 * @returns The matching status, or PlanStatus::Planning for an unknown name.
 */
PlanStatus planStatusFromString(const QString& s);

/**
 * @brief Converts a step status to its stored name.
 * @param s Status to convert.
 * @returns "pending", "in_progress", "submitted", "done", "needs_rework"
 *          or "blocked".
 */
QString stepStatusToString(StepStatus s);
/**
 * @brief Parses a stored step status name.
 * @param s Name as written by stepStatusToString().
 * @returns The matching status, or StepStatus::Pending for an unknown name.
 */
StepStatus stepStatusFromString(const QString& s);

/**
 * @brief Converts an artifact type to its stored name.
 * @param t Type to convert.
 * @returns "file", "analysis", "decision", "draft", "code" or "tool_log".
 */
QString artifactTypeToString(ArtifactType t);
/**
 * @brief Parses a stored artifact type name.
 * @param s Name as written by artifactTypeToString().
 * @returns The matching type, or ArtifactType::Draft for an unknown name.
 */
ArtifactType artifactTypeFromString(const QString& s);

// ---------------------------------------------------------------------------
// AgentPlan
// ---------------------------------------------------------------------------

/**
 * @brief A tracked unit of work, bound to a home conversation.
 *
 * Created by `PlanService::createPlan(...)`, which is called from the
 * `start_task` tool dispatcher.
 *
 * Scope (for cross-chat awareness): the agent who owns a plan's steps
 * can see the plan from any chat that shares the home conversation's
 * project folder, organization folder, or (for loose chats) just the
 * home conversation itself. The scope fields are nullable. If both are
 * empty, the plan is scoped to the home conversation alone.
 */
struct AgentPlan {
    QString id;                                ///< UUID
    QString conversationId;                    ///< Home conversation
    QString projectFolderId;                   ///< Nullable; project scope
    QString organizationFolderId;              ///< Nullable; org scope
    QString goal;                              ///< 10–500 chars
    PlanStatus status = PlanStatus::Planning;  ///< Lifecycle status.
    QString startedBy;                         ///< "user" or initiating alias
    QDateTime createdAt;                       ///< When the plan was created.
    QDateTime updatedAt;                       ///< Last change to the plan row.
    /// Plan-wide turn counter. Stored and reported, but not incremented;
    /// per-step turns are counted in PlanStep::executorTurnsUsed.
    int turnsUsed = 0;
    /// Plan-wide heartbeat counter. Stored and reported, but not
    /// incremented.
    int heartbeatsUsed = 0;

    /**
     * @brief Reports whether the row is well-formed enough to persist.
     * @returns True iff `id`, `conversationId`, and `goal` are all set.
     */
    bool isValid() const { return !id.isEmpty() && !conversationId.isEmpty() && !goal.isEmpty(); }

    /**
     * @brief Serialises this plan to its JSON wire / UI form.
     * @returns JSON object mirroring the column-name shape.
     */
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// PlanStep
// ---------------------------------------------------------------------------

/**
 * @brief A single step inside an AgentPlan.
 *
 * Owned by an alias that must exist in the plan's home conversation
 * roster (or be "user" for steps requiring human input). Steps execute
 * one at a time, in ascending `ordering`.
 */
struct PlanStep {
    QString id;                               ///< UUID
    QString planId;                           ///< Parent plan
    int ordering = 0;                         ///< 0-based sequence
    QString title;                            ///< Short step name.
    QString description;                      ///< Longer explanation of the step.
    QString ownerAlias;                       ///< Must exist in home conv roster
    QString acceptanceCriteria;               ///< Non-empty; Critic's gate
    StepStatus status = StepStatus::Pending;  ///< Lifecycle status.
    int rejectionCount = 0;                   ///< Critic rejections; the cap is not enforced
    int toolRetryCount = 0;                   ///< Tool-only violations; the cap is not enforced
    int executorTurnsUsed = 0;    ///< Intermediate executor turns; the cap is not enforced
    QString lastRejectionReason;  ///< Last Critic verdict text or user note
    QDateTime createdAt;          ///< When the step was created.
    QDateTime updatedAt;          ///< Last change to the step row.

    /**
     * @brief Reports whether the row is well-formed enough to persist.
     * @returns True iff all required identity + content fields are set.
     */
    bool isValid() const {
        return !id.isEmpty() && !planId.isEmpty() && !title.isEmpty() && !ownerAlias.isEmpty() &&
               !acceptanceCriteria.isEmpty();
    }

    /**
     * @brief Serialises this step to its JSON wire / UI form.
     * @returns JSON object mirroring the column-name shape.
     */
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// StepArtifact
// ---------------------------------------------------------------------------

/**
 * @brief An artifact submitted against a plan step.
 *
 * Created by `submit_result(step_id, type, content, summary)`. For
 * `File` artifacts, the content has been written to disk at `filePath`
 * and `content` is empty. For other types, `content` holds inline text.
 *
 * `approved`: 0 = pending Critic review, 1 = approved, -1 = rejected.
 * Rejected artifacts are kept for audit; the next rework creates a new
 * artifact row rather than overwriting this one.
 */
struct StepArtifact {
    QString id;                               ///< UUID
    QString stepId;                           ///< Parent step
    ArtifactType type = ArtifactType::Draft;  ///< Kind of artifact.
    QString filePath;                         ///< Set only for type == File
    QString content;                          ///< Set for non-file types
    QString summary;                          ///< Short description for UI
    QString submittedByAlias;                 ///< Alias that submitted it.
    QDateTime createdAt;                      ///< When it was submitted.
    int approved = 0;                         ///< 0 pending, 1 approved, -1 rejected
    QStringList criticReasons;                ///< Rejection reasons, if any

    /**
     * @brief Reports whether the row is well-formed enough to persist.
     * @returns True iff `id`, `stepId`, and `summary` are all set.
     */
    bool isValid() const { return !id.isEmpty() && !stepId.isEmpty() && !summary.isEmpty(); }

    /**
     * @brief Serialises this artifact to its JSON wire / UI form.
     * @returns JSON object mirroring the column-name shape.
     */
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

/**
 * @brief Remove every plan whose home conversation id equals `excludeConvId`.
 *        Returns a new list preserving order.
 *
 * Used by ChatController when building the "cross-chat awareness" system-
 * prompt block. Plans whose home IS the current conversation are already
 * represented in that conversation's own message history. Listing them
 * again in the system prompt as "recently completed" reframes live
 * dialogue as stale background work and breaks group-cascade context.
 * Factored out so it's cleanly unit-testable
 * without a ChatController.
 *
 * @param plans         Source list (copied; the caller's list is unchanged).
 * @param excludeConvId Conversation id to filter out. If empty, no-op.
 * @return Filtered copy, order preserved.
 */
QList<AgentPlan> plansExcludingConversation(QList<AgentPlan> plans, const QString& excludeConvId);

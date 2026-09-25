// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file task-observer.h
 * @brief Passive recorder of activity that happens during an
 *        assistant turn while an active task is anchored to the
 *        conversation. Writes step_artifacts rows for tool calls,
 *        files, and substantive text replies. Does NOT validate,
 *        dispatch, retry, or gate anything.
 * @layer Service
 * @dependencies PlanService (append-only writes), agent-plan models
 *
 * ## Role
 *
 * The TaskObserver sits between ChatController (the turn-running
 * layer) and PlanService (the persistence layer). It has no control
 * over task execution. Its sole job is to write `step_artifacts` rows
 * describing what the LLM did during an active-task turn, so the
 * PlansOverlay and the system prompt's "past work" section can show
 * activity history.
 *
 * The observer is intentionally dumb:
 *   - It accepts a plan_id from outside (ChatController's m_activeTaskPlanId).
 *   - It inserts rows; it does not read or mutate plan/step status.
 *   - It is synchronous and cheap; safe to call from the main thread
 *     per-tool-call without batching.
 *
 * ## Types of recorded activity
 *
 *   recordToolCall(planId, toolName, argsPreview, resultPreview, filePath)
 *       → ArtifactType::ToolLog row. summary = toolName, content =
 *       argsPreview, filePath = toolFilePath (optional).
 *
 *   recordFile(planId, filePath, toolName)
 *       → ArtifactType::File row. Attribution of file-producing tools.
 *       Called in addition to recordToolCall when a tool writes a file.
 *
 *   recordTextArtifact(planId, submittedBy, content, summary)
 *       → ArtifactType::Draft row. The substantive text reply the model
 *       emitted during a task turn. Capped at kMaxDraftLength chars.
 *
 * ## Past-work context helper
 *
 *   recentActivity(planId, max) → QStringList of one-line summaries
 *       used by the ChatController prompt builder to surface "past
 *       work" context in the ACTIVE TASK framing block.
 */

#ifndef VERZETA_TASK_OBSERVER_H
#define VERZETA_TASK_OBSERVER_H

#include <QObject>
#include <QString>
#include <QStringList>

class PlanService;

/**
 * @brief Passive recorder of per-turn task activity. Writes
 *        `step_artifacts` rows for tool calls / files / drafts via
 *        PlanService; never reads or mutates plan or step status.
 */
class TaskObserver : public QObject {
    Q_OBJECT

  public:
    /**
     * @param plans  PlanService instance (append-only writes via addArtifact).
     * @param parent Qt parent.
     */
    explicit TaskObserver(PlanService& plans, QObject* parent = nullptr);
    ~TaskObserver() override = default;

    // Disable copy; observer owns nothing but the service reference.
    TaskObserver(const TaskObserver&) = delete;
    TaskObserver& operator=(const TaskObserver&) = delete;

    /**
     * @brief Records one tool call made during an active-task turn.
     * @param planId          Active plan id (from ChatController::m_activeTaskPlanId).
     * @param toolName        Tool id (e.g. "search_web").
     * @param argsPreview     Truncated JSON preview of the tool arguments.
     * @param resultSummary   One-line summary of the tool result
     *                        ("ok: 3 files" / "error: not found" / etc.).
     * @param filePath        Optional: set when the tool produced a file.
     * @param invokedByAlias  Alias of the agent that invoked the tool
     *                        (responder alias in group chats, or the
     *                        1:1 primary agent's name). May be empty;
     *                        the observer substitutes "system" when no
     *                        alias is available so the NOT NULL DB
     *                        constraint on submitted_by_alias is always
     *                        satisfied.
     * @return Artifact UUID on success, empty on failure.
     */
    QString recordToolCall(const QString& planId,
                           const QString& toolName,
                           const QString& argsPreview,
                           const QString& resultSummary,
                           const QString& filePath = {},
                           const QString& invokedByAlias = {});

    /**
     * @brief Records a file produced during an active-task turn as a
     *        separate File artifact. Use this in addition to
     *        recordToolCall when a tool writes a file. The ToolLog
     *        captures the call, the File row captures the output.
     * @param planId          Active plan id.
     * @param filePath        Absolute path of the produced file.
     * @param producedByAlias Alias of the agent that produced the
     *                        file; may be empty.
     * @param toolName        Tool id that produced the file.
     * @returns Artifact UUID on success, empty on failure.
     */
    QString recordFile(const QString& planId,
                       const QString& filePath,
                       const QString& producedByAlias,
                       const QString& toolName);

    /**
     * @brief Records the substantive text content of a task-turn
     *        reply as a Draft artifact. Capped at kMaxDraftLength
     *        chars.
     * @param planId           Active plan id.
     * @param submittedByAlias Alias of the responder agent.
     * @param content          Reply text to persist (capped).
     * @param summary          One-line summary of the draft.
     * @returns Artifact UUID on success, empty on failure.
     */
    QString recordTextArtifact(const QString& planId,
                               const QString& submittedByAlias,
                               const QString& content,
                               const QString& summary);

    /**
     * @brief Returns up to `max` recent activity lines for a plan,
     *        in reverse-chronological order, suitable for inclusion
     *        in the ACTIVE TASK framing block. Each line is a short
     *        human description.
     * @param planId Plan id whose activity to summarise.
     * @param max    Maximum number of lines to return.
     * @returns Activity summaries; empty when the plan has no
     *          recorded artifacts.
     */
    QStringList recentActivity(const QString& planId, int max = 8) const;

  private:
    /**
     * @brief Resolves the single step_id for an active plan. Such
     *        plans have exactly one step (the work unit); this returns
     *        its id or empty if the plan has no steps.
     */
    QString resolveActiveStepId(const QString& planId) const;

    static constexpr int kMaxDraftLength = 4000;   ///< cap on stored text
    static constexpr int kMaxArgsPreview = 500;    ///< cap on tool args preview
    static constexpr int kMaxResultPreview = 500;  ///< cap on tool result preview

    PlanService& m_plans;
};

#endif  // VERZETA_TASK_OBSERVER_H

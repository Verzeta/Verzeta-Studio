// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file plan-service.h
 * @brief Data-layer CRUD for the state-driven task execution loop.
 *
 *        Owns the `agent_plans`, `plan_steps`, and `step_artifacts`
 *        tables. Emits Qt signals when rows change so the plans
 *        overlay and TaskRunner stay in sync without polling.
 *
 *        Pure persistence: no LLM calls, no dispatch logic, no
 *        state-machine transitions. Business logic (which state to
 *        move to, when to invoke the Critic, how to build executor
 *        prompts) lives in TaskRunner. PlanService only knows how
 *        to read and write rows atomically and to notify listeners
 *        of changes.
 * @layer Service (Data Access)
 * @dependencies DbManager, models/agent-plan.h.
 */


#pragma once

#include "../models/agent-plan.h"

#include <optional>
#include <QList>
#include <QObject>
#include <QString>

class DbManager;

/**
 * @brief CRUD and change-notification service for agent plans.
 *
 * Lifecycle: instantiated by AppController at startup, lives as long as
 * the main window. The database connection is shared via DbManager.
 *
 * Threading: main thread only. All queries are synchronous.
 */
class PlanService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the service bound to a DbManager.
     * @param db      Shared database connection. Must outlive this object.
     * @param parent  Optional Qt parent.
     */
    explicit PlanService(DbManager& db, QObject* parent = nullptr);
    ~PlanService() override;

    // -----------------------------------------------------------------------
    // Plans
    // -----------------------------------------------------------------------

    /**
     * @brief Creates a plan with its initial steps atomically.
     *
     * Writes the parent `agent_plans` row plus every child `plan_steps`
     * row inside one SQL transaction. Each step gets a pre-generated
     * UUID and its `planId` / `ordering` fields are set automatically
     * (ordering = position in the input list).
     *
     * @param plan       Plan to create. `id` must be empty; a UUID is
     *                   generated. `createdAt`/`updatedAt` are stamped.
     * @param steps      Ordered list of steps. Each step's `planId` is
     *                   overwritten with the new plan id.
     * @return Plan UUID on success, empty string on failure.
     * @sideeffects Inserts rows; emits planCreated() on success.
     */
    QString createPlan(AgentPlan plan, QList<PlanStep> steps);

    /**
     * @brief Returns a plan by id.
     * @param planId  Plan UUID.
     * @returns Populated plan, or nullopt when no row matches.
     */
    std::optional<AgentPlan> getPlan(const QString& planId) const;

    /**
     * @brief Returns all plans whose home conversation is the given id.
     * @param conversationId  Conversation UUID.
     * @returns Plans ordered by `updated_at` DESC.
     */
    QList<AgentPlan> plansForConversation(const QString& conversationId) const;

    /**
     * @brief Returns active plans (status in {planning, executing,
     *        critiquing, blocked}) for a conversation.
     * @param conversationId  Conversation UUID.
     * @returns Plans ordered by `updated_at` DESC.
     */
    QList<AgentPlan> activePlansForConversation(const QString& conversationId) const;

    /**
     * @brief Returns active plans that have any step owned by
     *        `ownerAlias` and whose scope matches the given project /
     *        organization folder.
     * @param ownerAlias            Alias to match against
     *                              `plan_steps.owner_alias`.
     * @param projectFolderId       Nullable; when non-empty, matches
     *                              plans with this project_folder_id or
     *                              whose home conv is inside this
     *                              project.
     * @param organizationFolderId  Nullable; same semantics for org folder.
     * @returns Matching plans ordered by `updated_at` DESC.
     *
     * Used by the conversational prompt builder for cross-chat awareness.
     */
    QList<AgentPlan> activePlansForOwnerInScope(const QString& ownerAlias,
                                                const QString& projectFolderId,
                                                const QString& organizationFolderId) const;

    /**
     * @brief Returns recently completed plans owned by `ownerAlias`
     *        within the given scope.
     * @param ownerAlias            Owner alias to match.
     * @param projectFolderId       Nullable scope filter.
     * @param organizationFolderId  Nullable scope filter.
     * @param limitDays             Only plans updated in the last N
     *                              days are returned. Defaults to 7.
     * @returns Matching completed plans ordered by `updated_at` DESC.
     *
     * Used by the conversational prompt builder so an agent asked
     * about a task they recently finished can still recall it and
     * point to the artifact.
     */
    QList<AgentPlan> recentlyCompletedPlansForOwnerInScope(const QString& ownerAlias,
                                                           const QString& projectFolderId,
                                                           const QString& organizationFolderId,
                                                           int limitDays = 7) const;

    /**
     * @brief Returns all plans where any step is owned by an alias that
     *        maps to the given agent template id, ACROSS ALL CHATS.
     *        Uses the conversation_members / primary_agent_id tables to
     *        resolve aliases through agent identity, so Writer_Rob in
     *        chat A and Writer_Rob in chat B (same agent template) are
     *        treated as the same agent regardless of folder placement.
     *
     * @param agentId     Target agent template id (agents.id).
     * @param statuses    SQL IN-clause values, e.g. {"planning","executing",
     *                    "critiquing","blocked"} for active, or
     *                    {"completed"} for completed.
     * @param limitDays   If > 0, filter to plans updated in the last N days.
     * @return Plans ordered by updated_at DESC.
     */
    QList<AgentPlan> plansByAgentIdAcrossChats(const QString& agentId,
                                               const QStringList& statuses,
                                               int limitDays = 0) const;

    /**
     * @brief Sets plan status + updatedAt and emits planStatusChanged.
     * @param planId  Plan UUID.
     * @param status  New status.
     * @returns True on a successful row update.
     */
    bool updatePlanStatus(const QString& planId, PlanStatus status);

    /**
     * @brief Deletes a plan and cascades to its steps and artifacts.
     * @param planId  Plan UUID.
     * @returns True on a successful row delete.
     *
     * Used when a conversation is deleted or a plan is force-dropped.
     */
    bool deletePlan(const QString& planId);

    /**
     * @brief Deletes every plan whose home conversation is the given
     *        id. Used by the onConversationDeleted hook as a belt-and-
     *        suspenders cleanup alongside the FK ON DELETE CASCADE,
     *        so any in-memory state tied to those plans also gets
     *        proper deletion signals.
     * @param conversationId Home conversation of the plans to delete.
     * @returns Number of plans deleted; 0 when there were none or a query
     *          failed. planDeleted is emitted once per deleted plan.
     */
    int deletePlansForConversation(const QString& conversationId);

    // -----------------------------------------------------------------------
    // Steps
    // -----------------------------------------------------------------------

    /**
     * @brief Returns a step by id.
     * @param stepId  Step UUID.
     * @returns Populated step, or nullopt when no row matches.
     */
    std::optional<PlanStep> getStep(const QString& stepId) const;

    /**
     * @brief Returns all steps for a plan.
     * @param planId  Plan UUID.
     * @returns Steps ordered by `ordering` ASC.
     */
    QList<PlanStep> stepsForPlan(const QString& planId) const;

    /**
     * @brief Appends a new step to an existing plan.
     * @param planId  Parent plan UUID.
     * @param step    Step payload; `planId` and `ordering` are
     *                overwritten by this call.
     * @returns Step UUID on success, empty string on failure.
     */
    QString appendStepToPlan(const QString& planId, PlanStep step);

    /**
     * @brief Sets step status + updatedAt and emits stepStatusChanged.
     * @param stepId  Step UUID.
     * @param status  New status.
     * @param reason  Optional reason text persisted with the transition.
     * @returns True on a successful row update.
     */
    bool updateStepStatus(const QString& stepId, StepStatus status, const QString& reason = {});

    /**
     * @brief Reassigns a step to a new owner alias (delegate_task).
     * @param stepId         Step UUID.
     * @param newOwnerAlias  Alias to take over the step.
     * @returns True on a successful row update.
     */
    bool updateStepOwner(const QString& stepId, const QString& newOwnerAlias);

    /**
     * @brief Increments the per-step critic-rejection counter.
     * @param stepId  Step UUID.
     * @returns True on a successful row update.
     */
    bool incrementStepRejectionCount(const QString& stepId);

    /**
     * @brief Increments the per-step tool-only-violation counter.
     * @param stepId  Step UUID.
     * @returns True on a successful row update.
     */
    bool incrementStepToolRetryCount(const QString& stepId);

    /**
     * @brief Increments the per-step executor-turn counter.
     * @param stepId  Step UUID.
     * @returns True on a successful row update.
     */
    bool incrementStepExecutorTurns(const QString& stepId);

    /**
     * @brief Resets all step counters to zero and status to Pending.
     * @param stepId    Step UUID.
     * @param userNote  Note text persisted alongside the reset.
     * @returns True on a successful row update.
     *
     * Used when a user clicks "Retry" on a blocked step.
     */
    bool resetStepForRetry(const QString& stepId, const QString& userNote);

    /**
     * @brief Returns the active step for an owner in a conversation.
     * @param conversationId  Conversation UUID.
     * @param ownerAlias      Owner alias.
     * @returns Step whose status is Pending / InProgress / NeedsRework,
     *          or nullopt when no such step exists.
     *
     * Used by ChatController to decide conversational vs task turn.
     * The schema guarantees at most one active step per (conversation,
     * alias) pair (plan creation enforces this), so the first match
     * is the right answer.
     */
    std::optional<PlanStep> activeStepForOwnerInConversation(const QString& conversationId,
                                                             const QString& ownerAlias) const;

    // -----------------------------------------------------------------------
    // Artifacts
    // -----------------------------------------------------------------------

    /**
     * @brief Inserts an artifact row for a step.
     * @param artifact  Artifact payload; the `approved` field starts
     *                  at 0 (pending critic).
     * @returns Artifact UUID on success, empty string on failure.
     */
    QString addArtifact(StepArtifact artifact);

    /**
     * @brief Returns all artifacts for a step.
     * @param stepId  Step UUID.
     * @returns Artifacts ordered newest first.
     */
    QList<StepArtifact> artifactsForStep(const QString& stepId) const;

    /**
     * @brief Sets the `approved` flag on an artifact.
     * @param artifactId  Artifact UUID.
     * @param approved    1 for approved, -1 for rejected, 0 for pending.
     * @param reasons     Optional rejection-reason list persisted as JSON.
     * @returns True on a successful row update.
     */
    bool
    setArtifactApproval(const QString& artifactId, int approved, const QStringList& reasons = {});

  signals:
    /**
     * @brief Emitted when a plan is created via createPlan().
     * @param planId  UUID of the newly-created plan.
     */
    void planCreated(const QString& planId);

    /**
     * @brief Emitted when a plan's status changes.
     * @param planId     Plan UUID.
     * @param newStatus  Status after the change.
     */
    void planStatusChanged(const QString& planId, PlanStatus newStatus);

    /**
     * @brief Emitted when any plan row changes (status, counters).
     * @param planId  Plan UUID.
     */
    void planUpdated(const QString& planId);

    /**
     * @brief Emitted when a step's status changes.
     * @param stepId     Step UUID.
     * @param newStatus  Status after the change.
     */
    void stepStatusChanged(const QString& stepId, StepStatus newStatus);

    /**
     * @brief Emitted when any step row changes (status or counters).
     * @param stepId  Step UUID.
     */
    void stepUpdated(const QString& stepId);

    /**
     * @brief Emitted when a new artifact row is inserted.
     * @param artifactId  Artifact UUID.
     */
    void artifactAdded(const QString& artifactId);

    /**
     * @brief Emitted when an existing artifact's approval flips.
     * @param artifactId  Artifact UUID.
     * @param approved    New approval value (1 / -1 / 0).
     */
    void artifactApprovalChanged(const QString& artifactId, int approved);

    /**
     * @brief Emitted when a plan (and its cascade) is deleted.
     * @param planId  Plan UUID.
     */
    void planDeleted(const QString& planId);

  private:
    DbManager& m_db;

    /** @brief Fills an AgentPlan from a SELECT row at the current cursor. */
    static AgentPlan plainPlanFromRow(const class QSqlQuery& q);

    /** @brief Fills a PlanStep from a SELECT row at the current cursor. */
    static PlanStep plainStepFromRow(const class QSqlQuery& q);

    /** @brief Fills a StepArtifact from a SELECT row at the current cursor. */
    static StepArtifact plainArtifactFromRow(const class QSqlQuery& q);
};

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file plans-model.h
 * @brief Push-based Qt list model of agent plans for the active
 *        conversation. Subscribes to PlanService signals in its constructor
 *        and translates every plan/step/artifact change into row-level
 *        Qt Model/View signals. Replaces the old
 *        ChatController::activePlansForCurrentChat() QVariantList that the
 *        PlansOverlay used to poll via Timer.
 * @layer Service (Model)
 * @dependencies PlanService, Qt6::Core
 *
 * Design:
 *   - Each row is one plan (flat list). Steps live inside the plan row as
 *     a `steps` role returning a QVariantList of step dictionaries.
 *   - setActiveConversation(id) triggers a full reload, the ONLY reset after
 *     construction. Subsequent plan/step changes are row-level.
 *   - Time-dependent values (elapsedMs, wallMsRemaining) are NOT exposed.
 *     The model serves `createdAtMs` and `updatedAtMs` as raw epoch-ms
 *     numbers and QML formats them as absolute timestamps or computes
 *     snapshots at render time. There is NO ticker and NO polling; the
 *     only refresh trigger is a PlanService signal. If the user wants a
 *     "still active" indicator, the `status` role flips to Completed /
 *     Failed / Blocked on a real state change, which naturally re-renders
 *     the row.
 */

#pragma once

#include "agent-plan.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

class PlanService;

/**
 * @brief QAbstractListModel exposing one row per agent plan in the
 *        active conversation. Push-driven from PlanService signals;
 *        QML never polls.
 */
class PlansModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString activeConversationId READ activeConversationId WRITE setActiveConversation
                   NOTIFY activeConversationChanged)

    /**
     * @brief List of "busy member" hints for the current conversation's
     *        active plans. Each entry is a string of the form
     *        "@alias on \"step title\"" for steps whose status is
     *        InProgress or Submitted and whose parent plan is Executing
     *        or Critiquing. This is computed purely from the data the
     *        model already holds and is recomputed on every
     *        countChanged / dataChanged / reset, with no polling and no QML JS.
     */
    Q_PROPERTY(QStringList busyMembers READ busyMembers NOTIFY busyMembersChanged)

    /**
     * @brief True if at least one plan in the active conversation is
     *        in a non-terminal state (planning/executing/critiquing/blocked).
     *        Used by the chat input bar to show the "Stop Task" button.
     */
    Q_PROPERTY(bool hasActiveTask READ hasActiveTask NOTIFY hasActiveTaskChanged)

  public:
    enum PlanRoles {
        IdRole = Qt::UserRole + 1,
        GoalRole,
        StatusRole,
        StartedByRole,
        CreatedAtMsRole,
        UpdatedAtMsRole,
        TotalStepsRole,
        DoneStepsRole,
        StepsRole
    };
    Q_ENUM(PlanRoles)

    /**
     * @brief Constructs the model bound to a PlanService instance.
     * @param svc     PlanService to subscribe for plan / step events.
     *                Must outlive this model.
     * @param parent  Optional Qt parent.
     */
    explicit PlansModel(PlanService& svc, QObject* parent = nullptr);

    /**
     * @brief Cascade cleanup hook for conversation deletion.
     * @param convId  UUID of the deleted conversation.
     *
     * Plan rows are removed from the DB via ON DELETE CASCADE; the
     * PlanService::planDeleted signal does not fire for cascade deletes,
     * so this model receives a direct listener on
     * ConversationService::conversationDeleted that drops any in-memory
     * rows belonging to the deleted conversation. Wired by AppController
     * alongside the other conv-delete cleanup chain.
     */
    void onSourceConversationDeleted(const QString& convId);

    /**
     * @brief Loads plans for the given conversation.
     * @param convId  Conversation UUID. Empty clears the model.
     *
     * This is the only path that triggers a full model reset.
     */
    void setActiveConversation(const QString& convId);

    /**
     * @brief Returns the currently-bound conversation id.
     * @returns Conversation UUID, or empty when no scope is set.
     */
    QString activeConversationId() const;

    /**
     * @brief Returns the number of plan rows.
     * @returns Row count.
     */
    int count() const;

    /**
     * @brief Returns the derived "busy members" list for the active scope.
     * @returns One string per active step in the form `@alias on "step title"`.
     */
    QStringList busyMembers() const;

    /**
     * @brief Returns whether any plan in the active scope is non-terminal.
     * @returns True when at least one plan is planning / executing /
     *          critiquing / blocked.
     */
    bool hasActiveTask() const;

    // QAbstractListModel

    /**
     * @brief QAbstractListModel row count.
     * @param parent  Ignored (flat list).
     * @returns Row count.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractListModel data accessor.
     * @param index  Row index requested.
     * @param role   PlanRoles enum value.
     * @returns QVariant with the role value, or an invalid QVariant when
     *          index / role is out of range.
     */
    QVariant data(const QModelIndex& index, int role) const override;

    /**
     * @brief QAbstractListModel role-name map for QML.
     * @returns Hash mapping PlanRoles values to QML role names.
     */
    QHash<int, QByteArray> roleNames() const override;

  signals:
    /** @brief Emitted whenever the plan row count changes. */
    void countChanged();

    /** @brief Emitted whenever setActiveConversation changes the bound conv. */
    void activeConversationChanged();

    /** @brief Emitted whenever the derived busyMembers list changes. */
    void busyMembersChanged();

    /** @brief Emitted whenever the derived hasActiveTask flag changes. */
    void hasActiveTaskChanged();

  private slots:
    /**
     * @brief Inserts a row when a new plan in the active scope is created.
     * @param planId  Plan UUID emitted by PlanService::planCreated.
     */
    void onPlanCreated(const QString& planId);

    /**
     * @brief Refreshes the row for an updated plan in the active scope.
     * @param planId  Plan UUID emitted by PlanService::planUpdated.
     */
    void onPlanUpdated(const QString& planId);

    /**
     * @brief Removes the row for a deleted plan.
     * @param planId  Plan UUID emitted by PlanService::planDeleted.
     */
    void onPlanDeleted(const QString& planId);

    /**
     * @brief Re-renders the nested `steps` payload for the affected plan.
     * @param stepId  Step UUID emitted by PlanService::stepUpdated.
     */
    void onStepUpdated(const QString& stepId);

    /**
     * @brief Re-renders the nested `steps` payload when an artifact is
     *        attached, since the step's artifact list is part of the
     *        step roll-up.
     * @param artifactId  Artifact UUID emitted by PlanService::artifactAdded.
     */
    void onArtifactAdded(const QString& artifactId);

  private:
    PlanService& m_svc;
    QString m_activeConvId;
    QList<AgentPlan> m_plans;
    QStringList m_busyMembers;
    bool m_hasActiveTask = false;

    /**
     * @brief Recomputes the busy-members list and hasActiveTask flag
     *        from the current plan/step state. Emits the matching
     *        signals only when the values actually change so QML
     *        bindings don't churn on every insert.
     */
    void recomputeDerivedState();

    /** @brief Linear scan for a plan row by id. Returns -1 if absent. */
    int findPlanRow(const QString& planId) const;

    /** @brief Full reload from service, wrapped in beginResetModel. */
    void reloadActiveConversation();

    /** @brief Returns true if a plan row exists for the given conversation. */
    bool planBelongsToActiveConversation(const AgentPlan& plan) const;

    /**
     * @brief Builds the nested `steps` QVariantList for a plan row.
     *        Called on demand from data() and whenever a plan/step update
     *        arrives for that plan.
     */
    QVariantList buildStepsForPlan(const AgentPlan& plan) const;
};

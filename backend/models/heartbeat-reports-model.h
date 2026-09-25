// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file heartbeat-reports-model.h
 * @brief Push-based Qt list model of heartbeat reports for the active
 *        scope (conversation or project folder).
 *
 *        Subscribes to HeartbeatSubagentService signals and translates each
 *        run-completion into a row-level dataChanged / insertion event so
 *        the overlay's ListView re-renders without a timer-driven poll.
 *
 *        Model surface mirrors the PlansModel pattern:
 *          - One row per heartbeat_reports row in scope.
 *          - setActiveConversation(convId) reloads rows for the union
 *            of (1to1 + group) configs in that conversation.
 *          - count, activeConversationId, hasPendingReports as
 *            Q_PROPERTYs so QML bindings don't poll.
 *          - Time-dependent fields (started_at, completed_at) are
 *            exposed as raw epoch-ms numbers so QML formats them at
 *            render time, with no live ticker.
 *
 *        Read-only display: the manual "Run now" / "Dismiss" affordances
 *        are wired through the HeartbeatSubagent QML singleton, not
 *        through this model.
 *
 * @layer Service (Model)
 * @dependencies HeartbeatConfigService, HeartbeatSubagentService,
 *               DbManager, AgentRegistry, Qt6::Core, Qt6::Sql
 */


#pragma once

#include "heartbeat-config.h"
#include "heartbeat-report.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class AgentRegistry;
class DbManager;
class HeartbeatConfigService;
class HeartbeatSubagentService;

/**
 * @brief QAbstractListModel exposing one row per heartbeat report in
 *        the active scope (conversation or project folder).
 *
 * The model is push-driven: row inserts and updates come from
 * HeartbeatSubagentService signals; QML bindings never poll. Time
 * fields are exposed as raw epoch-ms for client-side formatting.
 */
class HeartbeatReportsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString activeConversationId READ activeConversationId WRITE setActiveConversation
                   NOTIFY activeConversationChanged)

    /**
     * @brief True if at least one report row in the model has
     *        surface_status == "pending" or any of the skipped_*
     *        states. The overlay header uses this to badge the
     *        "needs review" count separately from total.
     */
    Q_PROPERTY(int pendingReviewCount READ pendingReviewCount NOTIFY pendingReviewCountChanged)

  public:
    enum ReportRoles {
        IdRole = Qt::UserRole + 1,
        ConfigIdRole,
        AgentIdRole,
        AgentNameRole,      ///< Resolved from AgentRegistry. Empty if agent deleted.
        AgentIconNameRole,  ///< Resolved from AgentRegistry. Empty if agent deleted.
        AliasRole,          ///< From the config row (denormalised).
        StartedAtMsRole,
        CompletedAtMsRole,  ///< -1 if not completed.
        DurationMsRole,     ///< completed_at - started_at; -1 if not completed.
        OutcomeRole,
        TitleRole,
        BodyRole,
        SummaryRole,
        ParentReviewStatusRole,
        SurfaceStatusRole,
        SurfacedMessageIdRole,
        ErrorRole,
    };
    Q_ENUM(ReportRoles)

    /**
     * @brief Constructor.
     * @param configSvc HeartbeatConfigService for resolving configs in scope.
     * @param subagentSvc HeartbeatSubagentService for run-completion signals.
     * @param db DbManager for direct heartbeat_reports queries.
     * @param agents AgentRegistry for resolving agent name + icon for display.
     * @param parent Standard QObject parent.
     */
    HeartbeatReportsModel(HeartbeatConfigService& configSvc,
                          HeartbeatSubagentService& subagentSvc,
                          DbManager& db,
                          AgentRegistry& agents,
                          QObject* parent = nullptr);

    /**
     * @brief Loads report rows for the union of conversation_1to1 +
     *        conversation_group configs in the given conversation.
     * @param convId  Conversation UUID. Empty clears the model.
     *
     * Clears any folder-scope binding set via setActiveFolder. The model
     * holds either a conversation scope or a folder scope, never both.
     */
    void setActiveConversation(const QString& convId);

    /**
     * @brief Loads report rows for the folder-scoped configs in a project
     *        folder. Used by the project-level overlay variant.
     * @param folderId  Project folder UUID. Empty clears the model.
     *
     * Clears any conversation-scope binding set via setActiveConversation.
     */
    Q_INVOKABLE void setActiveFolder(const QString& folderId);

    /**
     * @brief Returns the currently-bound conversation id.
     * @returns Conversation UUID, or empty when a folder scope is bound
     *          or no scope is set.
     */
    QString activeConversationId() const { return m_activeConvId; }

    /**
     * @brief Returns the currently-bound project folder id.
     * @returns Folder UUID, or empty when a conversation scope is bound
     *          or no scope is set.
     */
    QString activeFolderId() const { return m_activeFolderId; }

    /**
     * @brief Returns the number of report rows in the model.
     * @returns Row count.
     */
    int count() const { return static_cast<int>(m_rows.size()); }

    /**
     * @brief Returns the count of rows whose surface_status indicates
     *        they need human review.
     * @returns Pending-review row count.
     */
    int pendingReviewCount() const { return m_pendingReviewCount; }

    /**
     * @brief Cascade cleanup hook fired when a conversation is deleted.
     * @param convId  UUID of the deleted conversation.
     *
     * The FK CASCADE removes configs + reports from the DB; this hook
     * drops any in-memory rows belonging to the deleted conversation
     * so QML does not render dangling entries.
     */
    void onSourceConversationDeleted(const QString& convId);

    /**
     * @brief Cascade cleanup hook fired when a project folder is deleted.
     * @param folderId  UUID of the deleted folder.
     *
     * Called by AppController on ConversationService::folderDeleted; the
     * FK CASCADE removed configs + reports from the DB and this hook
     * drops the matching in-memory rows so the project overlay does not
     * render ghosts.
     */
    void onSourceFolderDeleted(const QString& folderId);

    // QAbstractListModel

    /**
     * @brief QAbstractListModel row count.
     * @param parent  Ignored (flat list model).
     * @returns Row count.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractListModel data accessor.
     * @param index  Row index requested.
     * @param role   Role from the ReportRoles enum.
     * @returns QVariant holding the role value, or an invalid QVariant
     *          when index / role is out of range.
     */
    QVariant data(const QModelIndex& index, int role) const override;

    /**
     * @brief QAbstractListModel role-name map for QML.
     * @returns Hash mapping ReportRoles enum values to QML role names.
     */
    QHash<int, QByteArray> roleNames() const override;

  signals:
    /** @brief Emitted whenever the row count changes. */
    void countChanged();

    /** @brief Emitted whenever setActiveConversation changes the bound conversation. */
    void activeConversationChanged();

    /** @brief Emitted whenever setActiveFolder changes the bound folder. */
    void activeFolderChanged();

    /** @brief Emitted whenever pendingReviewCount changes. */
    void pendingReviewCountChanged();

  private slots:
    /**
     * @brief Inserts a placeholder row when a heartbeat run starts.
     * @param runId     New report id (also the run id).
     * @param configId  Owning config id; used to scope-filter.
     */
    void onRunStarted(QString runId, QString configId);

    /**
     * @brief Updates the matching row with the run's final outcome.
     * @param runId    Report id emitted by HeartbeatSubagentService::runCompleted.
     * @param outcome  Outcome string ("success", "error", "timeout", ...).
     */
    void onRunCompleted(QString runId, QString outcome);

    /**
     * @brief Updates the matching row with a failure outcome and error text.
     * @param runId  Report id emitted by HeartbeatSubagentService::runFailed.
     * @param error  Human-readable error message.
     */
    void onRunFailed(QString runId, QString error);

    /**
     * @brief Refreshes the cached config used for per-row alias / agent
     *        resolution when the underlying config changes.
     * @param configId  Config id emitted by HeartbeatConfigService::configChanged.
     */
    void onConfigChanged(const QString& configId);

    /**
     * @brief Drops any rows belonging to a removed config from the model.
     * @param configId  Config id emitted by HeartbeatConfigService::configRemoved.
     */
    void onConfigRemoved(const QString& configId);

  private:
    HeartbeatConfigService& m_configSvc;
    HeartbeatSubagentService& m_subagentSvc;
    DbManager& m_db;
    AgentRegistry& m_agents;

    QString m_activeConvId;
    QString m_activeFolderId;

    /// Materialised row state (sorted by started_at DESC).
    QList<HeartbeatReport> m_rows;

    /// Cached config-by-id lookup so per-row alias / agentId resolution
    /// doesn't roundtrip the DB on every data() call.
    QHash<QString, HeartbeatConfig> m_configsByConfigId;

    int m_pendingReviewCount = 0;

    /**
     * @brief Resolves the set of config_ids that belong to the active
     *        scope: conversation (1to1 + group rows) OR folder
     *        (folder-scope rows). The two scopes are mutually
     *        exclusive; only one is non-empty at a time.
     */
    QList<QString> configIdsForActiveScope() const;

    /**
     * @brief Loads all reports whose config_id is in `configIds`,
     *        ordered by started_at DESC. Pure DB query; does NOT
     *        emit Model signals.
     */
    QList<HeartbeatReport> queryReports(const QList<QString>& configIds) const;

    /**
     * @brief Linear scan for a row by report id. Returns -1 if absent.
     */
    int findRow(const QString& reportId) const;

    /**
     * @brief Recompute pendingReviewCount from m_rows and emit if changed.
     */
    void recomputePendingReviewCount();

    /**
     * @brief Refreshes m_configsByConfigId from configSvc for the
     *        active conversation. Used on conv switch + config events.
     */
    void refreshConfigsCache();

    /**
     * @brief Performs a single-row reload for the given report id.
     *        Used after run-completion events. Inserts a new row if
     *        the report wasn't already in m_rows (the typical case).
     */
    void reloadOneRow(const QString& reportId);
};

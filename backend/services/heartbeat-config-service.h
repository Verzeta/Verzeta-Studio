// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-config-service.h
 * @brief CRUD + specialised lookups over the heartbeat_configs
 *        table. HeartbeatSubagentService consumes this service to
 *        discover schedules, persist last_fire_at anchors after
 *        every fire, and resolve the (agent, scope, alias) tuple
 *        that owns a given run.
 * @layer Service
 * @dependencies DbManager, HeartbeatConfig POD, Qt6::Core, Qt6::Sql
 */

#pragma once

#include "../models/db-manager.h"
#include "../models/heartbeat-config.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

/**
 * @brief Heartbeat-config CRUD service. Owns the heartbeat_configs
 *        table and the heartbeat_config_changes audit log.
 */
class HeartbeatConfigService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the service.
     * @param db     Non-owning DbManager reference.
     * @param parent Qt parent (AppController).
     */
    explicit HeartbeatConfigService(DbManager& db, QObject* parent = nullptr);
    ~HeartbeatConfigService() override;

    // -----------------------------------------------------------------
    // CRUD
    // -----------------------------------------------------------------

    /**
     * @brief Inserts a new heartbeat_configs row.
     * @param cfg Config to insert. The id field MUST be a pre-generated
     *            UUID; createdAt / updatedAt may be left default and
     *            will be filled by this method.
     * @return UUID on success, empty string on failure (uniqueness
     *         violation, missing FK target, etc.). Emits configChanged
     *         on success.
     * @sideeffects Inserts into heartbeat_configs.
     */
    QString upsertConfig(HeartbeatConfig cfg);

    /**
     * @brief Fetches a config by id.
     * @param id Config UUID.
     * @return Populated HeartbeatConfig on hit, default-constructed on miss.
     */
    HeartbeatConfig configById(const QString& id) const;

    /**
     * @brief Fetches the unique config row for a given (agent,
     *        scope, alias) tuple, if one exists.
     * @param agentId   Agent UUID.
     * @param scopeType One of HeartbeatScopeType enum values.
     * @param scopeId   Conversation / folder UUID for the scope.
     * @param alias     Membership alias (empty for 1:1).
     * @return Populated HeartbeatConfig on hit, default-constructed
     *         on miss.
     */
    HeartbeatConfig configFor(const QString& agentId,
                              HeartbeatScopeType scopeType,
                              const QString& scopeId,
                              const QString& alias) const;

    /**
     * @brief Updates an existing config row by id. Mutates
     *        updatedAt.
     * @param cfg Config (id field identifies the row to update).
     * @return true on success.
     * @sideeffects Updates the row; emits configChanged.
     */
    bool updateConfig(const HeartbeatConfig& cfg);

    /**
     * @brief Deletes a config row.
     * @param id Config UUID to delete.
     * @return true if a row was actually removed.
     * @sideeffects ON DELETE CASCADE in the schema removes related
     *              heartbeat_reports rows. Emits configRemoved.
     */
    bool deleteConfig(const QString& id);

    // -----------------------------------------------------------------
    // Scheduler-targeted lookups
    // -----------------------------------------------------------------

    /**
     * @brief Returns every enabled config row (enabled=1). Used by
     *        HeartbeatSubagentService::initialize() to populate its
     *        scheduling state on startup, and by the queue's tick to
     *        decide which configs are due for a fire.
     * @return Vector of configs, ordered by last_fire_at ASC NULLS
     *         FIRST so configs that have never fired (or fired longest
     *         ago) come first, as a fairness primitive for tied schedules.
     */
    QList<HeartbeatConfig> enabledConfigs() const;

    /**
     * @brief Returns every config in a given scope. Used by the
     *        per-conversation / per-folder activity overlay UI.
     * @param scopeType One of HeartbeatScopeType enum values.
     * @param scopeId   Conversation / folder UUID for the scope.
     * @return Vector of configs in the scope; empty when none.
     */
    QList<HeartbeatConfig> configsInScope(HeartbeatScopeType scopeType,
                                          const QString& scopeId) const;

    /**
     * @brief Updates ONLY the last_fire_at + last_fire_outcome
     *        scheduler-anchor fields. Cheaper than a full
     *        updateConfig because it touches two columns; fires on
     *        EVERY attempt outcome (success, error, timeout,
     *        rate_limited, queue_overflow, cancelled, skipped_busy)
     *        so the scheduler has a reliable anchor across
     *        crash-restart.
     * @param configId  Config UUID.
     * @param at        New last_fire_at (Qt::ISODateWithMs string).
     * @param outcome   New last_fire_outcome (one of HeartbeatFireOutcome::*).
     * @return true on success.
     */
    bool updateLastFire(const QString& configId, const QString& at, const QString& outcome);

    // -----------------------------------------------------------------
    // App-layer cleanup hooks (polymorphic FK substitute)
    // -----------------------------------------------------------------

    /**
     * @brief Drop every config whose scope_id matches the given
     *        conversation id. Called by AppController on
     *        ConversationService::conversationDeleted to keep the
     *        polymorphic scope_id from dangling.
     * @param conversationId Conversation UUID that was just
     *                       deleted.
     * @return Number of rows deleted.
     */
    int onConversationDeleted(const QString& conversationId);

    /**
     * @brief Drop every config whose scope_id matches the given
     *        folder id (covers folder/project/organization).
     * @param folderId Folder UUID that was just deleted.
     * @return Number of rows deleted.
     */
    int onFolderDeleted(const QString& folderId);

    // -----------------------------------------------------------------
    // QML-friendly Q_INVOKABLE wrappers. These translate the typed C++
    // surface above into QVariantMap / QVariantList / QString-keyed
    // scope strings so AddChatMemberDialog, RightSettingsPanel, and
    // HeartbeatActivityOverlay can call the service directly without
    // touching the HeartbeatScopeType enum or HeartbeatConfig POD.
    //
    // Direct exposure — these live here on the service rather than on
    // a forwarder object.
    // -----------------------------------------------------------------

    /**
     * @brief Returns every heartbeat_configs row scoped to a given
     *        conversation (union of conversation_1to1 and
     *        conversation_group). Each entry is a QVariantMap with
     *        the same key set as @c configMapFromConfig.
     * @param conversationId Conversation UUID.
     * @return QVariantList of config dicts. Empty for unknown ids.
     */
    Q_INVOKABLE QVariantList configsForConversation(const QString& conversationId) const;

    /**
     * @brief Returns one heartbeat_configs row by id, as a
     *        QVariantMap.
     * @param id Config UUID.
     * @returns Populated map on hit; empty (default-constructed)
     *          map on miss.
     */
    Q_INVOKABLE QVariantMap configByIdMap(const QString& id) const;

    /**
     * @brief Returns the existing config for a given (agent, scope,
     *        alias) tuple, as a QVariantMap, or an empty map on
     *        miss. Used by AddChatMemberDialog to detect when an
     *        alias already has a config row (rather than creating a
     *        duplicate).
     * @param agentId       Agent UUID.
     * @param scopeTypeStr  One of `"conversation_1to1"` /
     *                      `"conversation_group"` / `"folder"`.
     * @param scopeId       Conversation or folder UUID.
     * @param alias         Membership alias (`""` for 1:1).
     * @returns Populated map on hit; empty map on miss.
     */
    Q_INVOKABLE QVariantMap configForMap(const QString& agentId,
                                         const QString& scopeTypeStr,
                                         const QString& scopeId,
                                         const QString& alias) const;

    /**
     * @brief Inserts or updates a config row from a QVariantMap. The
     *        map keys mirror the HeartbeatConfig POD field names (camel
     *        case). Missing optional keys take struct defaults; an
     *        absent `id` triggers an insert with a freshly-generated
     *        UUID.
     *
     *        Required keys for inserts: agentId, scopeType,
     *        scopeId, alias (may be empty for 1:1). Optional:
     *        enabled, schedule, goal, surfaceCriteria,
     *        maxRunsPerDay, autoSurfaceTargetConversationId,
     *        selfConfigAllowed.
     * @param fields QVariantMap of config fields (see body).
     * @return The config UUID on success, empty string on failure.
     */
    Q_INVOKABLE QString upsertConfigMap(const QVariantMap& fields);

    /**
     * @brief Q_INVOKABLE wrapper around deleteConfig().
     * @param id Config UUID to remove.
     * @returns true if a row was actually removed.
     */
    Q_INVOKABLE bool removeConfig(const QString& id);

    /**
     * @brief Computes the intelligent default for a conversation's
     *        auto_surface_max_per_day when the user first toggles
     *        the gate. Sums firesIn24h() over enabled configs in
     *        scope (clamped per-config to max_runs_per_day), then
     *        caps the total at 24. Floor 1 so a fresh enable always
     *        yields a usable cap.
     * @param conversationId Conversation UUID.
     * @return Suggested cap. Always >= 1.
     */
    Q_INVOKABLE int suggestedAutoSurfaceCap(const QString& conversationId) const;

    /**
     * @brief List every heartbeat_configs row scoped to a given
     *        folder (project / organization / regular). Mirrors
     *        configsForConversation but for folder scope.
     * @param folderId Folder UUID.
     * @return QVariantList of config dicts (same shape as
     *         configsForConversation entries). Empty for unknown
     *         ids or folders with no folder-scoped configs.
     */
    Q_INVOKABLE QVariantList configsForFolder(const QString& folderId) const;

    /**
     * @brief Folder-scope variant of suggestedAutoSurfaceCap. The
     *        folder's enabled-HB configs all post into the same
     *        target conversation, so the cap sums over folder
     *        configs whose target is the given conversation. Used
     *        when the user picks a target conv + flips the per-conv
     *        gate ON inside FolderSettingsDialog.
     * @param folderId      Folder UUID.
     * @param targetConvId  The auto_surface_target_conversation_id
     *                      that folder configs are pointed at.
     * @return Suggested cap >= 1.
     */
    Q_INVOKABLE int suggestedAutoSurfaceCapForFolder(const QString& folderId,
                                                     const QString& targetConvId) const;

    // -----------------------------------------------------------------
    // heartbeat_config_changes audit log.
    //
    // recordChange writes one row of (config, field, old, new,
    // source). Called by the self-config tool implementations
    // (source='agent') and by the user-facing UI surfaces
    // (source='user') so the diagnostics audit panel can show every
    // mutation regardless of origin.
    // -----------------------------------------------------------------

    /**
     * @brief Append a single audit row to heartbeat_config_changes.
     * @param configId  Heartbeat config UUID. Must reference an
     *                  existing row (FK enforced by the schema).
     * @param field     Logical field name (e.g. "goal", "schedule",
     *                  "surface_criteria", "enabled",
     *                  "self_config_allowed"). Caller-defined; the
     *                  schema does not enumerate.
     * @param oldValue  Previous value as a string (numbers / bools
     *                  serialised by the caller). Empty for "new
     *                  config" rows.
     * @param newValue  New value as a string.
     * @param source    "agent" or "user" (CHECK constraint enforces).
     * @return true on successful insert.
     */
    bool recordChange(const QString& configId,
                      const QString& field,
                      const QString& oldValue,
                      const QString& newValue,
                      const QString& source);

    /**
     * @brief Recent audit rows across all configs, for the
     *        Diagnostics → Self-config audit tab. Returned
     *        newest-first.
     * @param limit  Max rows. Clamped to [1, 500].
     * @return QVariantList of {id, configId, alias, agentName,
     *         changedAtMs, field, oldValue, newValue, source},
     *         with alias + agentName resolved through the join with
     *         heartbeat_configs + agents so QML doesn't need a
     *         second lookup.
     */
    Q_INVOKABLE QVariantList recentConfigChanges(int limit = 100) const;

  signals:
    /**
     * @brief Emitted when a config is inserted or updated.
     * @param configId UUID of the affected config row.
     */
    void configChanged(const QString& configId);

    /**
     * @brief Emitted when a config is deleted (any path).
     * @param configId UUID of the removed config row.
     */
    void configRemoved(const QString& configId);

  private:
    DbManager& m_db;
};

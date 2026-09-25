// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file db-manager.h
 * @brief Singleton managing all SQLite database access via Qt SQL.
 *        Handles schema creation, migrations, and provides transaction support.
 * @layer Data Access
 * @dependencies Qt6::Sql, SQLite3
 */

#pragma once

#include <memory>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

class QLockFile;

/**
 * @brief Singleton database manager for all SQLite access in Verzeta Studio.
 *
 * All queries must use QSqlQuery::prepare() with parameter binding to prevent
 * SQL injection. Direct string interpolation into SQL is strictly forbidden.
 *
 * The schema is versioned via the `settings` table. Migrations are applied
 * incrementally from the current version to the latest defined version.
 */
class DbManager : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Returns the singleton instance of the database manager.
     * @return Reference to the global DbManager.
     * @complexity O(1)
     */
    static DbManager& instance();

    // Non-copyable, non-movable singleton
    DbManager(const DbManager&) = delete;
    DbManager& operator=(const DbManager&) = delete;
    DbManager(DbManager&&) = delete;
    DbManager& operator=(DbManager&&) = delete;

    /**
     * @brief Opens (or creates) the SQLite database at the given path.
     * @param path Absolute file path to the .db file.
     * @return true if the database was opened successfully.
     * @sideeffects Creates the file and parent directories if they don't exist.
     */
    bool open(const QString& path);

    /**
     * @brief Applies schema migrations from current version to latest.
     * @return true if all migrations succeeded.
     * @sideeffects Modifies database schema, writes to settings table.
     */
    bool runMigrations();

    /**
     * @brief Returns a reference to the underlying QSqlDatabase.
     * @return QSqlDatabase reference.
     */
    QSqlDatabase& db();

    /**
     * @brief Begins a database transaction.
     * @return true if transaction started successfully.
     */
    bool transaction();

    /**
     * @brief Commits the current transaction.
     * @return true if commit succeeded.
     */
    bool commit();

    /**
     * @brief Rolls back the current transaction.
     * @return true if rollback succeeded.
     */
    bool rollback();

    /**
     * @brief Returns whether the database is currently open and valid.
     * @return true if database is open and ready for queries.
     */
    bool isOpen() const;

    /**
     * @brief Closes the database connection.
     * @sideeffects Releases SQLite file lock.
     */
    void close();

    /**
     * @brief Returns the path to the database file.
     * @return Absolute path to the .db file.
     */
    QString databasePath() const;

    /**
     * @brief Loads the sqlite-vec loadable extension on the open connection,
     *        enabling vec0 KNN vector search.
     * @param loadablePath Absolute path to the vec0 loadable (.so/.dll/.dylib).
     * @returns true if the extension loaded and vec_version() responded;
     *          false on any failure (missing file, sqlite-version mismatch
     *          with Qt's driver, or sqlite3 C API unavailable). Callers then
     *          fall back to brute-force vector search. Never throws or crashes.
     * @note Must be called after open(). The extension is loaded on the single
     *       DbManager connection; vector queries run on that connection.
     *       Loading is re-disabled immediately after (security hardening).
     */
    bool loadVectorExtension(const QString& loadablePath);

    /**
     * @brief Whether vec0 vector search is available on this connection.
     * @returns true iff loadVectorExtension() previously succeeded.
     */
    bool isVectorSearchAvailable() const;

  signals:
    /**
     * @brief Emitted when a database error occurs.
     * @param error Human-readable error description (never contains SQL query text
     *              or sensitive data).
     */
    void errorOccurred(const QString& error);

  private:
    DbManager();
    // Defined out-of-line so the unique_ptr<QLockFile> destructor can
    // see the complete QLockFile type (header forward-declares it to
    // avoid pulling QtCore's QLockFile include into every consumer).
    ~DbManager() override;

    QSqlDatabase m_db;
    QString m_path;
    bool m_vectorSearchAvailable = false;  ///< set true by loadVectorExtension()

    /**
     * @brief Engine lock, held for the lifetime of the DbManager
     *        instance to prevent a second `verzeta-studio` process
     *        from opening the same database file.
     *
     * Acquired inside `open()` BEFORE `m_db.open()` so a refusal
     * surfaces before any database state is created or modified.
     * Released by the destructor or by `close()`. PID-based stale-
     * lock detection (handled by Qt) auto-reclaims the lockfile
     * after a crashed previous process, so users never need to
     * manually delete the file.
     */
    std::unique_ptr<QLockFile> m_engineLock;

    /**
     * @brief Creates all tables and indexes for schema version 1.
     * @sideeffects Executes CREATE TABLE and CREATE INDEX statements.
     */
    void applySchema();

    /**
     * @brief Adds agent-support tables and columns (schema v1 → v2).
     * @return true on success.
     */
    bool applySchemaV2();

    /**
     * @brief Adds group-chat column (schema v2 → v3).
     *        Adds conversations.group_agent_ids (JSON array of agent UUIDs).
     * @return true on success.
     */
    bool applySchemaV3();

    /**
     * @brief Adds aliased membership tables (schema v3 → v4).
     *        Adds project_members, conversation_members tables,
     *        agents.is_coordinator, messages.member_alias.
     *        Migrates existing agent_ids / group_agent_ids JSON into the
     *        new tables using the agent's name as the default alias.
     * @return true on success.
     */
    bool applySchemaV4();

    /**
     * @brief Adds state-driven execution loop tables (schema v4 → v5).
     *        Creates agent_plans, plan_steps, step_artifacts tables for
     *        tracked task execution. No data migration needed;
     *        the tables start empty.
     * @return true on success.
     */
    bool applySchemaV5();

    /**
     * @brief Separates message data from tool execution data (v5 → v6).
     *
     *        Adds:
     *          - messages.turn_id TEXT (groups all rows produced in
     *            response to one user message)
     *          - idx_messages_turn (turn_id, agent_id) for the per-turn
     *            tool chain query
     *
     *        Cleans (test-environment wipe approved by the user):
     *          - DELETE rows with role='tool' (these were a hack; they
     *            belong in tool_calls)
     *          - DELETE empty assistant rows whose finish_reason was
     *            'tool_calls' but had no associated tool_calls entry
     *            (orphan anchors from the same hack)
     *
     *        Backfills:
     *          - turn_id for every existing row by walking each
     *            conversation chronologically: each user message opens
     *            a new turn (turn_id = its own id), every subsequent
     *            assistant/system row inherits that turn_id until the
     *            next user message.
     *
     * @return true on success.
     */
    bool applySchemaV6();

    /**
     * @brief Migration v6 → v7: Tool-call → plan-step lineage.
     *
     * Adds tool_calls.plan_step_id as a nullable FK to plan_steps(id).
     * ChatController stamps the currently-active plan step id on every
     * tool_calls row it writes during a task turn, so the lineage
     * "which step executed which tool" becomes a single join instead
     * of a multi-table derivation through TaskObserver rows.
     */
    bool applySchemaV7();

    /**
     * @brief Migration v7 → v8: Canvas feature.
     *
     * Adds the canvas_artifacts table, with one row per artifact open in a
     * conversation's Canvas panel. Each row tracks filename, language,
     * full content, monotonic revision counter (++ on every edit), and
     * an is_archived flag (canvases the user closed via X but kept in
     * the history dropdown). FK CASCADE on conversation_id matches the
     * messages / tool_calls behaviour: deleting a conversation removes
     * its canvas rows; the disk-mirror files in the project artifact
     * directory are NOT auto-deleted (disk-backed
     * mode) and the user can keep or remove them manually.
     *
     * Adds idx_canvas_active(conversation_id, is_archived, updated_at
     * DESC) so `activeCanvasFor(convId)` (the most-recent non-archived
     * row) and `historyForConversation(convId)` (every row reverse-
     * chronological) both index-resolve in O(log n).
     */
    bool applySchemaV8();

    /**
     * @brief Migration v8 → v9: Conversation pinning.
     *
     * Adds the conversations.is_pinned column (INTEGER NOT NULL DEFAULT 0).
     * Pinning is a per-conversation flag that drives the new sidebar
     * "Pinned" section. When true, the row appears in
     * the Pinned section IN ADDITION TO its natural section (project /
     * standalone group / direct agent / plain). Pinning duplicates, it
     * does not move.
     *
     * Adds idx_conversations_pinned(is_pinned, updated_at DESC) so the
     * sidebar's "all pinned" sweep resolves with an index seek instead
     * of a full-table scan. updated_at DESC matches the per-section sort
     * order the sidebar uses.
     *
     * The migration is purely additive: no data migration, no schema
     * mutation on existing columns. Existing rows get is_pinned = 0 via
     * the column DEFAULT, so users with an old DB see all conversations
     * unpinned, which is the intended initial state.
     */
    bool applySchemaV9();

    /**
     * @brief Migration v9 → v10: Heartbeat subagents.
     *
     * Adds two new tables:
     *
     *   1. heartbeat_configs: one row per (agent, scope, alias) tuple.
     *      Identity model: a heartbeat belongs to an agent IN a membership
     *      context, not to an agent template alone (because the same
     *      template can occupy multiple seats in a group chat under
     *      different aliases, e.g. @ResearcherNorthAmerica vs
     *      @ResearcherTikTok). The (agent_id, scope_type, scope_id, alias)
     *      tuple is unique. scope_type is one of:
     *        - 'conversation_1to1'  : 1:1 chat. scope_id = convId.
     *                                 alias = '' (single seat).
     *        - 'conversation_group' : group chat. scope_id = convId.
     *                                 alias = membership alias.
     *        - 'folder'             : folder/project/organization (folders
     *                                 row, regardless of folder_type).
     *                                 scope_id = folderId.
     *                                 alias = project_members.alias.
     *
     *      Other columns: enabled, schedule (one of the 4 named
     *      formats), goal, surface_criteria, max_runs_per_day,
     *      auto_surface_target_conversation_id, self_config_allowed,
     *      last_fire_at (the scheduler anchor, updated on EVERY fire
     *      attempt regardless of outcome so crash-restart amnesty is
     *      reliable), last_fire_outcome, created_at, updated_at.
     *
     *      FK agent_id → agents(id) ON DELETE CASCADE so deleting an
     *      agent template tears down its heartbeat configs. No FK on
     *      scope_id because the column is polymorphic (conv id or folder
     *      id depending on scope_type); the app layer handles cleanup on
     *      conversation/folder deletion.
     *
     *   2. heartbeat_reports: one row per Tier-1 subagent run.
     *      Carries the structured report (TITLE / RESULTS / SUMMARY parsed
     *      from the agent's structured output), outcome, scheduler
     *      timestamps, and TWO orthogonal status fields:
     *        - parent_review_status : 'pending' | 'reviewed' |
     *                                 'not_reviewable_yet'
     *        - surface_status       : 'pending' | 'posted_auto' |
     *                                 'posted_manual' | 'skipped_by_agent' |
     *                                 'skipped_by_gate' |
     *                                 'skipped_by_rate_limit' |
     *                                 'dismissed_by_user'
     *      The two are orthogonal so a gate-OFF report is correctly
     *      "not_reviewable_yet + pending" (manual override still
     *      available) rather than falsely "reviewed + skipped".
     *
     *      FK config_id → heartbeat_configs(id) ON DELETE CASCADE.
     *
     * Indexes added:
     *   - idx_heartbeat_configs_scope     (scope_type, scope_id)
     *   - idx_heartbeat_configs_enabled   (enabled, last_fire_at)
     *   - idx_heartbeat_reports_config_started
     *                                     (config_id, started_at DESC)
     *   - idx_heartbeat_reports_pending_review
     *                                     (parent_review_status, started_at DESC)
     *
     * Purely additive: no existing column changes, no data migration.
     * Default-OFF semantics: every existing install is unaffected
     * (heartbeat_configs is empty until a user creates a config row).
     */
    bool applySchemaV10();

    /**
     * @brief Schema v11 adds:
     *
     *   - heartbeat_config_changes: append-only audit log of every
     *     mutation to a heartbeat_configs row. Required for the
     *     diagnostics "Self-configuration audit" surface and for the
     *     accountability invariants on agent self-config tools (any
     *     change attributed to an agent gets a row tagged
     *     source='agent', any change made via the user UI gets
     *     source='user'). FK config_id → heartbeat_configs(id) ON
     *     DELETE CASCADE so removing a heartbeat config also clears
     *     its history.
     *
     *   - agents.default_heartbeat_goal,
     *     agents.default_heartbeat_schedule,
     *     agents.default_heartbeat_surface_criteria:
     *     three optional pre-fill strings, deferred.
     *     When the user picks the agent template in AddChatMember-
     *     Dialog (or Folder settings), these populate the heartbeat
     *     expander's fields so power users with many memberships
     *     don't retype the same goal each time. Source of truth
     *     remains the heartbeat_configs row per (agent, scope,
     *     alias). These agents-level fields are hint-only.
     *
     * Indexes added:
     *   - idx_heartbeat_config_changes_cfg
     *                                     (config_id, changed_at DESC)
     *
     * Purely additive: no existing column changes, no data migration.
     * Default semantics: every new column has DEFAULT '' so existing
     * installs see no behavioural change.
     */
    bool applySchemaV11();

    /**
     * @brief Schema v12: provenance columns on
     *        project_members + conversation_members. Adds two columns
     *        to each table:
     *
     *          - added_by_kind    TEXT NOT NULL DEFAULT 'user'
     *          - added_by_agent_id TEXT NOT NULL DEFAULT ''
     *
     *        Backfill: every existing row defaults to 'user' / '' so
     *        pre-existing memberships are protected from agent-driven
     *        removal by the new restriction in remove_project_member.
     *        New agent-added members tag their row with 'agent' + the
     *        calling agent's id so future tools can audit who added
     *        what.
     *
     *        Purely additive: every UI write path that doesn't pass
     *        the new columns gets the default 'user' kind, preserving
     *        all existing behaviour (UI removal still works for every
     *        member regardless of kind).
     */
    bool applySchemaV12();

    /**
     * @brief Schema v13 adds three new tables for
     *        group-chat coordination polls / voting.
     *
     *          polls:         one row per poll (conv-scoped).
     *          poll_options:  N options per poll, ordered.
     *          poll_votes:    votes cast by agents and/or the user.
     *
     *        v1 supports two vote modes: 'single' (one vote per voter)
     *        and 'multi' (multiple options per voter). Ranked-choice is
     *        deferred. The schema reserves the column shape for it but
     *        v13 does not accept mode='ranked'.
     *
     *        Lazy auto-close: pollResults() flips status='open' to
     *        'closed' when now > closes_at. No background sweep job;
     *        this keeps the schema additive and the code paths simple.
     *
     *        Purely additive migration; existing tables unchanged.
     */
    bool applySchemaV13();

    /**
     * @brief Schema v14 adds per-member model
     *        override columns (model_provider / model_name /
     *        allowed_tools) to project_members + conversation_members,
     *        and the nullable member_alias column to conversations.
     *        Verify-at-source: re-queries PRAGMA
     *        table_info and returns false if any expected column is
     *        missing, so runMigrations cannot bump schema_version on a
     *        half-applied migration.
     * @return true only when every expected column is confirmed present.
     */
    bool applySchemaV14();

    /**
     * @brief Schema v15: unified audit trail. Adds
     *        the append-only `activity_log` table (13 columns + 3
     *        indexes):
     *
     *          id PK, created_at, project_folder_id, conversation_id,
     *          turn_id, actor_kind, actor_alias, actor_agent_id,
     *          actor_client_id, event_type, tool_name, event_summary,
     *          event_detail
     *
     *        Indexes: idx_activity_project (project_folder_id,
     *        created_at DESC), idx_activity_conv (conversation_id,
     *        created_at DESC), idx_activity_turn (turn_id).
     *
     *        Verify at source: after CREATE TABLE + CREATE
     *        INDEX, re-queries PRAGMA table_info(activity_log) and
     *        returns false if any expected column is missing. The
     *        v11+ "recorded but columns missing" failure class is
     *        structurally impossible.
     *
     * @return true only when every expected column is confirmed.
     */
    bool applySchemaV15();

    /**
     * @brief Adds the `messages.thinking_content` column for the
     *        display-only reasoning sidecar.  NOT NULL DEFAULT '' so
     *        existing rows backfill to empty.  Verifies the column
     *        landed via PRAGMA before returning true.
     * @return true only when the column is confirmed present.
     */
    bool applySchemaV16();

    /**
     * @brief Creates the `folder_mounts` table, the additive metadata
     *        substrate that records which connected client owns a
     *        virtual workspace mount bound to a given folder. Bytes
     *        of workspace files are never persisted here; only the
     *        registration metadata (client id, mount id, owner
     *        label, registered_at, last_seen, tree manifest JSON,
     *        per-mount blocklist + allowlist additions, permission
     *        tier).
     *
     *        `folder_id` is PRIMARY KEY with `ON DELETE CASCADE`
     *        REFERENCES folders(id) so deleting a folder atomically
     *        removes its mount registration. This mirrors the existing
     *        `project_members.folder_id` cascade contract.
     *
     *        Index: `idx_folder_mounts_client(client_id)` to support
     *        the per-client cleanup path that drops every mount owned
     *        by a wire-revoked client.
     *
     *        Verify-at-source per the v14/v15/v16 pattern: PRAGMA
     *        re-checks the column set after CREATE so a half-applied
     *        migration cannot advance `schema_version`.
     *
     * @return true only when the table and every expected column are
     *         confirmed present.
     */
    bool applySchemaV17();

    /**
     * @brief Schema v18 widens `activity_log.actor_kind`'s CHECK
     *        constraint to include `'client'`, the actor kind already
     *        emitted by the five paired-client factories
     *        (`forWorkspaceMountRegistered` / Unregistered / Replaced /
     *        TreeUpdated / TierChanged). Those factories were added
     *        without bumping the schema, so every
     *        `workspace.mount.*` event was silently rejected by the v15
     *        CHECK and never reached `activity_log`. v18 fixes that
     *        structurally.
     *
     *        SQLite cannot ALTER TABLE to modify a CHECK constraint, so
     *        the migration follows the rename-recreate-copy-drop recipe
     *        all within the surrounding transaction:
     *
     *          1. SELECT COUNT(*) FROM activity_log (pre)
     *          2. ALTER TABLE activity_log RENAME TO activity_log_v17_backup
     *          3. CREATE TABLE activity_log (... with the new CHECK ...)
     *          4. INSERT INTO activity_log SELECT * FROM
     *             activity_log_v17_backup
     *          5. SELECT COUNT(*) FROM activity_log (post)
     *          6. Refuse to advance schema_version unless pre == post
     *          7. DROP TABLE activity_log_v17_backup
     *          8. Recreate the three indexes
     *          9. Verify-at-source: query sqlite_master and confirm
     *             the new SQL contains the literal `'client'`
     *
     *        Existing rows are preserved verbatim. Zero rows of
     *        kind=`'client'` exist anywhere historically (the v15 CHECK
     *        rejected them), so the widening cannot conflict with
     *        existing data.
     *
     * @return true only when every step above succeeds AND the post-
     *         migration sqlite_master entry reflects the new CHECK.
     */
    bool applySchemaV18();

    /**
     * @brief Schema v19 widens `folder_mounts`'s PRIMARY KEY from
     *        `(folder_id)` alone to the composite `(folder_id, client_id)`.
     *
     *        Pre-v19 the table allowed exactly one mount per folder;
     *        attempting to register a second client on the same
     *        Verzeta folder would either silently replace the first
     *        row or fail with a UNIQUE constraint violation. v19
     *        lifts that to per-client rows: every connected VS Code
     *        installation (or other wire client) can register its own
     *        mount on the same folder.
     *
     *        Migration uses the rename-recreate-copy-drop pattern with
     *        a pre/post COUNT(*) parity guard. Existing single-mount
     *        rows migrate 1:1 with no data loss; the foreign-key,
     *        indexes, CHECKs and DEFAULTs are preserved byte-identical
     *        to v17's shape.
     *
     * @return true on full success including the post-migration
     *         row-count parity check.
     */
    bool applySchemaV19();

    /**
     * @brief Schema v20: `conversation_summaries`
     *        table for dynamic compaction. One row per conversation
     *        (PK conversation_id, ON DELETE CASCADE); new summaries
     *        overwrite. Verified at source via PRAGMA table_info
     *        after creation.
     * @returns true on success.
     */
    bool applySchemaV20();

    /**
     * @brief Schema v21: `subagent_runs` (one row
     *        per agent-spawned sub-agent run; lifecycle state machine
     *        queued→running→done|failed|cancelled) +
     *        `subagent_messages` (run-scoped transcript, NOT rows in
     *        `messages`, so the parent conversation's context stays
     *        clean). Verified at source via PRAGMA table_info.
     * @returns true on success.
     */
    bool applySchemaV21();

    /**
     * @brief Schema v22: RAG corpus scope and model identity. Adds
     *        `owner_scope` to `embeddings` and `documents`
     *        (`global` | `conversation:\<id\>` | `project:\<id\>` | `agent:\<id\>`)
     *        so retrieval can be scope-filtered instead of global, plus an
     *        `idx_embeddings_scope` index. Backfills existing message rows'
     *        scope from their source message's conversation. Column-existence
     *        guarded so a re-run is a no-op (defensive self-heal).
     * @returns true on success.
     */
    bool applySchemaV22();

    /**
     * @brief Migration v22 → v23: per-agent memory (AIM). Creates the
     *        `agent_memories` table (durable memory entries scoped by
     *        owner_scope, carrying an embedding BLOB + model_used so it shares
     *        the VectorStore substrate) plus its `agent_memories_fts` FTS5
     *        index for lexical recall and an owner_scope index. Idempotent
     *        (CREATE ... IF NOT EXISTS), so a defensive re-run is a no-op.
     * @returns true on success.
     */
    bool applySchemaV23();

    /**
     * @brief Migration v23 → v24: team/project memory (ACN). Creates the
     *        `acn_entries` table (project/org-scoped memory written at
     *        compaction; carries embedding/model_used/owner_scope for the
     *        VectorStore substrate) + its `acn_entries_fts` FTS5 index + a
     *        scope index, and adds `folders.acn_enabled` (per-project/org master
     *        switch, default 1, column-guarded ALTER). Idempotent.
     * @returns true on success.
     */
    bool applySchemaV24();

    /**
     * @brief Defensive self-heal for the membership-
     *        provenance columns (`added_by_kind` + `added_by_agent_id`
     *        on project_members + conversation_members). Idempotent.
     *
     *        Runs unconditionally at the end of runMigrations()
     *        regardless of recorded schema_version. Detects DBs that
     *        somehow ended up at v12+ recorded but missing the actual
     *        columns (seen on real installations; the symptom is
     *        "Parameter count mismatch" on every membership read /
     *        write because the SQL references columns the table
     *        doesn't have). Cost when healthy: 2 PRAGMA table_info
     *        queries. Cost when broken: 2 PRAGMAs + 1-4 ALTERs.
     *
     * @return true on success (no-op or all needed columns added).
     */
    bool repairMembershipProvenanceColumns();

    /**
     * @brief Retrieves the current schema version from the settings table.
     * @return Current schema version, or 0 if not set.
     * @complexity O(1)
     */
    int currentSchemaVersion();

    /**
     * @brief Stores the schema version in the settings table.
     * @param version Schema version number to record.
     * @sideeffects Writes to settings table.
     */
    void setSchemaVersion(int version);

    /**
     * @brief Enables SQLite WAL mode and foreign key enforcement.
     * @sideeffects Sets PRAGMA journal_mode=WAL and PRAGMA foreign_keys=ON.
     */
    void configurePragmas();
};

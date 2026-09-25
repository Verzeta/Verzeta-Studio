// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent-registry.h
 * @brief Persistent registry of named Agents. Provides CRUD, seeding
 *        of built-in templates, and a QML-invokable API for the
 *        Agents page. Exposed as QML context property "AgentRegistry".
 * @layer Service
 * @dependencies DbManager, Qt6::Core, Qt6::Sql
 */

#pragma once

#include "../models/agent.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class DbManager;

/**
 * @brief CRUD registry for persistent Agent definitions.
 *
 * Backed by the `agents` table (schema v2+). Built-in templates are seeded
 * on first run and marked with `is_builtin = 1`. Users can still edit or
 * delete built-ins. They reappear on next first-run *only* if the table is
 * empty (so deleting all defaults makes them stay deleted).
 *
 * ## Threading
 * All methods must be called from the main thread.
 */
class AgentRegistry : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the registry.
     * @param db     Non-owning DbManager reference.
     * @param parent Qt parent.
     */
    explicit AgentRegistry(DbManager& db, QObject* parent = nullptr);
    ~AgentRegistry() override;

    /**
     * @brief Load all agents and seed built-in templates if the table
     *        is empty. Called once at AppController initialize.
     */
    void initialize();

    // -----------------------------------------------------------------------
    // C++ API
    // -----------------------------------------------------------------------

    /**
     * @brief All registered agents ordered by name.
     * @returns Full agent list.
     */
    QList<Agent> allAgents() const;

    /**
     * @brief Fetch an agent by id.
     * @param id Agent UUID.
     * @returns Agent row, or an invalid Agent (empty id) if not found.
     */
    Agent getAgent(const QString& id) const;

    /**
     * @brief Fetch an agent by name.
     * @param name Agent display name to look up.
     * @returns Agent row, or an invalid Agent if not found.
     */
    Agent getAgentByName(const QString& name) const;

    /**
     * @brief Insert a new agent row.
     * @param agent Populated Agent to insert.
     * @returns The new agent's UUID, or empty string on failure.
     */
    QString createAgent(const Agent& agent);

    /**
     * @brief Update an existing agent.
     * @param agent Agent whose id identifies the row to update.
     * @returns true on success.
     */
    bool updateAgent(const Agent& agent);

    /**
     * @brief Delete an agent by UUID.
     * @param id Agent UUID to delete.
     * @returns true if a row was actually removed.
     */
    bool deleteAgent(const QString& id);

    // -----------------------------------------------------------------------
    // QML API
    // -----------------------------------------------------------------------

    /**
     * @brief Agent list rendered as a QVariantList of maps for QML.
     * @returns List of `{id, name, description, ...}` maps.
     */
    Q_INVOKABLE QVariantList agentList() const;

    /**
     * @brief Create or update an agent from a QML-friendly map. If
     *        'id' is set, updates; otherwise creates.
     * @param map Must contain at least 'name' and 'systemPrompt'.
     * @returns The agent's UUID on success, empty on failure.
     */
    Q_INVOKABLE QString saveAgent(const QVariantMap& map);

    /**
     * @brief QML-facing wrapper around deleteAgent.
     * @param id Agent UUID to remove.
     * @returns true if a row was deleted.
     */
    Q_INVOKABLE bool removeAgent(const QString& id);

    /**
     * @brief Lookup map of an agent's fields for QML.
     * @param id Agent UUID.
     * @returns Map of the agent's fields, or empty map if not found.
     */
    Q_INVOKABLE QVariantMap agentInfo(const QString& id) const;

    /**
     * @brief List of built-in template definitions (not yet in DB).
     *        Used by the Agents page to show a "Templates" gallery
     *        for users who have deleted all defaults.
     * @returns List of template maps.
     */
    Q_INVOKABLE QVariantList builtInTemplates() const;

    /**
     * @brief Create an agent from a built-in template by name.
     * @param templateName Canonical template name to instantiate.
     * @returns The new agent's UUID on success.
     */
    Q_INVOKABLE QString createFromTemplate(const QString& templateName);

    /**
     * @brief Get the id of the built-in agent with @p name. If no
     *        such agent exists in the DB (e.g. user deleted it),
     *        seeds it from the canonical template list and returns
     *        the new id. Used by the Agents page to resolve template
     *        clicks to a stable agent identity without creating
     *        disambiguated copies.
     * @param name Canonical built-in name to resolve.
     * @returns The agent's UUID (existing or freshly seeded).
     */
    Q_INVOKABLE QString getOrCreateBuiltinByName(const QString& name);

  signals:
    /** @brief Emitted whenever the agent registry changes. */
    void agentsChanged();

    /**
     * @brief Emitted when a single agent is deleted, carrying its id so
     *        scope-keyed dependents (e.g. per-agent memory) can purge it.
     * @param id The deleted agent's id.
     */
    void agentDeleted(const QString& id);

  private:
    DbManager& m_db;

    /**
     * @brief Seeds the agents table with built-in templates if empty.
     */
    void seedBuiltInsIfEmpty();

    /**
     * @brief Aggressive one-shot cleanup of built-in agent duplicates.
     *        Removes every "is_built_in = 1" row whose name matches
     *        "<canonical-template> <N>" (e.g. "Assistant 2",
     *        "Researcher 3"). These accumulated under the previous
     *        template-click semantics that auto-disambiguated on
     *        unique-name collision.
     *
     *        FK references are migrated to the canonical row before
     *        the duplicate is deleted, in a single transaction:
     *          - conversations.primary_agent_id   → canonical id
     *          - conversation_members.agent_id    → canonical id
     *            (UPDATE OR IGNORE; rows that would collide on the
     *             (conversation_id, alias) unique constraint are then
     *             dropped, with no data loss because the canonical row
     *             already holds the equivalent membership).
     *          - project_members.agent_id         → canonical id
     *            (same OR IGNORE + sweep).
     *          - heartbeat_configs.agent_id       → canonical id
     *            (same OR IGNORE + sweep).
     *
     *        A rollback fires on any single rebind / delete failure;
     *        partial cleanup is never persisted.
     *
     * @return Number of agent rows deleted (0 on rollback, ≥0 otherwise).
     */
    int pruneOrphanBuiltinDuplicates();

    /**
     * @brief Returns the hard-coded list of built-in agent templates.
     */
    static QList<Agent> builtInAgentTemplates();
};

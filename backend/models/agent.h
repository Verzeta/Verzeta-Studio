// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent.h
 * @brief Data model for a named Agent: a persistent role with a
 *        system prompt, default execution pattern, and optional model/tool
 *        restrictions. Agents may be assigned to conversations (1:1 or group)
 *        and to folders (projects/organizations).
 * @layer Data Model
 * @dependencies Qt6::Core
 */

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * @brief Persistent agent definition.
 *
 * ## Fields
 *  - id: UUID primary key.
 *  - name: unique display name.
 *  - description: short one-line description.
 *  - iconName: KDE icon name (e.g. "face-smile", "user-group-new").
 *  - systemPrompt: the role definition sent to the LLM.
 *  - defaultPattern: agent's default execution strategy
 *    ("direct","react","planner","router","multi_agent","memory").
 *  - modelProvider, modelName: optional; forces a specific model.
 *  - allowedTools: optional whitelist; empty = all enabled tools.
 *  - isBuiltIn: true for seeded templates; user can still edit/delete.
 *  - createdAt: creation timestamp.
 */
struct Agent {
    QString id;                                         ///< UUID primary key.
    QString name;                                       ///< Unique display name.
    QString description;                                ///< Short one-line description.
    QString iconName;                                   ///< KDE icon name.
    QString systemPrompt;                               ///< Role definition sent to the LLM.
    QString defaultPattern = QStringLiteral("direct");  ///< Default execution strategy.
    /// Optional provider override, used in 1:1 and plain chats with
    /// this agent. Project and group chats use the member's own
    /// setting instead. Empty keeps the conversation's provider.
    QString modelProvider;
    QString modelName;         ///< Optional model override; same rules as modelProvider.
    QStringList allowedTools;  ///< Optional tool whitelist; empty allows all enabled tools.
    bool isBuiltIn = false;    ///< True for seeded templates; still editable.
    /** @brief Suitable as a group coordinator (Team Lead, Manager, etc.) */
    bool isCoordinator = false;
    QDateTime createdAt;  ///< Creation time.

    /**
     * @brief Optional pre-fill defaults for
     *        the heartbeat expander when this agent is added to a new
     *        context (per-conv membership or project membership).
     *        An empty default means no pre-fill: the
     *        AddChatMemberDialog starts with blank heartbeat fields.
     *        The source of truth for any actually-running
     *        heartbeat is always the heartbeat_configs row keyed by
     *        (agent, scope, alias); these agents-level fields are
     *        hint-only and never read by the dispatch path.
     */
    QString defaultHeartbeatGoal;
    QString defaultHeartbeatSchedule;         ///< Pre-fill for the heartbeat schedule.
    QString defaultHeartbeatSurfaceCriteria;  ///< Pre-fill for the surface criteria.

    /**
     * @brief Reports whether the row is well-formed enough to persist.
     * @returns True iff `id` and `name` are both non-empty.
     */
    bool isValid() const { return !id.isEmpty() && !name.isEmpty(); }

    /**
     * @brief Serialises the agent template to its JSON wire / UI form.
     * @returns JSON object mirroring the column-name shape.
     */
    QJsonObject toJson() const;

    /**
     * @brief Deserialises an agent template from its JSON wire / UI form.
     * @param json  JSON object previously produced by toJson().
     * @returns Populated Agent record.
     */
    static Agent fromJson(const QJsonObject& json);
};

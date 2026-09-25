// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file member.h
 * @brief Data model for a team/group membership: a single "seat"
 *        occupied by an agent template within a project folder or a group
 *        conversation. Each seat has a unique alias used for \@mention and
 *        display; the same agent template may occupy multiple seats under
 *        different aliases (e.g. two Engineers named Alice and Bob).
 * @layer Data Access
 * @dependencies Qt6::Core
 */

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * @brief Immutable data model for a project or conversation membership row.
 *
 * Used for both `project_members` (where containerId is a folder UUID) and
 * `conversation_members` (where containerId is a conversation UUID). The
 * model is intentionally polymorphic: a single Member struct represents
 * a row in either table.
 */
struct Member {
    QString containerId;         ///< Folder UUID (project) or Conversation UUID (group)
    QString agentId;             ///< References agents.id (the template)
    QString alias;               ///< \@mention name, unique per container
    bool isCoordinator = false;  ///< True when this member coordinates the group.
    QDateTime joinedAt;          ///< When the member was added.

    /**
     * @brief Who added this row (schema v12).
     *        "user" = added via the QML/Android UI surfaces (default for
     *                  every pre-existing row via the v12 migration backfill).
     *        "agent" = added by an agent tool call (add_project_member /
     *                  future conversation-membership tool). The agent
     *                  tool body sets this to "agent" + populates
     *                  addedByAgentId with the calling agent's id.
     *
     *        The remove_project_member agent tool only allows removal
     *        of `addedByKind == "agent"` rows. User-added members are
     *        protected from agent-driven removal. Removing a user-added
     *        member requires explicit user action via the QML/Android UI
     *        (which goes through MembershipService::removeProjectMember
     *        directly, not the tool).
     */
    QString addedByKind = QStringLiteral("user");

    /**
     * @brief Agent id that added this row (schema v12),
     *        populated when addedByKind == "agent".
     *        Empty for user-added rows. Used for audit + future per-
     *        agent removal-scope policies.
     */
    QString addedByAgentId;

    /**
     * @brief Per-member model override.
     *
     *        modelProvider / modelName: when non-empty, this member
     *        entity dispatches its turns through this provider/model
     *        instead of the conversation default. allowedTools: when
     *        non-empty, the member's turns are restricted to this tool
     *        whitelist (intersected with task-tool gating). All three
     *        empty = no override; the member inherits the conversation
     *        default. RequestBuilder reads these per member-scoped turn.
     *
     *        These are seeded from the agent template at add-time (the
     *        UI pre-fills from the template's defaults) but are then
     *        independent of the template: editing the template never
     *        reaches back into existing member rows. The member entity
     *        (keyed (folder_id, alias) for a project or
     *        (conversation_id, alias) for a non-project group chat) is
     *        the source of truth at request time, NOT the template.
     */
    QString modelProvider;
    QString modelName;         ///< Model override; see modelProvider.
    QStringList allowedTools;  ///< Tool whitelist override; see modelProvider.

    // Denormalized fields filled by the service when resolving — agent template
    // details so the UI can display icon/description without a second lookup.
    QString agentName;         ///< Source agent template name
    QString agentDescription;  ///< Source agent template description
    QString agentIconName;     ///< Source agent template icon

    /**
     * @brief Reports whether the row is well-formed enough to persist.
     * @returns True iff `containerId`, `agentId`, and `alias` are all set.
     */
    bool isValid() const {
        return !containerId.isEmpty() && !agentId.isEmpty() && !alias.isEmpty();
    }

    /**
     * @brief Serialises the membership row to its JSON wire / UI form.
     * @returns JSON object mirroring the column-name shape, plus the
     *          denormalised agent template fields when populated.
     */
    QJsonObject toJson() const;
};

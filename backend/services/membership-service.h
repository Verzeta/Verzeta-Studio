// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file membership-service.h
 * @brief CRUD service for project_members and conversation_members
 *        tables. Manages aliased agent memberships, so the same agent
 *        template can be added multiple times under different
 *        aliases. Exposed to QML as the `MembershipService` context
 *        property.
 * @layer Service
 * @dependencies DbManager
 */

#pragma once

#include "../models/member.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class AuditService;
class DbManager;

/**
 * @brief Read / write API for project and conversation memberships.
 *
 * Container type is determined by the method called: project_* methods
 * target the project_members table, conversation_* methods target
 * conversation_members. Both tables share the same row structure.
 */
class MembershipService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the membership service.
     * @param db     Non-owning reference to DbManager.
     * @param parent Qt parent (AppController).
     */
    explicit MembershipService(DbManager& db, QObject* parent = nullptr);
    ~MembershipService() override;

    /**
     * @brief Attach an AuditService so member_added / member_removed
     *        events are recorded to the activity_log table.
     *        Non-owning; AppController wires this once during
     *        initialize().
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setAuditService(AuditService* svc);

    /**
     * @brief Canonical alias normalization applied to EVERY membership
     *        write and alias-keyed lookup: trims whitespace and strips
     *        any leading `@` sigils. Aliases are stored WITHOUT the
     *        sigil. Display sites prepend `@` and the mention parser
     *        extracts handles without it, so a stored `@Engineer`
     *        renders as `@@Engineer` and can never be mention-routed.
     *        Callers (tools, dialogs) may pass sigiled input; the
     *        service guarantees clean storage and sigil-tolerant
     *        matching.
     * @param alias Raw alias input (may carry leading `@`s / spaces).
     * @returns The trimmed, sigil-free alias; empty when nothing
     *          remains (callers treat that as invalid).
     */
    Q_INVOKABLE static QString normalizedAlias(const QString& alias);

    // -----------------------------------------------------------------------
    // C++ API — Project memberships
    // -----------------------------------------------------------------------

    /**
     * @brief All members of a project folder, ordered by joinedAt
     *        ASC.
     * @param folderId Project folder UUID.
     * @returns Member rows in joined-at order; empty when the folder
     *          has no members.
     */
    QList<Member> projectMembers(const QString& folderId) const;

    /**
     * @brief Adds a new member to a project.
     * @param folderId      Project folder UUID.
     * @param agentId       Agent template UUID.
     * @param alias         Unique alias within the project.
     * @param isCoordinator Whether the member is a coordinator.
     * @param addedByKind   "user" (default, all
     *                      existing call sites) or "agent" (the
     *                      add_project_member tool body). Drives the
     *                      remove_project_member tool's safety gate
     *                      (agent-driven removal is rejected for
     *                      "user"-added rows).
     * @param addedByAgentId Agent UUID populated
     *                       when addedByKind == "agent"; empty for
     *                       user-added rows.
     * @param modelProvider Per-member provider
     *                       override; empty = inherit conversation
     *                       default.
     * @param modelName     Per-member model override.
     * @param allowedTools  Per-member tool whitelist;
     *                       empty = no restriction.
     * @return true on success. Fails if the alias is already taken.
     */
    bool addProjectMember(const QString& folderId,
                          const QString& agentId,
                          const QString& alias,
                          bool isCoordinator = false,
                          const QString& addedByKind = QStringLiteral("user"),
                          const QString& addedByAgentId = {},
                          const QString& modelProvider = {},
                          const QString& modelName = {},
                          const QStringList& allowedTools = {});

    /**
     * @brief Removes a project member by alias.
     * @param folderId Project folder UUID.
     * @param alias    Alias of the member to remove.
     * @returns true on success; false on unknown folder / alias.
     */
    bool removeProjectMember(const QString& folderId, const QString& alias);

    /**
     * @brief Replaces all members of a project atomically.
     * @param folderId Folder UUID.
     * @param members  New membership list. Existing rows are deleted first.
     * @return true on success.
     */
    bool setProjectMembers(const QString& folderId, const QList<Member>& members);

    // -----------------------------------------------------------------------
    // C++ API — Conversation (group) memberships
    // -----------------------------------------------------------------------

    /**
     * @brief All members of a group conversation, ordered by
     *        joinedAt ASC.
     * @param conversationId Conversation UUID.
     * @returns Member rows in joined-at order; empty when the
     *          conversation has no members.
     */
    QList<Member> conversationMembers(const QString& conversationId) const;

    /**
     * @brief Add a member to a group conversation.
     * @param conversationId  Conversation UUID.
     * @param agentId         Agent template UUID.
     * @param alias           Unique alias within the conversation.
     * @param isCoordinator   Whether the member is the coordinator.
     * @param addedByKind     `"user"` or `"agent"`; drives the
     *                        remove tool's safety gate.
     * @param addedByAgentId  Agent UUID populated when addedByKind ==
     *                        "agent"; empty for user-added rows.
     * @param modelProvider   Per-member provider override; empty =
     *                        inherit conversation default.
     * @param modelName       Per-member model override.
     * @param allowedTools    Per-member tool whitelist; empty = no
     *                        restriction.
     * @returns true on success. Fails if the alias is already taken.
     */
    bool addConversationMember(const QString& conversationId,
                               const QString& agentId,
                               const QString& alias,
                               bool isCoordinator = false,
                               const QString& addedByKind = QStringLiteral("user"),
                               const QString& addedByAgentId = {},
                               const QString& modelProvider = {},
                               const QString& modelName = {},
                               const QStringList& allowedTools = {});

    /**
     * @brief Remove a member from a group conversation by alias.
     * @param conversationId Conversation UUID.
     * @param alias          Alias of the member to remove.
     * @returns true on success; false on unknown conversation /
     *          alias.
     */
    bool removeConversationMember(const QString& conversationId, const QString& alias);

    /**
     * @brief Replace all members of a conversation atomically.
     * @param conversationId Conversation UUID.
     * @param members        New membership list. Existing rows are
     *                       deleted first.
     * @returns true on success.
     */
    bool setConversationMembers(const QString& conversationId, const QList<Member>& members);

    /**
     * @brief Look up a member by alias (case-insensitive).
     * @param conversationId Conversation UUID.
     * @param alias          Alias to look up.
     * @returns Valid Member if found, invalid Member otherwise.
     */
    Member findConversationMemberByAlias(const QString& conversationId, const QString& alias) const;

    /**
     * @brief The default responder for a group conversation:
     *        coordinator first, then the first member, then invalid.
     * @param conversationId Conversation UUID.
     * @returns Member row, or invalid Member when the conversation
     *          has no members.
     */
    Member defaultResponderForConversation(const QString& conversationId) const;

    /**
     * @brief Set the coordinator for a conversation (clears any
     *        other coordinator flag on the roster).
     * @param conversationId Conversation UUID.
     * @param alias          Alias of the new coordinator.
     * @returns true on success; false on unknown conversation /
     *          alias.
     */
    bool setConversationCoordinator(const QString& conversationId, const QString& alias);

    // -----------------------------------------------------------------------
    // QML API
    // -----------------------------------------------------------------------

    /**
     * @brief QML-friendly variant-list of project members.
     * @param folderId Project folder UUID.
     * @returns QVariantList of member projection maps.
     */
    Q_INVOKABLE QVariantList projectMembersList(const QString& folderId) const;

    /**
     * @brief QML-friendly variant-list of conversation members.
     * @param conversationId Conversation UUID.
     * @returns QVariantList of member projection maps.
     */
    Q_INVOKABLE QVariantList conversationMembersList(const QString& conversationId) const;

    /**
     * @brief Adds a project member from a QVariantMap.
     * @param map Must contain: folderId, agentId, alias; optional
     *            isCoordinator.
     * @returns true on success.
     */
    Q_INVOKABLE bool addProjectMemberMap(const QVariantMap& map);

    /**
     * @brief QML-friendly alias for removeProjectMember.
     * @param folderId Project folder UUID.
     * @param alias    Member alias to remove.
     * @returns true on success.
     */
    Q_INVOKABLE bool removeProjectMemberAlias(const QString& folderId, const QString& alias);

    /**
     * @brief Replace a project's member list from a QVariantList of
     *        member-shaped maps.
     * @param folderId   Project folder UUID.
     * @param memberMaps List of member projection maps.
     * @returns true on success.
     */
    Q_INVOKABLE bool setProjectMembersFromList(const QString& folderId,
                                               const QVariantList& memberMaps);

    /**
     * @brief Adds a conversation member from a QVariantMap.
     * @param map Must contain: conversationId, agentId, alias;
     *            optional isCoordinator.
     * @returns true on success.
     */
    Q_INVOKABLE bool addConversationMemberMap(const QVariantMap& map);

    /**
     * @brief QML-friendly alias for removeConversationMember.
     * @param conversationId Conversation UUID.
     * @param alias          Member alias to remove.
     * @returns true on success.
     */
    Q_INVOKABLE bool removeConversationMemberAlias(const QString& conversationId,
                                                   const QString& alias);

    /**
     * @brief Replace a conversation's member list from a
     *        QVariantList of member-shaped maps.
     * @param conversationId Conversation UUID.
     * @param memberMaps     List of member projection maps.
     * @returns true on success.
     */
    Q_INVOKABLE bool setConversationMembersFromList(const QString& conversationId,
                                                    const QVariantList& memberMaps);

    /**
     * @brief QML-friendly alias for setConversationCoordinator.
     * @param conversationId Conversation UUID.
     * @param alias          Alias of the new coordinator.
     * @returns true on success.
     */
    Q_INVOKABLE bool setConversationCoordinatorAlias(const QString& conversationId,
                                                     const QString& alias);

    // -----------------------------------------------------------------------
    // Per-member model override (incremental edit)
    // -----------------------------------------------------------------------

    /**
     * @brief Updates ONLY the model-override columns of one project
     *        member, keyed by (folderId, alias). Incremental UPDATE
     *        that does NOT churn the rest of the roster the way
     *        setProjectMembers (delete-all-reinsert) would. This is
     *        the path the per-member editor in FolderSettingsDialog
     *        and a project chat's Conversation Settings use to
     *        retune one member's provider/model/tools.
     * @param folderId      Project folder UUID.
     * @param alias         Member alias (the entity key within the
     *                      folder).
     * @param modelProvider Provider override; empty clears it.
     * @param modelName     Model override; empty clears it.
     * @param allowedTools  Tool whitelist; empty clears it (no
     *                      restriction).
     * @returns true if a row matched and was updated.
     */
    Q_INVOKABLE bool updateProjectMemberModelOverride(const QString& folderId,
                                                      const QString& alias,
                                                      const QString& modelProvider,
                                                      const QString& modelName,
                                                      const QStringList& allowedTools);

    /**
     * @brief Conversation-members counterpart of
     *        updateProjectMemberModelOverride, keyed by
     *        (conversationId, alias). Used by a non-project group
     *        chat's Conversation Settings.
     * @param conversationId Conversation UUID.
     * @param alias          Member alias.
     * @param modelProvider  Provider override; empty clears it.
     * @param modelName      Model override; empty clears it.
     * @param allowedTools   Tool whitelist; empty clears it.
     * @returns true if a row matched and was updated.
     */
    Q_INVOKABLE bool updateConversationMemberModelOverride(const QString& conversationId,
                                                           const QString& alias,
                                                           const QString& modelProvider,
                                                           const QString& modelName,
                                                           const QStringList& allowedTools);

  signals:
    /**
     * @brief Emitted when a project's member list changes.
     * @param folderId Project folder UUID whose roster mutated.
     */
    void projectMembersChanged(const QString& folderId);

    /**
     * @brief Emitted when a conversation's member list changes.
     * @param conversationId Conversation UUID whose roster mutated.
     */
    void conversationMembersChanged(const QString& conversationId);

  private:
    DbManager& m_db;
    AuditService* m_auditService = nullptr;  // non-owning; optional

    QList<Member>
    loadMembers(const QString& table, const QString& idColumn, const QString& containerId) const;
    bool
    deleteAllMembers(const QString& table, const QString& idColumn, const QString& containerId);

    /**
     * @brief Single INSERT path shared by addProjectMember /
     *        addConversationMember / setProjectMembers /
     *        setConversationMembers. Four previously near-identical
     *        INSERT sites became this one, so a new column is a
     *        single-point change with no risk of the four drifting out
     *        of sync.
     *        `table` / `idColumn` are internal constants, never user
     *        input; the `.arg()` interpolation matches loadMembers' style
     *        and is injection-safe.
     * @param table      "project_members" | "conversation_members".
     * @param idColumn   "folder_id" | "conversation_id".
     * @param m          Fully-populated member row (containerId set).
     * @param joinedAtMs Resolved joined-at timestamp (caller decides
     *                   currentMSecsSinceEpoch vs the member's own).
     * @return true if the INSERT succeeded.
     */
    bool insertMemberRow(const QString& table,
                         const QString& idColumn,
                         const Member& m,
                         qint64 joinedAtMs);

    /**
     * @brief One-time self-heal run at construction: strips leading `@`
     *        sigils from any alias already persisted in project_members
     *        / conversation_members (rows written before normalization
     *        existed, e.g. an agent tool call that passed
     *        alias="@Engineer"). Idempotent (matches only rows LIKE
     *        '@%'); a row whose cleaned alias would collide with an
     *        existing member in the same container is left untouched
     *        and logged rather than silently merged.
     */
    void normalizeLegacyAliases();
};

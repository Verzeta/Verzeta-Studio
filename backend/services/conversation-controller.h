// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-controller.h
 * @brief QML-exposed controller that owns conversation + folder +
 *        group-conversation CRUD. Registered as the `Conversations`
 *        QML singleton by AppController, in parallel with
 *        `ChatController` (the reduced chat-session controller).
 * @layer Service (UI orchestration)
 * @dependencies ConversationService, ModelRouter (non-owning refs);
 *               FileService, MembershipService (non-owning pointers,
 *               attached via setters; AppController wires them after
 *               construction).
 *
 * Star-topology refusal: this controller does NOT hold a ChatController
 * pointer. Cross-controller coordination (e.g. cancelling an in-flight
 * request when the active conversation is deleted, clearing the
 * active-conversation label when a conversation is renamed) happens
 * via signals. Conversations emits `conversationDeleted(id)`,
 * `conversationRenamed(id, title)`, etc.; ChatController subscribes
 * directly to the signals it cares about. No hub hops.
 *
 * Threading: strictly main-thread. Every public method asserts via
 * VERZETA_ASSERT_MAIN_THREAD().
 *
 * Ownership: constructed as `std::unique_ptr<ConversationController>`
 * on AppController. AppController is the QObject parent. QML holds a
 * non-owning singleton pointer via `qmlRegisterSingletonInstance`; the
 * pointer stays valid until AppController destructs.
 */


#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class ConversationService;
class FileService;
class MembershipService;
class ModelRouter;

/**
 * @brief QML singleton for conversation + folder + group CRUD.
 */
class ConversationController : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the conversation controller.
     * @param convSvc Non-owning reference; must outlive this object.
     *                Every folder / group / CRUD method delegates to
     *                ConversationService.
     * @param router  Non-owning reference; used to stamp the active
     *                provider/model into the llm_config of newly
     *                created conversations.
     * @param parent  Qt parent (AppController). Backup destruction
     *                via the parent-chain behind the std::unique_ptr
     *                member cleanup.
     */
    explicit ConversationController(ConversationService& convSvc,
                                    ModelRouter& router,
                                    QObject* parent = nullptr);
    ~ConversationController() override;

    /**
     * @brief Attach an optional filesystem gateway used by the
     *        project-document methods. Null disables those methods,
     *        which then return empty / false.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setFileService(FileService* svc);

    /**
     * @brief Attach an optional membership service used by
     *        group-creation and roster queries. Null disables group
     *        creation; 1:1 lookups still work.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setMembershipService(MembershipService* svc);

    // ---------------------------------------------------------------
    // Q_INVOKABLE — Conversation CRUD
    // ---------------------------------------------------------------

    /**
     * @brief Creates a new conversation with the given title.
     * @param title Display title for the conversation row.
     * @returns UUID of the created conversation, or empty string on
     *          failure (errorOccurred emitted on failure).
     * @sideeffects Inserts a conversations row; stamps the active
     *              provider/model into llm_config when one is set;
     *              emits conversationCreated(id) on success.
     */
    Q_INVOKABLE QString newConversation(const QString& title = QStringLiteral("New Chat"));

    /**
     * @brief Creates a new root-level 1:1 conversation pinned to a
     *        specific agent. The conversation's primary_agent_id is
     *        set so subsequent turns route to that agent.
     * @param agentId Agent UUID to pin as the conversation's primary.
     * @param title   Optional display title (defaults to the agent's
     *                name when empty).
     * @returns UUID of the created conversation, or empty on failure.
     */
    Q_INVOKABLE QString newConversationWithAgent(const QString& agentId, const QString& title = {});

    /**
     * @brief Deletes a conversation.
     * @param id Conversation UUID to delete.
     * @sideeffects Calls ConversationService::deleteConversation.
     *              Emits conversationDeleted(id) on success so
     *              ChatController can clear its active state if the
     *              deleted conversation was active.
     */
    Q_INVOKABLE void deleteConversation(const QString& id);

    /**
     * @brief Renames a conversation.
     * @param id    Conversation UUID to rename.
     * @param title New display title.
     * @returns true on success; false + errorOccurred on failure.
     * @sideeffects Emits conversationRenamed(id, newTitle) on success
     *              so ChatController can update its cached title.
     */
    Q_INVOKABLE bool renameConversation(const QString& id, const QString& title);

    /**
     * @brief Deletes every conversation in the database.
     * @sideeffects Emits conversationDeleted(id) for each removed
     *              conversation (via ConversationService row-level
     *              signals propagating through this controller).
     */
    Q_INVOKABLE void clearAllConversations();

    // ---------------------------------------------------------------
    // Q_INVOKABLE — Folder CRUD
    // ---------------------------------------------------------------

    /**
     * @brief Create a folder.
     * @param name Display name for the new folder.
     * @returns UUID of the new folder, or empty string on failure.
     */
    Q_INVOKABLE QString createFolder(const QString& name);

    /**
     * @brief Move a conversation into a folder.
     * @param convId   Conversation UUID to relocate.
     * @param folderId Destination folder UUID; empty means root.
     * @returns true on success; false + errorOccurred on failure.
     */
    Q_INVOKABLE bool moveToFolder(const QString& convId, const QString& folderId);

    /**
     * @brief Folder metadata for QML editing.
     * @param folderId Folder UUID to inspect.
     * @returns QVariantMap with keys {id, name, parentId, folderType,
     *          goal, description, agentIds}, or an empty map if the
     *          folder is absent.
     */
    Q_INVOKABLE QVariantMap folderInfo(const QString& folderId) const;

    /**
     * @brief Sets a project/organization folder's ACN (team-memory) master
     *        switch. Surfaced to the Folder Settings dialog.
     * @param folderId Folder UUID.
     * @param enabled  true to enable team memory for this project/org.
     * @returns true on success.
     */
    Q_INVOKABLE bool setFolderAcnEnabled(const QString& folderId, bool enabled);

    /**
     * @brief Update the project/organization metadata fields on a
     *        folder. The plain `name` field is updated separately via
     *        `renameFolder`.
     * @param folderId   Folder UUID to update.
     * @param folderType One of `"project"` / `"organization"` /
     *                   `"folder"`.
     * @param goal       Project/org goal text.
     * @param description Project/org description text.
     * @param agentIds   Roster of agent UUIDs available in the folder.
     * @returns true on success; false + errorOccurred on failure.
     */
    Q_INVOKABLE bool updateFolderMetadata(const QString& folderId,
                                          const QString& folderType,
                                          const QString& goal,
                                          const QString& description,
                                          const QStringList& agentIds);

    /**
     * @brief Rename a folder.
     * @param folderId Folder UUID to rename.
     * @param newName  New display name.
     * @returns true on success; false + errorOccurred on failure.
     */
    Q_INVOKABLE bool renameFolder(const QString& folderId, const QString& newName);

    /**
     * @brief Delete a folder and clear the folder reference from any
     *        conversations that lived inside it (those conversations
     *        move to root, they are not deleted).
     * @param folderId Folder UUID to delete.
     * @returns true on success; false + errorOccurred on failure.
     */
    Q_INVOKABLE bool deleteFolder(const QString& folderId);

    /**
     * @brief Walk every folder, returning the project/organization
     *        ones.
     * @returns QVariantList of {id, name, type} maps; empty when no
     *          project / organization folders exist.
     */
    Q_INVOKABLE QVariantList listProjectFolders() const;

    // ---------------------------------------------------------------
    // Q_INVOKABLE — Project documents (delegate to FileService)
    // ---------------------------------------------------------------

    /**
     * @brief List documents attached to a project folder.
     * @param folderId Folder UUID to inspect.
     * @returns QVariantList of document metadata maps; empty when the
     *          folder has no documents or FileService is not attached.
     */
    Q_INVOKABLE QVariantList projectDocuments(const QString& folderId) const;

    /**
     * @brief Import a file as a project document.
     * @param folderId   Destination project folder UUID.
     * @param sourcePath Path to the source file on disk. May carry a
     *                   `"file://"` URL prefix (stripped internally).
     * @returns Stored filename on success; empty on failure (file
     *          missing, FileService not attached, etc.).
     */
    Q_INVOKABLE QString addProjectDocument(const QString& folderId, const QString& sourcePath);

    /**
     * @brief Remove a project document by filename.
     * @param folderId Folder UUID holding the document.
     * @param fileName Name of the document to remove.
     * @returns true on success; false when the document is absent or
     *          FileService is not attached.
     */
    Q_INVOKABLE bool removeProjectDocument(const QString& folderId, const QString& fileName);

    // ---------------------------------------------------------------
    // Q_INVOKABLE — Group conversations
    // ---------------------------------------------------------------

    /**
     * @brief Creates a new group conversation and populates its
     *        membership table.
     * @param title    Display title for the group.
     * @param members  QVariantList of member descriptors (each carries
     *                 agentId + alias + isCoordinator).
     * @param folderId Optional folder UUID to nest the group under;
     *                 empty means root.
     * @returns UUID of the new group on success; empty + errorOccurred
     *          on failure. Does NOT switch active conversation;
     *          callers (QML or Chat) decide what to do after.
     */
    Q_INVOKABLE QString newGroupConversation(const QString& title,
                                             const QVariantList& members,
                                             const QString& folderId = {});

    /**
     * @brief Reuse-or-create a 1:1 chat with a specific project member.
     * @param folderId Project folder the member belongs to.
     * @param agentId  Agent UUID for the member.
     * @param alias    Project-scoped alias for the member.
     * @returns UUID of the (possibly reused) conversation, or empty
     *          on failure.
     */
    Q_INVOKABLE QString openDirectChatWithMember(const QString& folderId,
                                                 const QString& agentId,
                                                 const QString& alias);

    /**
     * @brief Fan out: create a 1:1 chat for every project member that
     *        doesn't already have one. Does NOT switch active.
     * @param folderId Project folder whose roster to fan out across.
     * @returns Number of conversations created.
     */
    Q_INVOKABLE int createIndividualChatsForProject(const QString& folderId);

    /**
     * @brief Create a single group conversation containing every
     *        project member.
     * @param folderId Project folder whose roster becomes the group.
     * @returns UUID of the new group on success; empty +
     *          errorOccurred on failure.
     */
    Q_INVOKABLE QString createGroupChatForProject(const QString& folderId);

    // ---------------------------------------------------------------
    // Q_INVOKABLE — Parameterised queries (replace ChatController's
    // "active*" convenience variants — QML call sites pass the active
    // conversation id explicitly).
    // ---------------------------------------------------------------

    /**
     * @brief Test whether a conversation is a group chat.
     * @param convId Conversation UUID to inspect.
     * @returns true iff `is_group=1` on the conversation row.
     */
    Q_INVOKABLE bool isGroup(const QString& convId) const;

    /**
     * @brief Return the member roster for a group conversation.
     * @param convId Conversation UUID to inspect.
     * @returns QVariantList of member descriptor maps; empty for
     *          non-group conversations.
     */
    Q_INVOKABLE QVariantList groupMembers(const QString& convId) const;

    /**
     * @brief Folder UUID for a conversation, or empty when the
     *        conversation lives at the root. Used by
     *        RightSettingsPanel to decide whether the
     *        "Override Project / Organization Preferred Skills List"
     *        checkbox should be visible.
     * @param convId Conversation UUID.
     * @returns Folder UUID, or empty for root-level conversations or
     *          unknown ids.
     */
    Q_INVOKABLE QString folderIdOf(const QString& convId) const;

    /**
     * @brief The project-member alias a 1:1 direct chat is the channel
     *        for, or an empty string for group / plain / non-project
     *        conversations. Stamped at direct-chat creation by
     *        openDirectChatWithMember / createIndividualChatsForProject.
     *        RightSettingsPanel uses it to resolve which
     *        `project_members` row a project 1:1 chat's per-member
     *        override editor should write to (the same row
     *        RequestBuilder resolves at request time).
     * @param convId Conversation UUID.
     * @returns Member alias, or empty when the conversation is not a
     *          project 1:1 chat.
     */
    Q_INVOKABLE QString conversationMemberAlias(const QString& convId) const;

    /**
     * @brief List conversations inside a folder, for the heartbeat
     *        target-conv picker. Project-scope configs post into a
     *        designated conversation when auto-surface fires.
     * @param folderId Folder UUID. Empty returns root-level
     *                 conversations (`folder_id IS NULL`).
     * @returns QVariantList of conversation dicts:
     *          {id, title, isGroup, updatedAtMs}.
     */
    Q_INVOKABLE QVariantList conversationsInFolder(const QString& folderId);

    // ---------------------------------------------------------------
    // Per-conversation heartbeat auto-surface gate.
    // The gate is read/written via the existing llm_config JSON column;
    // these wrappers give QML a typed surface so RightSettingsPanel
    // doesn't have to know LlmConfig's full shape.
    // ---------------------------------------------------------------

    /**
     * @brief Returns the per-conversation auto-surface gate.
     * @param convId Conversation UUID.
     * @returns true iff the gate is ON. Returns false for unknown ids
     *          and for conversations that have never had it set.
     */
    Q_INVOKABLE bool heartbeatAutoSurface(const QString& convId) const;

    /**
     * @brief Returns the per-conversation daily cap on auto-surface posts.
     * @param convId Conversation UUID.
     * @returns The cap stored on the conversation, or 1 if unset.
     */
    Q_INVOKABLE int heartbeatAutoSurfaceMaxPerDay(const QString& convId) const;

    /**
     * @brief Sets both heartbeat-gate fields atomically.
     * @param convId    Conversation UUID.
     * @param allow     New value for the gate.
     * @param maxPerDay New value for the daily cap (clamped to >= 1).
     * @returns true on success.
     */
    Q_INVOKABLE bool setHeartbeatAutoSurface(const QString& convId, bool allow, int maxPerDay);

  signals:
    /**
     * @brief Emitted after a successful newConversation /
     *        group-creation / direct-chat creation. ChatController
     *        subscribes so it can auto-select the new conversation
     *        when that's the caller's intent. That is handled by Chat's
     *        slot, not this controller.
     * @param id UUID of the newly-created conversation.
     */
    void conversationCreated(const QString& id);

    /**
     * @brief Emitted BEFORE the conversation row is removed from the
     *        database. Gives subscribers (notably ChatController) a
     *        chance to cancel any in-flight work targeting the row so
     *        the cancel path doesn't hit an already-deleted FK. Fired
     *        once per conversation on both direct deletions and
     *        clearAll.
     * @param id UUID of the conversation about to be deleted.
     */
    void conversationAboutToBeDeleted(const QString& id);

    /**
     * @brief Emitted after a successful deleteConversation.
     *        ChatController subscribes to clear active-conversation
     *        state if the deleted id matches the currently active
     *        one.
     * @param id UUID of the deleted conversation.
     */
    void conversationDeleted(const QString& id);

    /**
     * @brief Emitted after a successful renameConversation.
     *        ChatController subscribes to update its cached
     *        m_activeConvTitle when the renamed id matches the
     *        active one.
     * @param id       UUID of the renamed conversation.
     * @param newTitle New display title.
     */
    void conversationRenamed(const QString& id, const QString& newTitle);

    /**
     * @brief Emitted when a user-facing operation fails. QML binds to
     *        this for the error banner specific to Conversations
     *        operations.
     * @param message Human-readable error description.
     */
    void errorOccurred(const QString& message);

  private:
    ConversationService& m_convSvc;
    ModelRouter& m_router;
    FileService* m_fileSvc = nullptr;
    MembershipService* m_membership = nullptr;
};

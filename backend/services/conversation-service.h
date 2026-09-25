// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-service.h
 * @brief Business logic for conversation and folder CRUD operations.
 *        Provides the primary interface for creating, listing, and managing
 *        conversations and their folder groupings.
 * @layer Service
 * @dependencies DbManager (Data Access)
 */

#pragma once

#include "../models/conversation.h"
#include "../models/db-manager.h"
#include "../models/llm-config.h"

#include <optional>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// Forward declarations
class QSqlRecord;

/**
 * @brief Lightweight folder data model for conversation grouping.
 *
 * Supports arbitrary nesting (parent_id references another Folder).
 * Root-level folders have an empty parentId.
 *
 * Schema v2+: folders may optionally be marked as a Project or an Organization,
 * in which case the `goal`, `description`, and `agentIds` fields become the
 * shared context that's injected into the system prompt of any conversation
 * inside that folder.
 */
struct Folder {
    QString id;           ///< UUID v4 primary key
    QString name;         ///< Display name
    QString parentId;     ///< References folders.id; empty if top-level
    QDateTime createdAt;  ///< Creation time.

    /** @brief "regular" | "project" | "organization" (default: "regular") */
    QString folderType = QStringLiteral("regular");

    /** @brief High-level goal (project/organization only). */
    QString goal;

    /** @brief Longer description (project/organization only). */
    QString description;

    /** @brief IDs of agents assigned to this project/organization. */
    QStringList agentIds;

    /**
     * @brief Serialize the folder as a JSON object.
     * @returns JSON representation of every public field.
     */
    QJsonObject toJson() const;

    /**
     * @brief Construct a Folder from its JSON representation.
     * @param json JSON object produced by toJson().
     * @returns Reconstructed Folder; isValid() == false on invalid
     *          input.
     */
    static Folder fromJson(const QJsonObject& json);

    /**
     * @brief Construct a Folder from a folders-table SQL row.
     * @param record SQL record produced by a SELECT on the folders
     *               table.
     * @returns Reconstructed Folder.
     */
    static Folder fromSqlRecord(const QSqlRecord& record);

    /**
     * @brief Whether id + name are populated.
     * @returns true iff the folder has both an id and a name.
     */
    bool isValid() const { return !id.isEmpty() && !name.isEmpty(); }

    /**
     * @brief Whether the folder is a project or organization.
     * @returns true iff folderType is "project" or "organization".
     */
    bool isProject() const {
        return folderType == QStringLiteral("project") ||
               folderType == QStringLiteral("organization");
    }
};

/**
 * @brief Service for conversation and folder management.
 *
 * All write operations emit the appropriate signal on success. The service
 * does not cache in memory; all reads go through the database. Callers
 * relying on UI models should listen to signals to invalidate cached views.
 *
 * Thread safety: Designed for use on the main thread only. Database access
 * through DbManager uses the same named connection established at startup.
 */
class ConversationService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the service with a reference to the database manager.
     * @param db Reference to the open DbManager singleton.
     * @param parent Optional QObject parent for memory management.
     */
    explicit ConversationService(DbManager& db, QObject* parent = nullptr);

    // -----------------------------------------------------------------------
    // Conversation operations
    // -----------------------------------------------------------------------

    /**
     * @brief Creates a new conversation with a generated UUID.
     * @param title Display title for the conversation.
     * @param folderId Optional folder UUID. Empty string places in root.
     * @return UUID of the created conversation, or empty string on failure.
     * @sideeffects Inserts row into conversations table, emits conversationCreated.
     */
    QString createConversation(const QString& title, const QString& folderId = {});

    /**
     * @brief Creates a new group conversation with a set of member agents.
     * @param title Display title.
     * @param agentIds Member agent UUIDs (at least one).
     * @param folderId Optional parent folder.
     * @return UUID of the created conversation, or empty on failure.
     */
    QString createGroupConversation(const QString& title,
                                    const QStringList& agentIds,
                                    const QString& folderId = {});

    /**
     * @brief Updates the group member list for an existing conversation.
     * @param convId Conversation UUID.
     * @param agentIds New member agent UUIDs.
     * @return true on success.
     */
    bool updateGroupAgents(const QString& convId, const QStringList& agentIds);

    /**
     * @brief Deletes a conversation and all its messages (cascade delete).
     * @param id UUID of the conversation to delete.
     * @return true if deletion succeeded.
     * @sideeffects Removes conversation + all child messages/attachments/tool_calls
     *              via ON DELETE CASCADE. Emits conversationDeleted on success.
     */
    bool deleteConversation(const QString& id);

    /**
     * @brief Renames a conversation.
     * @param id UUID of the conversation.
     * @param title New display title (must be non-empty).
     * @return true if update succeeded.
     * @sideeffects Updates title and updated_at in conversations table.
     *              Emits conversationUpdated on success.
     */
    bool renameConversation(const QString& id, const QString& title);

    /**
     * @brief Moves a conversation to a different folder.
     * @param convId UUID of the conversation.
     * @param folderId UUID of the target folder, or empty string for root.
     * @return true if update succeeded.
     * @sideeffects Updates folder_id in conversations table.
     *              Emits conversationUpdated on success.
     */
    bool moveToFolder(const QString& convId, const QString& folderId);

    /**
     * @brief Wipes the conversation's auxiliary content for the /flashmemory
     *        command: canvas_artifacts rows and the
     *        token_total counter. Message deletion is MessageService's
     *        job; the summary row is ConversationSummarizer's. The
     *        conversation row (title, members, settings) is preserved.
     * @param convId Conversation UUID.
     * @returns true when both statements executed.
     */
    bool clearAuxiliaryContent(const QString& convId);

    /**
     * @brief Lists conversations, optionally filtered by folder.
     * @param folderId If non-empty, returns conversations in that folder only.
     *                 If empty, returns all root-level conversations (folder_id IS NULL).
     * @return List of Conversation structs ordered by updated_at DESC.
     * @complexity O(n) where n is the number of conversations in the folder.
     */
    QList<Conversation> listConversations(const QString& folderId = {});

    /**
     * @brief Lists all conversations across all folders.
     * @return List of all Conversation structs ordered by updated_at DESC.
     * @complexity O(n) where n is total conversation count.
     */
    QList<Conversation> listAllConversations();

    /**
     * @brief Retrieves a single conversation by ID.
     * @param id UUID of the conversation.
     * @return The conversation wrapped in std::optional, or std::nullopt if not found.
     * @complexity O(1) (primary key lookup).
     */
    std::optional<Conversation> getConversation(const QString& id);

    /**
     * @brief Updates the LLM configuration stored on a conversation.
     * @param id UUID of the conversation.
     * @param config New LlmConfig to store as JSON.
     * @return true if update succeeded.
     * @sideeffects Updates llm_config and updated_at. Emits conversationUpdated.
     */
    bool updateLlmConfig(const QString& id, const LlmConfig& config);

    /**
     * @brief Fine-grained setter for the per-conversation heartbeat
     *        auto-surface gate. Reads the existing llmConfig, mutates
     *        ONLY the two heartbeat fields, writes back. Avoids
     *        forcing the caller to know the full LlmConfig surface,
     *        and avoids accidentally clobbering provider / model /
     *        temperature with their default values when the caller
     *        only meant to toggle the gate.
     *
     *        The gate is default-OFF; flipping it on enables the
     *        Tier-2 review path on the next heartbeat run.
     * @param id        Conversation UUID.
     * @param allow     New value for `allow_heartbeat_auto_surface`.
     * @param maxPerDay New value for `auto_surface_max_per_day`. Clamped
     *                  to >= 1 (the safe floor) to prevent UI from
     *                  persisting an unusable 0.
     * @return true on success. Emits conversationUpdated on success.
     */
    bool setHeartbeatAutoSurface(const QString& id, bool allow, int maxPerDay);

    /**
     * @brief Updates the system prompt for a conversation.
     * @param id UUID of the conversation.
     * @param prompt New system prompt text (may be empty to clear).
     * @return true if update succeeded.
     * @sideeffects Updates system_prompt and updated_at. Emits conversationUpdated.
     */
    bool updateSystemPrompt(const QString& id, const QString& prompt);

    /**
     * @brief Assigns a primary agent to a conversation (or clears it).
     * @param id       Conversation UUID.
     * @param agentId  Agent UUID, or empty string to clear.
     * @return true on success.
     */
    bool updatePrimaryAgent(const QString& id, const QString& agentId);

    /**
     * @brief Bumps the updated_at timestamp on a conversation.
     *        Called by MessageService after adding a message.
     * @param id UUID of the conversation.
     * @return true if update succeeded.
     */
    bool touchConversation(const QString& id);

    /**
     * @brief Toggle the sidebar pin flag.
     *
     *        Updates `conversations.is_pinned` and bumps `updated_at`
     *        so the row also moves to the top of its section's
     *        "ORDER BY updated_at DESC" sort. Emits `conversationUpdated(id)`
     *        so existing model subscribers (ConversationListModel,
     *        SidebarFlatModel) refresh their cached row state without a
     *        full reset.
     *
     * @param id     Conversation UUID.
     * @param pinned True = appear in the new Pinned section.
     * @return true if update succeeded.
     */
    Q_INVOKABLE bool setConversationPinned(const QString& id, bool pinned);

    /**
     * @brief Stamp the project-1:1 → project member binding on a
     *        conversation. Set at 1:1-direct-chat creation
     *        (openDirectChatWithMember / createIndividualChatsForProject)
     *        so RequestBuilder can resolve the chat's project member,
     *        and that member's per-member model override, even when
     *        several members share one agent template.
     *
     *        Does NOT bump updated_at (structural binding, not a
     *        user-visible mutation) and does NOT emit conversationUpdated
     *        (no model subscriber needs to react). An empty memberAlias
     *        clears the binding (stores SQL NULL).
     *
     * @param id          Conversation UUID.
     * @param memberAlias The project member alias this 1:1 chat is the
     *                    channel for; empty clears the binding.
     * @return true if the update matched a row.
     */
    bool setConversationMemberAlias(const QString& id, const QString& memberAlias);

    // -----------------------------------------------------------------------
    // Folder operations
    // -----------------------------------------------------------------------

    /**
     * @brief Creates a new folder.
     * @param name Display name (must be non-empty).
     * @param parentId Optional parent folder UUID. Empty string for top-level.
     * @return UUID of the created folder, or empty string on failure.
     * @sideeffects Inserts row into folders table. Emits folderCreated.
     */
    QString createFolder(const QString& name, const QString& parentId = {});

    /**
     * @brief Deletes a folder. Conversations inside are moved to root (folder_id = NULL).
     * @param id UUID of the folder to delete.
     * @return true if deletion succeeded.
     * @sideeffects Deletes folder row. Emits folderDeleted.
     */
    bool deleteFolder(const QString& id);

    /**
     * @brief Renames a folder.
     * @param id UUID of the folder.
     * @param name New display name (must be non-empty).
     * @return true if update succeeded.
     */
    bool renameFolder(const QString& id, const QString& name);

    /**
     * @brief Lists folders at the given nesting level.
     * @param parentId If non-empty, returns children of that folder.
     *                 If empty, returns top-level folders (parent_id IS NULL).
     * @return List of Folder structs ordered by name ASC.
     * @complexity O(n) where n is the number of folders at this level.
     */
    QList<Folder> listFolders(const QString& parentId = {});

    /**
     * @brief Retrieves a single folder by ID (including v2 metadata fields).
     * @param id Folder UUID.
     * @return Folder struct, or empty optional if not found.
     */
    std::optional<Folder> getFolder(const QString& id);

    /**
     * @brief Walks up the folder chain from a conversation to the root.
     * @param convId Conversation UUID.
     * @return List of folders from innermost (direct parent) to outermost (root).
     *         Empty list if conversation has no folder or not found.
     */
    QList<Folder> folderChainForConversation(const QString& convId);

    /**
     * @brief The conversation's nearest project/organization ancestor folder id
     *        (the ACN scope owner). Walks the folder chain for the first folder
     *        whose type is `project` or `organization`.
     * @param convId Conversation UUID.
     * @returns The ancestor folder id, or an empty QString when the conversation
     *          is not inside any project/organization.
     */
    QString projectOrgAncestorId(const QString& convId);

    /**
     * @brief Reads a folder's ACN master switch (folders.acn_enabled).
     * @param folderId Folder UUID.
     * @returns true when team memory is enabled for the folder (default true when
     *          the row/column is absent).
     */
    bool folderAcnEnabled(const QString& folderId);

    /**
     * @brief Sets a folder's ACN master switch (folders.acn_enabled).
     * @param folderId Folder UUID.
     * @param enabled  true to enable team memory for this project/organization.
     * @returns true on a successful update.
     * @sideeffects Emits folderUpdated(folderId).
     */
    bool setFolderAcnEnabled(const QString& folderId, bool enabled);

    /**
     * @brief Updates the v2 metadata on a folder (type, goal, description, agent_ids).
     * @param id Folder UUID.
     * @param folderType "regular" | "project" | "organization"
     * @param goal High-level project goal.
     * @param description Longer description.
     * @param agentIds List of agent UUIDs assigned to this project.
     * @return true on success.
     */
    bool updateFolderMetadata(const QString& id,
                              const QString& folderType,
                              const QString& goal,
                              const QString& description,
                              const QStringList& agentIds);

  signals:
    /**
     * @brief Emitted after a conversation is created.
     * @param id UUID of the new conversation.
     */
    void conversationCreated(const QString& id);

    /**
     * @brief Emitted after a conversation is deleted.
     * @param id UUID of the deleted conversation.
     */
    void conversationDeleted(const QString& id);

    /**
     * @brief Emitted after a conversation is renamed, moved, or its
     *        config updated.
     * @param id UUID of the mutated conversation.
     */
    void conversationUpdated(const QString& id);

    /**
     * @brief Emitted by moveToFolder specifically . The
     *        generic conversationUpdated also fires for renames /
     *        touches; summary invalidation must react ONLY to folder
     *        moves, so this dedicated signal exists.
     * @param conversationId Conversation UUID that changed folder.
     */
    void conversationFolderChanged(const QString& conversationId);

    /**
     * @brief Emitted after a folder is created.
     * @param id UUID of the new folder.
     */
    void folderCreated(const QString& id);

    /**
     * @brief Emitted after a folder is deleted.
     * @param id UUID of the deleted folder.
     */
    void folderDeleted(const QString& id);

    /**
     * @brief Emitted after a folder's metadata (name / type / goal /
     *        …) changes.
     * @param id UUID of the updated folder.
     */
    void folderUpdated(const QString& id);

  private:
    DbManager& m_db;

    /**
     * @brief Helper that executes a prepared query and logs on failure.
     * @param q Prepared QSqlQuery to execute.
     * @param context Human-readable context for error messages.
     * @return true if query executed without errors.
     */
    bool execQuery(QSqlQuery& q, const QString& context);
};

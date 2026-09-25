// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-list-model.h
 * @brief Tree model exposing folders and conversations to the QML sidebar.
 *        Push-based: the model auto-subscribes to ConversationService signals in
 *        its constructor and translates row-level mutations into
 *        beginInsertRows/beginRemoveRows/dataChanged. There is NO refresh()
 *        method that QML can call. Any mutation path that bypasses the service
 *        is a bug in that mutation path.
 * @layer Service (Model)
 * @dependencies ConversationService (Service), Qt6::Core
 */

#pragma once

#include <QAbstractItemModel>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class ConversationService;

/**
 * @brief Tree model that exposes folders and conversations for QML TreeView / ListView.
 *
 * Logical structure:
 *   (invisible root)
 *   ├── Folder A          [type == "folder"]
 *   │   ├── Conversation  [type == "conversation"]
 *   │   └── Conversation  [type == "conversation"]
 *   ├── Folder B          [type == "folder"]
 *   └── Conversation      [type == "conversation", root-level]
 *
 * Push-based change handling: the constructor connects the six
 * ConversationService signals to private slots that mutate the tree in
 * place and emit the correct Qt Model/View signals (beginInsertRows,
 * endInsertRows, beginRemoveRows, endRemoveRows, dataChanged,
 * beginMoveRows, endMoveRows). No full model resets after construction.
 *
 * The initial tree population runs once in the constructor via buildTree()
 * inside a beginResetModel/endResetModel pair. After that, every change
 * flows through a service signal.
 */
class ConversationListModel : public QAbstractItemModel {
    Q_OBJECT

    Q_PROPERTY(int totalCount READ totalCount NOTIFY totalCountChanged)

  public:
    /** @brief Item data roles exposed to QML delegates. */
    enum Roles {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        TypeRole,
        FolderIdRole,
        UpdatedAtRole,
        TokenTotalRole,
        SystemPromptRole,
        ProviderIdRole,
        ModelNameRole,
        FolderTypeRole,
        FolderGoalRole,
        FolderDescriptionRole,
        FolderAgentIdsRole,
        /**
         * True iff the conversation row's `is_pinned`
         * flag is set. Drives the new sidebar Pinned section + the
         * 📌 badge on conversation rows. `dataChanged` for this role
         * fires from `onConversationUpdated` whenever
         * `ConversationService::setConversationPinned` runs.
         */
        IsPinnedRole,
        /**
         * True iff the conversation has isGroup=1.
         * Drives the SidebarFlatModel categoriser bucketing:
         *  - root + isGroup → Standalone Group Chats section.
         *  - inside project + isGroup → project's Group Chats subsection.
         */
        IsGroupRole,
        /**
         * The primary_agent_id of the conversation; empty
         * for legacy / no-agent rows. Distinguishes Direct Agent Chats
         * (root + non-empty primary agent) from Plain Chats (root +
         * empty primary agent).
         */
        PrimaryAgentIdRole
    };
    Q_ENUM(Roles)

    /**
     * @brief Constructs the model over the conversation service.
     * @param svc    Source of conversation rows and change signals.
     * @param parent Optional Qt parent.
     */
    explicit ConversationListModel(ConversationService& svc, QObject* parent = nullptr);
    ~ConversationListModel() override;

    /**
     * @brief Returns the conversation UUID at the given model index.
     * @param index  Model index requested.
     * @returns Conversation UUID, or empty when the index is not a
     *          conversation leaf node.
     */
    Q_INVOKABLE QString conversationIdAt(const QModelIndex& index) const;

    /**
     * @brief Returns the total number of conversation leaf nodes.
     * @returns Conversation count across all folders.
     */
    int totalCount() const;

    // QAbstractItemModel interface

    /**
     * @brief QAbstractItemModel index accessor.
     * @param row     Row within `parent`.
     * @param column  Column (always 0 for this model).
     * @param parent  Parent index, or default for top-level.
     * @returns Persistent model index, or invalid when out of range.
     */
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractItemModel parent accessor.
     * @param child  Child index.
     * @returns Parent model index, or invalid for top-level nodes.
     */
    QModelIndex parent(const QModelIndex& child) const override;

    /**
     * @brief QAbstractItemModel row count for the given parent.
     * @param parent  Parent index, or default for top-level.
     * @returns Child row count for the parent.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractItemModel column count.
     * @param parent  Parent index (ignored).
     * @returns Always 1, because this model exposes a single logical column.
     */
    int columnCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractItemModel data accessor.
     * @param index  Index requested.
     * @param role   Role from the Roles enum.
     * @returns QVariant with the role value, or invalid on out-of-range
     *          index / role.
     */
    QVariant data(const QModelIndex& index, int role) const override;

    /**
     * @brief QAbstractItemModel role-name map for QML.
     * @returns Hash mapping Roles values to QML role names.
     */
    QHash<int, QByteArray> roleNames() const override;

  signals:
    /** @brief Emitted whenever totalCount() changes. */
    void totalCountChanged();

  private slots:
    // Push-based handlers wired to ConversationService signals in ctor.

    /**
     * @brief Inserts a tree node for a newly-created conversation.
     * @param id  Conversation UUID.
     */
    void onConversationCreated(const QString& id);

    /**
     * @brief Refreshes the tree node for an updated conversation.
     * @param id  Conversation UUID.
     */
    void onConversationUpdated(const QString& id);

    /**
     * @brief Removes the tree node for a deleted conversation.
     * @param id  Conversation UUID.
     */
    void onConversationDeleted(const QString& id);

    /**
     * @brief Inserts a folder node when a folder is created.
     * @param id  Folder UUID.
     */
    void onFolderCreated(const QString& id);

    /**
     * @brief Refreshes the folder node for an updated folder.
     * @param id  Folder UUID.
     */
    void onFolderUpdated(const QString& id);

    /**
     * @brief Removes the folder node (and its descendants) for a
     *        deleted folder.
     * @param id  Folder UUID.
     */
    void onFolderDeleted(const QString& id);

  private:
    /**
     * @brief One node in the in-memory tree mirroring the DB conversation
     *        / folder hierarchy. Folder nodes have children; conversation
     *        nodes are leaves.
     */
    struct TreeNode {
        QString id;
        QString title;
        QString type;
        QDateTime updatedAt;
        int tokenTotal = 0;
        QJsonObject llmConfig;
        QString systemPrompt;
        QString folderType;
        QString goal;
        QString description;
        QStringList agentIds;
        bool isPinned = false;   ///< Sidebar pin flag (conversation rows only).
        bool isGroup = false;    ///< Categoriser bucket key (conversation rows only).
        QString primaryAgentId;  ///< Categoriser bucket key (conversation rows only).
        TreeNode* parent = nullptr;
        QList<TreeNode*> children;
    };

    ConversationService& m_svc;
    TreeNode* m_root = nullptr;
    int m_totalCount = 0;

    /** @brief Initial full load from DB. Called ONCE from constructor. */
    void buildTree();

    /** @brief Recursively frees a node and all its descendants. */
    void clearTree(TreeNode* node);

    /** @brief Returns the TreeNode stored in the given model index. */
    TreeNode* nodeForIndex(const QModelIndex& index) const;

    /** @brief Returns the row index of `node` within its parent's child list. */
    int rowOfNode(TreeNode* node) const;

    /** @brief Returns a QModelIndex pointing at `node`, or invalid if root. */
    QModelIndex indexForNode(TreeNode* node) const;

    /** @brief Linear search for a conversation node anywhere in the tree. */
    TreeNode* findConversationNode(const QString& id) const;

    /** @brief Linear search for a folder node (top-level only). */
    TreeNode* findFolderNode(const QString& id) const;

    /** @brief Looks up parent TreeNode by folder id ("" → root). */
    TreeNode* parentNodeForFolderId(const QString& folderId) const;

    /** @brief Populates a fresh TreeNode from a Conversation row. */
    TreeNode* makeConversationNode(const class Conversation& conv) const;

    /** @brief Populates a fresh TreeNode from a Folder row. */
    TreeNode* makeFolderNode(const class Folder& folder) const;
};

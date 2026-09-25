// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-list-model.cpp
 * @brief Push-based tree model for folders + conversations. Auto-
 *        connects to ConversationService signals in its constructor and
 *        translates every change into the appropriate Qt Model/View
 *        mutation (beginInsertRows / beginRemoveRows / dataChanged /
 *        beginMoveRows). No polling, no refresh() method, no full resets
 *        after the initial buildTree().
 * @layer Service (Model)
 * @dependencies ConversationService, Qt6::Core
 */

#include "conversation-list-model.h"

#include "../models/conversation.h"
#include "../models/llm-config.h"
#include "../services/conversation-service.h"
#include "../utils/logger.h"

#include <QJsonObject>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ConversationListModel::ConversationListModel(ConversationService& svc, QObject* parent)
    : QAbstractItemModel(parent), m_svc(svc), m_root(new TreeNode{}) {
    m_root->type = QStringLiteral("root");

    // Initial full load. This is the ONLY beginResetModel after construction.
    beginResetModel();
    buildTree();
    endResetModel();
    emit totalCountChanged();

    // Auto-subscribe to service signals. Every mutation path is row-level
    // from here on — no refresh, no polling.
    connect(&m_svc,
            &ConversationService::conversationCreated,
            this,
            &ConversationListModel::onConversationCreated);
    connect(&m_svc,
            &ConversationService::conversationUpdated,
            this,
            &ConversationListModel::onConversationUpdated);
    connect(&m_svc,
            &ConversationService::conversationDeleted,
            this,
            &ConversationListModel::onConversationDeleted);
    connect(
        &m_svc, &ConversationService::folderCreated, this, &ConversationListModel::onFolderCreated);
    connect(
        &m_svc, &ConversationService::folderUpdated, this, &ConversationListModel::onFolderUpdated);
    connect(
        &m_svc, &ConversationService::folderDeleted, this, &ConversationListModel::onFolderDeleted);
}

ConversationListModel::~ConversationListModel() {
    clearTree(m_root);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

QString ConversationListModel::conversationIdAt(const QModelIndex& index) const {
    const TreeNode* node = nodeForIndex(index);
    if (!node || node->type != QStringLiteral("conversation")) {
        return {};
    }
    return node->id;
}

int ConversationListModel::totalCount() const {
    return m_totalCount;
}

// ---------------------------------------------------------------------------
// QAbstractItemModel interface
// ---------------------------------------------------------------------------

QModelIndex ConversationListModel::index(int row, int column, const QModelIndex& parent) const {
    if (column != 0)
        return {};

    const TreeNode* parentNode = parent.isValid() ? nodeForIndex(parent) : m_root;

    if (!parentNode || row < 0 || row >= parentNode->children.count()) {
        return {};
    }
    return createIndex(row, column, parentNode->children[row]);
}

QModelIndex ConversationListModel::parent(const QModelIndex& child) const {
    if (!child.isValid())
        return {};

    const TreeNode* childNode = nodeForIndex(child);
    if (!childNode || !childNode->parent || childNode->parent == m_root) {
        return {};
    }

    const TreeNode* parentNode = childNode->parent;
    if (!parentNode->parent)
        return {};

    const int row = parentNode->parent->children.indexOf(const_cast<TreeNode*>(parentNode));
    if (row < 0)
        return {};

    return createIndex(row, 0, const_cast<TreeNode*>(parentNode));
}

int ConversationListModel::rowCount(const QModelIndex& parent) const {
    const TreeNode* parentNode = parent.isValid() ? nodeForIndex(parent) : m_root;

    return parentNode ? parentNode->children.count() : 0;
}

int ConversationListModel::columnCount(const QModelIndex& /*parent*/) const {
    return 1;
}

QVariant ConversationListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const TreeNode* node = nodeForIndex(index);
    if (!node)
        return {};

    switch (role) {
        case Qt::DisplayRole:
        case TitleRole:
            return node->title;
        case IdRole:
            return node->id;
        case TypeRole:
            return node->type;
        case FolderIdRole:
            if (node->parent && node->parent != m_root) {
                return node->parent->id;
            }
            return QString{};
        case UpdatedAtRole:
            return node->updatedAt.isValid() ? node->updatedAt.toString(Qt::ISODate) : QString{};
        case TokenTotalRole:
            return node->tokenTotal;
        case SystemPromptRole:
            return node->systemPrompt;
        case ProviderIdRole:
            return LlmConfig::fromJson(node->llmConfig).providerId;
        case ModelNameRole:
            return LlmConfig::fromJson(node->llmConfig).modelName;
        case FolderTypeRole:
            return node->folderType.isEmpty() ? QStringLiteral("regular") : node->folderType;
        case FolderGoalRole:
            return node->goal;
        case FolderDescriptionRole:
            return node->description;
        case FolderAgentIdsRole:
            return node->agentIds;
        case IsPinnedRole:
            // Only conversation rows carry the pin flag. Folders and the
            // root return false unconditionally so QML bindings can read
            // `model.isPinned` without role-type-checking.
            return node->type == QStringLiteral("conversation") && node->isPinned;
        case IsGroupRole:
            return node->type == QStringLiteral("conversation") && node->isGroup;
        case PrimaryAgentIdRole:
            return node->type == QStringLiteral("conversation") ? node->primaryAgentId : QString();
        default:
            return {};
    }
}

QHash<int, QByteArray> ConversationListModel::roleNames() const {
    return {
        {IdRole, "id"},
        {TitleRole, "title"},
        {TypeRole, "type"},
        {FolderIdRole, "folderId"},
        {UpdatedAtRole, "updatedAt"},
        {TokenTotalRole, "tokenTotal"},
        {SystemPromptRole, "systemPrompt"},
        {ProviderIdRole, "providerId"},
        {ModelNameRole, "modelName"},
        {FolderTypeRole, "folderType"},
        {FolderGoalRole, "folderGoal"},
        {FolderDescriptionRole, "folderDescription"},
        {FolderAgentIdsRole, "folderAgentIds"},
        {IsPinnedRole, "isPinned"},
        {IsGroupRole, "isGroup"},
        {PrimaryAgentIdRole, "primaryAgentId"},
    };
}

// ---------------------------------------------------------------------------
// Row-level change handlers (service signals → model mutations)
// ---------------------------------------------------------------------------

void ConversationListModel::onConversationCreated(const QString& id) {
    const auto convOpt = m_svc.getConversation(id);
    if (!convOpt.has_value()) {
        qCWarning(verzetaUi) << "ConversationListModel: conversationCreated signal fired for" << id
                             << "but service could not find the row";
        return;
    }

    TreeNode* parentNode = parentNodeForFolderId(convOpt->folderId);
    if (!parentNode)
        parentNode = m_root;

    TreeNode* node = makeConversationNode(*convOpt);
    node->parent = parentNode;

    // New conversations are freshest → sort to top of their parent's children
    // (mirrors listConversations(folder) which is ORDER BY updated_at DESC).
    const QModelIndex parentIdx = (parentNode == m_root) ? QModelIndex() : indexForNode(parentNode);
    const int insertRow = 0;

    beginInsertRows(parentIdx, insertRow, insertRow);
    parentNode->children.prepend(node);
    ++m_totalCount;
    endInsertRows();

    emit totalCountChanged();
}

void ConversationListModel::onConversationUpdated(const QString& id) {
    TreeNode* node = findConversationNode(id);
    if (!node) {
        // Updated row for a conversation we don't know about yet. Treat as
        // a create — this can happen if the model missed an earlier signal.
        onConversationCreated(id);
        return;
    }

    const auto convOpt = m_svc.getConversation(id);
    if (!convOpt.has_value()) {
        // Row vanished between signal emission and read — treat as delete.
        onConversationDeleted(id);
        return;
    }

    TreeNode* currentParent = node->parent ? node->parent : m_root;
    TreeNode* newParent = parentNodeForFolderId(convOpt->folderId);
    if (!newParent)
        newParent = m_root;

    // Case A: parent folder changed → move across parents, land at row 0
    // (new parent's freshest). This is a real cross-parent reparent and
    // requires beginMoveRows.
    //
    // Case B: same parent, but updated_at changed → re-order within the
    // parent's children so the freshest row is at the top. This keeps
    // the sidebar presenting conversations in "most recently updated
    // first" order, matching listConversations' DB sort.
    //
    // Case C: same parent, no reorder → just dataChanged on the row.
    if (currentParent != newParent) {
        const int srcRow = currentParent->children.indexOf(node);
        if (srcRow < 0)
            return;
        const QModelIndex srcParentIdx =
            (currentParent == m_root) ? QModelIndex() : indexForNode(currentParent);
        const QModelIndex dstParentIdx =
            (newParent == m_root) ? QModelIndex() : indexForNode(newParent);
        const int dstRow = 0;

        beginMoveRows(srcParentIdx, srcRow, srcRow, dstParentIdx, dstRow);
        currentParent->children.removeAt(srcRow);
        newParent->children.prepend(node);
        node->parent = newParent;
        endMoveRows();
    } else {
        // Same parent: check whether the updatedAt timestamp has moved
        // forward enough that this row should become the freshest child.
        const QDateTime prevUpdatedAt = node->updatedAt;
        const QDateTime newUpdatedAt = convOpt->updatedAt;

        if (newUpdatedAt.isValid() && newUpdatedAt > prevUpdatedAt) {
            const int srcRow = currentParent->children.indexOf(node);
            // Only move if we're not already at row 0 and there are
            // other children to shift past.
            if (srcRow > 0) {
                const QModelIndex parentIdx =
                    (currentParent == m_root) ? QModelIndex() : indexForNode(currentParent);
                // Qt's beginMoveRows requires destinationRow to NOT be in
                // the range [first, last+1], i.e. to insert BEFORE row 0
                // you pass destinationRow=0; Qt moves the row out of its
                // old slot and inserts before index 0.
                beginMoveRows(parentIdx, srcRow, srcRow, parentIdx, 0);
                currentParent->children.removeAt(srcRow);
                currentParent->children.prepend(node);
                endMoveRows();
            }
        }
    }

    // Copy refreshed fields into the node
    node->title = convOpt->title;
    node->updatedAt = convOpt->updatedAt;
    node->tokenTotal = convOpt->tokenTotal;
    node->llmConfig = convOpt->llmConfig;
    node->systemPrompt = convOpt->systemPrompt;
    node->isPinned = convOpt->isPinned;
    node->isGroup = convOpt->isGroup;
    node->primaryAgentId = convOpt->primaryAgentId;

    const QModelIndex idx = indexForNode(node);
    if (idx.isValid()) {
        emit dataChanged(idx,
                         idx,
                         {TitleRole,
                          UpdatedAtRole,
                          TokenTotalRole,
                          SystemPromptRole,
                          ProviderIdRole,
                          ModelNameRole,
                          FolderIdRole,
                          IsPinnedRole,
                          IsGroupRole,
                          PrimaryAgentIdRole});
    }
}

void ConversationListModel::onConversationDeleted(const QString& id) {
    TreeNode* node = findConversationNode(id);
    if (!node)
        return;

    TreeNode* parentNode = node->parent ? node->parent : m_root;
    const int row = parentNode->children.indexOf(node);
    if (row < 0)
        return;

    const QModelIndex parentIdx = (parentNode == m_root) ? QModelIndex() : indexForNode(parentNode);

    beginRemoveRows(parentIdx, row, row);
    parentNode->children.removeAt(row);
    delete node;
    --m_totalCount;
    endRemoveRows();

    emit totalCountChanged();
}

void ConversationListModel::onFolderCreated(const QString& id) {
    const auto folderOpt = m_svc.getFolder(id);
    if (!folderOpt.has_value())
        return;
    if (!folderOpt->parentId.isEmpty()) {
        // Nested folders are not yet a visible tree level — ignore for now.
        // When nesting support is added, walk the parent chain.
        return;
    }

    TreeNode* node = makeFolderNode(*folderOpt);
    node->parent = m_root;

    // Prepend so new folders appear at the top (mirrors listFolders order).
    const int insertRow = 0;
    beginInsertRows({}, insertRow, insertRow);
    m_root->children.prepend(node);
    endInsertRows();
}

void ConversationListModel::onFolderUpdated(const QString& id) {
    TreeNode* node = findFolderNode(id);
    if (!node) {
        onFolderCreated(id);
        return;
    }

    const auto folderOpt = m_svc.getFolder(id);
    if (!folderOpt.has_value()) {
        onFolderDeleted(id);
        return;
    }

    node->title = folderOpt->name;
    node->folderType = folderOpt->folderType;
    node->goal = folderOpt->goal;
    node->description = folderOpt->description;
    node->agentIds = folderOpt->agentIds;

    const QModelIndex idx = indexForNode(node);
    if (idx.isValid()) {
        emit dataChanged(
            idx,
            idx,
            {TitleRole, FolderTypeRole, FolderGoalRole, FolderDescriptionRole, FolderAgentIdsRole});
    }
}

void ConversationListModel::onFolderDeleted(const QString& id) {
    TreeNode* node = findFolderNode(id);
    if (!node)
        return;

    // Children (conversations inside the folder) get moved to root
    // because the service's ON DELETE SET NULL reparents them in DB.
    // We walk the children in reverse so index arithmetic stays valid.
    const QModelIndex folderIdx = indexForNode(node);
    for (int i = node->children.size() - 1; i >= 0; --i) {
        TreeNode* child = node->children[i];
        const int childRow = i;
        const int dstRow = 0;
        beginMoveRows(folderIdx, childRow, childRow, {}, dstRow);
        node->children.removeAt(childRow);
        child->parent = m_root;
        m_root->children.prepend(child);
        endMoveRows();
    }

    const int row = m_root->children.indexOf(node);
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_root->children.removeAt(row);
    delete node;
    endRemoveRows();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void ConversationListModel::buildTree() {
    for (TreeNode* child : m_root->children) {
        clearTree(child);
    }
    m_root->children.clear();
    m_totalCount = 0;

    // 1. Top-level folders
    const QList<Folder> folders = m_svc.listFolders({});
    for (const Folder& folder : folders) {
        TreeNode* folderNode = makeFolderNode(folder);
        folderNode->parent = m_root;

        const QList<Conversation> folderConvs = m_svc.listConversations(folder.id);
        folderNode->children.reserve(folderConvs.size());
        for (const Conversation& conv : folderConvs) {
            TreeNode* convNode = makeConversationNode(conv);
            convNode->parent = folderNode;
            folderNode->children.append(convNode);
            ++m_totalCount;
        }
        m_root->children.append(folderNode);
    }

    // 2. Root-level conversations
    const QList<Conversation> rootConvs = m_svc.listConversations({});
    m_root->children.reserve(m_root->children.size() + rootConvs.size());
    for (const Conversation& conv : rootConvs) {
        TreeNode* convNode = makeConversationNode(conv);
        convNode->parent = m_root;
        m_root->children.append(convNode);
        ++m_totalCount;
    }

    qCDebug(verzetaUi) << "ConversationListModel: built tree with" << m_totalCount
                       << "conversations and" << folders.size() << "folders";
}

void ConversationListModel::clearTree(TreeNode* node) {
    if (!node)
        return;
    for (TreeNode* child : node->children) {
        clearTree(child);
    }
    delete node;
}

ConversationListModel::TreeNode*
ConversationListModel::nodeForIndex(const QModelIndex& index) const {
    if (!index.isValid())
        return nullptr;
    return static_cast<TreeNode*>(index.internalPointer());
}

int ConversationListModel::rowOfNode(TreeNode* node) const {
    if (!node || !node->parent)
        return -1;
    return node->parent->children.indexOf(node);
}

QModelIndex ConversationListModel::indexForNode(TreeNode* node) const {
    if (!node || node == m_root)
        return {};
    const int row = rowOfNode(node);
    if (row < 0)
        return {};
    return createIndex(row, 0, node);
}

ConversationListModel::TreeNode*
ConversationListModel::findConversationNode(const QString& id) const {
    if (id.isEmpty() || !m_root)
        return nullptr;
    for (TreeNode* top : m_root->children) {
        if (top->type == QStringLiteral("conversation") && top->id == id) {
            return top;
        }
        if (top->type == QStringLiteral("folder")) {
            for (TreeNode* child : top->children) {
                if (child->type == QStringLiteral("conversation") && child->id == id) {
                    return child;
                }
            }
        }
    }
    return nullptr;
}

ConversationListModel::TreeNode* ConversationListModel::findFolderNode(const QString& id) const {
    if (id.isEmpty() || !m_root)
        return nullptr;
    for (TreeNode* top : m_root->children) {
        if (top->type == QStringLiteral("folder") && top->id == id) {
            return top;
        }
    }
    return nullptr;
}

ConversationListModel::TreeNode*
ConversationListModel::parentNodeForFolderId(const QString& folderId) const {
    if (folderId.isEmpty())
        return m_root;
    return findFolderNode(folderId);
}

ConversationListModel::TreeNode*
ConversationListModel::makeConversationNode(const Conversation& conv) const {
    auto* node = new TreeNode{};
    node->id = conv.id;
    node->title = conv.title;
    node->type = QStringLiteral("conversation");
    node->updatedAt = conv.updatedAt;
    node->tokenTotal = conv.tokenTotal;
    node->llmConfig = conv.llmConfig;
    node->systemPrompt = conv.systemPrompt;
    node->isPinned = conv.isPinned;
    node->isGroup = conv.isGroup;
    node->primaryAgentId = conv.primaryAgentId;
    return node;
}

ConversationListModel::TreeNode* ConversationListModel::makeFolderNode(const Folder& folder) const {
    auto* node = new TreeNode{};
    node->id = folder.id;
    node->title = folder.name;
    node->type = QStringLiteral("folder");
    node->updatedAt = folder.createdAt;
    node->folderType = folder.folderType;
    node->goal = folder.goal;
    node->description = folder.description;
    node->agentIds = folder.agentIds;
    return node;
}

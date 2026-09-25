// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file message-list-model.cpp
 * @brief Push-based message list model. All mutations come from
 *        MessageService signals; this model never writes to the DB and
 *        never calls the service imperatively from outside
 *        setActiveConversation().
 * @layer Service (Model)
 * @dependencies MessageService, AgentRegistry, MarkdownConverter, Qt6::Core.
 */

#include "message-list-model.h"

#include "../models/attachment.h"
#include "../services/agent-registry.h"
#include "../services/message-service.h"
#include "../utils/logger.h"

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MessageListModel::MessageListModel(MessageService& svc, QObject* parent)
    : QAbstractListModel(parent), m_svc(svc) {
    connect(&m_svc, &MessageService::messageAdded, this, &MessageListModel::onMessageAdded);
    connect(&m_svc, &MessageService::messageUpdated, this, &MessageListModel::onMessageUpdated);
    connect(&m_svc, &MessageService::messageDeleted, this, &MessageListModel::onMessageDeleted);
    connect(&m_svc,
            &MessageService::messageStreamingStarted,
            this,
            &MessageListModel::onStreamingStarted);
    connect(&m_svc,
            &MessageService::messageContentStreamed,
            this,
            &MessageListModel::onContentStreamed);
    connect(&m_svc,
            &MessageService::messageContentRewritten,
            this,
            &MessageListModel::onContentRewritten);
    connect(&m_svc,
            &MessageService::messageStreamingAborted,
            this,
            &MessageListModel::onStreamingAborted);
    connect(&m_svc,
            &MessageService::messageEphemeralPosted,
            this,
            &MessageListModel::onEphemeralPosted);
    // /clear path — when ephemeral output for the active conv is
    // cleared, refresh the visible list so slash-command bubbles
    // disappear (DB-backed messages stay).
    connect(&m_svc, &MessageService::messageEphemeralCleared, this, [this](const QString& convId) {
        if (convId == m_activeConvId) {
            reloadActiveConversation();
        }
    });
}

// ---------------------------------------------------------------------------
// Active conversation binding
// ---------------------------------------------------------------------------

void MessageListModel::setActiveConversation(const QString& convId) {
    if (convId == m_activeConvId)
        return;
    m_activeConvId = convId;
    reloadActiveConversation();
    emit activeConversationChanged();
}

QString MessageListModel::activeConversationId() const {
    return m_activeConvId;
}

void MessageListModel::reloadActiveConversation() {
    beginResetModel();
    m_messages.clear();
    m_streamingIds.clear();

    if (!m_activeConvId.isEmpty()) {
        // Persisted rows from DB
        QList<Message> all = m_svc.getMessages(m_activeConvId);
        m_messages.reserve(all.size());
        for (const Message& m : all) {
            // Don't show raw tool messages or blank tool_calls-only assistant
            // rows in the chat view. They remain in the DB for LLM history.
            if (m.role == QStringLiteral("tool"))
                continue;
            if (m.role == QStringLiteral("assistant") && m.content.trimmed().isEmpty() &&
                m.finishReason == QStringLiteral("tool_calls")) {
                continue;
            }
            m_messages.append(m);
        }

        // Any streaming placeholders currently in-flight for this conv
        const QList<Message> streaming = m_svc.streamingMessagesForConversation(m_activeConvId);
        for (const Message& m : streaming) {
            m_messages.append(m);
            m_streamingIds.insert(m.id);
        }

        // Any ephemeral (non-persisted) messages that were posted
        // during this session — typically slash-command output.
        const QList<Message> ephemeral = m_svc.ephemeralMessagesForConversation(m_activeConvId);
        for (const Message& m : ephemeral) {
            m_messages.append(m);
        }
    }

    endResetModel();
    emit countChanged();
    emit streamingChanged();
}

// ---------------------------------------------------------------------------
// Counts / streaming state
// ---------------------------------------------------------------------------

int MessageListModel::count() const {
    return static_cast<int>(m_messages.size());
}

bool MessageListModel::hasStreamingMessage() const {
    return !m_streamingIds.isEmpty();
}

// ---------------------------------------------------------------------------
// QAbstractListModel interface
// ---------------------------------------------------------------------------

int MessageListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_messages.size());
}

QVariant MessageListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_messages.size())) {
        return {};
    }

    const Message& m = m_messages.at(index.row());
    switch (role) {
        case IdRole:
            return m.id;
        case RoleRole:
            return m.role;
        case ContentRole:
            return m.content;
        case ContentHtmlRole:
            return m.contentHtml;
        case CreatedAtRole:
            return m.createdAt.toString(Qt::ISODate);
        case TokenCountRole:
            return m.tokenCount;
        case ModelUsedRole:
            return m.modelUsed;
        case FinishReasonRole:
            return m.finishReason;
        case AttachmentsRole: {
            const QList<Attachment> rows = m_svc.getAttachments(m.id);
            if (rows.isEmpty())
                return QVariantList{};
            QVariantList out;
            out.reserve(rows.size());
            for (const Attachment& a : rows) {
                QVariantMap map;
                map[QStringLiteral("id")] = a.id;
                map[QStringLiteral("type")] = a.type;
                map[QStringLiteral("filename")] = a.filename;
                map[QStringLiteral("mimeType")] = a.mimeType;
                map[QStringLiteral("dataPath")] = a.dataPath;
                map[QStringLiteral("createdAt")] = a.createdAt.toString(Qt::ISODate);
                out.append(map);
            }
            return out;
        }
        case ToolCallsRole:
            return QVariant{};
        case IsStreamingRole:
            return m_streamingIds.contains(m.id);
        case MetadataRole:
            return QVariant::fromValue(m.metadata);
        case AgentIdRole:
            return m.agentId;
        case MemberAliasRole:
            return m.memberAlias;
        case AgentNameRole: {
            if (!m.memberAlias.isEmpty())
                return m.memberAlias;
            if (m.agentId.isEmpty() || !m_agentRegistry)
                return QString();
            const Agent a = m_agentRegistry->getAgent(m.agentId);
            return a.isValid() ? a.name : QString();
        }
        case AgentRoleNameRole: {
            if (m.agentId.isEmpty() || !m_agentRegistry)
                return QString();
            const Agent a = m_agentRegistry->getAgent(m.agentId);
            return a.isValid() ? a.name : QString();
        }
        case AgentIconRole: {
            if (m.agentId.isEmpty() || !m_agentRegistry)
                return QString();
            const Agent a = m_agentRegistry->getAgent(m.agentId);
            return a.isValid() ? a.iconName : QString();
        }
        case ThinkingContentRole:
            return m.thinkingContent;
        default:
            return {};
    }
}

QHash<int, QByteArray> MessageListModel::roleNames() const {
    return {
        {IdRole, QByteArrayLiteral("id")},
        {RoleRole, QByteArrayLiteral("role")},
        {ContentRole, QByteArrayLiteral("content")},
        {ContentHtmlRole, QByteArrayLiteral("contentHtml")},
        {CreatedAtRole, QByteArrayLiteral("createdAt")},
        {TokenCountRole, QByteArrayLiteral("tokenCount")},
        {ModelUsedRole, QByteArrayLiteral("modelUsed")},
        {FinishReasonRole, QByteArrayLiteral("finishReason")},
        {AttachmentsRole, QByteArrayLiteral("attachments")},
        {ToolCallsRole, QByteArrayLiteral("toolCalls")},
        {IsStreamingRole, QByteArrayLiteral("isStreaming")},
        {MetadataRole, QByteArrayLiteral("metadata")},
        {AgentIdRole, QByteArrayLiteral("agentId")},
        {AgentNameRole, QByteArrayLiteral("agentName")},
        {AgentRoleNameRole, QByteArrayLiteral("agentRoleName")},
        {AgentIconRole, QByteArrayLiteral("agentIcon")},
        {MemberAliasRole, QByteArrayLiteral("memberAlias")},
        {ThinkingContentRole, QByteArrayLiteral("thinkingContent")},
    };
}

void MessageListModel::setAgentRegistry(AgentRegistry* registry) {
    m_agentRegistry = registry;
    if (!m_messages.isEmpty()) {
        emit dataChanged(index(0),
                         index(m_messages.size() - 1),
                         {AgentNameRole, AgentRoleNameRole, AgentIconRole});
    }
}

// ---------------------------------------------------------------------------
// Row-level signal handlers
// ---------------------------------------------------------------------------

void MessageListModel::onMessageAdded(const QString& convId, const QString& msgId) {
    if (convId != m_activeConvId)
        return;

    // Case A — this is the finalization of an already-displayed streaming row.
    // The row already exists; flip IsStreaming off and re-fetch authoritative
    // data from DB so tokenCount / finishReason / contentHtml are filled in.
    const int existingRow = findById(msgId);
    if (existingRow >= 0) {
        m_streamingIds.remove(msgId);

        // Re-read the persisted row so we have canonical values.
        Message canonical;
        const QList<Message> all = m_svc.getMessages(convId);
        for (const Message& m : all) {
            if (m.id == msgId) {
                canonical = m;
                break;
            }
        }

        // If the finalized row is "empty assistant + tool_calls" — i.e.
        // the model invoked a tool without saying anything — it has no
        // user-visible content; the tool_calls row it parents lives in
        // ToolCallLogModel. Remove it from the chat view instead of
        // letting a blank bubble stick around. The DB row remains intact
        // as the FK parent for its tool_calls children.
        const bool isToolOnlyGhost = canonical.role == QStringLiteral("assistant") &&
                                     canonical.content.trimmed().isEmpty() &&
                                     canonical.finishReason == QStringLiteral("tool_calls");

        if (isToolOnlyGhost) {
            beginRemoveRows({}, existingRow, existingRow);
            m_messages.removeAt(existingRow);
            endRemoveRows();
            emit countChanged();
            emit streamingChanged();
            return;
        }

        m_messages[existingRow] = canonical;
        const QModelIndex idx = index(existingRow);
        emit dataChanged(idx,
                         idx,
                         {ContentRole,
                          ContentHtmlRole,
                          TokenCountRole,
                          FinishReasonRole,
                          IsStreamingRole,
                          MetadataRole,
                          ModelUsedRole,
                          ThinkingContentRole});
        emit streamingChanged();
        return;
    }

    // Case B — brand new persisted row (user message, tool result, finalised
    // response from a different code path, etc.). Fetch authoritative data.
    const QList<Message> all = m_svc.getMessages(convId);
    for (const Message& m : all) {
        if (m.id != msgId)
            continue;

        // Apply the same "don't render tool/empty tool_calls" filter used
        // in reloadActiveConversation so those stay out of the UI.
        if (m.role == QStringLiteral("tool"))
            return;
        if (m.role == QStringLiteral("assistant") && m.content.trimmed().isEmpty() &&
            m.finishReason == QStringLiteral("tool_calls")) {
            return;
        }

        const int row = static_cast<int>(m_messages.size());
        beginInsertRows({}, row, row);
        m_messages.append(m);
        endInsertRows();
        emit countChanged();
        return;
    }
}

void MessageListModel::onMessageUpdated(const QString& convId, const QString& msgId) {
    if (convId != m_activeConvId)
        return;
    const int row = findById(msgId);
    if (row < 0)
        return;

    const QList<Message> all = m_svc.getMessages(convId);
    for (const Message& m : all) {
        if (m.id == msgId) {
            m_messages[row] = m;
            const QModelIndex idx = index(row);
            emit dataChanged(idx,
                             idx,
                             {ContentRole,
                              ContentHtmlRole,
                              TokenCountRole,
                              FinishReasonRole,
                              ModelUsedRole,
                              MetadataRole,
                              ThinkingContentRole});
            return;
        }
    }
}

void MessageListModel::onMessageDeleted(const QString& convId, const QString& msgId) {
    if (convId != m_activeConvId)
        return;
    const int row = findById(msgId);
    if (row < 0)
        return;

    beginRemoveRows({}, row, row);
    m_messages.removeAt(row);
    endRemoveRows();
    m_streamingIds.remove(msgId);
    emit countChanged();
    if (m_streamingIds.isEmpty())
        emit streamingChanged();
}

void MessageListModel::onStreamingStarted(const QString& convId,
                                          const QString& msgId,
                                          const QString& role,
                                          const QString& agentId,
                                          const QString& memberAlias) {
    if (convId != m_activeConvId)
        return;
    if (findById(msgId) >= 0)
        return;  // already present

    Message msg;
    msg.id = msgId;
    msg.conversationId = convId;
    msg.role = role;
    msg.content = {};
    msg.createdAt = QDateTime::currentDateTimeUtc();
    msg.agentId = agentId;
    msg.memberAlias = memberAlias;

    const int row = static_cast<int>(m_messages.size());
    beginInsertRows({}, row, row);
    m_messages.append(msg);
    m_streamingIds.insert(msgId);
    endInsertRows();
    emit countChanged();
    emit streamingChanged();
}

void MessageListModel::onContentStreamed(const QString& convId,
                                         const QString& msgId,
                                         const QString& delta) {
    if (convId != m_activeConvId)
        return;
    const int row = findById(msgId);
    if (row < 0)
        return;

    m_messages[row].content += delta;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ContentRole});
}

void MessageListModel::onContentRewritten(const QString& convId,
                                          const QString& msgId,
                                          const QString& newContent) {
    if (convId != m_activeConvId)
        return;
    const int row = findById(msgId);
    if (row < 0)
        return;

    m_messages[row].content = newContent;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ContentRole});
}

void MessageListModel::onStreamingAborted(const QString& convId, const QString& msgId) {
    if (convId != m_activeConvId)
        return;
    const int row = findById(msgId);
    if (row < 0) {
        m_streamingIds.remove(msgId);
        return;
    }

    beginRemoveRows({}, row, row);
    m_messages.removeAt(row);
    endRemoveRows();
    m_streamingIds.remove(msgId);
    emit countChanged();
    if (m_streamingIds.isEmpty())
        emit streamingChanged();
}

void MessageListModel::onEphemeralPosted(const QString& convId, const QString& msgId) {
    if (convId != m_activeConvId)
        return;
    if (findById(msgId) >= 0)
        return;  // already present

    const QList<Message> ephemeral = m_svc.ephemeralMessagesForConversation(convId);
    for (const Message& m : ephemeral) {
        if (m.id != msgId)
            continue;
        const int row = static_cast<int>(m_messages.size());
        beginInsertRows({}, row, row);
        m_messages.append(m);
        endInsertRows();
        emit countChanged();
        return;
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

int MessageListModel::findById(const QString& id) const {
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages.at(i).id == id)
            return i;
    }
    return -1;
}

bool MessageListModel::isFinalizingStreamingRow(const QString& msgId) const {
    return m_streamingIds.contains(msgId);
}

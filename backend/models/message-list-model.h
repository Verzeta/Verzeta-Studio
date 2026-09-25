// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file message-list-model.h
 * @brief Push-based Qt list model for the active conversation's
 *        messages. Subscribes to MessageService signals in its constructor
 *        and translates every mutation into row-level dataChanged /
 *        beginInsertRows / beginRemoveRows. Streaming content arrives via
 *        messageContentStreamed signals. The model never calls the
 *        service imperatively and ChatController never manipulates the
 *        model's rows directly.
 * @layer Service (Model)
 * @dependencies MessageService (Service), Qt6::Core
 */

#pragma once

#include "../models/message.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class MessageService;

/**
 * @brief QAbstractListModel for chat messages in the currently-active conversation.
 *
 * Design:
 *   - Exactly one "active conversation id". Calling setActiveConversation(id)
 *     loads that conversation's persisted rows from MessageService plus any
 *     in-memory streaming placeholders for that conversation.
 *   - All mutations are push-based: the constructor subscribes to
 *     MessageService signals. Persisted adds/updates/deletes that target a
 *     different conversation are ignored.
 *   - Streaming messages live in MessageService's in-memory buffer. Chunks
 *     arrive via messageContentStreamed and update a single row in-place.
 *   - Switching conversations does not corrupt in-flight streaming work:
 *     the stream continues to exist in MessageService's buffer and will be
 *     visible again when the user switches back to that conversation.
 */
class MessageListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool hasStreamingMessage READ hasStreamingMessage NOTIFY streamingChanged)
    Q_PROPERTY(QString activeConversationId READ activeConversationId WRITE setActiveConversation
                   NOTIFY activeConversationChanged)

  public:
    /** @brief Item data roles exposed to QML delegates. */
    enum MessageRoles {
        IdRole = Qt::UserRole + 1,
        RoleRole,
        ContentRole,
        ContentHtmlRole,
        CreatedAtRole,
        TokenCountRole,
        ModelUsedRole,
        FinishReasonRole,
        AttachmentsRole,
        ToolCallsRole,
        IsStreamingRole,
        MetadataRole,
        AgentIdRole,
        AgentNameRole,
        AgentRoleNameRole,
        AgentIconRole,
        MemberAliasRole,
        ThinkingContentRole  ///< Reasoning sidecar captured from the provider's thinking channel.
                             ///< Display-only.
    };
    Q_ENUM(MessageRoles)

    /**
     * @brief Constructs the model bound to a MessageService.
     * @param svc     MessageService whose signals drive the model.
     *                Must outlive this object.
     * @param parent  Optional Qt parent.
     */
    explicit MessageListModel(MessageService& svc, QObject* parent = nullptr);

    /**
     * @brief Attaches an AgentRegistry used to resolve agent name / role
     *        / icon for display roles.
     * @param registry  Non-owning AgentRegistry pointer. Nullable.
     */
    void setAgentRegistry(class AgentRegistry* registry);

    // -----------------------------------------------------------------------
    // Active-conversation binding
    // -----------------------------------------------------------------------

    /**
     * @brief Loads the given conversation's persisted rows + any streaming
     *        placeholders for it.
     * @param convId  Conversation UUID. Empty clears the model.
     *
     * Uses beginResetModel once; subsequent mutations are row-level via
     * service signals.
     */
    void setActiveConversation(const QString& convId);

    /**
     * @brief Returns the id of the conversation currently displayed.
     * @returns Conversation UUID, or empty when no conversation is bound.
     */
    QString activeConversationId() const;

    // -----------------------------------------------------------------------
    // QAbstractListModel interface
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the number of message rows in the model.
     * @returns Row count.
     */
    int count() const;

    /**
     * @brief Reports whether any visible row is currently streaming.
     * @returns True when at least one row is in `m_streamingIds`.
     */
    bool hasStreamingMessage() const;

    /**
     * @brief QAbstractListModel row count.
     * @param parent  Ignored (flat list).
     * @returns Row count.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractListModel data accessor.
     * @param index  Row index requested.
     * @param role   Role from the MessageRoles enum.
     * @returns QVariant with the role value, or an invalid QVariant on
     *          out-of-range index / role.
     */
    QVariant data(const QModelIndex& index, int role) const override;

    /**
     * @brief QAbstractListModel role-name map for QML.
     * @returns Hash mapping MessageRoles values to QML role names.
     */
    QHash<int, QByteArray> roleNames() const override;

  signals:
    /** @brief Emitted whenever the row count changes. */
    void countChanged();

    /** @brief Emitted whenever hasStreamingMessage() flips. */
    void streamingChanged();

    /** @brief Emitted whenever setActiveConversation changes the bound conv. */
    void activeConversationChanged();

  private slots:
    // Row-level handlers wired to MessageService signals in the constructor.

    /**
     * @brief Inserts a row for a newly-persisted message in the active scope.
     * @param convId  Conversation id of the inserted message.
     * @param msgId   Inserted message id.
     */
    void onMessageAdded(const QString& convId, const QString& msgId);

    /**
     * @brief Refreshes the row for an updated message in the active scope.
     * @param convId  Conversation id of the updated message.
     * @param msgId   Updated message id.
     */
    void onMessageUpdated(const QString& convId, const QString& msgId);

    /**
     * @brief Removes the row for a deleted message.
     * @param convId  Conversation id of the deleted message.
     * @param msgId   Deleted message id.
     */
    void onMessageDeleted(const QString& convId, const QString& msgId);

    /**
     * @brief Inserts a placeholder row for a streaming-started message.
     * @param convId       Conversation id of the streaming message.
     * @param msgId        Placeholder message id.
     * @param role         Role string ("assistant", etc.).
     * @param agentId      Responder agent template id, or empty for 1:1.
     * @param memberAlias  Responder alias, or empty.
     */
    void onStreamingStarted(const QString& convId,
                            const QString& msgId,
                            const QString& role,
                            const QString& agentId,
                            const QString& memberAlias);

    /**
     * @brief Appends a streaming-delta chunk to the matching row's text.
     * @param convId  Conversation id of the streaming message.
     * @param msgId   Streaming message id.
     * @param delta   Text fragment to append.
     */
    void onContentStreamed(const QString& convId, const QString& msgId, const QString& delta);

    /**
     * @brief Replaces the row's text wholesale (anti-echo / sanitiser rewrite).
     * @param convId      Conversation id of the rewritten message.
     * @param msgId       Rewritten message id.
     * @param newContent  Replacement content.
     */
    void onContentRewritten(const QString& convId, const QString& msgId, const QString& newContent);

    /**
     * @brief Marks the streaming row as not-streaming when the stream aborts.
     * @param convId  Conversation id of the aborted stream.
     * @param msgId   Streaming message id.
     */
    void onStreamingAborted(const QString& convId, const QString& msgId);

    /**
     * @brief Inserts a row for an ephemeral (non-persisted) post.
     * @param convId  Conversation id.
     * @param msgId   Ephemeral message id.
     */
    void onEphemeralPosted(const QString& convId, const QString& msgId);

  private:
    MessageService& m_svc;
    QList<Message> m_messages;     ///< Rows currently visible in the UI
    QSet<QString> m_streamingIds;  ///< Message IDs still streaming (for IsStreaming role)
    QString m_activeConvId;
    class AgentRegistry* m_agentRegistry = nullptr;

    /** @brief Returns row index of msg by UUID, or -1 if absent. */
    int findById(const QString& id) const;

    /**
     * @brief Returns true if a persisted row with this id already exists.
     *        Used to deduplicate the messageAdded signal for messages that
     *        finished streaming (they were already placeholders).
     */
    bool isFinalizingStreamingRow(const QString& msgId) const;

    /** @brief Full (non-reset) reload from service for the current conv. */
    void reloadActiveConversation();
};

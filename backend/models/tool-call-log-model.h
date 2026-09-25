// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-call-log-model.h
 * @brief Push-based QAbstractListModel of tool_calls rows for
 *        the active conversation. Subscribes to MessageService's
 *        toolCallAdded / toolCallUpdated signals and surfaces the
 *        ToolCall struct directly (including arguments, result, status,
 *        and start/complete timestamps).
 * @layer Service (Model)
 * @dependencies MessageService, Qt6::Core
 */

#pragma once

#include "tool-call.h"

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QString>

class MessageService;

/**
 * @brief QAbstractListModel exposing one row per `tool_calls` record in
 *        the active conversation. Push-driven from MessageService
 *        toolCall* signals.
 */
class ToolCallLogModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString activeConversationId READ activeConversationId WRITE setActiveConversation
                   NOTIFY activeConversationChanged)

  public:
    /** @brief Item data roles exposed to QML delegates. */
    enum Roles {
        IdRole = Qt::UserRole + 1,
        ToolNameRole,
        ArgsRole,
        ResultRole,
        StatusRole,
        TimestampRole,
        CompletedAtRole
    };
    Q_ENUM(Roles)

    /**
     * @brief Constructs the model bound to a MessageService.
     * @param svc     MessageService whose toolCall* signals drive the model.
     *                Must outlive this object.
     * @param parent  Optional Qt parent.
     */
    explicit ToolCallLogModel(MessageService& svc, QObject* parent = nullptr);

    /**
     * @brief Loads tool-call rows for the given conversation.
     * @param convId  Conversation UUID. Empty clears the model.
     */
    void setActiveConversation(const QString& convId);

    /**
     * @brief Returns the currently-bound conversation id.
     * @returns Conversation UUID, or empty when no scope is set.
     */
    QString activeConversationId() const;

    /**
     * @brief Returns the number of tool-call rows.
     * @returns Row count.
     */
    int count() const;

    /**
     * @brief QAbstractListModel row count.
     * @param parent  Ignored (flat list).
     * @returns Row count.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractListModel data accessor.
     * @param index  Row index requested.
     * @param role   Role from the Roles enum.
     * @returns QVariant with the role value.
     */
    QVariant data(const QModelIndex& index, int role) const override;

    /**
     * @brief QAbstractListModel role-name map for QML.
     * @returns Hash mapping Roles values to QML role names.
     */
    QHash<int, QByteArray> roleNames() const override;

  signals:
    /** @brief Emitted whenever the row count changes. */
    void countChanged();

    /** @brief Emitted whenever setActiveConversation changes the bound conv. */
    void activeConversationChanged();

  private slots:
    /**
     * @brief Inserts a row for a new tool-call in the active scope.
     * @param convId  Conversation id of the new tool call.
     * @param callId  Tool-call UUID.
     */
    void onToolCallAdded(const QString& convId, const QString& callId);

    /**
     * @brief Refreshes the row for an updated tool-call.
     * @param convId  Conversation id of the updated tool call.
     * @param callId  Tool-call UUID.
     */
    void onToolCallUpdated(const QString& convId, const QString& callId);

  private:
    MessageService& m_svc;
    QString m_activeConvId;
    QList<ToolCall> m_calls;

    int findRow(const QString& callId) const;
    void reload();
};

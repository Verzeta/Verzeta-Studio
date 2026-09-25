// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-call-log-model.cpp
 * @brief Push-based tool call log fed by MessageService tool_calls signals.
 * @layer Service (Model)
 * @dependencies MessageService, Qt6::Core.
 */

#include "tool-call-log-model.h"

#include "../services/message-service.h"
#include "../utils/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

ToolCallLogModel::ToolCallLogModel(MessageService& svc, QObject* parent)
    : QAbstractListModel(parent), m_svc(svc) {
    connect(&m_svc, &MessageService::toolCallAdded, this, &ToolCallLogModel::onToolCallAdded);
    connect(&m_svc, &MessageService::toolCallUpdated, this, &ToolCallLogModel::onToolCallUpdated);
}

void ToolCallLogModel::setActiveConversation(const QString& convId) {
    if (convId == m_activeConvId)
        return;
    m_activeConvId = convId;
    reload();
    emit activeConversationChanged();
}

QString ToolCallLogModel::activeConversationId() const {
    return m_activeConvId;
}

int ToolCallLogModel::count() const {
    return static_cast<int>(m_calls.size());
}

int ToolCallLogModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_calls.size());
}

QVariant ToolCallLogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_calls.size())) {
        return {};
    }
    const ToolCall& c = m_calls.at(index.row());
    switch (role) {
        case IdRole:
            return c.id;
        case ToolNameRole:
            return c.toolName;
        case ArgsRole:
            return QString::fromUtf8(QJsonDocument(c.arguments).toJson(QJsonDocument::Compact));
        case ResultRole: {
            QString s;
            if (c.result.isObject()) {
                s = QString::fromUtf8(
                    QJsonDocument(c.result.toObject()).toJson(QJsonDocument::Compact));
            } else if (c.result.isArray()) {
                s = QString::fromUtf8(
                    QJsonDocument(c.result.toArray()).toJson(QJsonDocument::Compact));
            } else {
                s = c.result.toString();
            }
            // Defensive cap: never hand a giant string to the QML delegate.
            // An unbounded tool result (a recursive list produced 319 MB)
            // crashed the delegate binding; truncate for DISPLAY only (the
            // full result stays in the DB and the model's context path).
            constexpr int kMaxDisplayChars = 16384;
            if (s.size() > kMaxDisplayChars) {
                s = s.left(kMaxDisplayChars) +
                    QStringLiteral("\n… [truncated, %1 more characters not shown]")
                        .arg(s.size() - kMaxDisplayChars);
            }
            return s;
        }
        case StatusRole:
            return c.status.isEmpty() ? QStringLiteral("pending") : c.status;
        case TimestampRole:
            return c.startedAt.isValid() ? c.startedAt.toString(Qt::ISODate) : QString{};
        case CompletedAtRole:
            return c.completedAt.isValid() ? c.completedAt.toString(Qt::ISODate) : QString{};
        default:
            return {};
    }
}

QHash<int, QByteArray> ToolCallLogModel::roleNames() const {
    return {
        {IdRole, QByteArrayLiteral("id")},
        {ToolNameRole, QByteArrayLiteral("toolName")},
        {ArgsRole, QByteArrayLiteral("args")},
        {ResultRole, QByteArrayLiteral("result")},
        {StatusRole, QByteArrayLiteral("status")},
        {TimestampRole, QByteArrayLiteral("timestamp")},
        {CompletedAtRole, QByteArrayLiteral("completedAt")},
    };
}

void ToolCallLogModel::onToolCallAdded(const QString& convId, const QString& callId) {
    if (convId != m_activeConvId)
        return;
    const ToolCall c = m_svc.getToolCall(callId);
    if (c.id.isEmpty())
        return;

    const int row = static_cast<int>(m_calls.size());
    beginInsertRows({}, row, row);
    m_calls.append(c);
    endInsertRows();
    emit countChanged();
}

void ToolCallLogModel::onToolCallUpdated(const QString& convId, const QString& callId) {
    if (convId != m_activeConvId)
        return;
    const int row = findRow(callId);
    if (row < 0) {
        // The row was added before we were subscribed (e.g. setActive
        // happened after the add fired). Fetch it now as a new row.
        onToolCallAdded(convId, callId);
        return;
    }

    const ToolCall c = m_svc.getToolCall(callId);
    if (c.id.isEmpty())
        return;
    m_calls[row] = c;

    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ResultRole, StatusRole, CompletedAtRole});
}

int ToolCallLogModel::findRow(const QString& callId) const {
    for (int i = 0; i < m_calls.size(); ++i) {
        if (m_calls.at(i).id == callId)
            return i;
    }
    return -1;
}

void ToolCallLogModel::reload() {
    beginResetModel();
    m_calls.clear();
    if (!m_activeConvId.isEmpty()) {
        m_calls = m_svc.toolCallsForConversation(m_activeConvId);
    }
    endResetModel();
    emit countChanged();
}

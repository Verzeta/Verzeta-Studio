// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-conversation-tool.cpp
 * @brief Implementation of Tools::ReadConversationTool.
 * @layer Service (Tool subsystem)
 * @dependencies MessageService.
 */

#include "read-conversation-tool.h"

#include "../../models/message.h"
#include "../../services/message-service.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

namespace Tools {

static constexpr int kDefaultLimit = 20;  ///< Messages returned when no valid limit is given.
static constexpr int kMaxLimit = 100;     ///< Largest limit accepted.
static constexpr int kPerMessageCharCap = 2000;  ///< Longer messages are truncated to this.

ReadConversationTool::ReadConversationTool(MessageService& msgService,
                                           std::function<QString()> activeConvIdGetter)
    : m_msgService(msgService), m_activeConvIdGetter(std::move(activeConvIdGetter)) {}

QString ReadConversationTool::name() const {
    return QStringLiteral("read_conversation");
}

QString ReadConversationTool::description() const {
    return QStringLiteral("Reads the most recent messages from a conversation by ID. "
                          "Use this after search_messages or list_project_conversations "
                          "to load real context for a specific chat. Always bounded by "
                          "limit so it cannot blow the context window.");
}

QList<ToolParameterSchema> ReadConversationTool::parameters() const {
    ToolParameterSchema cid;
    cid.name = QStringLiteral("conversation_id");
    cid.type = QStringLiteral("string");
    cid.description = QStringLiteral("UUID of the conversation to read. Pass \"current\" for the "
                                     "active conversation.");
    cid.required = true;

    ToolParameterSchema limit;
    limit.name = QStringLiteral("limit");
    limit.type = QStringLiteral("integer");
    limit.description =
        QStringLiteral("Number of most-recent messages to return (default 20, max 100).");
    limit.required = false;

    return {cid, limit};
}

bool ReadConversationTool::runsOnMainThread() const {
    return true;
}

QJsonValue ReadConversationTool::invoke(const QJsonObject& args) {
    QString convId = args[QStringLiteral("conversation_id")].toString();
    if (convId == QStringLiteral("current") || convId.isEmpty()) {
        // Args-injected conv id wins over the captured-LOCAL getter
        // so wire-side per-client cascades resolve "current" against
        // THEIR conversation, not the LOCAL CC's.
        convId = args.value(QStringLiteral("__caller_conv_id")).toString();
        if (convId.isEmpty() && m_activeConvIdGetter) {
            convId = m_activeConvIdGetter();
        }
    }

    int limit = args[QStringLiteral("limit")].toInt(kDefaultLimit);
    if (limit <= 0)
        limit = kDefaultLimit;
    if (limit > kMaxLimit)
        limit = kMaxLimit;

    if (convId.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("no active conversation")}};
    }

    const QList<Message> all = m_msgService.getRecentMessages(convId, limit);

    QJsonArray arr;
    for (const Message& m : all) {
        QJsonObject o;
        o[QStringLiteral("id")] = m.id;
        o[QStringLiteral("role")] = m.role;
        if (!m.memberAlias.isEmpty()) {
            o[QStringLiteral("from")] = m.memberAlias;
        }
        QString content = m.content;
        if (content.size() > kPerMessageCharCap) {
            content = content.left(kPerMessageCharCap) + QStringLiteral("\n\u2026[truncated]");
        }
        o[QStringLiteral("content")] = content;
        o[QStringLiteral("createdAt")] = m.createdAt.toString(Qt::ISODate);
        arr.append(o);
    }

    QJsonObject result;
    result[QStringLiteral("conversationId")] = convId;
    result[QStringLiteral("messages")] = arr;
    result[QStringLiteral("count")] = arr.size();
    result[QStringLiteral("totalInDb")] = all.size();
    return result;
}

}  // namespace Tools

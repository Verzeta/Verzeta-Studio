// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file list-project-conversations-tool.cpp
 * @brief Implementation of Tools::ListProjectConversationsTool.
 * @layer Service (Tool subsystem)
 * @dependencies ConversationService.
 */

#include "list-project-conversations-tool.h"

#include "../../models/conversation.h"
#include "../../services/conversation-service.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

namespace Tools {

ListProjectConversationsTool::ListProjectConversationsTool(
    ConversationService& convService, std::function<QString()> activeConvIdGetter)
    : m_convService(convService), m_activeConvIdGetter(std::move(activeConvIdGetter)) {}

QString ListProjectConversationsTool::name() const {
    return QStringLiteral("list_project_conversations");
}

QString ListProjectConversationsTool::description() const {
    return QStringLiteral("Lists other conversations inside the same project/organization "
                          "folder as the current chat. Useful when you want to look up what "
                          "was discussed in a 1:1 chat with a teammate or in a parallel "
                          "thread. Returns id + title; use read_conversation to load one.");
}

QList<ToolParameterSchema> ListProjectConversationsTool::parameters() const {
    return {};
}

bool ListProjectConversationsTool::runsOnMainThread() const {
    return true;
}

QJsonValue ListProjectConversationsTool::invoke(const QJsonObject& args) {
    // Args-injected conv id wins over the captured-LOCAL getter so
    // wire-side per-client cascades list conversations from THEIR
    // current project, not the LOCAL CC's.
    QString activeConvId = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (activeConvId.isEmpty() && m_activeConvIdGetter) {
        activeConvId = m_activeConvIdGetter();
    }

    if (activeConvId.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("no active conversation")}};
    }

    const QList<Folder> chain = m_convService.folderChainForConversation(activeConvId);
    QString projectId;
    for (const Folder& f : chain) {
        if (f.isProject()) {
            projectId = f.id;
            break;
        }
    }
    if (projectId.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("current chat is not inside a project")}};
    }

    const QList<Conversation> convs = m_convService.listConversations(projectId);
    QJsonArray arr;
    for (const Conversation& c : convs) {
        if (c.id == activeConvId)
            continue;
        QJsonObject o;
        o[QStringLiteral("id")] = c.id;
        o[QStringLiteral("title")] = c.title;
        o[QStringLiteral("isGroup")] = c.isGroup;
        o[QStringLiteral("updatedAt")] = c.updatedAt.toString(Qt::ISODate);
        arr.append(o);
    }

    QJsonObject result;
    result[QStringLiteral("projectFolderId")] = projectId;
    result[QStringLiteral("conversations")] = arr;
    result[QStringLiteral("count")] = arr.size();
    return result;
}

}  // namespace Tools

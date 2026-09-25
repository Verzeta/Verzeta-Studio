// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file discover-skills-tool.cpp
 * @brief Implementation of the `discover_skills` tool body.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, ConversationService, Chat::CascadeController.
 */

#include "discover-skills-tool.h"

#include "../../models/conversation.h"
#include "../../services/chat/cascade-controller.h"
#include "../../services/conversation-service.h"
#include "../../services/skill-service.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>

namespace Tools {

QString DiscoverSkillsTool::name() const {
    return QStringLiteral("discover_skills");
}

QString DiscoverSkillsTool::description() const {
    return QStringLiteral("List approved skills available in the application library that "
                          "are NOT in your preferred list for this conversation. Use this "
                          "when the preferred skills do not match your task. Pagination "
                          "via `page` and `page_size`. Returns an empty result if the "
                          "active conversation is set to expose only its preferred list.");
}

QList<ToolParameterSchema> DiscoverSkillsTool::parameters() const {
    ToolParameterSchema query;
    query.name = QStringLiteral("query");
    query.type = QStringLiteral("string");
    query.description =
        QStringLiteral("Optional substring filter against id, description, and tags.");
    query.required = false;

    ToolParameterSchema page;
    page.name = QStringLiteral("page");
    page.type = QStringLiteral("integer");
    page.description = QStringLiteral("1-indexed page number (default 1).");
    page.required = false;

    ToolParameterSchema pageSize;
    pageSize.name = QStringLiteral("page_size");
    pageSize.type = QStringLiteral("integer");
    pageSize.description = QStringLiteral("Results per page (default 20, max 50).");
    pageSize.required = false;

    return {query, page, pageSize};
}

QJsonValue DiscoverSkillsTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("skill tool deps not configured")}};
    }
    // Args-injected conv id wins over the captured-LOCAL getter so
    // wire-side per-client cascades discover skills against THEIR
    // conversation's folder / project, not the LOCAL CC's.
    QString activeConv = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (activeConv.isEmpty())
        activeConv = m_deps.activeConvIdGetter();
    if (activeConv.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("no active responder context")}};
    }

    // Scope-aware enable check.
    const auto resolved = m_deps.skills->resolveForConversation(activeConv);
    if (resolved.exposeOnly) {
        return QJsonObject{{QStringLiteral("error"),
                            QStringLiteral("discover_skills disabled — only preferred skills "
                                           "are exposed in this scope")}};
    }

    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    int page = args.value(QStringLiteral("page")).toInt(1);
    int pageSize = args.value(QStringLiteral("page_size")).toInt(20);
    if (page < 1)
        page = 1;
    if (pageSize < 1)
        pageSize = 20;
    if (pageSize > 50)
        pageSize = 50;

    // Pull every approved skill and filter.
    const QVariantList approved = m_deps.skills->approvedSkills();
    QJsonArray matches;
    QString needle = query.toLower();
    for (const QVariant& v : approved) {
        const QVariantMap m = v.toMap();
        const QString id = m.value(QStringLiteral("id")).toString();
        const QString desc = m.value(QStringLiteral("description")).toString();
        const QStringList tags = m.value(QStringLiteral("tags")).toStringList();

        if (!needle.isEmpty()) {
            bool hit = id.toLower().contains(needle) || desc.toLower().contains(needle);
            if (!hit) {
                for (const QString& t : tags) {
                    if (t.toLower().contains(needle)) {
                        hit = true;
                        break;
                    }
                }
            }
            if (!hit)
                continue;
        }
        QJsonObject row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("description"), desc);
        row.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
        row.insert(QStringLiteral("version"), m.value(QStringLiteral("version")).toString());
        matches.append(row);
    }

    const int total = matches.size();
    const int begin = (page - 1) * pageSize;
    QJsonArray slice;
    for (int i = begin; i < total && i < begin + pageSize; ++i) {
        slice.append(matches.at(i));
    }

    QJsonObject out;
    out.insert(QStringLiteral("skills"), slice);
    out.insert(QStringLiteral("page"), page);
    out.insert(QStringLiteral("page_size"), pageSize);
    out.insert(QStringLiteral("total"), total);
    return out;
}

}  // namespace Tools

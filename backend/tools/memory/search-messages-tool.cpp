// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file search-messages-tool.cpp
 * @brief Implementation of Tools::SearchMessagesTool.
 * @layer Service (Tool subsystem)
 * @dependencies SearchService.
 */

#include "search-messages-tool.h"

#include "../../services/search-service.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

namespace Tools {

static constexpr int kDefaultLimit = 10;  ///< Results returned when no valid limit is given.
static constexpr int kMaxLimit = 30;      ///< Largest limit accepted.

SearchMessagesTool::SearchMessagesTool(SearchService& searchService)
    : m_searchService(searchService) {}

QString SearchMessagesTool::name() const {
    return QStringLiteral("search_messages");
}

QString SearchMessagesTool::description() const {
    return QStringLiteral("Searches past chat messages across conversations using full-text "
                          "search. Use this when you need to recall something said earlier "
                          "in the current chat, in a sibling chat within the same project, "
                          "or in any previous conversation. Returns matching snippets with "
                          "conversation titles and timestamps so you can decide what to "
                          "read in full via read_conversation.");
}

QList<ToolParameterSchema> SearchMessagesTool::parameters() const {
    ToolParameterSchema query;
    query.name = QStringLiteral("query");
    query.type = QStringLiteral("string");
    query.description = QStringLiteral("FTS5 query (supports AND, OR, NOT, \"phrase\", prefix*).");
    query.required = true;

    ToolParameterSchema limit;
    limit.name = QStringLiteral("limit");
    limit.type = QStringLiteral("integer");
    limit.description = QStringLiteral("Max results to return (default 10, capped at 30).");
    limit.required = false;

    return {query, limit};
}

bool SearchMessagesTool::runsOnMainThread() const {
    // SearchService::searchFullText touches SQLite; respect the
    // existing ToolDispatcher main-thread gate for database-bound
    // tools.
    return true;
}

QJsonValue SearchMessagesTool::invoke(const QJsonObject& args) {
    const QString query = args[QStringLiteral("query")].toString();
    int limit = args[QStringLiteral("limit")].toInt(kDefaultLimit);
    if (limit <= 0)
        limit = kDefaultLimit;
    if (limit > kMaxLimit)
        limit = kMaxLimit;

    if (query.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("query is required")}};
    }

    const QList<SearchResult> hits = m_searchService.searchFullText(query, limit);

    QJsonArray arr;
    for (const SearchResult& r : hits) {
        QJsonObject o;
        o[QStringLiteral("conversationId")] = r.conversationId;
        o[QStringLiteral("conversationTitle")] = r.conversationTitle;
        o[QStringLiteral("messageId")] = r.messageId;
        o[QStringLiteral("role")] = r.role;
        o[QStringLiteral("snippet")] = r.snippet;
        o[QStringLiteral("timestamp")] = r.timestamp.toString(Qt::ISODate);
        arr.append(o);
    }

    QJsonObject result;
    result[QStringLiteral("matches")] = arr;
    result[QStringLiteral("count")] = arr.size();
    return result;
}

}  // namespace Tools

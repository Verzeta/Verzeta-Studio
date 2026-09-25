// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file search-web-tool.cpp
 * @brief Implementation of Tools::SearchWebTool, which formats the result of
 *        Search::WebSearchService into the tool-call JSON the LLM sees.
 * @layer Service (Tool subsystem)
 * @dependencies Search::WebSearchService, Qt6::Core (QJsonArray,
 *               QJsonObject).
 */

#include "search-web-tool.h"

#include "../../services/search/web-search-service.h"
#include "../../services/search/web-search-types.h"

#include <QJsonArray>
#include <QJsonObject>

namespace Tools {

namespace {
QString statusToString(Search::WebSearchStatus s) {
    switch (s) {
        case Search::WebSearchStatus::Ok:
            return QStringLiteral("ok");
        case Search::WebSearchStatus::NoResults:
            return QStringLiteral("no_results");
        case Search::WebSearchStatus::Unavailable:
            return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}
}  // namespace

SearchWebTool::SearchWebTool(Search::WebSearchService& service) : m_service(service) {}

QString SearchWebTool::name() const {
    return QStringLiteral("search_web");
}

QString SearchWebTool::description() const {
    return QStringLiteral("Search the web for current information and return ranked results "
                          "(title, URL, snippet). Use it for facts, research, recent events, "
                          "or anything outside your training knowledge. If it returns no "
                          "results, proceed with what you know or ask the user — never invent "
                          "sources.");
}

QList<ToolParameterSchema> SearchWebTool::parameters() const {
    ToolParameterSchema query;
    query.name = QStringLiteral("query");
    query.type = QStringLiteral("string");
    query.description = QStringLiteral("Search query string");
    query.required = true;
    return {query};
}

bool SearchWebTool::runsOnMainThread() const {
    // WebSearchService performs a synchronous HTTPS round-trip — worker only.
    return false;
}

QJsonValue SearchWebTool::invoke(const QJsonObject& args) {
    const QString searchQuery = args[QStringLiteral("query")].toString();
    if (searchQuery.trimmed().isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("query is required")}};
    }

    const Search::WebSearchResponse r = m_service.search(searchQuery);

    QJsonObject result;
    result[QStringLiteral("query")] = r.query;
    result[QStringLiteral("status")] = statusToString(r.status);
    if (!r.providerId.isEmpty())
        result[QStringLiteral("provider")] = r.providerId;

    if (!r.results.isEmpty()) {
        QJsonArray arr;
        for (const Search::WebSearchResult& hit : r.results) {
            QJsonObject o;
            o[QStringLiteral("title")] = hit.title;
            o[QStringLiteral("url")] = hit.url;
            if (!hit.snippet.isEmpty())
                o[QStringLiteral("snippet")] = hit.snippet;
            arr.append(o);
        }
        result[QStringLiteral("results")] = arr;
    }

    if (!r.answer.isEmpty())
        result[QStringLiteral("answer")] = r.answer;
    if (!r.note.isEmpty())
        result[QStringLiteral("note")] = r.note;

    return result;
}

}  // namespace Tools

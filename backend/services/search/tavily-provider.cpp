// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tavily-provider.cpp
 * @brief Implementation of Search::TavilyProvider.
 * @layer Service (Search subsystem)
 * @dependencies web-search-http.h, Qt6::Core (QJson*).
 */

#include "tavily-provider.h"

#include "web-search-http.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Search {

namespace {
const QString kDefaultEndpoint = QStringLiteral("https://api.tavily.com/search");
}

QString TavilyProvider::id() const {
    return QStringLiteral("tavily");
}
QString TavilyProvider::displayName() const {
    return QStringLiteral("Tavily");
}
bool TavilyProvider::requiresApiKey() const {
    return true;
}

WebSearchResponse
TavilyProvider::parseTavily(const QByteArray& json, const QString& query, int maxResults) {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = QStringLiteral("tavily");

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("Tavily returned a response that could not be read.");
        return resp;
    }

    const QJsonObject root = doc.object();
    resp.answer = root.value(QStringLiteral("answer")).toString();

    const QJsonArray results = root.value(QStringLiteral("results")).toArray();
    for (const QJsonValue& rv : results) {
        if (resp.results.size() >= maxResults)
            break;
        const QJsonObject o = rv.toObject();
        const QString url = o.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
            continue;
        WebSearchResult r;
        r.title = o.value(QStringLiteral("title")).toString();
        r.url = url;
        r.snippet = o.value(QStringLiteral("content")).toString();
        resp.results.append(r);
    }

    resp.status = (!resp.results.isEmpty() || !resp.answer.isEmpty()) ? WebSearchStatus::Ok
                                                                      : WebSearchStatus::NoResults;
    return resp;
}

WebSearchResponse TavilyProvider::search(const WebSearchConfig& cfg, const QString& query) const {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = id();

    if (cfg.apiKey.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("The Tavily API key is missing. Add it on this card.");
        return resp;
    }

    QJsonObject body;
    body[QStringLiteral("query")] = query;
    body[QStringLiteral("max_results")] = cfg.maxResults > 0 ? cfg.maxResults : 8;
    body[QStringLiteral("include_answer")] = true;

    const QString endpoint = cfg.baseUrl.isEmpty() ? kDefaultEndpoint : cfg.baseUrl;
    const HttpResult res = postJson(
        endpoint,
        {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + cfg.apiKey.toUtf8()}},
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        cfg.timeoutMs);

    if (!res.error.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = res.error;
        return resp;
    }
    if (res.httpStatus != 200) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("Tavily HTTP %1").arg(res.httpStatus);
        return resp;
    }

    return parseTavily(res.body, query, cfg.maxResults > 0 ? cfg.maxResults : 8);
}

}  // namespace Search

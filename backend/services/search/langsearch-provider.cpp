// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file langsearch-provider.cpp
 * @brief Implementation of Search::LangSearchProvider.
 * @layer Service (Search subsystem)
 * @dependencies web-search-http.h, Qt6::Core (QJson*).
 */

#include "langsearch-provider.h"

#include "web-search-http.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Search {

namespace {
const QString kDefaultEndpoint = QStringLiteral("https://api.langsearch.com/v1/web-search");
}

QString LangSearchProvider::id() const {
    return QStringLiteral("langsearch");
}
QString LangSearchProvider::displayName() const {
    return QStringLiteral("LangSearch");
}
bool LangSearchProvider::requiresApiKey() const {
    return true;
}

WebSearchResponse
LangSearchProvider::parseLangSearch(const QByteArray& json, const QString& query, int maxResults) {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = QStringLiteral("langsearch");

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("LangSearch returned a response that could not be read.");
        return resp;
    }

    // Results nest under data.webPages.value[].
    const QJsonArray value = doc.object()
                                 .value(QStringLiteral("data"))
                                 .toObject()
                                 .value(QStringLiteral("webPages"))
                                 .toObject()
                                 .value(QStringLiteral("value"))
                                 .toArray();
    for (const QJsonValue& rv : value) {
        if (resp.results.size() >= maxResults)
            break;
        const QJsonObject o = rv.toObject();
        const QString url = o.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
            continue;
        WebSearchResult r;
        r.title = o.value(QStringLiteral("name")).toString();
        r.url = url;
        const QString summary = o.value(QStringLiteral("summary")).toString();
        r.snippet = summary.isEmpty() ? o.value(QStringLiteral("snippet")).toString() : summary;
        resp.results.append(r);
    }

    resp.status = resp.results.isEmpty() ? WebSearchStatus::NoResults : WebSearchStatus::Ok;
    return resp;
}

WebSearchResponse LangSearchProvider::search(const WebSearchConfig& cfg,
                                             const QString& query) const {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = id();

    if (cfg.apiKey.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason =
            QStringLiteral("The LangSearch API key is missing. Add it on this card.");
        return resp;
    }

    // LangSearch `count` is 1-10.
    int count = cfg.maxResults > 0 ? cfg.maxResults : 8;
    if (count > 10)
        count = 10;

    QJsonObject body;
    body[QStringLiteral("query")] = query;
    body[QStringLiteral("count")] = count;
    body[QStringLiteral("summary")] = false;

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
        resp.errorReason = QStringLiteral("LangSearch HTTP %1").arg(res.httpStatus);
        return resp;
    }

    return parseLangSearch(res.body, query, count);
}

}  // namespace Search

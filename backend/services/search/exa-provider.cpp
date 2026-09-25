// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file exa-provider.cpp
 * @brief Implementation of Search::ExaProvider.
 * @layer Service (Search subsystem)
 * @dependencies web-search-http.h, Qt6::Core (QJson*).
 */

#include "exa-provider.h"

#include "web-search-http.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Search {

namespace {
const QString kDefaultEndpoint = QStringLiteral("https://api.exa.ai/search");

QString snippetFromExaResult(const QJsonObject& o) {
    // Prefer highlights (relevant excerpts), then summary, then a clipped
    // slice of full text.
    const QJsonArray highlights = o.value(QStringLiteral("highlights")).toArray();
    if (!highlights.isEmpty()) {
        QStringList parts;
        for (const QJsonValue& h : highlights) {
            const QString s = h.toString().trimmed();
            if (!s.isEmpty())
                parts.append(s);
        }
        if (!parts.isEmpty())
            return parts.join(QStringLiteral(" … "));
    }
    const QString summary = o.value(QStringLiteral("summary")).toString();
    if (!summary.isEmpty())
        return summary;
    const QString text = o.value(QStringLiteral("text")).toString();
    return text.left(400);
}
}  // namespace

QString ExaProvider::id() const {
    return QStringLiteral("exa");
}
QString ExaProvider::displayName() const {
    return QStringLiteral("Exa");
}
bool ExaProvider::requiresApiKey() const {
    return true;
}

WebSearchResponse
ExaProvider::parseExa(const QByteArray& json, const QString& query, int maxResults) {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = QStringLiteral("exa");

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("Exa returned a response that could not be read.");
        return resp;
    }

    const QJsonArray results = doc.object().value(QStringLiteral("results")).toArray();
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
        r.snippet = snippetFromExaResult(o);
        resp.results.append(r);
    }

    resp.status = resp.results.isEmpty() ? WebSearchStatus::NoResults : WebSearchStatus::Ok;
    return resp;
}

WebSearchResponse ExaProvider::search(const WebSearchConfig& cfg, const QString& query) const {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = id();

    if (cfg.apiKey.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("The Exa API key is missing. Add it on this card.");
        return resp;
    }

    QJsonObject contents;
    contents[QStringLiteral("highlights")] = true;
    QJsonObject body;
    body[QStringLiteral("query")] = query;
    body[QStringLiteral("numResults")] = cfg.maxResults > 0 ? cfg.maxResults : 8;
    body[QStringLiteral("contents")] = contents;

    const QString endpoint = cfg.baseUrl.isEmpty() ? kDefaultEndpoint : cfg.baseUrl;
    const HttpResult res = postJson(endpoint,
                                    {{QByteArrayLiteral("x-api-key"), cfg.apiKey.toUtf8()}},
                                    QJsonDocument(body).toJson(QJsonDocument::Compact),
                                    cfg.timeoutMs);

    if (!res.error.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = res.error;
        return resp;
    }
    if (res.httpStatus != 200) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("Exa HTTP %1").arg(res.httpStatus);
        return resp;
    }

    return parseExa(res.body, query, cfg.maxResults > 0 ? cfg.maxResults : 8);
}

}  // namespace Search

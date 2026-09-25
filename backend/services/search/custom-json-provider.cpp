// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file custom-json-provider.cpp
 * @brief Implementation of Search::CustomJsonProvider.
 * @layer Service (Search subsystem)
 * @dependencies web-search-http.h, Qt6::Core (QJson*, QUrl).
 */

#include "custom-json-provider.h"

#include "web-search-http.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace Search {

namespace {

QString extraStr(const QVariantMap& m, const QString& key, const QString& def) {
    const QString v = m.value(key).toString().trimmed();
    return v.isEmpty() ? def : v;
}

}  // namespace

QString CustomJsonProvider::id() const {
    return QStringLiteral("custom");
}
QString CustomJsonProvider::displayName() const {
    return QStringLiteral("Custom (JSON API)");
}
bool CustomJsonProvider::requiresApiKey() const {
    return false;
}

WebSearchResponse CustomJsonProvider::parseCustom(const QByteArray& json,
                                                  const QString& resultsPath,
                                                  const QString& titleKey,
                                                  const QString& urlKey,
                                                  const QString& snippetKey,
                                                  const QString& query,
                                                  int maxResults) {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = QStringLiteral("custom");

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason =
            QStringLiteral("The custom endpoint returned JSON that could not be read.");
        return resp;
    }

    // Resolve the results array: empty path = a top-level JSON array;
    // otherwise descend the dot-path through nested objects.
    QJsonArray results;
    if (resultsPath.trimmed().isEmpty()) {
        results = doc.array();
    } else {
        QJsonValue cur = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
        const QStringList parts = resultsPath.split(QLatin1Char('.'), Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            cur = cur.toObject().value(part);
        }
        results = cur.toArray();
    }

    const QString tk = titleKey.isEmpty() ? QStringLiteral("title") : titleKey;
    const QString uk = urlKey.isEmpty() ? QStringLiteral("url") : urlKey;
    const QString sk = snippetKey.isEmpty() ? QStringLiteral("content") : snippetKey;

    for (const QJsonValue& rv : results) {
        if (resp.results.size() >= maxResults)
            break;
        const QJsonObject o = rv.toObject();
        const QString url = o.value(uk).toString();
        if (url.isEmpty())
            continue;
        WebSearchResult r;
        r.title = o.value(tk).toString();
        r.url = url;
        r.snippet = o.value(sk).toString();
        resp.results.append(r);
    }

    resp.status = resp.results.isEmpty() ? WebSearchStatus::NoResults : WebSearchStatus::Ok;
    return resp;
}

WebSearchResponse CustomJsonProvider::search(const WebSearchConfig& cfg,
                                             const QString& query) const {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = id();

    const QVariantMap& x = cfg.extra;
    const QString url = x.value(QStringLiteral("url")).toString().trimmed();
    if (url.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("The custom endpoint has no URL. "
                                          "Enter one on this card.");
        return resp;
    }

    const QString method = extraStr(x, QStringLiteral("method"), QStringLiteral("GET")).toUpper();
    const QString auth = extraStr(x, QStringLiteral("auth"), QStringLiteral("none")).toLower();
    const QString authHdr = extraStr(x, QStringLiteral("authHeader"), QStringLiteral("x-api-key"));
    const QString authQp = extraStr(x, QStringLiteral("authQueryParam"), QStringLiteral("key"));
    const QString queryKey = extraStr(x, QStringLiteral("queryParam"), QStringLiteral("q"));
    const QString countKey = x.value(QStringLiteral("countParam")).toString().trimmed();
    const int maxResults = cfg.maxResults > 0 ? cfg.maxResults : 8;

    const QString resultsPath = x.value(QStringLiteral("resultsPath")).toString();
    const QString titleKey = x.value(QStringLiteral("titleKey")).toString();
    const QString urlKey = x.value(QStringLiteral("urlKey")).toString();
    const QString snippetKey = x.value(QStringLiteral("snippetKey")).toString();

    QList<QPair<QByteArray, QByteArray>> headers;
    if (auth == QStringLiteral("bearer") && !cfg.apiKey.isEmpty()) {
        headers.append({QByteArrayLiteral("Authorization"),
                        QByteArrayLiteral("Bearer ") + cfg.apiKey.toUtf8()});
    } else if (auth == QStringLiteral("header") && !cfg.apiKey.isEmpty()) {
        headers.append({authHdr.toUtf8(), cfg.apiKey.toUtf8()});
    }

    HttpResult res;
    if (method == QStringLiteral("POST")) {
        QJsonObject body;
        body.insert(queryKey, query);
        if (!countKey.isEmpty())
            body.insert(countKey, maxResults);
        res = postJson(
            url, headers, QJsonDocument(body).toJson(QJsonDocument::Compact), cfg.timeoutMs);
    } else {
        QUrl u(url);
        QUrlQuery q(u);
        q.addQueryItem(queryKey, query);
        if (!countKey.isEmpty())
            q.addQueryItem(countKey, QString::number(maxResults));
        if (auth == QStringLiteral("query") && !cfg.apiKey.isEmpty())
            q.addQueryItem(authQp, cfg.apiKey);
        u.setQuery(q);
        res = getRequest(u.toString(QUrl::FullyEncoded), headers, cfg.timeoutMs);
    }

    if (!res.error.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = res.error;
        return resp;
    }
    if (res.httpStatus != 200) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("custom endpoint HTTP %1").arg(res.httpStatus);
        return resp;
    }

    return parseCustom(res.body, resultsPath, titleKey, urlKey, snippetKey, query, maxResults);
}

}  // namespace Search

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file searxng-provider.cpp
 * @brief Implementation of Search::SearxngProvider.
 * @layer Service (Search subsystem)
 * @dependencies Qt6::Network, Qt6::Core (QJson*).
 */

#include "searxng-provider.h"

#include <QTimer>

#include <QByteArray>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace Search {

QString SearxngProvider::id() const {
    return QStringLiteral("searxng");
}
QString SearxngProvider::displayName() const {
    return QStringLiteral("SearXNG (self-hosted)");
}
bool SearxngProvider::requiresApiKey() const {
    return false;
}

WebSearchResponse
SearxngProvider::parseSearxng(const QByteArray& json, const QString& query, int maxResults) {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = QStringLiteral("searxng");

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("SearXNG returned a response that could not be read.");
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
        r.snippet = o.value(QStringLiteral("content")).toString();
        resp.results.append(r);
    }

    resp.status = resp.results.isEmpty() ? WebSearchStatus::NoResults : WebSearchStatus::Ok;
    return resp;
}

WebSearchResponse SearxngProvider::search(const WebSearchConfig& cfg, const QString& query) const {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = id();

    if (cfg.baseUrl.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason =
            QStringLiteral("No SearXNG base URL is set. Enter your instance URL on this card.");
        return resp;
    }

    // Normalise: strip a trailing slash, then append /search?q=…&format=json.
    QString base = cfg.baseUrl;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    const QUrl url(base + QStringLiteral("/search?q=%1&format=json")
                              .arg(QString::fromUtf8(QUrl::toPercentEncoding(query))));

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Verzeta/1.0"));
    if (!cfg.apiKey.isEmpty()) {
        req.setRawHeader(QByteArrayLiteral("Authorization"),
                         QByteArrayLiteral("Bearer ") + cfg.apiKey.toUtf8());
    }

    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(cfg.timeoutMs > 0 ? cfg.timeoutMs : 15000, &loop, &QEventLoop::quit);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("The request timed out after %1 ms.").arg(cfg.timeoutMs);
        reply->deleteLater();
        return resp;
    }
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && httpStatus != 200) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = reply->errorString();
        reply->deleteLater();
        return resp;
    }
    const QByteArray payload = reply->readAll();
    reply->deleteLater();

    if (httpStatus != 200) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("SearXNG HTTP %1").arg(httpStatus);
        return resp;
    }

    return parseSearxng(payload, query, cfg.maxResults > 0 ? cfg.maxResults : 8);
}

}  // namespace Search

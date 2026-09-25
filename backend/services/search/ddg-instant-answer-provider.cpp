// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ddg-instant-answer-provider.cpp
 * @brief Implementation of Search::DdgInstantAnswerProvider.
 * @layer Service (Search subsystem)
 * @dependencies Qt6::Network (QNetworkAccessManager, QNetworkReply),
 *               Qt6::Core (QEventLoop, QTimer, QJsonDocument).
 */

#include "ddg-instant-answer-provider.h"

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

QString DdgInstantAnswerProvider::id() const {
    return QStringLiteral("ddg-instant");
}

QString DdgInstantAnswerProvider::displayName() const {
    return QStringLiteral("DuckDuckGo Instant Answers");
}

bool DdgInstantAnswerProvider::requiresApiKey() const {
    return false;
}

WebSearchResponse DdgInstantAnswerProvider::parseInstantAnswer(const QByteArray& json,
                                                               const QString& query,
                                                               int maxResults) {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = QStringLiteral("ddg-instant");

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("Failed to parse Instant Answer JSON");
        return resp;
    }

    const QJsonObject ddg = doc.object();

    const QString abstract = ddg.value(QStringLiteral("AbstractText")).toString();
    const QString answer = ddg.value(QStringLiteral("Answer")).toString();
    const QString heading = ddg.value(QStringLiteral("Heading")).toString();
    const QString absUrl = ddg.value(QStringLiteral("AbstractURL")).toString();

    if (!answer.isEmpty()) {
        resp.answer = answer;
    } else if (!abstract.isEmpty()) {
        resp.answer = abstract;
    }

    // The abstract (with its source URL + heading) becomes the first result.
    if (!abstract.isEmpty() && !absUrl.isEmpty()) {
        WebSearchResult r;
        r.title = heading.isEmpty() ? query : heading;
        r.url = absUrl;
        r.snippet = abstract;
        resp.results.append(r);
    }

    const QJsonArray related = ddg.value(QStringLiteral("RelatedTopics")).toArray();
    for (const QJsonValue& rv : related) {
        if (resp.results.size() >= maxResults)
            break;
        const QJsonObject rt = rv.toObject();
        const QString text = rt.value(QStringLiteral("Text")).toString();
        const QString firstUrl = rt.value(QStringLiteral("FirstURL")).toString();
        if (text.isEmpty() || firstUrl.isEmpty())
            continue;
        WebSearchResult r;
        // RelatedTopics have no separate title; use the leading clause as
        // the title and the full text as the snippet.
        const int dash = text.indexOf(QStringLiteral(" - "));
        r.title = (dash > 0) ? text.left(dash) : text;
        r.url = firstUrl;
        r.snippet = text;
        resp.results.append(r);
    }

    const bool any = !resp.results.isEmpty() || !resp.answer.isEmpty();
    resp.status = any ? WebSearchStatus::Ok : WebSearchStatus::NoResults;
    return resp;
}

WebSearchResponse DdgInstantAnswerProvider::search(const WebSearchConfig& cfg,
                                                   const QString& query) const {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = id();

    const QUrl url(QStringLiteral("https://api.duckduckgo.com/?q=%1&format=json"
                                  "&no_html=1&skip_disambig=1")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(query))));

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Verzeta/1.0"));

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
    if (reply->error() != QNetworkReply::NoError) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = reply->errorString();
        reply->deleteLater();
        return resp;
    }

    const QByteArray payload = reply->readAll();
    reply->deleteLater();

    const int maxResults = cfg.maxResults > 0 ? cfg.maxResults : 8;
    return parseInstantAnswer(payload, query, maxResults);
}

}  // namespace Search

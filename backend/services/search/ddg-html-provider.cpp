// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ddg-html-provider.cpp
 * @brief Implementation of Search::DdgHtmlProvider.
 * @layer Service (Search subsystem)
 * @dependencies Qt6::Network (QNetworkAccessManager, QNetworkReply),
 *               Qt6::Core (QEventLoop, QTimer, QRegularExpression).
 */

#include "ddg-html-provider.h"

#include "web-search-util.h"

#include <QTimer>

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

namespace Search {

QString DdgHtmlProvider::id() const {
    return QStringLiteral("ddg-html");
}

QString DdgHtmlProvider::displayName() const {
    return QStringLiteral("DuckDuckGo (web results)");
}

bool DdgHtmlProvider::requiresApiKey() const {
    return false;
}

WebSearchResponse
DdgHtmlProvider::parseLite(const QByteArray& html, const QString& query, int maxResults) {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = QStringLiteral("ddg-html");

    const QString page = QString::fromUtf8(html);

    // Result anchors: <a … href="URL" … class='result-link'>TITLE</a>.
    // The lite page uses single-quoted class attributes and places href
    // before class. DotMatchesEverything so a title that wraps lines is
    // still captured; non-greedy to stop at the first </a>.
    static const QRegularExpression linkRe(
        QStringLiteral("<a[^>]*href=\"([^\"]+)\"[^>]*class='result-link'[^>]*>"
                       "(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption);

    // Snippet cells: <td class='result-snippet'> … </td>.
    static const QRegularExpression snippetRe(
        QStringLiteral("<td[^>]*class='result-snippet'[^>]*>(.*?)</td>"),
        QRegularExpression::DotMatchesEverythingOption);

    QStringList snippets;
    {
        auto it = snippetRe.globalMatch(page);
        while (it.hasNext()) {
            snippets.append(stripHtmlToText(it.next().captured(1)));
        }
    }

    int idx = 0;
    auto it = linkRe.globalMatch(page);
    while (it.hasNext() && resp.results.size() < maxResults) {
        const QRegularExpressionMatch m = it.next();
        const QString url = unwrapDdgRedirect(m.captured(1).trimmed());
        const QString title = stripHtmlToText(m.captured(2));
        if (url.isEmpty() || title.isEmpty()) {
            ++idx;
            continue;
        }
        WebSearchResult r;
        r.title = title;
        r.url = url;
        r.snippet = (idx < snippets.size()) ? snippets.at(idx) : QString();
        resp.results.append(r);
        ++idx;
    }

    resp.status = resp.results.isEmpty() ? WebSearchStatus::NoResults : WebSearchStatus::Ok;
    return resp;
}

WebSearchResponse DdgHtmlProvider::search(const WebSearchConfig& cfg, const QString& query) const {
    WebSearchResponse resp;
    resp.query = query;
    resp.providerId = id();

    QNetworkAccessManager nam;
    QUrl url(QStringLiteral("https://lite.duckduckgo.com/lite/"));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    // A realistic UA — the lite endpoint serves an empty body to clients
    // it does not recognise.
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Mozilla/5.0 (X11; Linux x86_64; rv:128.0) "
                                 "Gecko/20100101 Firefox/128.0"));

    QByteArray body = "q=";
    body += QUrl::toPercentEncoding(query);

    QNetworkReply* reply = nam.post(req, body);

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

    // A non-200 (DDG serves HTTP 202 with an "anomaly"/challenge page when
    // it rate-limits scraping) is "blocked", not "no results for this
    // query" — surface it as Unavailable so the service falls back and the
    // model is told search is unreachable rather than that nothing matched.
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    reply->deleteLater();

    if (httpStatus != 200) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("DuckDuckGo returned HTTP %1. It is probably "
                                          "rate-limiting requests; try again later.")
                               .arg(httpStatus);
        return resp;
    }

    const int maxResults = cfg.maxResults > 0 ? cfg.maxResults : 8;
    return parseLite(payload, query, maxResults);
}

}  // namespace Search

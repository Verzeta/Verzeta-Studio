// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file web-search-http.cpp
 * @brief Implementation of the shared synchronous JSON-POST helper.
 * @layer Service (Search subsystem)
 * @dependencies Qt6::Network (QNetworkAccessManager, QNetworkReply),
 *               Qt6::Core (QEventLoop, QTimer).
 */

#include "web-search-http.h"

#include <QTimer>

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace Search {

HttpResult postJson(const QString& url,
                    const QList<QPair<QByteArray, QByteArray>>& headers,
                    const QByteArray& jsonBody,
                    int timeoutMs) {
    HttpResult out;

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    for (const auto& h : headers) {
        req.setRawHeader(h.first, h.second);
    }

    QNetworkReply* reply = nam.post(req, jsonBody);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs > 0 ? timeoutMs : 15000, &loop, &QEventLoop::quit);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        out.error = QStringLiteral("The request timed out after %1 ms.").arg(timeoutMs);
        reply->deleteLater();
        return out;
    }

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && out.httpStatus == 0) {
        // Transport-level failure with no HTTP response at all.
        out.error = reply->errorString();
        reply->deleteLater();
        return out;
    }

    out.body = reply->readAll();
    reply->deleteLater();
    return out;
}

HttpResult
getRequest(const QString& url, const QList<QPair<QByteArray, QByteArray>>& headers, int timeoutMs) {
    HttpResult out;

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Verzeta/1.0"));
    for (const auto& h : headers) {
        req.setRawHeader(h.first, h.second);
    }

    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs > 0 ? timeoutMs : 15000, &loop, &QEventLoop::quit);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        out.error = QStringLiteral("The request timed out after %1 ms.").arg(timeoutMs);
        reply->deleteLater();
        return out;
    }
    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && out.httpStatus == 0) {
        out.error = reply->errorString();
        reply->deleteLater();
        return out;
    }
    out.body = reply->readAll();
    reply->deleteLater();
    return out;
}

}  // namespace Search

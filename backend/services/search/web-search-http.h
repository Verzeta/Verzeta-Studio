// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file web-search-http.h
 * @brief Shared synchronous JSON-POST helper for keyed web-search
 *        providers (Tavily / Exa / LangSearch), so each provider holds
 *        only its request-shaping + response-parsing logic, not the QNAM
 *        boilerplate.
 * @layer Service (Search subsystem)
 * @dependencies Qt6::Network, Qt6::Core.
 */
#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

namespace Search {

/**
 * @brief Result of a synchronous HTTP request.
 */
struct HttpResult {
    int httpStatus = 0;  ///< HTTP status code (0 if no response).
    QByteArray body;     ///< Response body (empty on transport error).
    QString error;       ///< Transport/timeout error string; empty on
                         ///< a completed response (any HTTP status).
};

/**
 * @brief POST a JSON body and return the raw response. Synchronous; safe
 *        on a worker thread (creates a local QNetworkAccessManager).
 * @param url Endpoint URL.
 * @param headers Extra request headers as (name, value) byte pairs
 *                (e.g. {"Authorization", "Bearer ..."}). Content-Type is
 *                set to application/json automatically.
 * @param jsonBody UTF-8 JSON request body.
 * @param timeoutMs Abort the request after this many milliseconds.
 * @returns HttpResult with the status code + body, or a populated `error`
 *          on transport failure / timeout.
 */
HttpResult postJson(const QString& url,
                    const QList<QPair<QByteArray, QByteArray>>& headers,
                    const QByteArray& jsonBody,
                    int timeoutMs);

/**
 * @brief GET a URL and return the raw response. Synchronous; safe on a
 *        worker thread (local QNetworkAccessManager).
 * @param url Full request URL (query string already encoded).
 * @param headers Extra request headers as (name, value) byte pairs.
 * @param timeoutMs Abort the request after this many milliseconds.
 * @returns HttpResult with the status code + body, or a populated `error`
 *          on transport failure / timeout.
 */
HttpResult
getRequest(const QString& url, const QList<QPair<QByteArray, QByteArray>>& headers, int timeoutMs);

}  // namespace Search

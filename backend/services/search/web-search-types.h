// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file web-search-types.h
 * @brief Plain data types for the pluggable web-search subsystem: the
 *        result/response/config structs shared by every web-search
 *        provider, the WebSearchService, and the search_web tool.
 * @layer Service (Search subsystem)
 * @dependencies Qt6::Core (QString, QList).
 *
 * These are POD-like value types with no Qt object identity; they are
 * copied freely across threads (the search_web tool runs on a worker
 * thread and receives a WebSearchResponse by value).
 */
#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

namespace Search {

/**
 * @brief One web-search hit.
 */
struct WebSearchResult {
    QString title;    ///< Result title (HTML stripped, entities decoded).
    QString url;      ///< Absolute result URL.
    QString snippet;  ///< Short summary text (HTML stripped, entities decoded).
};

/**
 * @brief Outcome class for a web search.
 *
 * Drives how the search_web tool frames the result to the model:
 * Ok = real results; NoResults = the query ran but surfaced nothing;
 * Unavailable = the backend failed (network/timeout/parse/credentials).
 */
enum class WebSearchStatus {
    Ok,
    NoResults,
    Unavailable,
};

/**
 * @brief The full response from a web search.
 *
 * Always well-formed even on failure: on NoResults/Unavailable the
 * `note` carries explicit guidance for the model (proceed with your own
 * knowledge / tell the user; do not fabricate) so a failed search never
 * derails the turn the way a bare error does.
 */
struct WebSearchResponse {
    WebSearchStatus status = WebSearchStatus::NoResults;  ///< Outcome of the search.
    QString query;                                        ///< Echoed query.
    QList<WebSearchResult> results;                       ///< Ordered hits (may be empty).
    QString answer;                                       ///< Optional direct/instant answer.
    QString providerId;                                   ///< Backend that produced this.
    QString note;                                         ///< Model-facing guidance.
    QString errorReason;                                  ///< Internal diagnostic (optional).
};

/**
 * @brief Static descriptor of a registered backend, for selection UI/wire.
 */
struct WebSearchProviderInfo {
    QString id;                   ///< Stable id (e.g. "tavily").
    QString displayName;          ///< Human-readable name.
    bool requiresApiKey = false;  ///< Whether a host-side key is needed.
};

/**
 * @brief Resolved, immutable-per-call configuration for one search.
 *
 * WebSearchService snapshots this under its mutex before dispatching, so
 * a provider's search() reads a stable copy on the worker thread while
 * the main thread is free to change the active configuration.
 */
struct WebSearchConfig {
    QString providerId;     ///< Active provider id (e.g. "ddg-html").
    QString baseUrl;        ///< Override/self-host base URL (SearXNG).
    QString apiKey;         ///< Resolved key (host-side only); may be empty.
    int maxResults = 8;     ///< Upper bound on returned hits.
    int timeoutMs = 15000;  ///< Per-request network timeout.
    QVariantMap extra;      ///< Provider-specific config (the Custom JSON
                            ///< provider reads url/method/auth/query +
                            ///< response field-mapping from here).
};

}  // namespace Search

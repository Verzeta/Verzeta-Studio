// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ddg-html-provider.h
 * @brief Web-search backend that scrapes DuckDuckGo's HTML "lite"
 *        endpoint for real SERP results. No API key required.
 * @layer Service (Search subsystem)
 * @dependencies iweb-search-provider.h, Qt6::Network.
 *
 * Unlike the Instant Answer API (which only answers definitions/entities),
 * the lite endpoint returns ranked general web results. The parser is a
 * pure static method (parseLite) so it is unit-tested against a captured
 * real page with no network. Stateless and worker-thread safe.
 */
#pragma once

#include "iweb-search-provider.h"

namespace Search {

/**
 * @brief DuckDuckGo lite-HTML scraping provider.
 */
class DdgHtmlProvider : public IWebSearchProvider {
  public:
    /** @brief Backend id. @returns "ddg-html". */
    QString id() const override;
    /** @brief Display name. @returns "DuckDuckGo (web results)". */
    QString displayName() const override;
    /** @brief No key needed. @returns false. */
    bool requiresApiKey() const override;

    /**
     * @brief POST the query to lite.duckduckgo.com and parse the results.
     * @param cfg Resolved config (maxResults + timeoutMs honoured; apiKey
     *            unused).
     * @param query Search text.
     * @returns Ok with results on success; NoResults if the page parsed
     *          but held no rows; Unavailable on network/timeout/HTTP error.
     * @sideeffects One HTTPS POST via a locally-created QNAM. No state.
     */
    WebSearchResponse search(const WebSearchConfig& cfg, const QString& query) const override;

    /**
     * @brief Pure parser for the lite-HTML result page (testable, no I/O).
     * @param html Raw lite-endpoint HTML bytes.
     * @param query Echoed into the response.
     * @param maxResults Upper bound on returned hits.
     * @returns Parsed response (Ok/NoResults; never Unavailable, because failure
     *          to fetch is the caller's concern).
     */
    static WebSearchResponse
    parseLite(const QByteArray& html, const QString& query, int maxResults);
};

}  // namespace Search

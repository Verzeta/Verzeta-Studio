// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tavily-provider.h
 * @brief Web-search backend for the Tavily Search API (LLM-oriented).
 * @layer Service (Search subsystem)
 * @dependencies iweb-search-provider.h, web-search-http.h.
 *
 * POST https://api.tavily.com/search, Bearer auth, JSON body
 * {query, max_results, include_answer}. Response: {answer?, results:[
 * {title, url, content}]}. Pure parseTavily() is unit-tested against the
 * documented example payload; live calls need the user's API key (a
 * spike, not a CI gate). Stateless / worker-thread safe.
 */
#pragma once

#include "iweb-search-provider.h"

namespace Search {

/** @brief Tavily Search API provider. */
class TavilyProvider : public IWebSearchProvider {
  public:
    /** @brief Backend id. @returns "tavily". */
    QString id() const override;
    /** @brief Display name. @returns "Tavily". */
    QString displayName() const override;
    /** @brief Requires a key. @returns true. */
    bool requiresApiKey() const override;

    /**
     * @brief Query the Tavily API.
     * @param cfg Resolved config (apiKey required; baseUrl optional
     *            override; maxResults + timeoutMs honoured).
     * @param query Search text.
     * @returns Ok with results; NoResults if the API returned none;
     *          Unavailable on missing key / HTTP / transport error.
     */
    WebSearchResponse search(const WebSearchConfig& cfg, const QString& query) const override;

    /**
     * @brief Pure parser for a Tavily response (testable, no I/O).
     * @param json Raw Tavily response bytes.
     * @param query Echoed into the response.
     * @param maxResults Upper bound on returned hits.
     * @returns Parsed response (Ok/NoResults; Unavailable on malformed JSON).
     */
    static WebSearchResponse
    parseTavily(const QByteArray& json, const QString& query, int maxResults);
};

}  // namespace Search

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file langsearch-provider.h
 * @brief Web-search backend for the LangSearch web-search API.
 * @layer Service (Search subsystem)
 * @dependencies iweb-search-provider.h, web-search-http.h.
 *
 * POST https://api.langsearch.com/v1/web-search, Bearer auth, JSON body
 * {query, count}. Response nests results under data.webPages.value[],
 * each {name, url, snippet, summary?}. Pure parseLangSearch() is
 * unit-tested; live calls need the user's API key. Stateless / worker
 * thread safe.
 */
#pragma once

#include "iweb-search-provider.h"

namespace Search {

/** @brief LangSearch web-search API provider. */
class LangSearchProvider : public IWebSearchProvider {
  public:
    /** @brief Backend id. @returns "langsearch". */
    QString id() const override;
    /** @brief Display name. @returns "LangSearch". */
    QString displayName() const override;
    /** @brief Requires a key. @returns true. */
    bool requiresApiKey() const override;

    /**
     * @brief Query the LangSearch API.
     * @param cfg Resolved config (apiKey required; baseUrl optional;
     *            count from maxResults capped at 10; timeoutMs honoured).
     * @param query Search text.
     * @returns Ok/NoResults/Unavailable as per the response.
     */
    WebSearchResponse search(const WebSearchConfig& cfg, const QString& query) const override;

    /**
     * @brief Pure parser for a LangSearch response (testable, no I/O).
     * @param json Raw LangSearch response bytes.
     * @param query Echoed into the response.
     * @param maxResults Upper bound on returned hits.
     * @returns Parsed response (Ok/NoResults; Unavailable on malformed JSON).
     */
    static WebSearchResponse
    parseLangSearch(const QByteArray& json, const QString& query, int maxResults);
};

}  // namespace Search

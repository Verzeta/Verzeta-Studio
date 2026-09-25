// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file searxng-provider.h
 * @brief Web-search backend for a self-hosted SearXNG instance.
 * @layer Service (Search subsystem)
 * @dependencies iweb-search-provider.h, Qt6::Network.
 *
 * GET \<baseUrl\>/search?q=\<query\>&format=json (the instance must have the
 * JSON output format enabled). Response: {results:[{title,url,content}]}.
 * No API key by default; an optional bearer token is sent if the user
 * configured one (some instances sit behind a reverse-proxy auth). The
 * base URL is REQUIRED (it is the user's own instance); without it
 * search() returns Unavailable. Pure parseSearxng() is unit-tested.
 */
#pragma once

#include "iweb-search-provider.h"

namespace Search {

/** @brief Self-hosted SearXNG metasearch provider. */
class SearxngProvider : public IWebSearchProvider {
  public:
    /** @brief Backend id. @returns "searxng". */
    QString id() const override;
    /** @brief Display name. @returns "SearXNG (self-hosted)". */
    QString displayName() const override;
    /** @brief No key required (base URL is what's required). @returns false. */
    bool requiresApiKey() const override;

    /**
     * @brief Query a SearXNG instance.
     * @param cfg Resolved config (baseUrl REQUIRED; apiKey optional bearer;
     *            maxResults + timeoutMs honoured).
     * @param query Search text.
     * @returns Ok/NoResults/Unavailable; Unavailable when no base URL is set.
     */
    WebSearchResponse search(const WebSearchConfig& cfg, const QString& query) const override;

    /**
     * @brief Pure parser for a SearXNG JSON response (testable, no I/O).
     * @param json Raw SearXNG response bytes.
     * @param query Echoed into the response.
     * @param maxResults Upper bound on returned hits.
     * @returns Parsed response (Ok/NoResults; Unavailable on malformed JSON).
     */
    static WebSearchResponse
    parseSearxng(const QByteArray& json, const QString& query, int maxResults);
};

}  // namespace Search

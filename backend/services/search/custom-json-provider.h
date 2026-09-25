// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file custom-json-provider.h
 * @brief User-configurable web-search backend for any JSON REST search API
 *        (Serper, Google CSE, Bing API, Brave, a Tavily/SearXNG-compatible
 *        gateway, …). Reads its request shape + response field-mapping from
 *        WebSearchConfig::extra, so the user can point it at an arbitrary
 *        endpoint without code changes.
 * @layer Service (Search subsystem)
 * @dependencies iweb-search-provider.h, web-search-http.h.
 *
 * Config keys consumed from cfg.extra (all strings unless noted):
 *   url:             endpoint (required)
 *   method:          "GET" (default) | "POST"
 *   auth:            "none" (default) | "bearer" | "header" | "query"
 *   authHeader:      header name when auth=="header" (default "x-api-key")
 *   authQueryParam:  query param name when auth=="query" (default "key")
 *   queryParam:      GET query param / POST body field for the query (default "q")
 *   countParam:      optional GET query param / POST body field for max results
 *   resultsPath:     dot-path to the results array (default "results"; empty =
 *                    a top-level JSON array)
 *   titleKey/urlKey/snippetKey: per-result field names (defaults
 *                    "title"/"url"/"content")
 *
 * JSON-only (no HTML scraping). Stateless / worker-thread safe.
 */
#pragma once

#include "iweb-search-provider.h"

namespace Search {

/** @brief Configurable JSON-API web-search provider. */
class CustomJsonProvider : public IWebSearchProvider {
  public:
    /** @brief Backend id. @returns "custom". */
    QString id() const override;
    /** @brief Display name. @returns "Custom (JSON API)". */
    QString displayName() const override;
    /** @brief Auth is configurable, so a key is not categorically required.
     *  @returns false. */
    bool requiresApiKey() const override;

    /**
     * @brief Query the user-configured endpoint and map the response.
     * @param cfg Config; cfg.extra carries the request/response mapping,
     *            cfg.apiKey the (optional) key.
     * @param query Search text.
     * @returns Ok/NoResults/Unavailable; Unavailable when no URL is set or
     *          on HTTP/transport/parse error.
     */
    WebSearchResponse search(const WebSearchConfig& cfg, const QString& query) const override;

    /**
     * @brief Pure parser: navigate resultsPath to the array and map each
     *        hit's title/url/snippet by the configured keys (testable, no I/O).
     * @param json Raw response bytes.
     * @param resultsPath Dot-path to the results array (empty = top-level array).
     * @param titleKey Per-result title field.
     * @param urlKey Per-result URL field.
     * @param snippetKey Per-result snippet field.
     * @param query Echoed into the response.
     * @param maxResults Upper bound on hits.
     * @returns Parsed response (Ok/NoResults; Unavailable on malformed JSON).
     */
    static WebSearchResponse parseCustom(const QByteArray& json,
                                         const QString& resultsPath,
                                         const QString& titleKey,
                                         const QString& urlKey,
                                         const QString& snippetKey,
                                         const QString& query,
                                         int maxResults);
};

}  // namespace Search

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file exa-provider.h
 * @brief Web-search backend for the Exa search API (neural search).
 * @layer Service (Search subsystem)
 * @dependencies iweb-search-provider.h, web-search-http.h.
 *
 * POST https://api.exa.ai/search, `x-api-key` auth, JSON body
 * {query, numResults, contents:{highlights:true}}. Response: {results:[
 * {title, url, highlights[], summary?, text?}]}. Snippet is built from
 * highlights (else summary/text). Pure parseExa() is unit-tested; live
 * calls need the user's API key. Stateless / worker-thread safe.
 */
#pragma once

#include "iweb-search-provider.h"

namespace Search {

/** @brief Exa search API provider. */
class ExaProvider : public IWebSearchProvider {
  public:
    /** @brief Backend id. @returns "exa". */
    QString id() const override;
    /** @brief Display name. @returns "Exa". */
    QString displayName() const override;
    /** @brief Requires a key. @returns true. */
    bool requiresApiKey() const override;

    /**
     * @brief Query the Exa API.
     * @param cfg Resolved config (apiKey required; baseUrl optional;
     *            numResults from maxResults; timeoutMs honoured).
     * @param query Search text.
     * @returns Ok/NoResults/Unavailable as per the response.
     */
    WebSearchResponse search(const WebSearchConfig& cfg, const QString& query) const override;

    /**
     * @brief Pure parser for an Exa response (testable, no I/O).
     * @param json Raw Exa response bytes.
     * @param query Echoed into the response.
     * @param maxResults Upper bound on returned hits.
     * @returns Parsed response (Ok/NoResults; Unavailable on malformed JSON).
     */
    static WebSearchResponse parseExa(const QByteArray& json, const QString& query, int maxResults);
};

}  // namespace Search

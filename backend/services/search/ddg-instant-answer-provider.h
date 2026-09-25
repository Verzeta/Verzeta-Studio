// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ddg-instant-answer-provider.h
 * @brief Web-search backend wrapping DuckDuckGo's Instant Answer API.
 * @layer Service (Search subsystem)
 * @dependencies iweb-search-provider.h, Qt6::Network.
 *
 * This is the app's original `search_web` behaviour preserved as a
 * selectable backend. The Instant Answer API only knows direct factual
 * answers (definitions, calculator queries, Wikipedia abstracts, well-
 * known entities), so it returns nothing for most natural-language
 * queries. It is best used as a FALLBACK / answer-augmentation source,
 * not the primary general-web backend. Parser is a pure static method.
 */
#pragma once

#include "iweb-search-provider.h"

namespace Search {

/**
 * @brief DuckDuckGo Instant Answer API provider.
 */
class DdgInstantAnswerProvider : public IWebSearchProvider {
  public:
    /** @brief Backend id. @returns "ddg-instant". */
    QString id() const override;
    /** @brief Display name. @returns "DuckDuckGo Instant Answers". */
    QString displayName() const override;
    /** @brief No key needed. @returns false. */
    bool requiresApiKey() const override;

    /**
     * @brief Query the Instant Answer API and parse the digest.
     * @param cfg Resolved config (timeoutMs honoured; apiKey unused).
     * @param query Search text.
     * @returns Ok if any answer/abstract/related topic was found;
     *          NoResults if the API had no instant answer; Unavailable
     *          on network/timeout/parse error.
     * @sideeffects One HTTPS GET via a locally-created QNAM. No state.
     */
    WebSearchResponse search(const WebSearchConfig& cfg, const QString& query) const override;

    /**
     * @brief Pure parser for the Instant Answer JSON (testable, no I/O).
     * @param json Raw Instant Answer API response bytes.
     * @param query Echoed into the response.
     * @param maxResults Upper bound on related-topic hits.
     * @returns Parsed response (Ok/NoResults).
     */
    static WebSearchResponse
    parseInstantAnswer(const QByteArray& json, const QString& query, int maxResults);
};

}  // namespace Search

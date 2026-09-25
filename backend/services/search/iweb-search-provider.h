// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file iweb-search-provider.h
 * @brief Abstract interface for a web-search backend.
 * @layer Service (Search subsystem)
 * @dependencies web-search-types.h.
 *
 * A web-search provider is a backend STRATEGY (analogous to ILLMProvider
 * for chat), NOT an LLM-callable tool. The model always calls the single
 * `search_web` tool; WebSearchService selects which provider services the
 * call. Implementations must be STATELESS and re-entrant: search() is
 * invoked on a QtConcurrent worker thread, so a provider must create any
 * QNetworkAccessManager locally per call and hold no cross-call mutable
 * state.
 */
#pragma once

#include "web-search-types.h"

#include <QString>

namespace Search {

/**
 * @brief Interface implemented by every web-search backend.
 */
class IWebSearchProvider {
  public:
    virtual ~IWebSearchProvider() = default;

    /**
     * @brief Stable backend identifier (e.g. "ddg-html", "tavily").
     * @returns Unique id used for selection + persistence.
     */
    virtual QString id() const = 0;

    /**
     * @brief Human-readable name shown in settings UI.
     * @returns Display name.
     */
    virtual QString displayName() const = 0;

    /**
     * @brief Whether this backend needs an API key to function.
     * @returns true iff search() requires a non-empty WebSearchConfig::apiKey.
     */
    virtual bool requiresApiKey() const = 0;

    /**
     * @brief Perform a synchronous web search.
     * @param cfg Snapshot of the resolved configuration (provider id,
     *            base url, api key, max results, timeout).
     * @param query The user/agent search query.
     * @returns A fully-formed WebSearchResponse. On failure the provider
     *          returns status Unavailable/NoResults with a populated
     *          `note`/`errorReason` rather than throwing.
     * @complexity Bounded by one network round-trip plus result parsing.
     * @sideeffects One outbound HTTP(S) request via a locally-created
     *              QNetworkAccessManager. No persistent state, no signals.
     *              Safe to call on a worker thread.
     */
    virtual WebSearchResponse search(const WebSearchConfig& cfg, const QString& query) const = 0;
};

}  // namespace Search

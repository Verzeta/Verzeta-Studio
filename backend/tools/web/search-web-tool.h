// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file search-web-tool.h
 * @brief `search_web` tool: the single LLM-callable web-search entry
 *        point. Delegates to Search::WebSearchService, which selects the
 *        active backend (DuckDuckGo scrape, Instant Answer, or a
 *        configured API/self-hosted provider) and guarantees a graceful
 *        result.
 * @layer Service (Tool subsystem)
 * @dependencies backend/tools/itool.h, Search::WebSearchService.
 *
 * The tool itself holds no network state; all backend selection,
 * fallback, and HTTP live in WebSearchService. invoke() runs on a worker
 * thread (runsOnMainThread() == false) because the service performs a
 * synchronous network round-trip.
 */
#pragma once

#include "../itool.h"

namespace Search {
class WebSearchService;
}

namespace Tools {

/** @brief ITool implementation for the `search_web` built-in. */
class SearchWebTool : public ITool {
  public:
    /**
     * @brief Construct bound to the web-search backend service.
     * @param service The WebSearchService that performs the actual search.
     */
    explicit SearchWebTool(Search::WebSearchService& service);

    /**
     * @brief Canonical tool name.
     * @returns "search_web".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the web-search behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `query` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns false, because the service performs a synchronous network round
     *          trip and must run on a worker thread.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Run a web search via the active backend.
     * @param args JSON object with:
     *               - "query" (required, string): search text.
     * @return On success, a JSON object with:
     *           - "query":   echoed input string
     *           - "status":  "ok" | "no_results" | "unavailable"
     *           - "provider": active backend id
     *           - "results": array of {title, url, snippet} (when any)
     *           - "answer":  optional direct answer (when present)
     *           - "note":    guidance for the model on empty/failed search
     *         On empty query, {"error": "query is required"}.
     * @complexity Bounded by one network round-trip plus parsing.
     * @sideeffects Delegates to WebSearchService::search (one outbound
     *              HTTP(S) request). No persistent state. No signals.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    Search::WebSearchService& m_service;
};

}  // namespace Tools

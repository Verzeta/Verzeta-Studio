// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file search-messages-tool.h
 * @brief `search_messages` tool: full-text search across the user's
 *        past conversations via SearchService's FTS5 index. Returns
 *        matching snippets (conversation title + role + timestamp)
 *        so the LLM can choose which conversation to read in full
 *        via read_conversation.
 * @layer Service (Tool subsystem)
 * @dependencies SearchService (non-owning reference),
 *               backend/tools/itool.h.
 *
 * Threading: runsOnMainThread() returns true. SearchService::searchFullText
 * touches SQLite; ToolDispatcher's main-thread gate applies.
 */
#pragma once

#include "../itool.h"

class SearchService;

namespace Tools {

/** @brief ITool implementation for `search_messages`. */
class SearchMessagesTool : public ITool {
  public:
    /**
     * @param searchService Non-owning reference used for every query.
     *                      Must outlive this tool.
     */
    explicit SearchMessagesTool(SearchService& searchService);

    /**
     * @brief Canonical tool name.
     * @returns "search_messages".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the FTS5 search behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `query` (required) and `limit` (optional int).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because SearchService::searchFullText touches SQLite.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Execute an FTS5 query against the message index.
     * @param args JSON object with:
     *               - "query" (required, string): FTS5 query syntax
     *                 (AND / OR / NOT / "phrase" / prefix*).
     *               - "limit" (optional, integer): 1–30; default 10.
     *                 Clamped: values ≤ 0 reset to 10; values > 30
     *                 clamp to 30.
     * @return On success, {"matches": \<array\>, "count": \<int\>}. Each
     *         match has conversationId / conversationTitle / messageId /
     *         role / snippet / timestamp (ISO-8601). On empty query,
     *         {"error": "query is required"}.
     * @complexity O(k log N) where k = limit, N = indexed rows.
     * @sideeffects Read-only SQLite query.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    SearchService& m_searchService;
};

}  // namespace Tools

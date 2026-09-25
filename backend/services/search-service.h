// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file search-service.h
 * @brief Provides full-text and semantic search across all conversations.
 *        Uses SQLite FTS5 for keyword search and RagService for semantic search.
 *        Results are combined and de-duplicated in searchCombined().
 * @layer Service (Search Index)
 * @dependencies DbManager (Data Access), RagService (Service)
 */


#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

class DbManager;
class RagService;
struct RagChunk;

/**
 * @brief A single search hit with conversation context and a highlighted snippet.
 */
struct SearchResult {
    QString conversationId;       ///< UUID of the containing conversation
    QString conversationTitle;    ///< Display title of the conversation
    QString messageId;            ///< UUID of the matching message
    QString role;                 ///< "user" | "assistant"
    QString snippet;              ///< Highlighted text extract (≤ ~160 chars)
    QDateTime timestamp;          ///< Message creation time (UTC)
    float relevanceScore = 0.0f;  ///< 0.0-1.0 (semantic) or FTS5 rank value
};

/**
 * @brief Search service providing FTS5 full-text and semantic search.
 *
 * Exposed as `SearchService` QML context property by AppController.
 * All three public search methods are Q_INVOKABLEs callable from QML.
 *
 * ## FTS5 index
 *
 * The `messages_fts` virtual table holds:
 *   - `content`: message text
 *   - `message_id`: UNINDEXED row identifier
 *   - `conversation_id`: UNINDEXED for filtering
 *
 * `MessageService::addMessage()` inserts into `messages_fts` for user/assistant
 * messages. `SearchService::indexMessage()` can be called explicitly
 * when re-indexing is needed.
 *
 * ## Semantic search
 *
 * Delegates to `RagService::retrieve()` which returns the top-K most similar
 * embeddings. Results are mapped back to message rows via `source_id` = messageId.
 */
class SearchService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the SearchService.
     * @param db  DbManager reference for SQL queries.
     * @param rag RagService reference for semantic search.
     * @param parent Optional Qt parent.
     */
    explicit SearchService(DbManager& db, RagService& rag, QObject* parent = nullptr);

    // -----------------------------------------------------------------------
    // Search methods (Q_INVOKABLE for QML)
    // -----------------------------------------------------------------------

    /**
     * @brief Full-text search using SQLite FTS5.
     *
     * Supports FTS5 query syntax: AND, OR, NOT, phrase search ("..."),
     * prefix search (word*), column filters.
     *
     * Matched terms are highlighted with SQLite's snippet() function. When
     * snippet() yields nothing, as on an FTS5 build without it, a snippet
     * is generated manually.
     *
     * @param query  FTS5 query string.
     * @param limit  Maximum number of results (default 20).
     * @return List of SearchResult sorted by FTS5 rank (best first).
     * @complexity O(log n) for FTS5 index + O(k) for result enrichment.
     */
    Q_INVOKABLE QList<SearchResult> searchFullText(const QString& query, int limit = 20);

    /**
     * @brief Semantic search using dense vector embeddings. Asynchronous.
     *
     * Embeds the query off-thread via `RagService::retrieveAllAsync()` (never
     * blocks the UI), then delivers mapped results via the searchResultsReady()
     * signal as a QVariantList. Cross-conversation (unscoped).
     *
     * @param query Natural language query.
     * @param topK  Number of results (default 10).
     * @sideeffects Emits searchResultsReady() when the embedding completes.
     *              An empty query, or no available embedder, emits it
     *              immediately with an empty list.
     */
    Q_INVOKABLE void searchSemantic(const QString& query, int topK = 10);

    /**
     * @brief Combined search: FTS5 (synchronous) plus semantic (async),
     *        merged, de-duplicated and re-ranked, delivered via
     *        searchResultsReady().
     *
     * FTS5 results are computed immediately. The semantic half requests
     * limit / 2 results, is embedded off-thread and is merged when it
     * returns, so the UI is never blocked. De-duplication is on messageId,
     * and a message found by both halves has its relevanceScore raised by
     * half its semantic score, capped at 1.0. Results are sorted by
     * relevanceScore, descending.
     *
     * @param query  Search query.
     * @param limit  Maximum results (default 20).
     * @sideeffects Emits searchResultsReady() when the embedding completes (or
     *              immediately with FTS-only results if no embedder is available).
     *              An empty query emits an empty list immediately.
     */
    Q_INVOKABLE void searchCombined(const QString& query, int limit = 20);

    /**
     * @brief Manually indexes a message in the FTS5 table.
     *
     * Typically not needed because MessageService::addMessage() already populates
     * the FTS5 index. Use this for re-indexing existing content.
     *
     * @param messageId      UUID of the message.
     * @param conversationId UUID of the conversation.
     * @param content        Raw message text.
     * @sideeffects Inserts or replaces a row in messages_fts.
     */
    void
    indexMessage(const QString& messageId, const QString& conversationId, const QString& content);

  signals:
    /**
     * @brief Emitted when search results are ready (for async future extension).
     * @param results The search results list.
     */
    void resultsReady(const QList<SearchResult>& results);

    /**
     * @brief Emitted when an async search (searchSemantic/searchCombined)
     *        completes. Carries a QVariantList of result maps (QML-native), so
     *        no custom metatype registration is needed. Each map has
     *        conversationId, conversationTitle, messageId, role, snippet,
     *        timestamp, relevanceScore.
     * @param results The result maps, ranked best-first.
     */
    void searchResultsReady(const QVariantList& results);

  private:
    DbManager& m_db;
    RagService& m_rag;

    /**
     * @brief Resumes an async search when its query embedding returns: maps
     *        chunks → results, merges with the pending FTS half for combined
     *        mode, and emits searchResultsReady. Matched by queryId; a
     *        result for an older search is ignored (id mismatch).
     * @param queryId The id returned by RagService::retrieveAllAsync.
     * @param chunks  The retrieved chunks.
     */
    void onSearchRetrievalReady(quint64 queryId, const QList<RagChunk>& chunks);

    /**
     * @brief Maps a retrieved chunk to a SearchResult (message chunks only).
     * @param chunk The retrieved chunk.
     * @param query The original query (used for snippet highlighting).
     * @returns The mapped SearchResult; messageId is empty for non-message
     *          chunks, which the caller skips.
     */
    SearchResult chunkToResult(const RagChunk& chunk, const QString& query) const;

    /**
     * @brief Converts results to a QVariantList of QVariantMaps for the QML
     *        searchResultsReady signal.
     * @param results The results to convert.
     * @returns A QVariantList of per-result maps.
     */
    QVariantList resultsToVariantList(const QList<SearchResult>& results) const;

    quint64 m_pendingSearchQueryId = 0;
    QString m_pendingSearchQuery;
    int m_pendingSearchLimit = 0;
    bool m_pendingSearchCombined = false;
    QList<SearchResult> m_pendingFtsResults;  ///< FTS half awaiting the semantic half

    /**
     * @brief Generates a highlighted text snippet around the first match.
     *
     * Finds the first occurrence of any query term in content, then returns
     * contextChars characters before/after the match with the matched term
     * wrapped in `<mark>…</mark>` tags.
     *
     * @param content      Full message text.
     * @param query        The search query (first word/term is highlighted).
     * @param contextChars Context characters around the match.
     * @return Snippet string with `<mark>` highlighting, or first 160 chars
     *         of content if no match found.
     */
    QString
    generateSnippet(const QString& content, const QString& query, int contextChars = 80) const;

    /**
     * @brief Looks up conversation title from the conversations table.
     * @param conversationId Conversation UUID.
     * @return Title string, or empty string if not found.
     */
    QString conversationTitle(const QString& conversationId) const;

    /**
     * @brief Looks up message role from the messages table.
     * @param messageId Message UUID.
     * @return Role string ("user" | "assistant"), or empty.
     */
    QString messageRole(const QString& messageId) const;

    /**
     * @brief Looks up message creation timestamp.
     * @param messageId Message UUID.
     * @return UTC QDateTime, or invalid QDateTime if not found.
     */
    QDateTime messageTimestamp(const QString& messageId) const;
};

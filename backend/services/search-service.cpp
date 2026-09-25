// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file search-service.cpp
 * @brief Implementation of FTS5 full-text and semantic search.
 * @layer Service (Search Index)
 * @dependencies DbManager, RagService, Qt6::Sql
 */


#include "search-service.h"

#include "../utils/thread-discipline.h"
#include "models/db-manager.h"
#include "services/rag-service.h"
#include "utils/logger.h"

#include <QTimeZone>

#include <QDateTime>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariantMap>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs SearchService.
 * @param db  DbManager for SQL access.
 * @param rag RagService for semantic embedding retrieval.
 * @param parent Optional Qt parent.
 */
SearchService::SearchService(DbManager& db, RagService& rag, QObject* parent)
    : QObject(parent), m_db(db), m_rag(rag) {
    // Async search continuation (signal-driven; no blocking, no timer). queryIds
    // are globally unique in RagService, so this service only acts on its own
    // in-flight search (others fail the identity match).
    connect(&m_rag, &RagService::retrievalReady, this, &SearchService::onSearchRetrievalReady);
}

// ---------------------------------------------------------------------------
// Public Search API
// ---------------------------------------------------------------------------

/*
 * @brief Performs FTS5 full-text search across all messages.
 *
 * Uses `SELECT snippet(…)` to let SQLite highlight matched terms.
 * Falls back to manual snippet generation if snippet() is unavailable.
 *
 * @param query  FTS5 query string.
 * @param limit  Maximum results.
 * @return SearchResult list sorted by FTS5 rank (best first).
 */
QList<SearchResult> SearchService::searchFullText(const QString& query, int limit) {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<SearchResult> results;
    if (query.trimmed().isEmpty()) {
        return results;
    }

    QSqlQuery q(m_db.db());
    // Use FTS5 snippet() for highlighted text (5 tokens before/after match)
    q.prepare(
        QStringLiteral("SELECT fts.message_id, fts.conversation_id, fts.content, "
                       "       snippet(messages_fts, 0, '<mark>', '</mark>', '…', 16) AS snip, "
                       "       m.role, m.created_at "
                       "FROM messages_fts AS fts "
                       "JOIN messages AS m ON m.id = fts.message_id "
                       "WHERE messages_fts MATCH ? "
                       "ORDER BY rank "
                       "LIMIT ?"));
    q.addBindValue(query.trimmed());
    q.addBindValue(limit);

    if (!q.exec()) {
        qCWarning(verzetaUi) << "SearchService FTS5 query failed:" << q.lastError().text();
        return results;
    }

    while (q.next()) {
        SearchResult r;
        r.messageId = q.value(0).toString();
        r.conversationId = q.value(1).toString();
        const QString content = q.value(2).toString();
        r.snippet = q.value(3).toString();
        r.role = q.value(4).toString();
        r.timestamp = QDateTime::fromSecsSinceEpoch(q.value(5).toLongLong(), QTimeZone::utc());
        r.conversationTitle = conversationTitle(r.conversationId);
        r.relevanceScore = 1.0f;  // FTS5 rank → normalised as "matched"

        // If snippet is empty (FTS5 build without snippet support), generate manually
        if (r.snippet.isEmpty()) {
            r.snippet = generateSnippet(content, query);
        }

        results.append(r);
    }

    return results;
}

/*
 * @brief Performs semantic search using RagService embeddings.
 *
 * Asynchronous. The query is embedded off-thread through
 * RagService::retrieveAllAsync(); onSearchRetrievalReady() maps the matching
 * chunks back to their messages and conversations and emits
 * searchResultsReady(). An empty query, or no available embedder, emits
 * searchResultsReady() with an empty list immediately.
 *
 * @param query Natural language query.
 * @param topK  Number of results.
 */
void SearchService::searchSemantic(const QString& query, int topK) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (query.trimmed().isEmpty()) {
        emit searchResultsReady({});
        return;
    }
    // Embed off-thread (cross-conversation, unscoped); results arrive via
    // onSearchRetrievalReady → searchResultsReady. Never blocks the UI.
    const quint64 qid = m_rag.retrieveAllAsync(query, topK);
    if (qid == 0) {
        emit searchResultsReady({});  // no embedder → no semantic results
        return;
    }
    m_pendingSearchQueryId = qid;
    m_pendingSearchQuery = query;
    m_pendingSearchLimit = topK;
    m_pendingSearchCombined = false;
    m_pendingFtsResults.clear();
}

SearchResult SearchService::chunkToResult(const RagChunk& chunk, const QString& query) const {
    SearchResult r;
    if (chunk.sourceType != QStringLiteral("message")) {
        return r;  // messageId stays empty → caller skips
    }
    r.messageId = chunk.sourceId;
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("SELECT conversation_id FROM messages WHERE id = ?"));
        q.addBindValue(chunk.sourceId);
        if (q.exec() && q.next()) {
            r.conversationId = q.value(0).toString();
        }
    }
    r.conversationTitle = conversationTitle(r.conversationId);
    r.role = messageRole(r.messageId);
    r.timestamp = messageTimestamp(r.messageId);
    r.snippet = generateSnippet(chunk.text, query);
    r.relevanceScore = qMax(0.0f, chunk.score);  // normalise to [0,1]
    return r;
}

QVariantList SearchService::resultsToVariantList(const QList<SearchResult>& results) const {
    QVariantList out;
    out.reserve(results.size());
    for (const SearchResult& r : results) {
        QVariantMap m;
        m[QStringLiteral("conversationId")] = r.conversationId;
        m[QStringLiteral("conversationTitle")] = r.conversationTitle;
        m[QStringLiteral("messageId")] = r.messageId;
        m[QStringLiteral("role")] = r.role;
        m[QStringLiteral("snippet")] = r.snippet;
        m[QStringLiteral("timestamp")] = r.timestamp;
        m[QStringLiteral("relevanceScore")] = r.relevanceScore;
        out.append(m);
    }
    return out;
}

/*
 * @brief Combined search: runs both FTS5 and semantic, de-duplicates, re-ranks.
 *
 * The FTS5 half runs synchronously. The semantic half requests limit / 2
 * results and is embedded off-thread; onSearchRetrievalReady() merges the
 * two and emits searchResultsReady(). De-duplication is on messageId, and a
 * message found by both halves has its relevanceScore raised by half its
 * semantic score, capped at 1.0. Results are sorted by relevanceScore,
 * descending. With no available embedder the FTS5 results are emitted
 * immediately on their own.
 *
 * @param query  Search query.
 * @param limit  Maximum results.
 */
void SearchService::searchCombined(const QString& query, int limit) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (query.trimmed().isEmpty()) {
        emit searchResultsReady({});
        return;
    }
    // FTS5 half is synchronous (no embed, instant). Semantic half is embedded
    // off-thread; the two are merged in onSearchRetrievalReady.
    const QList<SearchResult> ftsList = searchFullText(query, limit);
    const quint64 qid = m_rag.retrieveAllAsync(query, limit / 2);
    if (qid == 0) {
        // No embedder → deliver FTS-only results immediately.
        emit searchResultsReady(resultsToVariantList(ftsList));
        return;
    }
    m_pendingSearchQueryId = qid;
    m_pendingSearchQuery = query;
    m_pendingSearchLimit = limit;
    m_pendingSearchCombined = true;
    m_pendingFtsResults = ftsList;
}

void SearchService::onSearchRetrievalReady(quint64 queryId, const QList<RagChunk>& chunks) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (queryId == 0 || queryId != m_pendingSearchQueryId) {
        return;
    }
    m_pendingSearchQueryId = 0;
    const QString query = m_pendingSearchQuery;
    const int limit = m_pendingSearchLimit;
    const bool combined = m_pendingSearchCombined;
    const QList<SearchResult> ftsList = m_pendingFtsResults;
    m_pendingFtsResults.clear();

    // Map semantic chunks → results (message chunks only).
    QList<SearchResult> semList;
    for (const RagChunk& chunk : chunks) {
        const SearchResult r = chunkToResult(chunk, query);
        if (!r.messageId.isEmpty()) {
            semList.append(r);
        }
    }

    if (!combined) {
        emit searchResultsReady(resultsToVariantList(semList));
        return;
    }

    // Combined: merge FTS + semantic, de-dup on messageId, boost overlaps.
    QMap<QString, SearchResult> merged;
    for (const SearchResult& r : ftsList) {
        merged[r.messageId] = r;
    }
    for (const SearchResult& r : semList) {
        if (merged.contains(r.messageId)) {
            merged[r.messageId].relevanceScore =
                qMin(1.0f, merged[r.messageId].relevanceScore + r.relevanceScore * 0.5f);
        } else {
            merged[r.messageId] = r;
        }
    }
    QList<SearchResult> out = merged.values();
    std::sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.relevanceScore > b.relevanceScore;
    });
    if (out.size() > limit) {
        out = out.sliced(0, limit);
    }
    emit searchResultsReady(resultsToVariantList(out));
}

/*
 * @brief Manually inserts or replaces a message entry in the FTS5 index.
 *
 * Typically called by MessageService::addMessage() already. This method
 * can be used to force-re-index a message (e.g., after editing).
 *
 * @param messageId      Message UUID.
 * @param conversationId Conversation UUID.
 * @param content        Raw text content.
 */
void SearchService::indexMessage(const QString& messageId,
                                 const QString& conversationId,
                                 const QString& content) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    // Delete existing entry first (FTS5 does not support REPLACE)
    q.prepare(QStringLiteral("DELETE FROM messages_fts WHERE message_id = ?"));
    q.addBindValue(messageId);
    q.exec();

    q.prepare(QStringLiteral("INSERT INTO messages_fts(content, message_id, conversation_id) "
                             "VALUES (?, ?, ?)"));
    q.addBindValue(content);
    q.addBindValue(messageId);
    q.addBindValue(conversationId);

    if (!q.exec()) {
        qCWarning(verzetaUi) << "SearchService::indexMessage failed:" << q.lastError().text();
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Generates a `<mark>`-highlighted snippet from content around the first match.
 *
 * Finds the first case-insensitive occurrence of the first query token.
 * Returns contextChars characters either side of the match with the match
 * wrapped in `<mark>` tags.
 *
 * @param content      Full message text.
 * @param query        Search query (first word used for position).
 * @param contextChars Characters of context around the match.
 * @return Highlighted snippet string.
 */
QString SearchService::generateSnippet(const QString& content,
                                       const QString& query,
                                       int contextChars) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (content.isEmpty()) {
        return {};
    }

    // Extract first query term (split on whitespace and FTS5 operators)
    QString firstTerm;
    const QStringList tokens =
        query.split(QRegularExpression(QStringLiteral(R"(\s+|AND|OR|NOT)")), Qt::SkipEmptyParts);
    if (!tokens.isEmpty()) {
        firstTerm = tokens.first();
        firstTerm.remove(QLatin1Char('"'));
    }

    if (firstTerm.isEmpty()) {
        // Just return the first 160 characters
        return content.left(160);
    }

    const int matchPos = content.indexOf(firstTerm, 0, Qt::CaseInsensitive);
    if (matchPos == -1) {
        return content.left(160);
    }

    const int start = qMax(0, matchPos - contextChars);
    const int end = qMin(content.length(), matchPos + firstTerm.length() + contextChars);
    QString snippet = content.mid(start, end - start);

    // Wrap match with <mark> tags
    const int relPos = matchPos - start;
    snippet.insert(relPos + firstTerm.length(), QStringLiteral("</mark>"));
    snippet.insert(relPos, QStringLiteral("<mark>"));

    if (start > 0) {
        snippet.prepend(QStringLiteral("…"));
    }
    if (end < content.length()) {
        snippet.append(QStringLiteral("…"));
    }
    return snippet;
}

/**
 * @brief Returns the conversation title for a given ID.
 * @param conversationId Conversation UUID.
 * @return Title string, or empty if not found.
 */
QString SearchService::conversationTitle(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT title FROM conversations WHERE id = ?"));
    q.addBindValue(conversationId);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return {};
}

/**
 * @brief Returns the role of a message.
 * @param messageId Message UUID.
 * @return "user", "assistant", or empty if not found.
 */
QString SearchService::messageRole(const QString& messageId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT role FROM messages WHERE id = ?"));
    q.addBindValue(messageId);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return {};
}

/**
 * @brief Returns the UTC creation timestamp of a message.
 * @param messageId Message UUID.
 * @return UTC QDateTime, or invalid if not found.
 */
QDateTime SearchService::messageTimestamp(const QString& messageId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT created_at FROM messages WHERE id = ?"));
    q.addBindValue(messageId);
    if (q.exec() && q.next()) {
        return QDateTime::fromSecsSinceEpoch(q.value(0).toLongLong(), QTimeZone::utc());
    }
    return {};
}

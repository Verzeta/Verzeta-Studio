// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file memory-entry-store.h
 * @brief MemoryEntryStore: shared base for "one row = one embedded, scoped,
 *        FTS-mirrored memory entry" stores (per-agent AIM and team ACN). Owns the
 *        embed glue, FTS mirror maintenance, vec0 upsert, scoped FTS/vector
 *        lookup, purge, and count over the shared VectorStore substrate.
 * @layer Service (Search Index)
 * @dependencies DbManager, EmbeddingWorker, VectorStore
 */

#pragma once

#include "models/db-manager.h"
#include "services/vector-store.h"
#include "workers/embedding-worker.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

/**
 * @brief Common mechanics for an embedded memory-entry corpus.
 *
 * Subclasses (AgentMemoryService / AcnService) own their corpus-specific row
 * columns: they do their own INSERT and their own payload SELECTs, but delegate
 * the identical parts here: scheduling the async embed (keyed by an id prefix on
 * the shared worker), mirroring text into the FTS index, persisting the vector on
 * embeddingReady (+ vec0 upsert via VectorStore), scoped FTS and vector lookups
 * (returning ids/rowids the subclass resolves to its typed hits), purge, and
 * count. This keeps the embed/FTS/vector glue in ONE place. RAG stays separate
 * because it is a chunked-document corpus, a different shape.
 *
 * Schema contract for the subclass's base table: an implicit `rowid`, plus
 * `id` TEXT PRIMARY KEY, `text` TEXT, `owner_scope` TEXT, `embedding` BLOB,
 * `model_used` TEXT. The FTS table is content-less: `(text, <ftsIdCol> UNINDEXED,
 * owner_scope UNINDEXED)`. Strictly main-thread (shared DB handle).
 */
class MemoryEntryStore : public QObject {
    Q_OBJECT

  public:
    /** @brief One FTS lexical match: the row id and its bm25 score. */
    struct FtsHit {
        QString id;    ///< base-table `id`
        double score;  ///< bm25 (smaller = better)
    };

    /**
     * @brief Deletes every entry for one owner_scope (base rows + FTS mirror +
     *        the scope's vectors). The cascade-delete primitive.
     * @param ownerScope The scope whose entries to delete.
     * @returns The number of base rows deleted.
     */
    int purgeScope(const QString& ownerScope);

    /**
     * @brief Total stored entries (observability / tests).
     * @returns The base-table row count.
     */
    int count() const;

  protected:
    /**
     * @brief Constructs the base over a corpus's tables.
     * @param db           Open database (shared main-thread handle).
     * @param bulkEmbedder Shared worker; entries keyed by `idPrefix` so consumers
     *                     never cross-handle each other's results.
     * @param baseTable    The entry table (satisfies the schema contract).
     * @param ftsTable     The companion content-less FTS5 table.
     * @param ftsIdCol     The FTS table's id column name (e.g. "memory_id").
     * @param vecTable     The companion vec0 table.
     * @param idPrefix     Worker tracking-id prefix (e.g. "aim:", "acn:").
     * @param parent       Optional Qt parent.
     */
    MemoryEntryStore(DbManager& db,
                     EmbeddingWorker& bulkEmbedder,
                     QString baseTable,
                     QString ftsTable,
                     QString ftsIdCol,
                     QString vecTable,
                     QByteArray idPrefix,
                     QObject* parent = nullptr);

    /**
     * @brief Mirrors an already-inserted row's text into FTS and schedules its
     *        async embed. The subclass calls this right after its own INSERT.
     * @param id         The base row's id.
     * @param text       The (already-trimmed) entry text.
     * @param ownerScope The entry's owner_scope.
     */
    void indexAndEmbed(const QString& id, const QString& text, const QString& ownerScope);

    /**
     * @brief Scoped lexical (FTS5) lookup over this corpus.
     * @param query  Free text (tokenized into a safe OR-match).
     * @param scopes owner_scope restriction (empty → no match).
     * @param k      Max ids to return.
     * @returns Matched (id, bm25) in best-first order; the subclass resolves the
     *          payload by id.
     */
    QList<FtsHit> ftsScopedMatch(const QString& query, const QStringList& scopes, int k);

    /**
     * @brief Scoped vector (vec0 KNN / brute-force) lookup over this corpus.
     * @param queryVec The query embedding.
     * @param model    The model that produced it (dimension/model filter).
     * @param scopes   owner_scope restriction.
     * @param k        Max hits.
     * @returns Matched (rowid, cosine score); the subclass resolves the payload
     *          by rowid.
     */
    QList<VectorStore::Hit> searchVectors(const QVector<float>& queryVec,
                                          const QString& model,
                                          const QStringList& scopes,
                                          int k);

    /**
     * @brief The base database handle (for subclass INSERT / payload SELECT).
     * @returns The DbManager this store was constructed with.
     */
    DbManager& db() { return m_db; }

    /**
     * @brief The base table name.
     * @returns The table name given at construction.
     */
    const QString& baseTable() const { return m_base; }

  private slots:
    /**
     * @brief Persists a computed embedding onto its row + vec0 (prefix-filtered).
     * @param id        Tracking id ("<prefix><rowId>"); ignored if not ours.
     * @param embedding The float32 vector.
     * @param modelId   The model that produced it.
     */
    void
    onEmbeddingReady(const QString& id, const QVector<float>& embedding, const QString& modelId);

    /**
     * @brief Logs a failed embed (the row keeps a NULL vector, stays FTS-recallable).
     * @param id    Tracking id; ignored if not ours.
     * @param error Error description.
     */
    void onEmbeddingError(const QString& id, const QString& error);

  private:
    DbManager& m_db;
    EmbeddingWorker& m_bulkEmbedder;
    VectorStore m_vectors;
    QString m_base;
    QString m_fts;
    QString m_ftsIdCol;
    QByteArray m_idPrefix;
};

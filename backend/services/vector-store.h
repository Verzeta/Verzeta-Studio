// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file vector-store.h
 * @brief Reusable embedding vector store: float32 (de)serialization, cosine
 *        ranking, and an optional sqlite-vec (vec0) KNN index with a graceful
 *        brute-force fallback, over a caller-named base + companion vec table.
 *        Shared by every embedding consumer (RAG corpus, agent memory) so the
 *        vector mechanics live in exactly one place.
 * @layer Service (Search Index)
 * @dependencies DbManager (Data Access)
 */

#pragma once

#include "models/db-manager.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>

/**
 * @brief Vector mechanics over one logical corpus: a base table holding the
 *        embedding BLOBs and a companion vec0 table providing accelerated KNN.
 *
 * ## Schema contract (the base table the caller owns must have)
 * - an implicit integer `rowid` (every ordinary SQLite table has one),
 * - `embedding` BLOB: native-endian float32 vector (may be NULL until set),
 * - `model_used` TEXT: the model that produced the vector (dimension key),
 * - `owner_scope` TEXT: visibility scope used for filtered ranking/purge.
 *
 * The companion `vec0` table (named by `vecTable`) is created/owned by this
 * class: `vec0(embedding float[dim] distance_metric=cosine, model_id text,
 * owner_scope text)` with `rowid` equal to the base row's `rowid`. It is a
 * cache of the base BLOBs; on a dimension change it is dropped and rebuilt from
 * the base table, so it can always be recreated from authoritative data.
 *
 * When sqlite-vec is unavailable (or a vec operation fails) every method
 * degrades to a correct brute-force cosine scan over the base BLOBs. Results
 * are identical, only slower. Strictly main-thread (uses the shared DB handle).
 */
class VectorStore {
  public:
    /**
     * @brief One ranking result: the base row id and its cosine similarity.
     */
    struct Hit {
        qint64 rowid;  ///< Base-table rowid of the matched row
        float score;   ///< Cosine similarity in [−1, 1]; higher is more relevant
    };

    /**
     * @brief Constructs a store over a base table and its companion vec0 table.
     * @param db       Reference to the open database (shared main-thread handle).
     * @param baseTable Name of the table holding embedding BLOBs (see contract).
     * @param vecTable  Name of the vec0 KNN table this store creates/owns.
     */
    VectorStore(DbManager& db, QString baseTable, QString vecTable);

    /**
     * @brief Ranks the corpus against a query vector, restricted to one model
     *        and an optional scope set, returning the top-K rows by similarity.
     * @param query   The query embedding.
     * @param modelId Only rows stored with this model are compared (so the
     *                dimensions always match the query).
     * @param scopes  owner_scope values to restrict to; EMPTY = no scope filter
     *                (rank the whole corpus).
     * @param topK    Maximum number of hits to return.
     * @returns Hits ordered by descending similarity (possibly empty). Uses vec0
     *          KNN when available and the index matches the query dimension,
     *          otherwise an equivalent brute-force cosine scan.
     */
    QList<Hit> search(const QVector<float>& query,
                      const QString& modelId,
                      const QStringList& scopes,
                      int topK);

    /**
     * @brief Mirrors one already-written base row's vector into the vec0 index.
     * @param rowid      The base row's rowid (must already exist in the base).
     * @param vec        The embedding vector (its size sets/uses the index dim).
     * @param modelId    Stored as the vec0 `model_id` filter column.
     * @param ownerScope Stored as the vec0 `owner_scope` filter column.
     * @sideeffects No-op when sqlite-vec is unavailable; ensures the vec0 table
     *              exists for the vector's dimension (rebuilding on a change),
     *              then performs an idempotent delete+insert for `rowid`.
     */
    void upsert(qint64 rowid,
                const QVector<float>& vec,
                const QString& modelId,
                const QString& ownerScope);

    /**
     * @brief Deletes all base rows for one owner_scope and invalidates the vec0
     *        index so it rebuilds from what remains.
     * @param ownerScope The owner_scope whose rows to delete.
     * @returns The number of base rows deleted (0 on empty scope or failure).
     */
    int purgeScope(const QString& ownerScope);

    /**
     * @brief Deletes every base row and drops the vec0 index (a full reset).
     * @returns The number of base rows that existed before the reset.
     */
    int clearAll();

    /**
     * @brief Invalidates the vec0 index (drops it + resets the cached dimension)
     *        so the next search/upsert rebuilds it from the base table.
     * @sideeffects No-op when sqlite-vec is unavailable.
     */
    void invalidateVecIndex();

    // -----------------------------------------------------------------------
    // Pure helpers (no DB) — the single source of truth for the wire format.
    // -----------------------------------------------------------------------

    /**
     * @brief Serializes a float32 vector to raw native-endian bytes.
     * @param v The vector.
     * @returns Raw bytes (4 per float) suitable for the `embedding` BLOB column.
     */
    static QByteArray serialize(const QVector<float>& v);

    /**
     * @brief Deserializes raw native-endian bytes to a float32 vector.
     * @param data Bytes from the `embedding` BLOB column.
     * @returns The float32 vector (empty when data is empty).
     */
    static QVector<float> deserialize(const QByteArray& data);

    /**
     * @brief Cosine similarity between two equal-length vectors.
     * @param a First vector.
     * @param b Second vector.
     * @returns Similarity in [−1, 1]; 0 when sizes differ, either is empty, or a
     *          norm is ~0.
     */
    static float cosineSimilarity(const QVector<float>& a, const QVector<float>& b);

    /**
     * @brief Serializes a vector to the JSON-array text sqlite-vec expects
     *        (e.g. "[1,2,3]").
     * @param v The vector.
     * @returns The JSON-array string.
     */
    static QString toJsonArray(const QVector<float>& v);

  private:
    /**
     * @brief Ensures the vec0 table exists and matches `dim`, rebuilding and
     *        repopulating from the base table on a dimension change.
     * @param dim The embedding dimension required by the current query/store.
     * @returns true when a vec0 table for `dim` is ready; false when sqlite-vec
     *          is unavailable or a vec operation failed (caller uses brute-force).
     */
    bool ensureVecTable(int dim);

    /**
     * @brief Brute-force cosine ranking over the base BLOBs (the fallback path).
     * @param query   The query embedding.
     * @param modelId Restrict to rows with this model.
     * @param scopes  owner_scope restriction; empty = no filter.
     * @param topK    Maximum hits.
     * @returns Hits ordered by descending similarity.
     */
    QList<Hit> bruteForce(const QVector<float>& query,
                          const QString& modelId,
                          const QStringList& scopes,
                          int topK);

    DbManager& m_db;
    QString m_base;    ///< Base table name (holds embedding BLOBs)
    QString m_vec;     ///< vec0 KNN table name (owned by this class)
    int m_vecDim = 0;  ///< Dimension the vec0 table is built for (0 = none)
};

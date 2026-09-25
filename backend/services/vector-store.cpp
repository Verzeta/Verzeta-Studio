// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file vector-store.cpp
 * @brief Implementation of VectorStore: the shared float32 (de)serialization,
 *        cosine ranking, and sqlite-vec (vec0) KNN index with brute-force
 *        fallback, over a caller-named base + companion vec table.
 * @layer Service (Search Index)
 * @dependencies DbManager
 */

#include "vector-store.h"

#include "utils/logger.h"

#include <algorithm>
#include <cmath>
#include <QSqlError>
#include <QSqlQuery>
#include <utility>

VectorStore::VectorStore(DbManager& db, QString baseTable, QString vecTable)
    : m_db(db), m_base(std::move(baseTable)), m_vec(std::move(vecTable)) {}

// ---------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------

QList<VectorStore::Hit> VectorStore::search(const QVector<float>& query,
                                            const QString& modelId,
                                            const QStringList& scopes,
                                            int topK) {
    QList<Hit> results;
    if (query.isEmpty()) {
        return results;
    }

    // Fast path: sqlite-vec KNN when the extension is available and the index
    // matches the query dimension. Filtered by model_id (so dimensions match)
    // and the scope set, using the cosine metric (similarity = 1 - distance) to
    // match the brute-force ranking. Any failure falls through to brute-force.
    if (m_db.isVectorSearchAvailable() && ensureVecTable(query.size())) {
        QString knnScope;
        if (!scopes.isEmpty()) {
            QString ph;
            for (int i = 0; i < scopes.size(); ++i) {
                ph += (i == 0) ? QStringLiteral("?") : QStringLiteral(",?");
            }
            knnScope = QStringLiteral(" AND owner_scope IN (%1)").arg(ph);
        }
        QSqlQuery kq(m_db.db());
        kq.prepare(QStringLiteral("SELECT rowid, distance FROM %1 "
                                  "WHERE embedding MATCH ? AND k = ? "
                                  "AND model_id = ?%2 ORDER BY distance")
                       .arg(m_vec, knnScope));
        kq.addBindValue(toJsonArray(query));
        kq.addBindValue(topK);
        kq.addBindValue(modelId);
        for (const QString& s : scopes) {
            kq.addBindValue(s);
        }
        if (kq.exec()) {
            while (kq.next()) {
                Hit h;
                h.rowid = kq.value(0).toLongLong();
                // cosine distance → similarity (matches brute-force semantics)
                h.score = 1.0f - static_cast<float>(kq.value(1).toDouble());
                results.append(h);
            }
            return results;
        }
        qCWarning(verzetaRag) << "VectorStore: vec0 KNN failed on" << m_vec
                              << "— using brute-force:" << kq.lastError().text();
        // fall through to brute-force
    }

    return bruteForce(query, modelId, scopes, topK);
}

QList<VectorStore::Hit> VectorStore::bruteForce(const QVector<float>& query,
                                                const QString& modelId,
                                                const QStringList& scopes,
                                                int topK) {
    QList<Hit> results;

    // Empty scopes = no scope filter (search the whole corpus); otherwise
    // restrict to the visibility set.
    QString scopeClause;
    if (!scopes.isEmpty()) {
        QString placeholders;
        for (int i = 0; i < scopes.size(); ++i) {
            placeholders += (i == 0) ? QStringLiteral("?") : QStringLiteral(",?");
        }
        scopeClause = QStringLiteral(" AND owner_scope IN (%1)").arg(placeholders);
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT rowid, embedding FROM %1 "
                             "WHERE embedding IS NOT NULL AND model_used = ?%2")
                  .arg(m_base, scopeClause));
    q.addBindValue(modelId);
    for (const QString& s : scopes) {
        q.addBindValue(s);
    }
    if (!q.exec()) {
        qCWarning(verzetaRag) << "VectorStore: brute-force query failed on" << m_base
                              << q.lastError().text();
        return results;
    }

    // Filter by model means every stored vector matches the query dimension, so
    // cosineSimilarity never compares mismatched sizes.
    QList<QPair<float, qint64>> scored;
    while (q.next()) {
        const qint64 rowid = q.value(0).toLongLong();
        const QByteArray blob = q.value(1).toByteArray();
        if (blob.isEmpty()) {
            continue;
        }
        scored.append({cosineSimilarity(query, deserialize(blob)), rowid});
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });
    for (int i = 0; i < qMin(topK, static_cast<int>(scored.size())); ++i) {
        results.append(Hit{scored[i].second, scored[i].first});
    }
    return results;
}

// ---------------------------------------------------------------------------
// vec0 index lifecycle
// ---------------------------------------------------------------------------

void VectorStore::upsert(qint64 rowid,
                         const QVector<float>& vec,
                         const QString& modelId,
                         const QString& ownerScope) {
    if (!m_db.isVectorSearchAvailable() || !ensureVecTable(vec.size())) {
        return;  // vec0 unavailable → brute-force reads the base BLOB directly
    }
    // Idempotent upsert for the common same-dimension path. ensureVecTable
    // already rebuilt + repopulated (incl. this row) on a dimension change.
    QSqlQuery dq(m_db.db());
    dq.prepare(QStringLiteral("DELETE FROM %1 WHERE rowid = ?").arg(m_vec));
    dq.addBindValue(rowid);
    dq.exec();
    QSqlQuery vi(m_db.db());
    vi.prepare(QStringLiteral("INSERT INTO %1(rowid, embedding, model_id, owner_scope) "
                              "VALUES(?, ?, ?, ?)")
                   .arg(m_vec));
    vi.addBindValue(rowid);
    vi.addBindValue(toJsonArray(vec));
    vi.addBindValue(modelId);
    vi.addBindValue(ownerScope);
    if (!vi.exec()) {
        qCWarning(verzetaRag) << "VectorStore: vec0 upsert failed on" << m_vec
                              << vi.lastError().text();
    }
}

bool VectorStore::ensureVecTable(int dim) {
    if (!m_db.isVectorSearchAvailable() || dim <= 0) {
        return false;
    }
    if (m_vecDim == dim) {
        return true;  // already built for this dimension (fast path)
    }

    // (Re)create the vec0 table for the new dimension. vec0 tables are real
    // tables persisted in the DB, so a stale table from a prior session is
    // dropped and rebuilt from the authoritative base-table BLOBs.
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral("DROP TABLE IF EXISTS %1").arg(m_vec))) {
        qCWarning(verzetaRag) << "VectorStore: drop" << m_vec << "failed:" << q.lastError().text();
        m_vecDim = 0;
        return false;
    }
    if (!q.exec(QStringLiteral("CREATE VIRTUAL TABLE %1 USING vec0("
                               "embedding float[%2] distance_metric=cosine, "
                               "model_id text, owner_scope text)")
                    .arg(m_vec)
                    .arg(dim))) {
        qCWarning(verzetaRag) << "VectorStore: create" << m_vec
                              << "failed:" << q.lastError().text();
        m_vecDim = 0;
        return false;
    }

    // Repopulate from the base rows whose vector matches this dimension.
    QSqlQuery src(m_db.db());
    if (!src.exec(QStringLiteral("SELECT rowid, embedding, model_used, owner_scope FROM %1 "
                                 "WHERE embedding IS NOT NULL")
                      .arg(m_base))) {
        qCWarning(verzetaRag) << "VectorStore: repopulate scan failed on" << m_base
                              << src.lastError().text();
        m_vecDim = 0;
        return false;
    }
    QSqlQuery ins(m_db.db());
    ins.prepare(QStringLiteral("INSERT INTO %1(rowid, embedding, model_id, owner_scope) "
                               "VALUES(?, ?, ?, ?)")
                    .arg(m_vec));
    while (src.next()) {
        const QByteArray blob = src.value(1).toByteArray();
        if (blob.size() / static_cast<int>(sizeof(float)) != dim) {
            continue;  // a different model/dimension — not in this index
        }
        ins.bindValue(0, src.value(0));
        ins.bindValue(1, toJsonArray(deserialize(blob)));
        ins.bindValue(2, src.value(2));
        ins.bindValue(3, src.value(3));
        if (!ins.exec()) {
            qCWarning(verzetaRag) << "VectorStore: repopulate insert failed on" << m_vec
                                  << ins.lastError().text();
            m_vecDim = 0;
            return false;
        }
    }

    m_vecDim = dim;
    qCInfo(verzetaRag) << "VectorStore: vec0 index" << m_vec << "built for dimension" << dim;
    return true;
}

void VectorStore::invalidateVecIndex() {
    if (m_db.isVectorSearchAvailable()) {
        QSqlQuery q(m_db.db());
        q.exec(QStringLiteral("DROP TABLE IF EXISTS %1").arg(m_vec));
    }
    m_vecDim = 0;
}

// ---------------------------------------------------------------------------
// Lifecycle / purge
// ---------------------------------------------------------------------------

int VectorStore::purgeScope(const QString& ownerScope) {
    if (ownerScope.isEmpty()) {
        return 0;
    }
    int before = 0;
    {
        QSqlQuery c(m_db.db());
        c.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE owner_scope = ?").arg(m_base));
        c.addBindValue(ownerScope);
        if (c.exec() && c.next()) {
            before = c.value(0).toInt();
        }
    }
    QSqlQuery del(m_db.db());
    del.prepare(QStringLiteral("DELETE FROM %1 WHERE owner_scope = ?").arg(m_base));
    del.addBindValue(ownerScope);
    if (!del.exec()) {
        qCWarning(verzetaRag) << "VectorStore: purgeScope failed on" << m_base << "for"
                              << ownerScope << del.lastError().text();
        return 0;
    }
    // Invalidate the vec0 index so it rebuilds from the remaining rows. (vec0 is
    // a cache of the BLOBs; a rebuild is correct and cheap at these sizes.)
    invalidateVecIndex();
    return before;
}

int VectorStore::clearAll() {
    int before = 0;
    {
        QSqlQuery c(m_db.db());
        if (c.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(m_base)) && c.next()) {
            before = c.value(0).toInt();
        }
    }
    QSqlQuery del(m_db.db());
    if (!del.exec(QStringLiteral("DELETE FROM %1").arg(m_base))) {
        qCWarning(verzetaRag) << "VectorStore: clearAll failed on" << m_base
                              << del.lastError().text();
        return 0;
    }
    invalidateVecIndex();
    return before;
}

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

QByteArray VectorStore::serialize(const QVector<float>& v) {
    QByteArray data;
    data.resize(v.size() * static_cast<int>(sizeof(float)));
    memcpy(data.data(), v.constData(), static_cast<size_t>(data.size()));
    return data;
}

QVector<float> VectorStore::deserialize(const QByteArray& data) {
    const int count = data.size() / static_cast<int>(sizeof(float));
    QVector<float> result(count);
    memcpy(result.data(), data.constData(), static_cast<size_t>(data.size()));
    return result;
}

float VectorStore::cosineSimilarity(const QVector<float>& a, const QVector<float>& b) {
    if (a.size() != b.size() || a.isEmpty()) {
        return 0.0f;
    }
    float dot = 0.0f;
    float normA = 0.0f;
    float normB = 0.0f;
    for (int i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    const float denom = std::sqrt(normA) * std::sqrt(normB);
    if (denom < 1e-10f) {
        return 0.0f;
    }
    return dot / denom;
}

QString VectorStore::toJsonArray(const QVector<float>& v) {
    QString s = QStringLiteral("[");
    for (int i = 0; i < v.size(); ++i) {
        if (i != 0) {
            s += QLatin1Char(',');
        }
        s += QString::number(static_cast<double>(v[i]), 'g', 8);
    }
    s += QLatin1Char(']');
    return s;
}

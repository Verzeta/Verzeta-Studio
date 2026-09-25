// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file memory-entry-store.cpp
 * @brief Implementation of MemoryEntryStore, the shared embed/FTS/vector glue for
 *        the AIM + ACN memory-entry corpora.
 * @layer Service (Search Index)
 * @dependencies DbManager, EmbeddingWorker, VectorStore
 */

#include "memory-entry-store.h"

#include "utils/logger.h"

#include <QMetaObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <utility>

namespace {
/** Builds a safe FTS5 MATCH expression: word tokens quoted + OR-joined so any
 *  token may hit and punctuation can never raise a syntax error. */
QString ftsMatchExpr(const QString& raw) {
    static const QRegularExpression kNonWord(QStringLiteral("[^\\w]+"));
    const QStringList tokens = raw.split(kNonWord, Qt::SkipEmptyParts);
    QStringList quoted;
    quoted.reserve(tokens.size());
    for (const QString& t : tokens) {
        quoted << (QLatin1Char('"') + t + QLatin1Char('"'));
    }
    return quoted.join(QStringLiteral(" OR "));
}
}  // namespace

MemoryEntryStore::MemoryEntryStore(DbManager& db,
                                   EmbeddingWorker& bulkEmbedder,
                                   QString baseTable,
                                   QString ftsTable,
                                   QString ftsIdCol,
                                   QString vecTable,
                                   QByteArray idPrefix,
                                   QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_bulkEmbedder(bulkEmbedder)
    ,
    // m_vectors (declared before m_base) copies baseTable; m_base then moves it.
    m_vectors(db, baseTable, std::move(vecTable))
    , m_base(std::move(baseTable))
    , m_fts(std::move(ftsTable))
    , m_ftsIdCol(std::move(ftsIdCol))
    , m_idPrefix(std::move(idPrefix)) {
    // The worker's completion signal IS the continuation (queued, no blocking,
    // no timer). Multiple stores share one worker; the id-prefix guards keep each
    // store to its own results.
    connect(&m_bulkEmbedder,
            &EmbeddingWorker::embeddingReady,
            this,
            &MemoryEntryStore::onEmbeddingReady);
    connect(&m_bulkEmbedder,
            &EmbeddingWorker::errorOccurred,
            this,
            &MemoryEntryStore::onEmbeddingError);
}

void MemoryEntryStore::indexAndEmbed(const QString& id,
                                     const QString& text,
                                     const QString& ownerScope) {
    // Mirror text into the FTS index (manual maintenance, mirrors messages_fts).
    QSqlQuery fts(m_db.db());
    fts.prepare(QStringLiteral("INSERT INTO %1(text, %2, owner_scope) "
                               "VALUES(?, ?, ?)")
                    .arg(m_fts, m_ftsIdCol));
    fts.addBindValue(text);
    fts.addBindValue(id);
    fts.addBindValue(ownerScope);
    if (!fts.exec()) {
        qCWarning(verzetaMemory) << "MemoryEntryStore: FTS insert failed on" << m_fts
                                 << fts.lastError().text();
        // Non-fatal: the row exists; this entry's lexical recall is degraded but
        // vector recall (once embedded) + other entries are unaffected.
    }
    // Schedule the embed off the turn path; the vector lands in onEmbeddingReady.
    QMetaObject::invokeMethod(&m_bulkEmbedder,
                              "computeEmbedding",
                              Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromLatin1(m_idPrefix) + id),
                              Q_ARG(QString, text),
                              Q_ARG(QString, QStringLiteral("memory")));
}

QList<MemoryEntryStore::FtsHit>
MemoryEntryStore::ftsScopedMatch(const QString& query, const QStringList& scopes, int k) {
    QList<FtsHit> results;
    if (scopes.isEmpty() || k <= 0) {
        return results;
    }
    const QString match = ftsMatchExpr(query);
    if (match.isEmpty()) {
        return results;
    }
    QString scopePh;
    for (int i = 0; i < scopes.size(); ++i) {
        scopePh += (i == 0) ? QStringLiteral("?") : QStringLiteral(",?");
    }
    // The FTS table carries owner_scope (UNINDEXED) + the id column, so the
    // scoped match needs no JOIN — the subclass fetches its payload by id.
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT %1, bm25(%2) AS score FROM %2 "
                             "WHERE %2 MATCH ? AND owner_scope IN (%3) "
                             "ORDER BY score LIMIT ?")
                  .arg(m_ftsIdCol, m_fts, scopePh));
    q.addBindValue(match);
    for (const QString& s : scopes) {
        q.addBindValue(s);
    }
    q.addBindValue(k);
    if (!q.exec()) {
        qCWarning(verzetaMemory) << "MemoryEntryStore: FTS query failed on" << m_fts
                                 << q.lastError().text();
        return results;
    }
    while (q.next()) {
        results.append(FtsHit{q.value(0).toString(), q.value(1).toDouble()});
    }
    return results;
}

QList<VectorStore::Hit> MemoryEntryStore::searchVectors(const QVector<float>& queryVec,
                                                        const QString& model,
                                                        const QStringList& scopes,
                                                        int k) {
    if (queryVec.isEmpty() || scopes.isEmpty() || k <= 0) {
        return {};
    }
    return m_vectors.search(queryVec, model, scopes, k);
}

int MemoryEntryStore::purgeScope(const QString& ownerScope) {
    if (ownerScope.isEmpty()) {
        return 0;
    }
    // Remove the FTS mirror rows first (separate table, not covered by the
    // VectorStore base purge), then purge the base rows + invalidate vec0.
    QSqlQuery fts(m_db.db());
    fts.prepare(QStringLiteral("DELETE FROM %1 WHERE owner_scope = ?").arg(m_fts));
    fts.addBindValue(ownerScope);
    if (!fts.exec()) {
        qCWarning(verzetaMemory) << "MemoryEntryStore: FTS purge failed on" << m_fts
                                 << fts.lastError().text();
    }
    return m_vectors.purgeScope(ownerScope);
}

int MemoryEntryStore::count() const {
    QSqlQuery q(m_db.db());
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(m_base)) && q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

void MemoryEntryStore::onEmbeddingReady(const QString& id,
                                        const QVector<float>& embedding,
                                        const QString& modelId) {
    if (!id.startsWith(QString::fromLatin1(m_idPrefix))) {
        return;  // not ours (another corpus / a query embed)
    }
    const QString entryId = id.mid(m_idPrefix.size());
    if (entryId.isEmpty() || embedding.isEmpty()) {
        return;
    }
    const QString modelUsed = modelId.isEmpty() ? QStringLiteral("unknown") : modelId;

    QSqlQuery up(m_db.db());
    up.prepare(
        QStringLiteral("UPDATE %1 SET embedding = ?, model_used = ? WHERE id = ?").arg(m_base));
    up.addBindValue(VectorStore::serialize(embedding));
    up.addBindValue(modelUsed);
    up.addBindValue(entryId);
    if (!up.exec() || up.numRowsAffected() == 0) {
        return;  // row purged between insert and embed completion
    }

    // Mirror into the shared vec0 index (no-op when sqlite-vec is unavailable).
    QSqlQuery rq(m_db.db());
    rq.prepare(QStringLiteral("SELECT rowid, owner_scope FROM %1 WHERE id = ?").arg(m_base));
    rq.addBindValue(entryId);
    if (rq.exec() && rq.next()) {
        m_vectors.upsert(rq.value(0).toLongLong(), embedding, modelUsed, rq.value(1).toString());
    }
}

void MemoryEntryStore::onEmbeddingError(const QString& id, const QString& error) {
    if (!id.startsWith(QString::fromLatin1(m_idPrefix))) {
        return;
    }
    // The entry keeps a NULL vector and stays lexically (FTS) recallable (N4).
    qCWarning(verzetaMemory) << "MemoryEntryStore: embed failed for" << id << error;
}

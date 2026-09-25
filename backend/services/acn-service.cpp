// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file acn-service.cpp
 * @brief Implementation of AcnService: team/project memory (ACN) over the shared
 *        MemoryEntryStore base + the acn_entries-specific row columns / payloads.
 * @layer Service (Search Index)
 * @dependencies MemoryEntryStore, DbManager
 */

#include "acn-service.h"

#include "models/db-manager.h"
#include "utils/logger.h"

#include <QDateTime>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

AcnService::AcnService(DbManager& db, EmbeddingWorker& bulkEmbedder, QObject* parent)
    : MemoryEntryStore(db,
                       bulkEmbedder,
                       QStringLiteral("acn_entries"),
                       QStringLiteral("acn_entries_fts"),
                       QStringLiteral("entry_id"),
                       QStringLiteral("vec_acn_entries"),
                       QByteArrayLiteral("acn:"),
                       parent) {}

QString AcnService::record(const QString& text,
                           const QString& ownerScope,
                           const QString& entryKind,
                           const QString& sourceConvId,
                           const QString& coveredRange) {
    const QString clean = text.trimmed();
    if (clean.isEmpty() || ownerScope.isEmpty()) {
        return QString();
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QSqlQuery q(db().db());
    q.prepare(QStringLiteral("INSERT INTO acn_entries"
                             "(id, owner_scope, entry_kind, text, source_conv_id, covered_range, "
                             " created_at_ms) "
                             "VALUES(?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(id);
    q.addBindValue(ownerScope);
    q.addBindValue(entryKind);
    q.addBindValue(clean);
    q.addBindValue(sourceConvId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                          : QVariant(sourceConvId));
    q.addBindValue(coveredRange.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                          : QVariant(coveredRange));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qCWarning(verzetaMemory) << "AcnService: record insert failed:" << q.lastError().text();
        return QString();
    }

    indexAndEmbed(id, clean, ownerScope);  // shared: FTS mirror + async embed

    emit entryRecorded(id);
    return id;
}

QList<AcnHit> AcnService::recall(const QString& query, const QStringList& ownerScopes, int k) {
    QList<AcnHit> results;
    const QList<FtsHit> matches = ftsScopedMatch(query, ownerScopes, k);
    if (matches.isEmpty()) {
        return results;
    }
    QStringList ids;
    QHash<QString, double> scoreById;
    QString ph;
    for (int i = 0; i < matches.size(); ++i) {
        ids << matches[i].id;
        scoreById.insert(matches[i].id, matches[i].score);
        ph += (i == 0) ? QStringLiteral("?") : QStringLiteral(",?");
    }
    QSqlQuery q(db().db());
    q.prepare(QStringLiteral("SELECT id, text, entry_kind, owner_scope FROM acn_entries "
                             "WHERE id IN (%1)")
                  .arg(ph));
    for (const QString& id : ids) {
        q.addBindValue(id);
    }
    if (!q.exec()) {
        qCWarning(verzetaMemory) << "AcnService: recall payload failed:" << q.lastError().text();
        return results;
    }
    QHash<QString, AcnHit> byId;
    while (q.next()) {
        AcnHit h;
        h.id = q.value(0).toString();
        h.text = q.value(1).toString();
        h.entryKind = q.value(2).toString();
        h.ownerScope = q.value(3).toString();
        h.score = scoreById.value(h.id);
        byId.insert(h.id, h);
    }
    for (const FtsHit& m : matches) {
        const auto it = byId.constFind(m.id);
        if (it != byId.constEnd()) {
            results.append(it.value());
        }
    }
    qCDebug(verzetaMemory).nospace()
        << "ACN: recalled " << results.size() << " entry(ies) [lexical, scopes "
        << ownerScopes.join(QLatin1Char(',')) << "]";
    return results;
}

QList<AcnHit> AcnService::recallSemantic(const QVector<float>& queryVec,
                                         const QString& model,
                                         const QStringList& ownerScopes,
                                         int k) {
    QList<AcnHit> results;
    const QList<VectorStore::Hit> hits = searchVectors(queryVec, model, ownerScopes, k);
    if (hits.isEmpty()) {
        return results;
    }
    QString ph;
    for (int i = 0; i < hits.size(); ++i) {
        ph += (i == 0) ? QStringLiteral("?") : QStringLiteral(",?");
    }
    QSqlQuery q(db().db());
    q.prepare(QStringLiteral(
                  "SELECT rowid, id, text, entry_kind, owner_scope, embedding FROM acn_entries "
                  "WHERE rowid IN (%1)")
                  .arg(ph));
    for (const VectorStore::Hit& h : hits) {
        q.addBindValue(h.rowid);
    }
    if (!q.exec()) {
        qCWarning(verzetaMemory) << "AcnService: recallSemantic payload failed:"
                                 << q.lastError().text();
        return results;
    }
    QHash<qint64, AcnHit> byRow;
    while (q.next()) {
        AcnHit h;
        h.id = q.value(1).toString();
        h.text = q.value(2).toString();
        h.entryKind = q.value(3).toString();
        h.ownerScope = q.value(4).toString();
        // Carry the stored embedding for cross-source dedup/MMR in the retriever.
        h.embedding = VectorStore::deserialize(q.value(5).toByteArray());
        byRow.insert(q.value(0).toLongLong(), h);
    }
    for (const VectorStore::Hit& hit : hits) {
        const auto it = byRow.constFind(hit.rowid);
        if (it == byRow.constEnd()) {
            continue;
        }
        AcnHit h = it.value();
        h.score = static_cast<double>(hit.score);
        results.append(h);
    }
    qCDebug(verzetaMemory).nospace()
        << "ACN: recalled " << results.size() << " entry(ies) [semantic, scopes "
        << ownerScopes.join(QLatin1Char(',')) << "]";
    return results;
}

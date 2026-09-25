// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file agent-memory-service.cpp
 * @brief Implementation of AgentMemoryService: per-agent memory (AIM) over the
 *        shared MemoryEntryStore base (embed / FTS / vector glue) + the
 *        agent_memories-specific row columns and recall payloads.
 * @layer Service (Search Index)
 * @dependencies MemoryEntryStore, DbManager
 */

#include "agent-memory-service.h"

#include "models/db-manager.h"
#include "utils/logger.h"

#include <QDateTime>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

AgentMemoryService::AgentMemoryService(DbManager& db,
                                       EmbeddingWorker& bulkEmbedder,
                                       QObject* parent)
    : MemoryEntryStore(db,
                       bulkEmbedder,
                       QStringLiteral("agent_memories"),
                       QStringLiteral("agent_memories_fts"),
                       QStringLiteral("memory_id"),
                       QStringLiteral("vec_agent_memories"),
                       QByteArrayLiteral("aim:"),
                       parent) {}

QString AgentMemoryService::save(const QString& text,
                                 const QString& ownerScope,
                                 const QString& sourceConvId,
                                 const QString& sourceMsgId,
                                 const QString& kind) {
    const QString clean = text.trimmed();
    if (clean.isEmpty() || ownerScope.isEmpty()) {
        return QString();
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QSqlQuery q(db().db());
    q.prepare(QStringLiteral("INSERT INTO agent_memories"
                             "(id, owner_scope, kind, text, source_conv_id, source_msg_id, "
                             " use_count, created_at_ms) "
                             "VALUES(?, ?, ?, ?, ?, ?, 0, ?)"));
    q.addBindValue(id);
    q.addBindValue(ownerScope);
    q.addBindValue(kind);
    q.addBindValue(clean);
    q.addBindValue(sourceConvId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                          : QVariant(sourceConvId));
    q.addBindValue(sourceMsgId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                         : QVariant(sourceMsgId));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qCWarning(verzetaMemory) << "AgentMemoryService: save insert failed:"
                                 << q.lastError().text();
        return QString();
    }

    // Shared: FTS mirror + schedule the async embed (vec0 lands on completion).
    indexAndEmbed(id, clean, ownerScope);

    emit memorySaved(id);
    return id;
}

QList<AgentMemoryHit>
AgentMemoryService::recall(const QString& query, const QStringList& ownerScopes, int k) {
    QList<AgentMemoryHit> results;
    const QList<FtsHit> matches = ftsScopedMatch(query, ownerScopes, k);
    if (matches.isEmpty()) {
        return results;
    }
    // Resolve the entry payload by id, preserving the FTS rank order.
    QStringList ids;
    QHash<QString, double> scoreById;
    QString ph;
    for (int i = 0; i < matches.size(); ++i) {
        ids << matches[i].id;
        scoreById.insert(matches[i].id, matches[i].score);
        ph += (i == 0) ? QStringLiteral("?") : QStringLiteral(",?");
    }
    QSqlQuery q(db().db());
    q.prepare(QStringLiteral("SELECT id, text, kind, owner_scope FROM agent_memories "
                             "WHERE id IN (%1)")
                  .arg(ph));
    for (const QString& id : ids) {
        q.addBindValue(id);
    }
    if (!q.exec()) {
        qCWarning(verzetaMemory) << "AgentMemoryService: recall payload failed:"
                                 << q.lastError().text();
        return results;
    }
    QHash<QString, AgentMemoryHit> byId;
    while (q.next()) {
        AgentMemoryHit h;
        h.id = q.value(0).toString();
        h.text = q.value(1).toString();
        h.kind = q.value(2).toString();
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
        << "AIM: recalled " << results.size() << " memory(ies) [lexical, scopes "
        << ownerScopes.join(QLatin1Char(',')) << "]";
    return results;
}

QList<AgentMemoryHit> AgentMemoryService::recallSemantic(const QVector<float>& queryVec,
                                                         const QString& model,
                                                         const QStringList& ownerScopes,
                                                         int k) {
    QList<AgentMemoryHit> results;
    const QList<VectorStore::Hit> hits = searchVectors(queryVec, model, ownerScopes, k);
    if (hits.isEmpty()) {
        return results;
    }
    // Resolve the entry payload by rowid, preserving the ranked order.
    QString ph;
    for (int i = 0; i < hits.size(); ++i) {
        ph += (i == 0) ? QStringLiteral("?") : QStringLiteral(",?");
    }
    QSqlQuery q(db().db());
    q.prepare(
        QStringLiteral("SELECT rowid, id, text, kind, owner_scope, embedding FROM agent_memories "
                       "WHERE rowid IN (%1)")
            .arg(ph));
    for (const VectorStore::Hit& h : hits) {
        q.addBindValue(h.rowid);
    }
    if (!q.exec()) {
        qCWarning(verzetaMemory) << "AgentMemoryService: recallSemantic payload failed:"
                                 << q.lastError().text();
        return results;
    }
    QHash<qint64, AgentMemoryHit> byRow;
    while (q.next()) {
        AgentMemoryHit h;
        h.id = q.value(1).toString();
        h.text = q.value(2).toString();
        h.kind = q.value(3).toString();
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
        AgentMemoryHit h = it.value();
        h.score = static_cast<double>(hit.score);  // cosine similarity
        results.append(h);
    }
    qCDebug(verzetaMemory).nospace()
        << "AIM: recalled " << results.size() << " memory(ies) [semantic, scopes "
        << ownerScopes.join(QLatin1Char(',')) << "]";
    return results;
}

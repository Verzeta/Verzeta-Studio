// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file agent-memory-service.h
 * @brief Per-agent long-term memory (AIM): durable, scope-keyed memory entries
 *        with lexical (FTS5) recall and an async-embedded vector mirror. The
 *        embed / FTS / vector glue is shared via MemoryEntryStore.
 * @layer Service (Search Index)
 * @dependencies MemoryEntryStore (DbManager, EmbeddingWorker, VectorStore)
 */

#pragma once

#include "services/memory-entry-store.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

class DbManager;
class EmbeddingWorker;

/**
 * @brief One recalled memory entry.
 */
struct AgentMemoryHit {
    QString id;                ///< agent_memories.id
    QString text;              ///< The stored memory text
    QString kind;              ///< fact | preference | correction | observation | explicit
    QString ownerScope;        ///< Visibility scope the entry was stored under
    double score;              ///< Relevance (FTS bm25, where smaller is better; or cosine sim)
    QVector<float> embedding;  ///< Stored embedding (semantic recall only). Lets the
                               ///< unified retriever dedup/rank across sources (MMR)
};

/**
 * @brief Per-agent memory store (AIM): save durable facts and recall them later.
 *
 * Memory is keyed by `owner_scope`: `agent:<agentId>` for an agent's own memory
 * (shared across all that agent's conversations) or `conversation:<convId>` for
 * an agentless direct chat. Each entry is inserted synchronously (so recall sees
 * it immediately) and embedded asynchronously off the turn path; the vector is
 * mirrored into the shared VectorStore via the MemoryEntryStore base.
 *
 * `recall()` is lexical (FTS5) and synchronous. It is safe from a main-thread tool
 * and works with no embedder. `recallSemantic()` ranks by an already-computed
 * query vector (for the pre-turn coordinator). `purgeScope()`/`count()` are
 * inherited from the base.
 */
class AgentMemoryService : public MemoryEntryStore {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the service over the agent-memory tables.
     * @param db           Open database (main-thread handle).
     * @param bulkEmbedder Shared worker used to embed saved entries (async).
     * @param parent       Optional Qt parent.
     */
    AgentMemoryService(DbManager& db, EmbeddingWorker& bulkEmbedder, QObject* parent = nullptr);

    /**
     * @brief Saves a durable memory entry and schedules its embedding (async).
     * @param text         The memory text (non-empty after trimming).
     * @param ownerScope   Visibility scope (`agent:\<id\>` or `conversation:\<id\>`).
     * @param sourceConvId Originating conversation id (optional provenance).
     * @param sourceMsgId  Originating message id (optional provenance).
     * @param kind         Entry kind; defaults to "explicit" for tool-driven saves.
     * @returns The new entry id, or an empty QString on invalid input / failure.
     */
    QString save(const QString& text,
                 const QString& ownerScope,
                 const QString& sourceConvId = QString(),
                 const QString& sourceMsgId = QString(),
                 const QString& kind = QStringLiteral("explicit"));

    /**
     * @brief Lexically recalls the top-K entries for a query, scoped.
     * @param query       Free-text query (tokenized into an FTS5 OR-match).
     * @param ownerScopes Visibility set to restrict to (empty → no results).
     * @param k           Maximum entries to return.
     * @returns Entries ordered by FTS relevance (best first). Synchronous; never
     *          embeds, so non-blocking and works with no embedder.
     */
    QList<AgentMemoryHit> recall(const QString& query, const QStringList& ownerScopes, int k);

    /**
     * @brief Semantic recall by an already-computed query embedding, scoped, for
     *        the unified pre-turn coordinator. Empty when no vectors of the
     *        query's model are stored (the coordinator then uses recall()).
     * @param queryVec    The query embedding.
     * @param model       The model that produced it (dimension/model filter).
     * @param ownerScopes Visibility set to restrict to.
     * @param k           Maximum entries.
     * @returns Ranked entries (best first; score = cosine similarity).
     */
    QList<AgentMemoryHit> recallSemantic(const QVector<float>& queryVec,
                                         const QString& model,
                                         const QStringList& ownerScopes,
                                         int k);

    /**
     * @brief Total stored memory entries (alias of the base count(), for tests
     *        and observability).
     * @returns The agent_memories row count.
     */
    int memoryCount() const { return count(); }

  signals:
    /** @brief Emitted after an entry is saved (before its async embed lands). */
    void memorySaved(const QString& id);
};

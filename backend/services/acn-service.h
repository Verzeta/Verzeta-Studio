// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file acn-service.h
 * @brief Team/project memory (ACN): project/org-scoped memory entries written at
 *        compaction, with lexical (FTS5) recall and an async-embedded vector
 *        mirror. Shares the embed / FTS / vector glue via MemoryEntryStore.
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
 * @brief One recalled team-memory entry.
 */
struct AcnHit {
    QString id;                ///< acn_entries.id
    QString text;              ///< The entry text
    QString entryKind;         ///< summary | decision | fact | open_question
    QString ownerScope;        ///< 'project:\<folderId\>'
    double score;              ///< Relevance (FTS bm25, where smaller is better; or cosine sim)
    QVector<float> embedding;  ///< Stored embedding (semantic recall only). Lets the
                               ///< unified retriever dedup/rank across sources (MMR)
};

/**
 * @brief Team/project memory store (ACN): record entries at compaction, recall
 *        them for any conversation in the same project/org.
 *
 * Scoped by `owner_scope = project:\<folderId\>` (the conversation's nearest
 * project/org ancestor) and shared across that project/org's conversations.
 * Entries are written at compaction (a side-output of the summarizer; no extra
 * LLM call) and embedded async; the vector mirror + FTS + purge are handled by
 * the MemoryEntryStore base. `recall()` is lexical (synchronous, embedder-free
 * fallback); `recallSemantic()` ranks by a precomputed query vector for the
 * pre-turn coordinator.
 */
class AcnService : public MemoryEntryStore {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the service over the ACN tables.
     * @param db           Open database (main-thread handle).
     * @param bulkEmbedder Shared worker used to embed entries (async).
     * @param parent       Optional Qt parent.
     */
    AcnService(DbManager& db, EmbeddingWorker& bulkEmbedder, QObject* parent = nullptr);

    /**
     * @brief Records a team-memory entry and schedules its embedding (async).
     * @param text         The entry text (non-empty after trimming).
     * @param ownerScope   The project/org scope (`project:\<folderId\>`).
     * @param entryKind    summary | decision | fact | open_question (default
     *                     "summary", the compaction-summary side-output).
     * @param sourceConvId The conversation that produced it (provenance).
     * @param coveredRange Optional description of what range it covers.
     * @returns The new entry id, or an empty QString on invalid input / failure.
     */
    QString record(const QString& text,
                   const QString& ownerScope,
                   const QString& entryKind = QStringLiteral("summary"),
                   const QString& sourceConvId = QString(),
                   const QString& coveredRange = QString());

    /**
     * @brief Lexically recalls the top-K entries for a query, scoped.
     * @param query       Free-text query (tokenized into an FTS5 OR-match).
     * @param ownerScopes Visibility set (`project:\<folderId\>`; empty → none).
     * @param k           Maximum entries.
     * @returns Entries ordered by FTS relevance (best first). Synchronous.
     */
    QList<AcnHit> recall(const QString& query, const QStringList& ownerScopes, int k);

    /**
     * @brief Semantic recall by a precomputed query embedding, scoped, for the
     *        pre-turn coordinator. Empty when no vectors of the query's model are
     *        stored (the coordinator then uses recall()).
     * @param queryVec    The query embedding.
     * @param model       The model that produced it (dimension/model filter).
     * @param ownerScopes Visibility set to restrict to.
     * @param k           Maximum entries.
     * @returns Ranked entries (best first; score = cosine similarity).
     */
    QList<AcnHit> recallSemantic(const QVector<float>& queryVec,
                                 const QString& model,
                                 const QStringList& ownerScopes,
                                 int k);

    /**
     * @brief Total stored ACN entries (alias of the base count()).
     * @returns The acn_entries row count.
     */
    int entryCount() const { return count(); }

  signals:
    /** @brief Emitted after an entry is recorded (before its async embed lands). */
    void entryRecorded(const QString& id);
};

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file memory-retriever.h
 * @brief MemoryRetriever, the unified pre-turn recall coordinator. Embeds the
 *        query ONCE and fans it out to every enabled IMemorySource (RAG / AIM /
 *        ACN), concatenating their sections into one injected context block.
 * @layer Service (Search Index)
 * @dependencies EmbeddingWorker, IMemorySource
 */

#pragma once

#include "services/chat/memory-source.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

class EmbeddingWorker;

namespace Chat {

/**
 * @brief Coordinates pre-turn memory recall across all sources with a SINGLE
 *        query embed.
 *
 * One embed per turn (shared query worker) serves every enabled source, then
 * each source's synchronous KNN/FTS runs and their sections are concatenated.
 * This is what lets RAG, AIM and ACN run in ANY combination, independently or
 * together, without a second embed, without colliding on the shared worker,
 * and without one source ever blocking another. Gating is per-source and
 * independent; nothing enabled (or an empty query) returns 0 so the caller
 * dispatches un-augmented (no dead-block). Embedder unreachable → each source
 * falls back to lexical recall. Signal-driven; stale results are dropped by
 * requestId (no timers).
 *
 * App-level (one instance): concurrent conversations are disambiguated by the
 * requestId carried on retrievalReady. The caller (ConversationRun) holds the
 * id it is waiting for, mirroring the RagService::retrieveAsync contract.
 */
class MemoryRetriever : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the coordinator.
     * @param queryEmbedder The query-priority embedding worker (shared with RAG;
     *        the coordinator keys its jobs with a "mem:" id prefix so neither
     *        consumer handles the other's results).
     * @param parent Optional Qt parent.
     */
    explicit MemoryRetriever(EmbeddingWorker& queryEmbedder, QObject* parent = nullptr);

    /**
     * @brief Registers a recall source (non-owning; the source outlives this).
     * @param source The source to consult on every retrieval.
     */
    void addSource(IMemorySource* source);

    /**
     * @brief Begins a pre-turn retrieval for the given context.
     * @param ctx The turn context (conversation, responder, query text).
     * @returns A non-zero requestId to correlate retrievalReady, or 0 when there
     *          is nothing to do (no enabled source or empty query), in which
     *          case no signal is emitted and the caller dispatches un-augmented.
     */
    quint64 beginRetrieve(const MemoryContext& ctx);

  signals:
    /**
     * @brief Emitted when a retrieval completes.
     * @param requestId The id returned by beginRetrieve().
     * @param injectedContext The concatenated, labelled sections to append to the
     *        system prompt ("" when nothing was found).
     */
    void retrievalReady(quint64 requestId, const QString& injectedContext);

  private slots:
    /**
     * @brief Receives the once-computed query embedding and assembles+emits.
     * @param id    Worker id ("mem:<requestId>"); ignored if not ours.
     * @param vec   The query embedding.
     * @param model The model that produced it.
     */
    void onEmbeddingReady(const QString& id, const QVector<float>& vec, const QString& model);

    /**
     * @brief Handles a failed query embed by assembling via lexical fallback.
     * @param id    Worker id ("mem:<requestId>"); ignored if not ours.
     * @param error Error description (logged).
     */
    void onEmbeddingError(const QString& id, const QString& error);

  private:
    /**
     * @brief Unifies the enabled sources' candidates into one injected block.
     *
     * Pools every source's scored candidates, drops semantic candidates below the
     * relevance floor, runs MMR (relevance vs. diversity) with a hard cross-source
     * duplicate cutoff so the same memory is never injected twice, appends any
     * lexical (FTS) candidates de-duplicated by text, then renders a single
     * budgeted "## Relevant memory" block (entries flattened + length-capped).
     *
     * @param sources The enabled sources for this request.
     * @param ctx     The turn context.
     * @param vec     The query embedding; EMPTY → sources use lexical fallback.
     * @param model   The embedding model (empty when vec is empty).
     * @returns The unified injected-context block ("" when nothing qualifies).
     */
    QString assemble(const QList<IMemorySource*>& sources,
                     const MemoryContext& ctx,
                     const QVector<float>& vec,
                     const QString& model) const;

    EmbeddingWorker& m_queryEmbedder;
    QList<IMemorySource*> m_sources;  ///< non-owning; app-owned

    /** @brief In-flight retrieval parameters, held until the embed returns. */
    struct Pending {
        MemoryContext ctx;
        QList<IMemorySource*> sources;
    };
    QHash<quint64, Pending> m_pending;
    quint64 m_nextId = 0;
};

}  // namespace Chat

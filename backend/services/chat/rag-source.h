// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file rag-source.h
 * @brief RagSource: the RAG corpus as a MemoryRetriever source (per-conversation
 *        `rag_enabled`; semantic-only, preserving the existing RAG injection format).
 * @layer Service (Search Index)
 * @dependencies RagService, ConversationService
 */

#pragma once

#include "services/chat/memory-source.h"

class RagService;
class ConversationService;

namespace Chat {

/**
 * @brief Pre-turn recall over the RAG document/message corpus.
 *
 * Gate: `llm_config.rag_enabled` (per-conversation, default off). Ranking uses
 * the coordinator's once-computed query vector via `RagService::recallWithVector`;
 * each chunk is returned as a scored candidate (carrying its stored embedding) so
 * the coordinator can rank/dedup it against the other sources. RAG has no lexical
 * fallback (it never did): with no embedder it simply contributes nothing.
 */
class RagSource : public IMemorySource {
  public:
    /**
     * @brief Constructs the RAG source.
     * @param rag     The RAG service (ranking + formatting).
     * @param convSvc Conversation lookup for the per-conversation gate.
     */
    RagSource(RagService& rag, ConversationService& convSvc);

    /** @brief Source id. @returns "rag". */
    QString id() const override;

    /**
     * @brief Whether RAG recall runs for this turn, per `llm_config.rag_enabled`
     *        (default off).
     * @param ctx The turn context.
     * @returns true iff RAG is enabled for the conversation.
     */
    bool isEnabled(const MemoryContext& ctx) const override;

    /** @brief RAG ranks by embedding. @returns true. */
    bool usesEmbedding() const override;

    /**
     * @brief Scored RAG candidates for the turn (semantic only, no lexical
     *        fallback), via the coordinator's query vector. Each carries its
     *        stored embedding so the coordinator can dedup/rank it against the
     *        other sources (MMR).
     * @param ctx      The turn context.
     * @param queryVec The query embedding; empty → contributes nothing.
     * @param model    The embedding model (empty when queryVec is empty).
     * @returns The RAG chunk candidates (best-first), or empty.
     */
    QList<MemoryHit> candidates(const MemoryContext& ctx,
                                const QVector<float>& queryVec,
                                const QString& model) override;

  private:
    RagService& m_rag;
    ConversationService& m_convSvc;
};

}  // namespace Chat

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file aim-source.h
 * @brief AimSource: per-agent memory (AIM) as a MemoryRetriever source
 *        (per-conversation `aim_enabled`, default on; semantic primary with
 *        lexical fallback; scoped per responder).
 * @layer Service (Search Index)
 * @dependencies AgentMemoryService, ConversationService
 */

#pragma once

#include "services/chat/memory-source.h"

#include <QStringList>

class AgentMemoryService;
class ConversationService;

namespace Chat {

/**
 * @brief Pre-turn recall over the responder's agent memory.
 *
 * Gate: `llm_config.aim_enabled` (per-conversation, default ON). Recall is on
 * for everyone unless turned off, and it is NOT gated per-agent. Privacy comes from
 * the SCOPE: an agent recalls `agent:<responderAgentId>`; an agentless chat
 * recalls `conversation:<conversationId>`. Semantic recall is primary (the
 * coordinator's query vector); when no embedder/vectors are available it falls
 * back to lexical (FTS), so recall never blocks and always returns something
 * useful.
 */
class AimSource : public IMemorySource {
  public:
    /**
     * @brief Constructs the AIM source.
     * @param mem     The per-agent memory store (semantic + lexical recall).
     * @param convSvc Conversation lookup for the per-conversation gate.
     */
    AimSource(AgentMemoryService& mem, ConversationService& convSvc);

    /** @brief Source id. @returns "aim". */
    QString id() const override;

    /**
     * @brief Whether AIM recall runs for this turn, per `llm_config.aim_enabled`
     *        (default on).
     * @param ctx The turn context.
     * @returns true unless the conversation turned agent memory off.
     */
    bool isEnabled(const MemoryContext& ctx) const override;

    /** @brief AIM ranks by embedding. @returns true. */
    bool usesEmbedding() const override;

    /**
     * @brief Scored agent-memory candidates for the turn (semantic primary,
     *        lexical FTS fallback), scoped to the responder. Semantic hits carry
     *        their stored embedding for cross-source dedup/MMR.
     * @param ctx      The turn context.
     * @param queryVec The query embedding; empty → lexical (FTS) fallback.
     * @param model    The embedding model (empty when queryVec is empty).
     * @returns The candidates (best-first), or empty when nothing is recalled.
     */
    QList<MemoryHit> candidates(const MemoryContext& ctx,
                                const QVector<float>& queryVec,
                                const QString& model) override;

  private:
    /**
     * @brief The owner_scope set this turn's responder may recall: its own
     *        `agent:\<id\>`, or `conversation:\<id\>` in an agentless chat.
     * @param ctx The turn context.
     * @returns The scope list (single element), or empty when unresolvable.
     */
    QStringList scopesFor(const MemoryContext& ctx) const;

    AgentMemoryService& m_mem;
    ConversationService& m_convSvc;
};

}  // namespace Chat

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file acn-source.h
 * @brief AcnSource: team/project memory (ACN) as a MemoryRetriever source. Gated
 *        by the project/org folder master switch AND the per-conversation toggle;
 *        scoped to the conversation's nearest project/org ancestor.
 * @layer Service (Search Index)
 * @dependencies AcnService, ConversationService
 */

#pragma once

#include "services/chat/memory-source.h"

#include <QStringList>

class AcnService;
class ConversationService;

namespace Chat {

/**
 * @brief Pre-turn recall over the conversation's project/organization team memory.
 *
 * Applies ONLY to conversations inside a project/organization. Gate = the folder
 * ACN master switch (folders.acn_enabled) AND the per-conversation toggle
 * (llm_config.acn_enabled). Scope = `project:<nearest project/org ancestor>` so a
 * chat recalls (and contributes to) its project's shared memory. Semantic recall
 * is primary (the coordinator's query vector); lexical (FTS) is the fallback.
 */
class AcnSource : public IMemorySource {
  public:
    /**
     * @brief Constructs the ACN source.
     * @param acn     The team-memory store (semantic + lexical recall).
     * @param convSvc Conversation/folder lookups for gating + scope resolution.
     */
    AcnSource(AcnService& acn, ConversationService& convSvc);

    /** @brief Source id. @returns "acn". */
    QString id() const override;

    /**
     * @brief Whether team memory applies + is enabled for this turn.
     * @param ctx The turn context.
     * @returns true iff the conversation is in a project/org with the master
     *          switch on AND the conversation's own ACN toggle is on.
     */
    bool isEnabled(const MemoryContext& ctx) const override;

    /** @brief ACN ranks by embedding. @returns true. */
    bool usesEmbedding() const override;

    /**
     * @brief Scored team-memory candidates for the turn (semantic primary, lexical
     *        fallback), scoped to the project/org. Semantic hits carry their stored
     *        embedding for cross-source dedup/MMR.
     * @param ctx      The turn context.
     * @param queryVec The query embedding; empty → lexical fallback.
     * @param model    The embedding model (empty when queryVec is empty).
     * @returns The candidates (best-first), or empty when nothing is recalled.
     */
    QList<MemoryHit> candidates(const MemoryContext& ctx,
                                const QVector<float>& queryVec,
                                const QString& model) override;

  private:
    /**
     * @brief The project/org scope this turn recalls from, or empty when the
     *        conversation is not in an ACN-enabled project/org.
     * @param ctx The turn context.
     * @returns `{ "project:<ancestorId>" }` or an empty list.
     */
    QStringList scopesFor(const MemoryContext& ctx) const;

    AcnService& m_acn;
    ConversationService& m_convSvc;
};

}  // namespace Chat

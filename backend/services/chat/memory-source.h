// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file memory-source.h
 * @brief IMemorySource: a pluggable pre-turn recall source (RAG corpus,
 *        per-agent memory, team memory) behind the MemoryRetriever coordinator.
 * @layer Service (Search Index)
 * @dependencies (interface only)
 */

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace Chat {

/**
 * @brief The turn context a recall is scoped to.
 */
struct MemoryContext {
    QString conversationId;    ///< The turn's conversation
    QString responderAgentId;  ///< The responding agent (empty in an agentless chat)
    QString queryText;         ///< What to recall against (the latest user text)
    bool forceMemory = false;  ///< Override per-conversation gates ON (the
                               ///< MemoryAugmented agent pattern: always augment)
};

/// Default per-entry character cap for an injected memory bullet. A single ACN
/// team-memory entry is a whole compaction summary (hundreds of tokens); without
/// a cap one entry alone bloats the system prompt and starves the history budget.
constexpr int kMemoryEntryMaxChars = 480;

/**
 * @brief One scored recall candidate from a source.
 *
 * Sources return raw scored candidates; they do NOT format their own prompt
 * section. The MemoryRetriever pools candidates from every enabled source,
 * applies a relevance floor, dedups across sources, ranks globally by score, and
 * renders ONE unified, budgeted block. This is what makes recall "smart": the
 * model only ever sees the most relevant memory across all stores, never each
 * store's blind top-K string-joined together.
 */
struct MemoryHit {
    QString sourceId;       ///< Owning source ("rag" | "aim" | "acn"), for diagnostics
    QString label;          ///< Short provenance tag for the bullet ("team"/"you"/"context")
    QString text;           ///< The candidate text (raw; the coordinator flattens/truncates)
    double score = 0.0;     ///< Relevance: cosine similarity when `semantic`, else FTS rank
    bool semantic = false;  ///< true → `score` is a cross-source-comparable cosine sim
    QVector<float> vector;  ///< The candidate's stored embedding (semantic hits only);
                            ///< the coordinator's MMR/dedup compares these across sources
};

/**
 * @brief Flatten a memory entry to a single line and cap its length.
 *
 * Collapses whitespace/newlines (so a multi-line summary becomes one bullet) and
 * elides anything beyond `maxChars` so no single entry can dominate the budget.
 *
 * @param raw      The entry text.
 * @param maxChars Max characters to keep; longer text is elided with "…".
 * @returns The flattened, capped single-line text ("" if `raw` is blank).
 */
inline QString flattenMemoryEntry(const QString& raw, int maxChars = kMemoryEntryMaxChars) {
    QString t = raw.simplified();
    if (t.isEmpty()) {
        return {};
    }
    if (maxChars > 1 && t.length() > maxChars) {
        t = t.left(maxChars - 1).trimmed() + QStringLiteral("…");
    }
    return t;
}

/**
 * @brief One recall source contributing scored candidates to the unified ranker.
 *
 * Each source owns its OWN enablement gate, scope resolution, vec0 store, item
 * cap, and lexical fallback, but NOT formatting or cross-source ranking. The
 * MemoryRetriever consults each source's `isEnabled`, embeds the query ONCE for
 * whichever sources use embeddings, gathers each source's `candidates()`, then
 * unifies them (relevance floor + dedup + global rank + budget). This keeps the
 * coordinator owning the algorithm and lets a heavy future source (e.g. ACN's
 * relationship graph) live entirely inside its own module without blocking others.
 */
class IMemorySource {
  public:
    virtual ~IMemorySource() = default;

    /** @brief Stable id for logging/diagnostics. @returns "rag" | "aim" | "acn". */
    virtual QString id() const = 0;

    /**
     * @brief Whether this source contributes for the given turn. This is its OWN gate,
     *        consulted independently of every other source.
     * @param ctx The turn context.
     * @returns true to include this source in the retrieval.
     */
    virtual bool isEnabled(const MemoryContext& ctx) const = 0;

    /**
     * @brief Whether this source ranks by embedding (so the coordinator embeds
     *        the query once on its behalf). A purely-lexical source returns false.
     * @returns true if `candidates` benefits from a query vector.
     */
    virtual bool usesEmbedding() const = 0;

    /**
     * @brief Produce this source's scored recall candidates for the turn.
     * @param ctx      The turn context (scopes derive from it).
     * @param queryVec The once-computed query embedding; EMPTY means embedding
     *                 was unavailable/failed → fall back to lexical recall.
     * @param model    The model that produced `queryVec` (for dimension/model
     *                 filtering); empty when `queryVec` is empty.
     * @returns Scored candidates (unformatted), best-first; empty when nothing is
     *          recalled. Synchronous + main-thread (DB reads); must not block on
     *          the network. Semantic candidates set `MemoryHit::semantic = true`
     *          with a cosine score; lexical (FTS) fallbacks set it false.
     */
    virtual QList<MemoryHit>
    candidates(const MemoryContext& ctx, const QVector<float>& queryVec, const QString& model) = 0;
};

}  // namespace Chat

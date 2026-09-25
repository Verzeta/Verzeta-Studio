// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file memory-retriever.cpp
 * @brief Implementation of MemoryRetriever: embed-once, fan-out pre-turn recall.
 * @layer Service (Search Index)
 * @dependencies EmbeddingWorker, IMemorySource
 */

#include "services/chat/memory-retriever.h"

#include "services/vector-store.h"
#include "utils/logger.h"
#include "workers/embedding-worker.h"

#include <algorithm>
#include <QMetaObject>
#include <QSet>

namespace Chat {

namespace {
// Tracking-id prefix so the shared query worker's results route here (RagService
// uses "ragq:" and ignores ours; we ignore "ragq:"/other).
constexpr char kMemPrefix[] = "mem:";

// Hard ceiling on the TOTAL injected-memory block (all sources combined), in
// characters (~3 chars/token ⇒ ~1.3k tokens). Memory recall is a bounded hint,
// not history: without this cap the injected memory grows turn over turn, inflates
// the system prompt, and starves the history budget until the turn no longer fits.
constexpr int kMaxInjectionChars = 4000;

// Max memory bullets injected per turn (after relevance gating, dedup, ranking).
constexpr int kMaxEntries = 8;

// Cosine-similarity relevance floor. A semantic candidate is injected only if it
// is at least this related to the current prompt; below it the memory is dropped,
// so an off-topic turn injects nothing rather than blind top-K. Tunable knob.
constexpr double kRelevanceFloor = 0.35;

// Cosine similarity above which two candidates are treated as the SAME memory and
// the lower-ranked one is dropped. This is the cross-source de-duplication
// guarantee: even if RAG and ACN hold the same summary (different wording), their
// vectors are near-identical, so it is injected at most once. Tunable knob.
constexpr double kDuplicateSim = 0.92;

// MMR trade-off: relevance vs. diversity (1.0 = pure relevance; lower favours
// novelty so the injected set isn't several near-paraphrases of one fact).
constexpr double kMmrLambda = 0.7;

quint64 parseId(const QString& workerId) {
    if (!workerId.startsWith(QString::fromLatin1(kMemPrefix))) {
        return 0;
    }
    bool ok = false;
    const quint64 id = workerId.mid(static_cast<int>(sizeof(kMemPrefix)) - 1).toULongLong(&ok);
    return ok ? id : 0;
}

// Normalized key for textual de-dup of LEXICAL (vector-less) candidates, which
// can't participate in the embedding-based MMR/duplicate check.
QString normKey(const QString& text) {
    return text.simplified().toLower().left(160);
}
}  // namespace

MemoryRetriever::MemoryRetriever(EmbeddingWorker& queryEmbedder, QObject* parent)
    : QObject(parent), m_queryEmbedder(queryEmbedder) {
    connect(&m_queryEmbedder,
            &EmbeddingWorker::embeddingReady,
            this,
            &MemoryRetriever::onEmbeddingReady);
    connect(&m_queryEmbedder,
            &EmbeddingWorker::errorOccurred,
            this,
            &MemoryRetriever::onEmbeddingError);
}

void MemoryRetriever::addSource(IMemorySource* source) {
    if (source) {
        m_sources.append(source);
    }
}

quint64 MemoryRetriever::beginRetrieve(const MemoryContext& ctx) {
    if (ctx.queryText.trimmed().isEmpty()) {
        return 0;
    }
    // Collect the independently-gated, enabled sources.
    QList<IMemorySource*> enabled;
    bool needEmbed = false;
    for (IMemorySource* s : m_sources) {
        if (s && s->isEnabled(ctx)) {
            enabled.append(s);
            needEmbed = needEmbed || s->usesEmbedding();
        }
    }
    if (enabled.isEmpty()) {
        return 0;  // nothing to do → caller dispatches un-augmented (no block)
    }

    const quint64 rid = ++m_nextId;
    m_pending.insert(rid, Pending{ctx, enabled});

    if (needEmbed) {
        // ONE embed for ALL enabled sources (same query, same model) — the
        // anti-collision + performance core. On success → semantic; on failure
        // → lexical fallback (onEmbeddingError).
        QMetaObject::invokeMethod(
            &m_queryEmbedder,
            "computeEmbedding",
            Qt::QueuedConnection,
            Q_ARG(QString, QString::fromLatin1(kMemPrefix) + QString::number(rid)),
            Q_ARG(QString, ctx.queryText),
            Q_ARG(QString, QStringLiteral("query")));
    } else {
        // No source needs an embed → assemble lexically, but emit on the next
        // event-loop turn so the caller (which checks the non-zero id first) is
        // already waiting on retrievalReady. Signal-driven, no timer.
        QMetaObject::invokeMethod(
            this,
            [this, rid]() {
                const auto it = m_pending.find(rid);
                if (it == m_pending.end()) {
                    return;
                }
                const Pending p = it.value();
                m_pending.erase(it);
                emit retrievalReady(rid, assemble(p.sources, p.ctx, {}, QString()));
            },
            Qt::QueuedConnection);
    }
    return rid;
}

void MemoryRetriever::onEmbeddingReady(const QString& id,
                                       const QVector<float>& vec,
                                       const QString& model) {
    const quint64 rid = parseId(id);
    if (rid == 0) {
        return;  // not ours
    }
    const auto it = m_pending.find(rid);
    if (it == m_pending.end()) {
        return;
    }
    const Pending p = it.value();
    m_pending.erase(it);
    emit retrievalReady(rid, assemble(p.sources, p.ctx, vec, model));
}

void MemoryRetriever::onEmbeddingError(const QString& id, const QString& error) {
    const quint64 rid = parseId(id);
    if (rid == 0) {
        return;
    }
    const auto it = m_pending.find(rid);
    if (it == m_pending.end()) {
        return;
    }
    const Pending p = it.value();
    m_pending.erase(it);
    qCWarning(verzetaMemory) << "MemoryRetriever: query embed failed — lexical fallback:" << error;
    // Empty vector → every source uses its lexical fallback (N4).
    emit retrievalReady(rid, assemble(p.sources, p.ctx, {}, QString()));
}

QString MemoryRetriever::assemble(const QList<IMemorySource*>& sources,
                                  const MemoryContext& ctx,
                                  const QVector<float>& vec,
                                  const QString& model) const {
    // 1) Pool scored candidates from every enabled source (no formatting yet).
    QList<MemoryHit> pool;
    for (IMemorySource* s : sources) {
        pool += s->candidates(ctx, vec, model);
    }
    if (pool.isEmpty()) {
        return {};
    }

    // 2) Partition: semantic (vector-backed, cosine-scored, relevance-gated) vs.
    //    lexical (FTS matches — query-relevant by construction, but no vector to
    //    rank/dedup geometrically). A semantic candidate below the relevance floor
    //    is simply not related to the prompt → dropped (no blind injection).
    QList<MemoryHit> semantic;
    QList<MemoryHit> lexical;
    for (const MemoryHit& h : pool) {
        if (h.semantic && !h.vector.isEmpty()) {
            if (h.score >= kRelevanceFloor) {
                semantic.append(h);
            }
        } else {
            lexical.append(h);
        }
    }

    // 3) MMR over the semantic pool: greedily pick the candidate that maximises
    //    relevance while penalising redundancy with what's already chosen, and
    //    HARD-drop near-duplicates of any selected entry. This is what guarantees
    //    RAG/AIM/ACN cannot inject the same memory twice (vectors, not wording).
    QList<MemoryHit> selected;
    while (!semantic.isEmpty() && selected.size() < kMaxEntries) {
        int bestIdx = -1;
        double bestMmr = -1e9;
        for (int i = 0; i < semantic.size(); ++i) {
            double maxSim = 0.0;
            for (const MemoryHit& s : selected) {
                maxSim = std::max(maxSim,
                                  static_cast<double>(VectorStore::cosineSimilarity(
                                      semantic.at(i).vector, s.vector)));
            }
            const double mmr = kMmrLambda * semantic.at(i).score - (1.0 - kMmrLambda) * maxSim;
            if (mmr > bestMmr) {
                bestMmr = mmr;
                bestIdx = i;
            }
        }
        if (bestIdx < 0) {
            break;
        }
        const MemoryHit chosen = semantic.takeAt(bestIdx);
        selected.append(chosen);
        // Remove every remaining near-duplicate of the chosen entry.
        for (int i = semantic.size() - 1; i >= 0; --i) {
            if (VectorStore::cosineSimilarity(semantic.at(i).vector, chosen.vector) >=
                kDuplicateSim) {
                semantic.removeAt(i);
            }
        }
    }

    // 4) Append lexical (FTS) candidates not already covered. No vectors → de-dup
    //    by normalized text against the selected set and each other.
    QSet<QString> seenKeys;
    for (const MemoryHit& h : selected) {
        seenKeys.insert(normKey(h.text));
    }
    for (const MemoryHit& h : lexical) {
        if (selected.size() >= kMaxEntries) {
            break;
        }
        const QString key = normKey(h.text);
        if (key.isEmpty() || seenKeys.contains(key)) {
            continue;
        }
        seenKeys.insert(key);
        selected.append(h);
    }

    if (selected.isEmpty()) {
        return {};
    }

    // 5) Render ONE unified, budgeted block — entries are relevance-ordered, each
    //    flattened to a single line and capped so no summary dominates.
    const QString header = QStringLiteral("\n\n## Relevant memory\n");
    int remaining = kMaxInjectionChars - header.length();
    QString body;
    int injected = 0;
    for (const MemoryHit& h : selected) {
        const QString t = flattenMemoryEntry(h.text);
        if (t.isEmpty()) {
            continue;
        }
        const QString line = QStringLiteral("- (%1) %2\n").arg(h.label, t);
        if (line.length() > remaining) {
            break;  // budget exhausted; stop (highest-relevance entries kept)
        }
        body += line;
        remaining -= line.length();
        ++injected;
    }
    if (injected == 0) {
        return {};
    }
    qCDebug(verzetaMemory).nospace()
        << "MemoryRetriever: injected " << injected << "/" << pool.size() << " candidate(s) ["
        << (vec.isEmpty() ? "lexical" : "semantic") << ", "
        << (kMaxInjectionChars - header.length() - remaining) << " chars]";
    return header + body;
}

}  // namespace Chat

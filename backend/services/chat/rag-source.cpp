// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file rag-source.cpp
 * @brief Implementation of RagSource.
 * @layer Service (Search Index)
 * @dependencies RagService, ConversationService
 */

#include "services/chat/rag-source.h"

#include "models/llm-config.h"
#include "services/conversation-service.h"
#include "services/rag-service.h"

namespace Chat {

namespace {
constexpr int kRagTopK = 5;
}  // namespace

RagSource::RagSource(RagService& rag, ConversationService& convSvc)
    : m_rag(rag), m_convSvc(convSvc) {}

QString RagSource::id() const {
    return QStringLiteral("rag");
}

bool RagSource::isEnabled(const MemoryContext& ctx) const {
    if (ctx.conversationId.isEmpty()) {
        return false;
    }
    // forceMemory (the MemoryAugmented pattern) overrides the per-conversation
    // toggle ON so that pattern always augments — through this one coordinator
    // path, not a second retrieval.
    if (ctx.forceMemory) {
        return true;
    }
    const auto conv = m_convSvc.getConversation(ctx.conversationId);
    return conv.has_value() && LlmConfig::fromJson(conv->llmConfig).ragEnabled;
}

bool RagSource::usesEmbedding() const {
    return true;
}

QList<MemoryHit> RagSource::candidates(const MemoryContext& ctx,
                                       const QVector<float>& queryVec,
                                       const QString& model) {
    // RAG is semantic-only — no lexical fallback (matches pre-coordinator
    // behaviour: with no embedder it contributes nothing).
    if (queryVec.isEmpty()) {
        return {};
    }
    const QList<RagChunk> chunks =
        m_rag.recallWithVector(queryVec, model, ctx.conversationId, kRagTopK);
    QList<MemoryHit> out;
    out.reserve(chunks.size());
    for (const RagChunk& c : chunks) {
        MemoryHit h;
        h.sourceId = id();
        h.label = QStringLiteral("context");
        h.text = c.text;
        h.score = static_cast<double>(c.score);
        h.semantic = true;
        h.vector = c.embedding;
        out.append(h);
    }
    return out;
}

}  // namespace Chat

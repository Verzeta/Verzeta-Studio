// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file aim-source.cpp
 * @brief Implementation of AimSource.
 * @layer Service (Search Index)
 * @dependencies AgentMemoryService, ConversationService
 */

#include "services/chat/aim-source.h"

#include "models/llm-config.h"
#include "services/agent-memory-service.h"
#include "services/conversation-service.h"

namespace Chat {

namespace {
constexpr int kAimTopK = 5;
}  // namespace

AimSource::AimSource(AgentMemoryService& mem, ConversationService& convSvc)
    : m_mem(mem), m_convSvc(convSvc) {}

QString AimSource::id() const {
    return QStringLiteral("aim");
}

bool AimSource::isEnabled(const MemoryContext& ctx) const {
    if (ctx.conversationId.isEmpty()) {
        return false;
    }
    // forceMemory (the MemoryAugmented pattern) overrides the toggle ON.
    if (ctx.forceMemory) {
        return true;
    }
    const auto conv = m_convSvc.getConversation(ctx.conversationId);
    // Default ON: memory recall is on for everyone unless explicitly disabled.
    return !conv.has_value() || LlmConfig::fromJson(conv->llmConfig).aimEnabled;
}

bool AimSource::usesEmbedding() const {
    return true;
}

QStringList AimSource::scopesFor(const MemoryContext& ctx) const {
    if (!ctx.responderAgentId.isEmpty()) {
        return {QStringLiteral("agent:%1").arg(ctx.responderAgentId)};
    }
    if (!ctx.conversationId.isEmpty()) {
        return {QStringLiteral("conversation:%1").arg(ctx.conversationId)};
    }
    return {};
}

QList<MemoryHit> AimSource::candidates(const MemoryContext& ctx,
                                       const QVector<float>& queryVec,
                                       const QString& model) {
    const QStringList scopes = scopesFor(ctx);
    if (scopes.isEmpty()) {
        return {};
    }
    // Semantic primary; lexical (FTS) is the fallback when there's no embedder,
    // OR when the entries matching this query simply aren't embedded yet (so a
    // freshly-saved memory is still recallable immediately).
    const bool haveVec = !queryVec.isEmpty();
    QList<AgentMemoryHit> hits = haveVec ? m_mem.recallSemantic(queryVec, model, scopes, kAimTopK)
                                         : m_mem.recall(ctx.queryText, scopes, kAimTopK);
    bool semantic = haveVec;
    if (haveVec && hits.isEmpty()) {
        hits = m_mem.recall(ctx.queryText, scopes, kAimTopK);  // FTS fallback
        semantic = false;
    }
    QList<MemoryHit> out;
    out.reserve(hits.size());
    for (const AgentMemoryHit& h : hits) {
        MemoryHit m;
        m.sourceId = id();
        m.label = QStringLiteral("you");
        m.text = h.text;
        m.score = h.score;
        m.semantic = semantic && !h.embedding.isEmpty();
        m.vector = h.embedding;
        out.append(m);
    }
    return out;
}

}  // namespace Chat

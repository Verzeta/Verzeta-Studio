// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file acn-source.cpp
 * @brief Implementation of AcnSource.
 * @layer Service (Search Index)
 * @dependencies AcnService, ConversationService
 */

#include "services/chat/acn-source.h"

#include "models/llm-config.h"
#include "services/acn-service.h"
#include "services/conversation-service.h"

namespace Chat {

namespace {
constexpr int kAcnTopK = 5;
}  // namespace

AcnSource::AcnSource(AcnService& acn, ConversationService& convSvc)
    : m_acn(acn), m_convSvc(convSvc) {}

QString AcnSource::id() const {
    return QStringLiteral("acn");
}

QStringList AcnSource::scopesFor(const MemoryContext& ctx) const {
    if (ctx.conversationId.isEmpty()) {
        return {};
    }
    const QString ancestor = m_convSvc.projectOrgAncestorId(ctx.conversationId);
    if (ancestor.isEmpty()) {
        return {};  // not inside a project/organization → ACN N/A
    }
    return {QStringLiteral("project:%1").arg(ancestor)};
}

bool AcnSource::isEnabled(const MemoryContext& ctx) const {
    if (ctx.conversationId.isEmpty()) {
        return false;
    }
    const QString ancestor = m_convSvc.projectOrgAncestorId(ctx.conversationId);
    if (ancestor.isEmpty()) {
        return false;  // not in a project/org
    }
    // Master switch (folder) AND the per-conversation toggle. The MemoryAugmented
    // forceMemory override deliberately does NOT bypass the project's master
    // switch (a team-admin decision), so it is not consulted here.
    if (!m_convSvc.folderAcnEnabled(ancestor)) {
        return false;
    }
    const auto conv = m_convSvc.getConversation(ctx.conversationId);
    return !conv.has_value() || LlmConfig::fromJson(conv->llmConfig).acnEnabled;
}

bool AcnSource::usesEmbedding() const {
    return true;
}

QList<MemoryHit> AcnSource::candidates(const MemoryContext& ctx,
                                       const QVector<float>& queryVec,
                                       const QString& model) {
    const QStringList scopes = scopesFor(ctx);
    if (scopes.isEmpty()) {
        return {};
    }
    const bool haveVec = !queryVec.isEmpty();
    QList<AcnHit> hits = haveVec ? m_acn.recallSemantic(queryVec, model, scopes, kAcnTopK)
                                 : m_acn.recall(ctx.queryText, scopes, kAcnTopK);
    bool semantic = haveVec;
    if (haveVec && hits.isEmpty()) {
        hits = m_acn.recall(ctx.queryText, scopes, kAcnTopK);  // FTS fallback
        semantic = false;
    }
    QList<MemoryHit> out;
    out.reserve(hits.size());
    for (const AcnHit& h : hits) {
        MemoryHit m;
        m.sourceId = id();
        m.label = QStringLiteral("team");
        m.text = h.text;
        m.score = h.score;
        m.semantic = semantic && !h.embedding.isEmpty();
        m.vector = h.embedding;
        out.append(m);
    }
    return out;
}

}  // namespace Chat

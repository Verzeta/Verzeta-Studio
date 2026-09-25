// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file history-budgeter.cpp
 * @brief Implementation of token-budget-based history pruning.
 *        Walks messages newest-to-oldest, accumulating token estimates,
 *        and returns the largest recent subset that fits the budget.
 * @layer Service
 * @dependencies Qt6::Core, models/message.h
 */

#include "history-budgeter.h"

#include <QtMath>

// ---------------------------------------------------------------------------
// Token estimation
// ---------------------------------------------------------------------------

int HistoryBudgeter::estimateTokens(const QString& text) {
    if (text.isEmpty())
        return 0;
    return qCeil(static_cast<double>(text.length()) / 3.0);
}

int HistoryBudgeter::estimateTokens(const QString& text, ContentClass cls) {
    if (text.isEmpty())
        return 0;
    // CodeOrJson packs denser than prose; use a smaller divisor so the
    // estimate over-counts (never under-counts) the content that actually
    // tokenizes denser. Prose keeps the chars/3 default.
    const double divisor = (cls == ContentClass::CodeOrJson) ? 2.5 : 3.0;
    return qCeil(static_cast<double>(text.length()) / divisor);
}

int HistoryBudgeter::estimateMessageTokens(const Message& msg) {
    // Role token overhead: {"role":"assistant","content":"..."} framing.
    // ~4 tokens for the JSON structure + role name.
    constexpr int kRoleOverhead = 4;

    int tokens = kRoleOverhead + estimateTokens(msg.content);

    // Tool-call metadata adds overhead when present.
    if (!msg.metadata.isEmpty()) {
        const QString metaStr =
            QString::fromUtf8(QJsonDocument(msg.metadata).toJson(QJsonDocument::Compact));
        tokens += estimateTokens(metaStr);
    }

    return tokens;
}

// ---------------------------------------------------------------------------
// Budget selection
// ---------------------------------------------------------------------------

HistoryBudgeter::Result HistoryBudgeter::selectForBudget(const QList<Message>& messages,
                                                         int budgetTokens,
                                                         const QString& excludeMsgId) {
    Result result;
    result.totalDbRows = messages.size();

    if (messages.isEmpty() || budgetTokens <= 0) {
        result.ok = (budgetTokens > 0 || messages.isEmpty());
        return result;
    }


    /**
     * @brief A contiguous run of related messages: either a single
     *        non-tool message or an assistant{tool_calls} row plus
     *        every following role=tool row. Treated as one atomic
     *        unit for budget fitting.
     */
    struct Group {
        int startIdx;  // Inclusive index in `messages`
        int endIdx;    // Exclusive index in `messages`
        int tokens;    // Estimated token sum for all messages in group
        bool hasUser;  // True if group contains a role=user message
    };

    QList<Group> groups;
    groups.reserve(messages.size());

    int i = 0;
    const int n = messages.size();
    while (i < n) {
        const Message& m = messages.at(i);

        // Skip the streaming placeholder.
        if (!excludeMsgId.isEmpty() && m.id == excludeMsgId) {
            ++i;
            continue;
        }

        // Detect tool-call group: assistant with finishReason=tool_calls
        // followed by consecutive role=tool rows.
        if (m.role == QStringLiteral("assistant") &&
            m.finishReason == QStringLiteral("tool_calls")) {
            int groupTokens = estimateMessageTokens(m);
            int j = i + 1;
            while (j < n && messages.at(j).role == QStringLiteral("tool")) {
                if (!excludeMsgId.isEmpty() && messages.at(j).id == excludeMsgId) {
                    ++j;
                    continue;
                }
                groupTokens += estimateMessageTokens(messages.at(j));
                ++j;
            }
            groups.append(Group{i, j, groupTokens, false});
            i = j;
            continue;
        }

        // Single message group.
        const int tokens = estimateMessageTokens(m);
        const bool isUser = (m.role == QStringLiteral("user"));
        groups.append(Group{i, i + 1, tokens, isUser});
        ++i;
    }


    int newestUserGroupIdx = -1;
    for (int g = groups.size() - 1; g >= 0; --g) {
        if (groups.at(g).hasUser) {
            newestUserGroupIdx = g;
            break;
        }
    }

    // If the newest user message alone exceeds budget, signal failure.
    if (newestUserGroupIdx >= 0 && groups.at(newestUserGroupIdx).tokens > budgetTokens) {
        result.ok = false;
        result.droppedRows = result.totalDbRows;
        return result;
    }

    // Walk newest→oldest, marking groups for inclusion.
    QVector<bool> included(groups.size(), false);
    int remaining = budgetTokens;

    // Always include the newest user group first.
    if (newestUserGroupIdx >= 0) {
        included[newestUserGroupIdx] = true;
        remaining -= groups.at(newestUserGroupIdx).tokens;
    }

    // Then walk from the newest group backward, including each if it fits.
    for (int g = groups.size() - 1; g >= 0; --g) {
        if (included[g])
            continue;  // Already included (user group)

        if (groups.at(g).tokens <= remaining) {
            included[g] = true;
            remaining -= groups.at(g).tokens;
        }
        // If it doesn't fit, skip it AND skip all older groups.
        // We never include an older group while skipping a newer one —
        // that would create confusing gaps in the conversation.
        else {
            break;
        }
    }

    int includedTokens = 0;
    int includedCount = 0;
    for (int g = 0; g < groups.size(); ++g) {
        if (!included[g])
            continue;
        const Group& grp = groups.at(g);
        for (int idx = grp.startIdx; idx < grp.endIdx; ++idx) {
            const Message& m = messages.at(idx);
            if (!excludeMsgId.isEmpty() && m.id == excludeMsgId)
                continue;
            result.messages.append(m);
        }
        includedTokens += grp.tokens;
        includedCount += (grp.endIdx - grp.startIdx);
    }

    result.estimatedTokens = includedTokens;
    result.includedRows = includedCount;
    result.droppedRows = result.totalDbRows - includedCount;
    return result;
}

int HistoryBudgeter::applyMinimumHistoryFloor(int historyBudget, int outputReservation) {
    if (historyBudget >= kMinHistoryBudget)
        return historyBudget;
    const int reclaimable = qMax(0, outputReservation - kMinOutputReservation);
    const int deficit = kMinHistoryBudget - historyBudget;
    return historyBudget + qMin(deficit, reclaimable);
}

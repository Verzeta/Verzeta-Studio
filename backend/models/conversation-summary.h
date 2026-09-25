// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-summary.h
 * @brief Data model for a conversation's dynamic-compaction summary.
 *        One row per conversation (new summaries overwrite); maps 1:1
 *        to the `conversation_summaries` table (schema v20).
 * @layer Data Access
 * @dependencies Qt6::Core
 */

#pragma once

#include <QString>

/**
 * @brief One conversation's compaction summary.
 *
 * Produced by ConversationSummarizer, consumed by RequestBuilder
 * (prepended to LlmRequest.messages as a role=system entry when fresh).
 * A summary is FRESH when invalidationReason == "none"; any other value
 * means it must not be injected and the trigger heuristic may
 * regenerate it.
 */
struct ConversationSummary {
    QString conversationId;    ///< References conversations.id (PK)
    QString summaryText;       ///< The generated summary body
    QString lastMessageId;     ///< Newest message covered by the summary
    int lastMessageIdx = 0;    ///< 1-based index of that message at generation time
    int coveredCount = 0;      ///< How many messages the summary covers
    qint64 generatedAtMs = 0;  ///< Epoch ms of generation
    int tokenCount = 0;        ///< HistoryBudgeter::estimateTokens(summaryText)
    QString modelUsed;         ///< e.g. "ollama/qwen3.5:9b"
    QString triggerReason;     ///< "auto" | "manual"
    QString invalidationReason = QStringLiteral("none");
    ///< "none" | "stale" | "member_changed" | "folder_changed"
    ///  | "flashmemory" | "user_regenerated"

    /**
     * @brief True when the summary may be injected into context.
     * @returns true iff non-empty and invalidation_reason is 'none'.
     */
    bool fresh() const {
        return !summaryText.isEmpty() && invalidationReason == QStringLiteral("none");
    }
};

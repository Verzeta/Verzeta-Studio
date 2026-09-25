// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file history-budgeter.h
 * @brief Token-budget-based message history pruning for LLM requests.
 *        Selects the most recent messages that fit within a token budget,
 *        preserving atomic tool-call groups and guaranteeing the newest
 *        user message is never dropped.
 * @layer Service
 * @dependencies Qt6::Core, models/message.h
 */

#pragma once

#include "../models/message.h"

#include <QList>
#include <QString>

/**
 * @brief Selects conversation history messages that fit within a token budget.
 *
 * Pure function with no state, no side effects and no DB access. Takes a
 * chronologically-ordered (ascending) list of messages and returns the
 * largest recent subset that fits the given token budget, measured by
 * a conservative character-based estimator.
 *
 * Invariants enforced:
 *   1. The most recent user message is ALWAYS included. If it alone
 *      exceeds the budget, an empty list is returned and `ok` is set
 *      to false (caller should emit an error, not silently proceed).
 *   2. Tool-call groups are atomic: an assistant row with
 *      finishReason="tool_calls" and all immediately-following
 *      role="tool" rows are included or excluded as a unit.
 *   3. Messages are returned in ascending chronological order (same
 *      as the input), ready for assembleLlmHistory.
 *   4. The total estimated token count of included messages never
 *      exceeds budgetTokens.
 */
class HistoryBudgeter {
  public:
    /**
     * @brief Content class for token estimation.
     *
     * BPE tokenizers pack denser on code / JSON / tool-schema text than
     * on natural-language prose (more punctuation, identifiers, braces,
     * fewer whole-word merges). The default prose estimate (chars/3)
     * under-counts those by ~15-20%, which silently ate the output
     * reservation on tool-heavy turns. CodeOrJson uses a denser divisor
     * so the estimate stays conservative (never under-counts) for the
     * content that actually tokenizes denser, without shrinking the
     * usable window for plain-prose chats.
     */
    enum class ContentClass {
        Prose,      ///< Natural language (chars/3).
        CodeOrJson  ///< Code / JSON / tool schemas / tool payloads (chars/2.5).
    };

    /**
     * @brief Result of a budget selection.
     */
    struct Result {
        QList<Message> messages;  ///< Selected messages in ascending order
        int estimatedTokens = 0;  ///< Estimated token count of included messages
        int totalDbRows = 0;      ///< Total rows passed in
        int includedRows = 0;     ///< Rows included in output
        int droppedRows = 0;      ///< Rows dropped to fit budget
        bool ok = true;           ///< False if newest user message exceeds budget
    };

    /**
     * @brief Selects the most recent messages that fit within budgetTokens.
     * @param messages     All messages in ascending chronological order.
     * @param budgetTokens Available token budget for history (num_ctx minus
     *                     system prompt, tools, and response reserve).
     * @param excludeMsgId UUID to skip (streaming placeholder).
     * @return Result with selected messages and diagnostics.
     *
     * @complexity O(n) where n = messages.size(). Single backward pass.
     */
    static Result selectForBudget(const QList<Message>& messages,
                                  int budgetTokens,
                                  const QString& excludeMsgId = {});

    /**
     * @brief Conservative token estimate for a string.
     * @param text Input text.
     * @return Estimated token count (ceil(chars / 3)). Intentionally
     *         overestimates to avoid overflowing the model's context
     *         window, because underestimation would recreate the silent
     *         truncation bug this class exists to prevent.
     *
     * The divisor 3.0 is conservative: real BPE tokenizers for English
     * typically yield chars/3.5 to chars/4. We trade ~15-25% of usable
     * context capacity for the guarantee that we never exceed num_ctx.
     */
    static int estimateTokens(const QString& text);

    /**
     * @brief Content-class-aware token estimate.
     * @param text Input text.
     * @param cls  Prose (chars/3) or CodeOrJson (chars/2.5, denser).
     * @return Estimated token count; always conservative (over-estimates)
     *         so the assembled prompt can never silently exceed num_ctx.
     *
     * Use CodeOrJson for tool-schema text, tool-call arguments, tool
     * results, and fenced code. They tokenize denser than prose, and an
     * under-count there is what evaporated the output reservation.
     */
    static int estimateTokens(const QString& text, ContentClass cls);

    /**
     * @brief Estimates tokens for a full Message (role + content + metadata).
     * @param msg The message to estimate.
     * @return Token count including role overhead and content.
     */
    static int estimateMessageTokens(const Message& msg);

    /** Minimum history window the floor reclaim targets. Below this
     *  the model cannot even see the newest user ask plus one prior
     *  exchange, which manifests as agents repeating themselves
     *  verbatim across tool-continuation turns. */
    static constexpr int kMinHistoryBudget = 1536;

    /** How much of the output reservation the floor may reclaim. The
     *  reservation is budgeting arithmetic alone. The provider-side
     *  output cap is num_predict, set elsewhere; the longest real
     *  group-chat replies measure ~2000 tokens, so reclaiming down to
     *  a 2048-token reservation is safe for actual reply lengths. */
    static constexpr int kMinOutputReservation = 2048;

    /** Hard real-token output floor guaranteed by the post-assembly
     *  window-shaper: the assembled prompt + this floor must fit num_ctx,
     *  so the model always has room to actually reply. Distinct from the
     *  budgeting-target kOutputReservation (4096): the reservation is the
     *  pre-assembly estimate target; this floor is the guarantee enforced
     *  after the real prompt is measured. ~2000-token longest real reply
     *  + headroom for the estimate's residual error. */
    static constexpr int kRealOutputFloor = 3072;

    /**
     * @brief Applies the minimum-history floor: when the computed
     *        history budget falls below kMinHistoryBudget (large
     *        system prompt + many tools + summary on a small context
     *        window), reclaims the difference from the output
     *        reservation down to kMinOutputReservation. Prompt-side
     *        starvation is strictly worse than a tighter output
     *        ceiling: a starved turn cannot even see the user's ask.
     * @param historyBudget     Budget after subtracting system prompt,
     *                          tools, summary, and the full output
     *                          reservation. May be negative.
     * @param outputReservation The reservation that was subtracted.
     * @return Adjusted history budget; never less than the input,
     *         never adjusted past what the reservation can give.
     */
    static int applyMinimumHistoryFloor(int historyBudget, int outputReservation);
};

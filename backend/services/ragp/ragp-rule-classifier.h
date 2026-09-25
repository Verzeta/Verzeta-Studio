// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-rule-classifier.h
 * @brief Rule-based fast-path classifier (Tier 1) for RAGP. Catches
 *        obvious cases without invoking any LLM; runs in <1 ms.
 * @layer Service
 * @dependencies Qt6::Core
 *
 * Handles:
 *   - No \@mentions → empty classification
 *   - Self-mention only → empty classification (dropped)
 *   - Broadcast aliases (\@all, \@everyone, \@team) → BROADCAST_REQUEST
 *   - User aliases (\@owner, \@user, \@you, \@leader) → ESCALATION_TO_USER
 *   - \@mention inside markdown code fence  → REFERENCE
 *   - \@mention inside block-quote line     → QUOTING
 *   - \@mention with unknown alias          → dropped
 *
 * Anything Tier 1 can't fully classify is marked UNKNOWN so the
 * service can delegate to Tier 2 (cache) or Tier 3 (LLM backend).
 *
 * The classifier is stateless and safe to call from any thread.
 */

#pragma once

#include "ragp-types.h"

namespace Ragp {

/**
 * @brief Stateless rule-based fast-path RAGP classifier.
 */
class RuleClassifier {
  public:
    /**
     * @brief Classify the given request using only structural rules.
     * @param req RAGP classification request.
     * @returns Classification. If the classifier is confident about
     *          every detected \@mention, `confidence == 1.0` and each
     *          target has a definite Intent. If any mention couldn't
     *          be classified, that target's intent is UNKNOWN and
     *          overall `confidence < 1.0`; the service should then
     *          consult higher tiers.
     */
    static Classification classify(const Request& req);

    /**
     * @brief Canonicalize an alias: trim, replace spaces with
     *        underscores, preserve case for display while keeping
     *        comparison case-insensitive.
     * @param alias Raw alias token.
     * @returns Normalised alias string.
     */
    static QString normalizeAlias(const QString& alias);

    /**
     * @brief Whether the token matches a user-escalation alias:
     *        \@owner, \@user, \@you, \@leader.
     * @param token Mention token to test.
     * @returns true iff the token is a user alias.
     */
    static bool isUserAlias(const QString& token);

    /**
     * @brief Whether the token matches a broadcast alias:
     *        \@all, \@everyone, \@team.
     * @param token Mention token to test.
     * @returns true iff the token is a broadcast alias.
     */
    static bool isBroadcastAlias(const QString& token);
};

}  // namespace Ragp

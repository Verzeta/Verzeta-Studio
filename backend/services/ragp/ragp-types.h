// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-types.h
 * @brief Data types for the Routing & Gating Protocol (RAGP).
 *
 *        Classification schema, target representation, request
 *        envelope, and routing policy helpers. All of RAGP's per-turn
 *        decisions flow through these structures. Kept in a dedicated
 *        `Ragp` namespace so the names don't leak into ChatController
 *        or collide with LLM request types.
 * @layer Service
 * @dependencies Qt6::Core.
 */

#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace Ragp {

/**
 * @brief Classification of what an agent's \@mention is trying to do.
 *
 * Ordered by decision impact: routing intents first (cascade fires),
 * non-routing intents next (cascade suppressed), UNKNOWN last. See
 * shouldCascade() for the full cascade/no-cascade map.
 */
enum class Intent {
    // Non-routing (default / unclassified)
    UNKNOWN,  ///< Classifier unable to decide; fallback policy applies

    // Routing intents (cascade fires)
    DELEGATE_RESPONSE,      ///< Agent wants mentioned to respond with input
    DELEGATE_TASK,          ///< Handing off specific work (cascade + task context)
    BROADCAST_REQUEST,      ///< Wants everyone to respond (\@all style)
    CLARIFICATION_REQUEST,  ///< Asking mentioned to clarify their prior statement
    CORRECTION_REQUEST,     ///< Asking mentioned to revise/fix something
    STATUS_CHECK,           ///< Asking about progress
    HANDOFF_COMPLETE,       ///< "Done, over to you"
    PLURAL_ADDRESS,         ///< Addressing 2+ agents together by name
    ESCALATION_TO_USER,     ///< \@owner / \@user: notify the user, no agent cascade

    // Non-routing intents (cascade suppressed)
    REFERENCE,          ///< Just naming/citing, no routing
    ACKNOWLEDGMENT,     ///< Thanks / praise / agreement
    SUMMARY_LIST,       ///< Listing names in summary/roster format
    QUESTION_ABOUT,     ///< Asking ABOUT the person, not TO them
    QUOTING,            ///< Quoting prior speech
    GREETING_FAREWELL,  ///< Social pleasantries (hi / bye)
};

/**
 * @brief Converts an Intent to its string name (for logging, JSON).
 * @param intent Intent to convert.
 * @returns Its string name, or "UNKNOWN" for a value with no name.
 */
QString intentToString(Intent intent);

/**
 * @brief Parses an Intent from its string name. Returns UNKNOWN on miss.
 * @param s String name to parse; matched case-insensitively.
 * @returns The matching Intent, or Intent::UNKNOWN when none matches.
 */
Intent intentFromString(const QString& s);

/**
 * @brief Returns true when this intent should trigger a cascade dispatch.
 *
 * Routing policy:
 *   - DELEGATE_*, CLARIFICATION_REQUEST, CORRECTION_REQUEST,
 *     STATUS_CHECK, HANDOFF_COMPLETE, PLURAL_ADDRESS,
 *     BROADCAST_REQUEST → cascade
 *   - ESCALATION_TO_USER → no cascade (user notification only)
 *   - All other intents → no cascade
 * @param intent Classified intent.
 * @returns true for the intents listed above as triggering a cascade.
 */
bool shouldCascade(Intent intent);

/**
 * @brief One classified \@mention target.
 *
 * Each target carries its own intent because a single agent response can
 * mix intents, e.g. "Thanks @Alice! @Bob your turn" yields two targets
 * with ACKNOWLEDGMENT and HANDOFF_COMPLETE respectively.
 */
struct Target {
    QString alias;                    ///< Canonical alias (normalized)
    Intent intent = Intent::UNKNOWN;  ///< What the mention asks of this target.
    QString context;                  ///< Optional task hint for DELEGATE_TASK etc.
};

/**
 * @brief Result of classifying an agent response.
 */
struct Classification {
    QList<Target> targets;     ///< Empty = no mentions, no routing
    bool routeToUser = false;  ///< \@owner / \@user detected
    double confidence = 0.0;   ///< [0.0, 1.0]
    qint64 latencyMs = 0;      ///< For telemetry
    bool fromCache = false;    ///< True if Tier 2 hit
    QString source;            ///< "rule" | "cache" | "backend:<name>"

    /** True when the classifier judged that the AUTHOR committed to a
     *  concrete tool/file/canvas action they kept for THEMSELVES and
     *  did not execute this turn (semantic verdict, any language).
     *  Drives the group deferred-action continuation with no separate
     *  confirm call and no keyword gate. Tier-1 rules and the cache
     *  never set it (LLM-only judgement). Mutually exclusive with
     *  `pendingDelegateAlias` (a verdict is self, other, or none). */
    bool selfPendingAction = false;

    /** One-line description of the pending action when the verdict is
     *  self OR delegated (diagnostics + nudge/dispatch context). */
    QString pendingActionHint;

    /** Roster alias the pending action was DELEGATED to when the
     *  classifier's verdict is who="other" (the handoff arm of the
     *  LLM-decided continuation). Carries the delegation even when the
     *  teammate's @-mention was classified with a non-routing intent
     *  (e.g. the mention sits in a thanks sentence while a separate
     *  mention-less sentence assigns the work, such as a "thank you,
     *  please go ahead and call complete_task" reply that would
     *  otherwise dead-end silently). Roster-validated by Ragp::Service
     *  (fabricated
     *  aliases cleared; author-as-delegate converts to self). Consumed
     *  by the cascade ONLY when no classified target dispatched.
     *  Tier-1 rules and the cache never set it (LLM-only judgement). */
    QString pendingDelegateAlias;
};

/**
 * @brief Input to the classifier.
 */
struct Request {
    QString content;            ///< Agent's full response text
    QString authorAlias;        ///< Author's alias (normalized); never classified as target
    QStringList rosterAliases;  ///< All agent aliases in the conversation (normalized)

    /** Human-readable list of tool calls the author ALREADY executed
     *  earlier in this same turn-chain (empty = none ran). Stated as
     *  fact in the classify prompt so already-completed work is never
     *  judged to be a pending action. */
    QString executedToolsSummary;

    /** True when the caller needs the pending-action verdict for this
     *  reply (structural action tokens present). Forces the pipeline
     *  to Tier 3: the rule tier's confidence-1.0 "no mentions" short
     *  circuit and the cache CANNOT answer the pending question. The
     *  verdict depends on the executed-tools context, so it is only
     *  ever produced fresh by the LLM backend and never cached. When
     *  false, any selfPendingAction the backend volunteers is cleared
     *  (verdicts flow only when explicitly requested). */
    bool wantsPendingActionVerdict = false;
};

}  // namespace Ragp

Q_DECLARE_METATYPE(Ragp::Classification)
Q_DECLARE_METATYPE(Ragp::Target)

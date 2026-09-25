// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-rule-classifier.cpp
 * @brief Implementation of the Tier 1 rule-based RAGP classifier.
 * @layer Service
 * @dependencies Qt6::Core
 */

#include "ragp-rule-classifier.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace Ragp {

QString RuleClassifier::normalizeAlias(const QString& alias) {
    return alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
}

bool RuleClassifier::isUserAlias(const QString& token) {
    static const QSet<QString> kUserAliases = {
        QStringLiteral("owner"),
        QStringLiteral("user"),
        QStringLiteral("you"),
        QStringLiteral("leader"),
    };
    return kUserAliases.contains(token.toLower());
}

bool RuleClassifier::isBroadcastAlias(const QString& token) {
    static const QSet<QString> kBroadcastAliases = {
        QStringLiteral("all"),
        QStringLiteral("everyone"),
        QStringLiteral("team"),
    };
    return kBroadcastAliases.contains(token.toLower());
}

namespace {

/**
 * @brief Returns true if the position `pos` in `text` falls within a
 *        fenced code block (```...```). Scans from the start of text.
 *        O(n) per call; classifier runs this once per \@mention so total
 *        cost is O(n * mentions), which is negligible at message sizes.
 */
bool isInsideCodeFence(const QString& text, int pos) {
    // Count triple-backtick fences before pos. Odd count = inside.
    int fenceCount = 0;
    int i = 0;
    while (i < pos && i + 2 < text.length()) {
        if (text.at(i) == QLatin1Char('`') && text.at(i + 1) == QLatin1Char('`') &&
            text.at(i + 2) == QLatin1Char('`')) {
            ++fenceCount;
            i += 3;
        } else {
            ++i;
        }
    }
    return (fenceCount % 2) == 1;
}

/**
 * @brief Returns true if the \@mention at `pos` is on a blockquote line.
 *        A blockquote line starts with `>` (optionally after whitespace)
 *        from the nearest preceding newline.
 */
bool isInsideBlockQuote(const QString& text, int pos) {
    // Walk back to the start of the current line.
    int lineStart = pos;
    while (lineStart > 0 && text.at(lineStart - 1) != QLatin1Char('\n')) {
        --lineStart;
    }
    // Skip leading whitespace on the line.
    while (lineStart < pos && text.at(lineStart).isSpace() &&
           text.at(lineStart) != QLatin1Char('\n')) {
        ++lineStart;
    }
    return lineStart < text.length() && text.at(lineStart) == QLatin1Char('>');
}

/**
 * @brief Returns true if `token` matches any alias in roster
 *        (case-insensitive, space ↔ underscore tolerant).
 */
bool rosterContains(const QStringList& roster, const QString& token) {
    const QString normToken = RuleClassifier::normalizeAlias(token);
    for (const QString& alias : roster) {
        const QString normAlias = RuleClassifier::normalizeAlias(alias);
        if (normAlias.compare(normToken, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

}  // anonymous namespace

Classification RuleClassifier::classify(const Request& req) {
    Classification result;
    result.source = QStringLiteral("rule");
    result.confidence = 1.0;
    result.fromCache = false;

    // Fast path: no @mentions at all → empty result with full confidence.
    static const QRegularExpression kMentionRe(QStringLiteral("@([A-Za-z0-9_]+)"));
    QRegularExpressionMatchIterator it = kMentionRe.globalMatch(req.content);
    if (!it.hasNext()) {
        return result;  // No mentions → no targets, confidence 1.0
    }

    const QString selfNorm = normalizeAlias(req.authorAlias);

    // Deduplicate: same alias mentioned multiple times produces one target
    // at the first occurrence's classification (most deterministic).
    QSet<QString> seenAliases;

    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString token = match.captured(1);
        const int matchStart = match.capturedStart(0);

        // Rule 1: self-mention → skip silently.
        if (normalizeAlias(token).compare(selfNorm, Qt::CaseInsensitive) == 0) {
            continue;
        }

        // Rule 2: user-escalation aliases.
        if (isUserAlias(token)) {
            result.routeToUser = true;
            continue;  // Don't add as a target (no agent to cascade to).
        }

        // Rule 3: broadcast aliases.
        if (isBroadcastAlias(token)) {
            // Add one BROADCAST_REQUEST target per roster member
            // (other than self). Preserves deterministic output.
            for (const QString& alias : req.rosterAliases) {
                const QString normAlias = normalizeAlias(alias);
                if (normAlias.compare(selfNorm, Qt::CaseInsensitive) == 0) {
                    continue;
                }
                if (seenAliases.contains(normAlias.toLower())) {
                    continue;
                }
                seenAliases.insert(normAlias.toLower());
                Target t;
                t.alias = normAlias;
                t.intent = Intent::BROADCAST_REQUEST;
                result.targets.append(t);
            }
            continue;
        }

        // Rule 4: @mention inside a markdown code fence → REFERENCE.
        if (isInsideCodeFence(req.content, matchStart)) {
            // Only add if this alias is in the roster (otherwise silently drop).
            if (rosterContains(req.rosterAliases, token)) {
                const QString normTok = normalizeAlias(token);
                if (!seenAliases.contains(normTok.toLower())) {
                    seenAliases.insert(normTok.toLower());
                    Target t;
                    t.alias = normTok;
                    t.intent = Intent::REFERENCE;
                    result.targets.append(t);
                }
            }
            continue;
        }

        // Rule 5: @mention on a block-quoted line → QUOTING.
        if (isInsideBlockQuote(req.content, matchStart)) {
            if (rosterContains(req.rosterAliases, token)) {
                const QString normTok = normalizeAlias(token);
                if (!seenAliases.contains(normTok.toLower())) {
                    seenAliases.insert(normTok.toLower());
                    Target t;
                    t.alias = normTok;
                    t.intent = Intent::QUOTING;
                    result.targets.append(t);
                }
            }
            continue;
        }

        // Rule 6: @mention not in roster → drop silently (unknown alias).
        if (!rosterContains(req.rosterAliases, token)) {
            continue;
        }

        // Rule 7: @mention with no other rule firing → UNKNOWN.
        // Tier 2 (cache) or Tier 3 (LLM) will decide.
        const QString normTok = normalizeAlias(token);
        if (seenAliases.contains(normTok.toLower())) {
            continue;
        }
        seenAliases.insert(normTok.toLower());

        Target t;
        t.alias = normTok;
        t.intent = Intent::UNKNOWN;
        result.targets.append(t);
        result.confidence = 0.0;  // At least one unclassified target → drop confidence.
    }

    return result;
}

}  // namespace Ragp

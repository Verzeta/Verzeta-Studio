// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file content-sanitizer.cpp
 * @brief Pure pipeline implementation. Every pattern, flag, pass
 *        count, and diagnostic log line matches the behaviour the
 *        request-finished path expected before extraction. The port
 *        is mechanical: the observable output from a given set of
 *        inputs is identical.
 *
 *        Only non-mechanical adjustments:
 *          - The pipeline operates on a local QString `content`
 *            instead of reading/writing `m_streaming->content()` /
 *            `m_streaming->rewrite()` every step. The caller flushes
 *            the final value with one `rewrite()` call after the
 *            pipeline returns (same net observable effect).
 *          - The `m_membershipService`/`m_convSvc.getConversation(...)`
 *            roster lookup is resolved by the caller and threaded in
 *            as `SanitizeInputs::rosterAliases` + `isGroupChat` so
 *            the pipeline stays a pure function.
 *          - The `m_taskGate->activePlanId()` check becomes
 *            `inputs.activeTaskPlanSet`, and the log-prefix tail
 *            takes the 8-char id via `inputs.activePlanIdPrefix`.
 *
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core (QRegularExpression), utils/logger.h.
 *               No service pointers, no DbManager, no signal wiring.
 */

#include "content-sanitizer.h"

#include "../../utils/logger.h"

#include <QRegularExpression>
#include <QStringList>

namespace {
inline bool streamTraceEnabled() {
    static const bool s_enabled = qEnvironmentVariableIntValue("VERZETA_STREAM_TRACE") > 0;
    return s_enabled;
}
}  // namespace


namespace Chat {

ContentSanitizer::ContentSanitizer(QObject* parent) : QObject(parent) {}

ContentSanitizer::~ContentSanitizer() = default;

SanitizeResult ContentSanitizer::sanitize(const SanitizeInputs& inputs) {
    // Record starting length so `charsStripped` reflects the net
    // size change across all passes. The pre-extraction code only
    // exposed individual per-pass chars-stripped numbers via log
    // lines; the aggregate is new metadata.
    const int originalLength = inputs.content.length();

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    const bool kTrace = streamTraceEnabled();
    if (kTrace) {
        qCInfo(verzetaUi).noquote() << "TRACE: SANITIZE-ENTRY responder=" << inputs.responderAlias
                                    << " isGroupChat=" << inputs.isGroupChat
                                    << " rosterSize=" << inputs.rosterAliases.size()
                                    << " activeTaskPlanSet=" << inputs.activeTaskPlanSet
                                    << " inputChars=" << originalLength;
    }
    auto traceStage = [&](const QString& tag,
                          const QString& before,
                          const QString& after,
                          const QString& detail = QString()) {
        if (!kTrace)
            return;
        if (before == after) {
            qCInfo(verzetaUi).noquote()
                << "TRACE: " << tag << " no-op (" << before.size() << " chars)"
                << (detail.isEmpty() ? QString() : QStringLiteral(" ") + detail);
            return;
        }
        qCInfo(verzetaUi).noquote()
            << "TRACE: " << tag << " mutated"
            << " before=" << before.size() << " after=" << after.size()
            << " delta=" << (before.size() - after.size())
            << (detail.isEmpty() ? QString() : QStringLiteral(" ") + detail);
        qCInfo(verzetaUi).noquote() << "TRACE: " << tag << "-BEFORE-BEGIN\n"
                                    << before << "\nTRACE: " << tag << "-BEFORE-END";
        qCInfo(verzetaUi).noquote() << "TRACE: " << tag << "-AFTER-BEGIN\n"
                                    << after << "\nTRACE: " << tag << "-AFTER-END";
    };
    // === END STREAM-TRACE DEBUG ===

    // Working copy — replaces the pre-extraction interleaved
    // m_streaming->content() read + m_streaming->rewrite() write
    // pairs with a single local mutation. Net observable result is
    // the same; the StreamingManager flush happens once in the
    // caller after the pipeline returns.
    QString content = inputs.content;

    SanitizeResult result;
    // Short-circuit on empty content — matches the pre-extraction
    // `if (!content.isEmpty())` guards that wrapped every stage.
    if (content.isEmpty()) {
        result.sanitisedContent = content;
        result.declaredTaskStatus = QString();
        result.charsStripped = 0;
        return result;
    }

    static const QRegularExpression nudgeRx(
        QStringLiteral(
            R"(^\s*\[?@?[A-Za-z0-9_ ]{1,64},?\s*it is your turn to respond\.?\s*\]?\s*\n*)"),
        QRegularExpression::CaseInsensitiveOption);
    {
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        QString s0Before = content;
        // === END STREAM-TRACE DEBUG ===
        const auto nudgeMatch = nudgeRx.match(content);
        if (nudgeMatch.hasMatch() && nudgeMatch.capturedStart() == 0 &&
            nudgeMatch.capturedLength() > 0) {
            qCWarning(verzetaUi) << "Stripped echoed cascade nudge (" << nudgeMatch.capturedLength()
                                 << "chars) — responder was" << inputs.responderAlias;
            content = content.mid(nudgeMatch.capturedLength());
        }
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        traceStage(QStringLiteral("STAGE-0-NUDGE-STRIP"), s0Before, content);
        // === END STREAM-TRACE DEBUG ===
    }

    // -----------------------------------------------------------------
    // Stage 2: leading LARP attribution prefix strip.
    //
    // Build the alias alternation from:
    //   1. The responder's own alias (covers 1:1 and group self-LARP).
    //   2. Every OTHER roster member alias (group chat only) — covers
    //      cross-LARP where the model attributes its own reply to a
    //      different teammate.
    //   3. User pseudonyms (User, Human, Human user, Human_user, owner,
    //      you) — never legitimate content at the start of an agent
    //      reply.
    // -----------------------------------------------------------------
    QStringList aliasPatterns;
    if (!inputs.responderAlias.isEmpty()) {
        const QString a = inputs.responderAlias.trimmed();
        aliasPatterns.append(QRegularExpression::escape(a));
        aliasPatterns.append(
            QRegularExpression::escape(QString(a).replace(QLatin1Char(' '), QLatin1Char('_'))));
    }
    if (inputs.isGroupChat) {
        for (const QString& alias : inputs.rosterAliases) {
            if (alias.isEmpty())
                continue;
            const QString a = alias.trimmed();
            aliasPatterns.append(QRegularExpression::escape(a));
            aliasPatterns.append(
                QRegularExpression::escape(QString(a).replace(QLatin1Char(' '), QLatin1Char('_'))));
        }
    }
    for (const QString& userAlias : {
             QStringLiteral("User"),
             QStringLiteral("Human"),
             QStringLiteral("Human user"),
             QStringLiteral("Human_user"),
             QStringLiteral("owner"),
             QStringLiteral("you"),
         }) {
        aliasPatterns.append(QRegularExpression::escape(userAlias));
    }

    if (!aliasPatterns.isEmpty()) {
        aliasPatterns.removeDuplicates();
        const QString alternation = aliasPatterns.join(QLatin1Char('|'));
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        QString s2Before = content;
        // === END STREAM-TRACE DEBUG ===
        // Pattern loop: one prefix per pass (the alias is CAPTURED so
        // each stripped prefix can be identity-classified), with an
        // intervening-nudge strip between passes, so a response like
        //   "(Human said) foo [@Sam, it is your turn] (Alice said) bar"
        // still ends up with just "bar". Up to 8 passes so stacked
        // chain-echoes converge (previously 3 passes of an unbounded
        // `+` group — same convergence, per-prefix visibility).
        const QRegularExpression attribRx(
            QStringLiteral(R"(^\s*\**\(?\s*@?(%1)\s+said\s*\)?\**\s*:?\s*\n*)").arg(alternation),
            QRegularExpression::CaseInsensitiveOption);

        const auto namesMatch = [](const QString& a, const QString& b) {
            const QString na = QString(a).replace(QLatin1Char('_'), QLatin1Char(' ')).trimmed();
            const QString nb = QString(b).replace(QLatin1Char('_'), QLatin1Char(' ')).trimmed();
            return na.compare(nb, Qt::CaseInsensitive) == 0;
        };
        const QStringList userPseudonyms{
            QStringLiteral("User"),
            QStringLiteral("Human"),
            QStringLiteral("Human user"),
            QStringLiteral("Human_user"),
            QStringLiteral("owner"),
            QStringLiteral("you"),
        };

        for (int pass = 0; pass < 8; ++pass) {
            const auto match = attribRx.match(content);
            if (!match.hasMatch() || match.capturedLength() == 0)
                break;
            const QString namedAlias = match.captured(1);
            qCWarning(verzetaUi) << "Stripped LARP attribution prefix (" << match.capturedLength()
                                 << "chars, named" << namedAlias << ") — responder was"
                                 << inputs.responderAlias;

            if (inputs.isGroupChat && result.impersonatedAlias.isEmpty() &&
                !namesMatch(namedAlias, inputs.responderAlias)) {
                bool isPseudonym = false;
                for (const QString& p : userPseudonyms) {
                    if (namesMatch(namedAlias, p)) {
                        isPseudonym = true;
                        break;
                    }
                }
                if (!isPseudonym) {
                    for (const QString& rosterAlias : inputs.rosterAliases) {
                        if (namesMatch(namedAlias, rosterAlias)) {
                            result.impersonatedAlias = rosterAlias;
                            qCWarning(verzetaUi)
                                << "Misattributed reply: leading prefix"
                                << "names teammate @" << rosterAlias
                                << "but the seated responder is @" << inputs.responderAlias
                                << "— flagging for reject-and-retry";
                            break;
                        }
                    }
                }
            }

            content = content.mid(match.capturedLength()).trimmed();
            // Also strip a trailing nudge-echo that may sit between
            // two attribution prefixes.
            const auto nudgeMatch = nudgeRx.match(content);
            if (nudgeMatch.hasMatch() && nudgeMatch.capturedStart() == 0 &&
                nudgeMatch.capturedLength() > 0) {
                content = content.mid(nudgeMatch.capturedLength()).trimmed();
            }
        }
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        traceStage(QStringLiteral("STAGE-2-LARP-PREFIX-STRIP"), s2Before, content);
        QString s3Before = content;
        // === END STREAM-TRACE DEBUG ===

        // -------------------------------------------------------------
        // Stage 3: mid-content transcript-echo truncation.
        //
        // A `(<OTHER roster alias> said)` past position 0 is NEVER
        // legitimate agent output. Truncate at the match and chop any
        // dangling separator runs ('-', '---', '***').
        //
        // Self-attributions and user-pseudonyms ("(Sam said)",
        // "(User said)", "(Human said)") are also stripped — but only
        // for OTHER-than-self aliases. We rebuild the alternation here
        // so the cut catches only teammate attributions; the responder
        // own alias mid-content is left intact (rare but legitimate,
        // e.g. "as @Sam I think...").
        // -------------------------------------------------------------
        QStringList otherAliasPatterns;
        if (inputs.isGroupChat) {
            for (const QString& alias : inputs.rosterAliases) {
                if (alias.isEmpty())
                    continue;
                if (alias.compare(inputs.responderAlias, Qt::CaseInsensitive) == 0) {
                    continue;
                }
                const QString a = alias.trimmed();
                otherAliasPatterns.append(QRegularExpression::escape(a));
                otherAliasPatterns.append(QRegularExpression::escape(
                    QString(a).replace(QLatin1Char(' '), QLatin1Char('_'))));
            }
        }
        for (const QString& userAlias : {
                 QStringLiteral("User"),
                 QStringLiteral("Human"),
                 QStringLiteral("Human user"),
                 QStringLiteral("Human_user"),
             }) {
            otherAliasPatterns.append(QRegularExpression::escape(userAlias));
        }

        if (!otherAliasPatterns.isEmpty()) {
            otherAliasPatterns.removeDuplicates();
            const QString otherAlt = otherAliasPatterns.join(QLatin1Char('|'));
            const QRegularExpression midAttribRx(
                QStringLiteral(R"(\(\s*@?(?:%1)\s+said\s*\))").arg(otherAlt),
                QRegularExpression::CaseInsensitiveOption);
            const auto midMatch = midAttribRx.match(content);
            if (midMatch.hasMatch() && midMatch.capturedStart() > 0) {
                const int cutAt = midMatch.capturedStart();
                qCWarning(verzetaUi)
                    << "TRUNCATED mid-content transcript echo at"
                    << "position" << cutAt << "(was" << content.length() << "chars,"
                    << "dropping" << (content.length() - cutAt)
                    << "chars of fake attribution) — responder was" << inputs.responderAlias;
                // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
                // Dump the EXACT dropped tail so the operator can see
                // whether it really was a LARP `(Other said)` transcript
                // echo or whether the cut was a false positive on a
                // legitimate substring.
                if (kTrace) {
                    const QString droppedTail = content.mid(cutAt);
                    qCWarning(verzetaUi).noquote()
                        << "TRACE: STAGE-3-MIDCUT-DROPPED-TAIL-BEGIN cutAt=" << cutAt
                        << " matchedAlias=" << midMatch.captured(0) << "\n"
                        << droppedTail << "\nTRACE: STAGE-3-MIDCUT-DROPPED-TAIL-END";
                }
                // === END STREAM-TRACE DEBUG ===
                QString cleaned = content.left(cutAt).trimmed();
                // Drop a trailing horizontal rule or separator if it
                // was leading into the echoed block.
                while (cleaned.endsWith(QStringLiteral("---")) ||
                       cleaned.endsWith(QStringLiteral("***")) ||
                       cleaned.endsWith(QLatin1Char('-'))) {
                    cleaned.chop(1);
                }
                content = cleaned.trimmed();
            }
        }
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        traceStage(QStringLiteral("STAGE-3-MIDCONTENT-CUT"), s3Before, content);
        // === END STREAM-TRACE DEBUG ===
    }

    // -----------------------------------------------------------------
    // Stage 3b: deterministic self-mention de-sigil (group chats).
    //
    // A responder must never publish its OWN "@alias" — @-handles
    // address OTHER people, and a self-mention in persisted history
    // re-seeds the parroting on every later turn (the model copies its
    // own handle back out of the transcript). The prompt-side identity
    // guards reduce but cannot guarantee this on small models, and the
    // RAGP layers already skip self-mentions for ROUTING — so removing
    // the sigil here changes no routing behaviour; it fixes the OUTPUT
    // and the history-replay loop.
    //
    // The rewrite drops ONLY the '@' and keeps the name ("@Alice" →
    // "Alice") so prose still reads naturally. Other members' mentions
    // are untouched (the cascade needs them). The lookbehind keeps
    // email-like tokens ("ops@Alice.example") intact; the lookahead
    // keeps distinct handles ("@Alice2") intact when the alias is
    // "Alice". Idempotent: once the sigil is gone there is nothing
    // left to match.
    // -----------------------------------------------------------------
    if (inputs.isGroupChat && !inputs.responderAlias.isEmpty() && !content.isEmpty()) {
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        QString s3bBefore = content;
        // === END STREAM-TRACE DEBUG ===
        const QString selfRaw = inputs.responderAlias.trimmed();
        const QString selfNorm = QString(selfRaw).replace(QLatin1Char(' '), QLatin1Char('_'));
        QStringList selfPatterns;
        selfPatterns.append(QRegularExpression::escape(selfNorm));
        if (selfNorm != selfRaw) {
            selfPatterns.append(QRegularExpression::escape(selfRaw));
        }
        selfPatterns.removeDuplicates();
        const QRegularExpression selfMentionRx(
            QStringLiteral(R"((?<![A-Za-z0-9_])@(%1)(?![A-Za-z0-9_]))")
                .arg(selfPatterns.join(QLatin1Char('|'))),
            QRegularExpression::CaseInsensitiveOption);
        const int beforeLen = content.length();
        content.replace(selfMentionRx, QStringLiteral("\\1"));
        if (content.length() != beforeLen) {
            qCDebug(verzetaUi) << "de-sigiled" << (beforeLen - content.length())
                               << "self-mention(s) of @" << inputs.responderAlias
                               << "in the responder's own reply";
        }
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        traceStage(QStringLiteral("STAGE-3B-SELF-MENTION-DESIGIL"), s3bBefore, content);
        // === END STREAM-TRACE DEBUG ===
    }

    // Stage 4: final trim before the think/task passes.
    content = content.trimmed();

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    QString s5Before = content;
    // === END STREAM-TRACE DEBUG ===

    // -----------------------------------------------------------------
    // Stage 5: <think>...</think> removal — safety net.
    //
    // The primary path for inline `<think>` extraction is now
    // `Chat::InlineThinkingHoister` running inside each provider's
    // chunk parser; captured text lands in `Message::thinkingContent`
    // and is rendered by the assistant-bubble disclosure.  This stage
    // catches any blocks the hoister missed (odd-whitespace variants,
    // pathological partial-tag patterns that the hoister released as
    // content, etc.) so users never see raw `<think>` chrome in chat.
    // -----------------------------------------------------------------
    if (!content.isEmpty()) {
        static const QRegularExpression kThinkBlockRx(
            QStringLiteral(R"rx(<\s*think\s*>.*?<\s*/\s*think\s*>\s*)rx"),
            QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::DotMatchesEverythingOption);
        const int beforeLen = content.length();
        content.replace(kThinkBlockRx, QString());
        if (content.length() != beforeLen) {
            qCDebug(verzetaUi) << "stripped <think> block(s) from reply ("
                               << (beforeLen - content.length()) << "chars dropped)";
        }
    }

    // Stage 5b: orphan </think> closer at the head — qwen-family
    // models occasionally emit a closing tag without the matching
    // opener when the chat template already opened the think block
    // and the inner reasoning streamed empty. Strip a leading
    // </think> (with optional whitespace before it) so users never
    // see the bare tag.
    if (!content.isEmpty()) {
        static const QRegularExpression kOrphanThinkCloseRx(
            QStringLiteral(R"rx(^\s*<\s*/\s*think\s*>\s*)rx"),
            QRegularExpression::CaseInsensitiveOption);
        content.replace(kOrphanThinkCloseRx, QString());
    }

    // Stage 5d: harmony channel-control markers.
    //
    // Some models — and Ollama's chat template for certain tags
    // (observed on gemma4:e4b) — leak harmony channel headers like
    // "<channel|>" / "<|channel|>" / "<|message|>" / "<|final|>" into
    // message.content instead of using them to separate the hidden
    // analysis/commentary channel from the visible final reply. Left in,
    // they (a) show as raw chrome in the user's message ("…web
    // search.<channel|>") and (b) — worse — land in the PERSISTED message,
    // so the next turn's history feeds the marker back to the model, which
    // parrots it; the pollution compounds as the chat grows, degrading
    // into planning-as-reply LARP and half-finished turns. Strip the
    // marker tokens (and a trailing channel name when present). The regex
    // requires at least one pipe so ordinary text like "<end>" is never
    // touched; tool calls are unaffected (they ride a separate field).
    if (!content.isEmpty()) {
        static const QRegularExpression kHarmonyMarkerRx(
            QStringLiteral(
                R"rx((?:<\|(?:channel|message|start|end|constrain|return|call|final|analysis|commentary)\|?>|<(?:channel|message|start|end|constrain|return|call|final|analysis|commentary)\|>)(?:\s*(?:final|analysis|commentary)\b)?)rx"),
            QRegularExpression::CaseInsensitiveOption);
        const int beforeLen = content.length();
        content.replace(kHarmonyMarkerRx, QString());
        if (content.length() != beforeLen) {
            qCDebug(verzetaUi) << "stripped harmony channel marker(s) from reply ("
                               << (beforeLen - content.length()) << "chars dropped)";
        }
    }

    // Stage 5c: stray qwen-style in-band tool-call tags that the
    // OllamaProvider's hoister missed (e.g. variants with extra
    // whitespace, or models emitting them in non-Ollama providers
    // that don't run the hoister). Strip them so users never see
    // raw `<|tool_call|>...</|tool_call|>` chrome in chat — the
    // body of an unhoisted block is dropped (it represents a tool
    // intent the dispatcher could not action; the agent is told
    // to retry via the function-calling protocol next turn).
    if (!content.isEmpty()) {
        static const QRegularExpression kInbandToolRx(
            QStringLiteral(R"rx(<\|tool_call\|>.*?<\|/?tool_call\|>)rx"),
            QRegularExpression::DotMatchesEverythingOption);
        content.replace(kInbandToolRx, QString());
    }

    // -----------------------------------------------------------------
    // Stage 6: <task_state> / <task-state> / <task_status> marker
    // strip + value capture. The marker is ALWAYS stripped, but the
    // captured value is only returned to the caller when an active
    // task is anchored — without that the model is mimicking chat
    // history and must not drive plan state.
    // -----------------------------------------------------------------
    if (!content.isEmpty()) {
        static const QRegularExpression kTaskStateStripRx(
            QStringLiteral(
                R"rx(\s*<\s*task[_\s-]?(?:state|status)\s*>\s*"?(working|completed|waiting|blocked)"?\s*<\s*/\s*task[_\s-]?(?:state|status)\s*>\s*$)rx"),
            QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::DotMatchesEverythingOption);
        const auto stripMatch = kTaskStateStripRx.match(content);
        if (stripMatch.hasMatch()) {
            result.declaredTaskStatus = stripMatch.captured(1).toLower();
            content = content.left(stripMatch.capturedStart()).trimmed();
            if (inputs.activeTaskPlanSet) {
                qCDebug(verzetaUi) << "stripped task status marker =" << result.declaredTaskStatus
                                   << "(dropped" << stripMatch.capturedLength() << "chars)";
            } else {
                qCDebug(verzetaUi) << "stripped stray task marker =" << result.declaredTaskStatus
                                   << "(no active task; marker was mimicked from history)";
                // No active task → don't drive plan state from this.
                result.declaredTaskStatus.clear();
            }
        } else if (inputs.activeTaskPlanSet) {
            // Diagnostic: the active task is set but the model didn't
            // emit a recognisable marker. Log what the tail looks like.
            const QString tail = content.right(200).replace(QLatin1Char('\n'), QLatin1Char(' '));
            qCDebug(verzetaUi) << "active task" << inputs.activePlanIdPrefix
                               << "but no task_state marker parsed — tail:" << tail;
        }
    }

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    traceStage(QStringLiteral("STAGE-5-6-THINK-TASK-STRIP"), s5Before, content);
    if (kTrace) {
        qCInfo(verzetaUi).noquote() << "TRACE: SANITIZE-EXIT inputChars=" << originalLength
                                    << " outputChars=" << content.length()
                                    << " charsStripped=" << (originalLength - content.length());
    }
    // === END STREAM-TRACE DEBUG ===

    result.sanitisedContent = content;
    result.charsStripped = originalLength - content.length();
    return result;
}

}  // namespace Chat

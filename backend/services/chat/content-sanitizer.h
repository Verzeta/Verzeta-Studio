// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file content-sanitizer.h
 * @brief Pure, stateless sanitisation pipeline that ChatController's
 *        `onRequestFinished` applies to responder content before the
 *        row is persisted and before `m_streaming->finalize` runs.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core only. No service pointers, no DbManager, no
 *               signal wiring. Every operation is a function of
 *               (input, configuration) → output.
 *
 * The six pipeline stages, in execution order:
 *
 *   1. Leading nudge strip.
 *      Small models sometimes echo the cascade-tail routing nudge
 *      (`[@Alias, it is your turn to respond.]`) as the first line of
 *      their reply. The nudge has a fixed shape, so any match at
 *      position 0 is transcript regurgitation and is removed.
 *
 *   2. Leading LARP-prefix strip.
 *      Attribution prefixes of the form `(Alice said)` or
 *      `(Human said)`, whether for the responder's own alias, for any other
 *      roster member, or for any user pseudonym, are stripped from
 *      the start. Up to three passes catch stacked chains like
 *      `(Alice said)(Bob said) ...`. Between passes the nudge-strip
 *      runs again to catch `(Alice said) [nudge] (Bob said) ...`
 *      interleaving. The self-alias and user pseudonyms are ALWAYS
 *      considered; roster aliases only apply when the conversation is
 *      a group chat.
 *
 *   3. Mid-content transcript-echo truncation.
 *      A `(Alias said)` attribution appearing PAST position 0, where
 *      Alias is a roster member OTHER than the responder (or a user
 *      pseudonym), is never legitimate agent output. The content is
 *      truncated at the match start, and trailing separator runs
 *      (`---`, `***`, `-`) are chopped so the cut doesn't leave a
 *      dangling horizontal-rule line.
 *
 *   3b. Self-mention de-sigil (group chats only).
 *      A responder's OWN `@alias` inside its OWN reply is rewritten to
 *      the bare name (`@Alice` → `Alice`). @-handles address OTHER
 *      people; a persisted self-mention re-seeds the parroting on
 *      every later turn via history replay. Routing is unaffected
 *      (the RAGP layers already skip self-mentions), so this is a
 *      deterministic output/history guarantee on top of the
 *      prompt-side identity guards. Other members' mentions are
 *      untouched; email-like tokens and longer distinct handles are
 *      protected by boundary assertions; the rewrite is idempotent.
 *
 *   4. Final trim.
 *      After 1–3 the content is `.trimmed()` before the think/task
 *      passes run.
 *
 *   5. `\<think\>` block strip.
 *      Thinking-capable models (qwen3 family, etc) sometimes emit
 *      their internal reasoning inline in the content stream wrapped
 *      in `\<think\>...\</think\>` tags instead of the provider's
 *      separate `thinking` channel. Every such block is removed so
 *      the user doesn't see a wall of reasoning followed by a tiny
 *      final reply.
 *
 *   6. `\<task_state\>` / `<task-state>` / `<task_status>` marker strip.
 *      A marker at the END of the content names the responder's
 *      declared task status (working / completed / waiting / blocked).
 *      The marker is ALWAYS stripped from the output regardless of
 *      whether an active task is anchored, because the raw tag must
 *      not be visible. The captured value is RETURNED to the caller in
 *      `SanitizeResult::declaredTaskStatus`, but only when an active
 *      task was anchored. Without an anchor the value is the model
 *      mimicking history and must not drive plan state; the caller
 *      sees an empty string.
 *
 * This is a pure transform: identical inputs always produce identical
 * output. Every regex pattern, pass count, and flag matches the
 * behaviour expected by ChatController's request-finished path so the
 * extraction adds zero behavioural drift.
 *
 * Threading: lock-free and re-entrant. `sanitize()` may run on any
 * thread. The diagnostic log lines use `qCInfo` / `qCWarning` which
 * are thread-safe. The class remains a QObject so observability
 * signals (e.g. `contentSanitised(kind, chars)`) can be added later
 * without breaking call sites; none are emitted today.
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace Chat {

/**
 * @brief Inputs for a single invocation of the sanitisation pipeline.
 */
struct SanitizeInputs {
    /** Raw responder content before sanitisation. Empty is a valid
     *  input; the pipeline returns an empty result. */
    QString content;

    /** Alias of the current responder. Used by the self-LARP strip
     *  (stage 2) and to exclude self-attribution from the mid-content
     *  truncation alternation (stage 3). Empty in 1:1 chats or when
     *  cascade has not set a responder. */
    QString responderAlias;

    /** True iff the current conversation is a group chat. When false,
     *  the `rosterAliases` list is not consulted for the LARP patterns;
     *  the self-alias and user pseudonyms still apply. */
    bool isGroupChat = false;

    /** Every alias in the current conversation's roster (group chat
     *  members). Used by stages 2 and 3 to build the alternation of
     *  legitimate and illegitimate attribution prefixes. Ignored when
     *  `isGroupChat` is false. */
    QStringList rosterAliases;

    /** True iff an active-plan anchor is set on the conversation.
     *  Gates whether a captured `\<task_state\>` value is returned to
     *  the caller (stage 6). When false, the marker is still stripped
     *  from the output but the captured value is cleared, because the model
     *  was mimicking chat history and must not drive plan state. */
    bool activeTaskPlanSet = false;

    /** First 8 chars of the active-plan id. Used only by the
     *  diagnostic log line printed when an active task is set but no
     *  `\<task_state\>` marker is parsed. Empty is fine. */
    QString activePlanIdPrefix;
};

/**
 * @brief Outcome of a single invocation of the sanitisation pipeline.
 */
struct SanitizeResult {
    /** Final cleaned content, ready for DB persistence and UI. */
    QString sanitisedContent;

    /** Captured value from the `\<task_state\>` marker when the input
     *  had one AND an active task was anchored. Otherwise empty. One
     *  of: `""` / `"working"` / `"completed"` / `"waiting"` /
     *  `"blocked"`. */
    QString declaredTaskStatus;

    /** Sum of chars removed across every pass. Diagnostic only;
     *  the caller may log or ignore it. */
    int charsStripped = 0;

    /** Set when a LEADING attribution prefix named a DIFFERENT
     *  current group member than the seated responder, i.e. the
     *  model wrote the reply in a teammate's voice ("(Engineer
     *  said) …" from Alice's seat). The caller must treat the reply
     *  as MISATTRIBUTED (bounded delete-and-retry), never persist it
     *  under the responder: stripping the label alone would launder
     *  the imposter body into the responder's history and poison
     *  every later request. Holds the named teammate's alias as it
     *  appears in the roster. Empty for self-attribution, user
     *  pseudonyms, 1:1 chats, and clean replies. */
    QString impersonatedAlias;
};

/**
 * @brief Stateless QObject collaborator hosting the sanitisation
 *        pipeline. See the file-level comment for the full pipeline
 *        description.
 *
 * Ownership: constructed in ChatController's constructor as a
 * std::unique_ptr member. No dependencies on other Chat:: collaborators;
 * destruction order is whatever member-declaration order dictates.
 */
class ContentSanitizer : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the sanitizer.
     * @param parent Optional Qt parent.
     */
    explicit ContentSanitizer(QObject* parent = nullptr);
    ~ContentSanitizer() override;

    /**
     * @brief Run the full pipeline over a SanitizeInputs snapshot.
     * @param inputs Pre-populated input bundle.
     * @return       SanitizeResult with the cleaned content, captured
     *               declared status (if any), and total chars stripped.
     * @complexity   O(n · k) in content length n and pattern count k.
     *               Pattern compilation is done once per invocation for
     *               the alternation-based regexes (alias set varies)
     *               and via static QRegularExpression for the fixed
     *               ones (nudge, think, task_state).
     * @sideeffects  Emits zero signals. May log at `qCInfo` /
     *               `qCWarning` for diagnostic strip counts.
     */
    SanitizeResult sanitize(const SanitizeInputs& inputs);
};

}  // namespace Chat

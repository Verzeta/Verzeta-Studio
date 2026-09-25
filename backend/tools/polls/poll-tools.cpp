// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file poll-tools.cpp
 * @brief Implementations of the four agent-callable poll tools.
 * @layer Service (Tool subsystem)
 * @dependencies PollService, ConversationService, Chat::CascadeController.
 */


#include "poll-tools.h"

#include "../../services/chat/cascade-controller.h"
#include "../../services/poll-service.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace Tools {

namespace {

QJsonObject makeError(const QString& msg) {
    QJsonObject o;
    o.insert(QStringLiteral("error"), msg);
    return o;
}

/**
 * Convert PollService's QVariantMap projection into a QJsonObject
 * suitable for an agent tool result. Drops the per-vote rank (always
 * -1 in v1) and the conversation_id field (the agent already knows
 * what conversation it's in).
 */
QJsonObject projectionToJson(const QVariantMap& m) {
    QJsonObject out;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        const QString& k = it.key();
        if (k == QStringLiteral("conversation_id"))
            continue;
        const QVariant& v = it.value();
        // typeId() — not canConvert<QVariantList>() — because in Qt 6
        // every QString reports canConvert<QVariantList>() = true
        // (strings round-trip as single-element lists through the
        // metatype system). We only want to descend into the array
        // shape when the actual stored type IS a list.
        const auto tid = v.typeId();
        if (tid == QMetaType::QVariantList || tid == QMetaType::QStringList) {
            QJsonArray arr;
            for (const QVariant& item : v.toList()) {
                if (item.typeId() == QMetaType::QVariantMap) {
                    QJsonObject sub;
                    const QVariantMap subMap = item.toMap();
                    for (auto sit = subMap.constBegin(); sit != subMap.constEnd(); ++sit) {
                        sub.insert(sit.key(), QJsonValue::fromVariant(sit.value()));
                    }
                    arr.append(sub);
                } else {
                    arr.append(QJsonValue::fromVariant(item));
                }
            }
            out.insert(k, arr);
        } else {
            out.insert(k, QJsonValue::fromVariant(v));
        }
    }
    return out;
}

/**
 * Resolve { alias, kind } for the calling agent.
 *
 * Precedence chain:
 *
 *   1. `args["__caller_agent_alias"]`, host-injected by
 *      ToolDispatcher / ToolService from the calling CC's batch
 *      inputs so wire-side per-client cascades resolve to their
 *      own responder identity, not a statically-captured pointer.
 *   2. `deps.cascade->currentResponderAlias()`, the fallback for
 *      direct invocations / tests that bypass ToolService.
 *   3. Literal `"assistant"`, the 1:1-chat fallback.  In a GROUP chat
 *      the cascade alias is the agent's per-conversation alias
 *      ("Alice", "Bob").  In a 1:1 chat the alias is intentionally
 *      empty (ChatController only seeds the cascade with `agentId`,
 *      no alias, because 1:1 chats don't use member aliasing; see
 *      chat-controller.cpp:485).  The voter_alias column is just a
 *      unique vote key (not a UI label) so a stable identifier
 *      suffices.
 */
/**
 * @brief Resolved caller identity used to attribute poll writes
 *        (creator + voter). Held as a value-type intermediate; never
 *        persisted directly.
 */
struct CallerIdentity {
    QString alias;
    QString kind = QStringLiteral("agent");

    /**
     * @brief Reports whether the identity carries enough data to act on.
     * @returns True iff `alias` is non-empty.
     */
    bool valid() const { return !alias.isEmpty(); }
};

CallerIdentity resolveCaller(const QJsonObject& args, const PollToolDeps& deps) {
    CallerIdentity c;
    // Args-injected alias from per-CC ToolDispatcher wins over the
    // captured-LOCAL CascadeController pointer so wire-side per-client
    // cascades resolve correctly.
    const QString injectedAlias = args.value(QStringLiteral("__caller_agent_alias")).toString();
    if (!injectedAlias.isEmpty()) {
        c.alias = injectedAlias;
        return c;
    }
    // Fallback: read the cascade pointer directly.  Correct for
    // LOCAL-CC dispatch and for direct callers that bypass
    // ToolService.
    if (deps.cascade) {
        const QString cascadeAlias = deps.cascade->currentResponderAlias();
        if (!cascadeAlias.isEmpty()) {
            c.alias = cascadeAlias;
            return c;
        }
    }
    // 1:1-chat fallback. The agent is identified by the
    // conversation's primaryAgentId (which IS seeded on the
    // cascade — only the alias half stays empty).
    c.alias = QStringLiteral("assistant");
    return c;
}

/**
 * @brief Resolve the active conversation id for a poll tool invocation.
 *
 * Precedence chain:
 *   1. `args["__caller_conv_id"]`, host-injected by ToolDispatcher
 *      from the calling CC's batch inputs.
 *   2. `deps.activeConvIdGetter()`, the captured-LOCAL fallback.
 *   3. Empty string; the caller must error out with its own message.
 */
QString resolveConvId(const QJsonObject& args, const PollToolDeps& deps) {
    const QString injected = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (!injected.isEmpty())
        return injected;
    if (deps.activeConvIdGetter)
        return deps.activeConvIdGetter();
    return QString();
}

}  // namespace

// =============================================================================
// start_poll
// =============================================================================

QString StartPollTool::name() const {
    return QStringLiteral("start_poll");
}

QString StartPollTool::description() const {
    return QStringLiteral("Start a poll in the current conversation. Other agents and "
                          "the user can vote on the options. Use this to coordinate "
                          "group-chat decisions ('which approach should we take?', "
                          "'are we ready to ship?'). The poll appears inline as a card "
                          "in the conversation.\n\n"
                          "DO NOT call this if there is already an open poll covering "
                          "the same question — check the ACTIVE POLLS prompt block "
                          "FIRST. If the user's request matches an existing open poll, "
                          "call cast_vote / get_poll_results / close_poll on that "
                          "poll_id instead. Duplicate-question polls are hard-rejected "
                          "by the host with an 'existing_poll_id' field telling you "
                          "which poll to use instead.\n\n"
                          "Args: question (required), options (required, 2-10 "
                          "strings), mode (optional: 'single' default, or 'multi' to "
                          "allow multiple selections per voter), closes_in_minutes "
                          "(optional, omit for no auto-close).");
}

QList<ToolParameterSchema> StartPollTool::parameters() const {
    ToolParameterSchema q;
    q.name = QStringLiteral("question");
    q.type = QStringLiteral("string");
    q.description = QStringLiteral("The poll question. Keep it short and answerable by the "
                                   "options list (e.g. 'Should we ship today?').");
    q.required = true;

    ToolParameterSchema opts;
    opts.name = QStringLiteral("options");
    opts.type = QStringLiteral("array");
    opts.description = QStringLiteral("2-10 option strings. Each option's text is what voters "
                                      "(agents OR the user) pick. Empty / duplicate strings are "
                                      "dropped before insert.");
    opts.required = true;

    ToolParameterSchema mode;
    mode.name = QStringLiteral("mode");
    mode.type = QStringLiteral("string");
    mode.description = QStringLiteral("'single' (default — one vote per voter) or 'multi' (a voter "
                                      "may pick multiple options). Ranked voting is not yet "
                                      "supported; pass 'single' or 'multi'.");
    mode.required = false;

    ToolParameterSchema closes;
    closes.name = QStringLiteral("closes_in_minutes");
    closes.type = QStringLiteral("integer");
    closes.description =
        QStringLiteral("Optional auto-close deadline in minutes from now. Omit (or "
                       "pass 0) to leave the poll open until someone calls "
                       "close_poll explicitly.");
    closes.required = false;

    return {q, opts, mode, closes};
}

QJsonValue StartPollTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("poll tool deps not configured"));
    }
    const QString convId = resolveConvId(args, m_deps);
    if (convId.isEmpty()) {
        return makeError(QStringLiteral("start_poll requires an active conversation"));
    }
    const auto caller = resolveCaller(args, m_deps);
    if (!caller.valid()) {
        return makeError(QStringLiteral("could not resolve caller identity — cascade not seated"));
    }

    const QString question = args.value(QStringLiteral("question")).toString().trimmed();
    if (question.isEmpty()) {
        return makeError(QStringLiteral("question is required"));
    }

    const QJsonArray optsArr = args.value(QStringLiteral("options")).toArray();
    QStringList opts;
    opts.reserve(optsArr.size());
    for (const QJsonValue& v : optsArr)
        opts.append(v.toString());
    if (opts.size() < 2) {
        return makeError(QStringLiteral("options must contain at least 2 items"));
    }
    if (opts.size() > 10) {
        return makeError(QStringLiteral("options cannot exceed 10 items"));
    }

    QString mode = args.value(QStringLiteral("mode")).toString().toLower();
    if (mode.isEmpty())
        mode = QStringLiteral("single");
    if (mode != QStringLiteral("single") && mode != QStringLiteral("multi")) {
        return makeError(
            QStringLiteral("mode must be 'single' or 'multi' (ranked is not yet supported)"));
    }

    int closesInMinutes = args.value(QStringLiteral("closes_in_minutes")).toInt(0);
    if (closesInMinutes < 0)
        closesInMinutes = 0;

    {
        const QString needle = question.trimmed().toLower();
        const QVariantList existing = m_deps.polls->pollsForConversation(convId, /*openOnly=*/true);
        for (const QVariant& v : existing) {
            const QVariantMap m = v.toMap();
            const QString eq = m.value(QStringLiteral("question")).toString().trimmed().toLower();
            if (eq == needle) {
                const QString existingId = m.value(QStringLiteral("id")).toString();
                QJsonObject err;
                err.insert(QStringLiteral("error"),
                           QStringLiteral("An open poll with this question already exists "
                                          "in the conversation. Do NOT create a duplicate. "
                                          "Use cast_vote / get_poll_results / close_poll on "
                                          "the existing poll_id."));
                err.insert(QStringLiteral("existing_poll_id"), existingId);
                err.insert(QStringLiteral("question"), question);
                return err;
            }
        }
    }

    const QString pollId = m_deps.polls->createPoll(
        convId, caller.alias, caller.kind, question, opts, mode, closesInMinutes);
    if (pollId.isEmpty()) {
        return makeError(QStringLiteral("could not create poll — see logs for the underlying "
                                        "validation failure (mode must be 'single' or 'multi', "
                                        "options must be 2..10 distinct strings, question must "
                                        "be non-empty)"));
    }
    return projectionToJson(m_deps.polls->pollResults(pollId));
}

// =============================================================================
// cast_vote
// =============================================================================

QString CastVoteTool::name() const {
    return QStringLiteral("cast_vote");
}

QString CastVoteTool::description() const {
    return QStringLiteral("Cast a vote on an open poll. You may identify the option by "
                          "either option_id (UUID from the poll's options list) OR "
                          "option_text (case-insensitive match against the option "
                          "string — easier than copying the id). In 'single' mode, "
                          "casting a second vote replaces your prior one. In 'multi' "
                          "mode, you can cast votes for several options across "
                          "separate cast_vote calls.");
}

QList<ToolParameterSchema> CastVoteTool::parameters() const {
    ToolParameterSchema pid;
    pid.name = QStringLiteral("poll_id");
    pid.type = QStringLiteral("string");
    pid.description = QStringLiteral("The id of the poll, as shown in the ACTIVE POLLS prompt "
                                     "layer or returned by start_poll / get_poll_results.");
    pid.required = true;

    ToolParameterSchema oid;
    oid.name = QStringLiteral("option_id");
    oid.type = QStringLiteral("string");
    oid.description = QStringLiteral("Option uuid. Pass this OR option_text — not both. Prefer "
                                     "option_text when you have the literal option string.");
    oid.required = false;

    ToolParameterSchema otext;
    otext.name = QStringLiteral("option_text");
    otext.type = QStringLiteral("string");
    otext.description = QStringLiteral("Option text (case-insensitive match against the poll's "
                                       "options list). Use this instead of option_id when you have "
                                       "the literal option string — it's easier than copying a "
                                       "UUID. Pass this OR option_id, not both.");
    otext.required = false;

    return {pid, oid, otext};
}

QJsonValue CastVoteTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("poll tool deps not configured"));
    }
    const auto caller = resolveCaller(args, m_deps);
    if (!caller.valid()) {
        return makeError(QStringLiteral("could not resolve caller identity — cascade not seated"));
    }

    const QString pollId = args.value(QStringLiteral("poll_id")).toString().trimmed();
    const QString optionId = args.value(QStringLiteral("option_id")).toString().trimmed();
    const QString optionText = args.value(QStringLiteral("option_text")).toString().trimmed();
    if (pollId.isEmpty()) {
        return makeError(QStringLiteral("poll_id is required"));
    }
    if (optionId.isEmpty() && optionText.isEmpty()) {
        return makeError(QStringLiteral("either option_id or option_text is required"));
    }

    const bool ok = m_deps.polls->castVote(pollId, caller.alias, caller.kind, optionId, optionText);
    if (!ok) {
        return makeError(QStringLiteral("cast_vote refused — poll may be closed, option may not "
                                        "belong to this poll, or option_text didn't match any "
                                        "option. Call get_poll_results to inspect the current "
                                        "state."));
    }
    return projectionToJson(m_deps.polls->pollResults(pollId));
}

// =============================================================================
// get_poll_results
// =============================================================================

QString GetPollResultsTool::name() const {
    return QStringLiteral("get_poll_results");
}

QString GetPollResultsTool::description() const {
    return QStringLiteral("Return the full state of a poll: question, options, "
                          "per-option vote counts, status (open/closed), winner ids "
                          "if any. Pass poll_id to look up a specific poll, or omit "
                          "it to receive a list of every poll in the current "
                          "conversation (open + closed, newest first).");
}

QList<ToolParameterSchema> GetPollResultsTool::parameters() const {
    ToolParameterSchema pid;
    pid.name = QStringLiteral("poll_id");
    pid.type = QStringLiteral("string");
    pid.description = QStringLiteral("Optional. When provided, returns just that poll's state. "
                                     "When omitted, returns every poll in the current "
                                     "conversation.");
    pid.required = false;
    return {pid};
}

QJsonValue GetPollResultsTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("poll tool deps not configured"));
    }

    const QString pollId = args.value(QStringLiteral("poll_id")).toString().trimmed();
    if (!pollId.isEmpty()) {
        const QVariantMap m = m_deps.polls->pollResults(pollId);
        if (m.isEmpty()) {
            return makeError(QStringLiteral("unknown poll_id"));
        }
        return projectionToJson(m);
    }

    // No poll_id → list every poll in the active conversation.
    const QString convId = resolveConvId(args, m_deps);
    if (convId.isEmpty()) {
        return makeError(QStringLiteral("no poll_id provided and no active conversation"));
    }
    const QVariantList list = m_deps.polls->pollsForConversation(convId, /*openOnly=*/false);
    QJsonArray arr;
    for (const QVariant& v : list) {
        arr.append(projectionToJson(v.toMap()));
    }
    QJsonObject out;
    out.insert(QStringLiteral("polls"), arr);
    out.insert(QStringLiteral("count"), arr.size());
    return out;
}

// =============================================================================
// close_poll
// =============================================================================

QString ClosePollTool::name() const {
    return QStringLiteral("close_poll");
}

QString ClosePollTool::description() const {
    return QStringLiteral("Close a poll explicitly. Idempotent — closing an already-"
                          "closed poll succeeds without side effects. Closed polls "
                          "still appear in get_poll_results so historic decisions stay "
                          "visible.");
}

QList<ToolParameterSchema> ClosePollTool::parameters() const {
    ToolParameterSchema pid;
    pid.name = QStringLiteral("poll_id");
    pid.type = QStringLiteral("string");
    pid.description = QStringLiteral("The id of the poll to close.");
    pid.required = true;
    return {pid};
}

QJsonValue ClosePollTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("poll tool deps not configured"));
    }
    const QString pollId = args.value(QStringLiteral("poll_id")).toString().trimmed();
    if (pollId.isEmpty()) {
        return makeError(QStringLiteral("poll_id is required"));
    }
    if (!m_deps.polls->closePoll(pollId)) {
        return makeError(QStringLiteral("could not close poll — id may be unknown"));
    }
    return projectionToJson(m_deps.polls->pollResults(pollId));
}

}  // namespace Tools

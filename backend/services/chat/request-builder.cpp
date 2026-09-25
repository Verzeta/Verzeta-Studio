// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file request-builder.cpp
 * @brief Implementation of Chat::RequestBuilder. Two public entry
 *        points: assembleHistory (walk-and-pair reconstruction) and
 *        buildRequest (full LlmRequest composition). See the header
 *        for the contract and the regression test suite
 *        (test-request-builder.cpp) for the pinned behaviour.
 * @layer Service (Chat subsystem)
 * @dependencies ConversationService, MessageService, ModelRouter,
 *               AgentRegistry, FileService, MembershipService,
 *               PlanService, RagService, ToolService, TaskObserver,
 *               HistoryBudgeter, models/conversation.h,
 *               models/agent.h, models/agent-plan.h,
 *               models/member.h, models/message.h, models/tool-call.h,
 *               utils/logger.h, utils/thread-discipline.h.
 */

#include "request-builder.h"

#include "../../models/agent-plan.h"
#include "../../models/agent.h"
#include "../../models/conversation.h"
#include "../../models/member.h"
#include "../../models/message.h"
#include "../../models/skill.h"
#include "../../models/tool-call.h"
#include "../../services/agent-registry.h"
#include "../../services/conversation-service.h"
#include "../../services/file-service.h"
#include "../../services/heartbeat-config-service.h"
#include "../../services/history-budgeter.h"
#include "../../services/membership-service.h"
#include "../../services/message-service.h"
#include "../../services/model-router.h"
#include "../../services/model-sampling-profile.h"
#include "../../services/plan-service.h"
#include "../../services/poll-service.h"
#include "../../services/rag-service.h"
#include "../../services/skill-service.h"
#include "../../services/task-observer.h"
#include "../../services/tool-service.h"
#include "../../utils/http-client.h"
#include "../../utils/logger.h"
#include "../../utils/thread-discipline.h"
#include "../conversation-summarizer.h"
#include "tool-payload-digest.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace Chat {

RequestBuilder::RequestBuilder(ConversationService& convSvc,
                               MessageService& msgSvc,
                               ModelRouter& router,
                               QObject* parent)
    : QObject(parent), m_convSvc(convSvc), m_msgSvc(msgSvc), m_router(router) {}

RequestBuilder::~RequestBuilder() = default;

namespace {

/**
 * @brief Resolution outcome for the per-member provider / model /
 *        allowed-tools override lookup. memberScoped flags whether
 *        the conversation routes through the member layer at all;
 *        memberRowFound flags whether a matching member row was
 *        located (an all-empty row is still a valid "no override").
 */
struct ResolvedMemberOverride {
    /// true → this conversation resolves through the member layer; the
    /// agent template override must NOT be applied at request time.
    bool memberScoped = false;
    /// true → a matching member row was found. Its fields below ARE the
    /// override (all-empty is valid and means "no override").
    bool memberRowFound = false;
    /// true → the conversation lives in a project/organization folder.
    /// Reused (it is already computed here) to gate ProjectOnly tools
    /// without a second folder lookup.
    bool projectScoped = false;
    /// The responder alias the resolution keyed on, for diagnostics.
    QString alias;
    QString modelProvider;
    QString modelName;
    QStringList allowedTools;
};

ResolvedMemberOverride resolveMemberOverride(const Conversation& conv,
                                             const BuildRequestInputs& inputs,
                                             ConversationService& convSvc) {
    ResolvedMemberOverride r;
    if (!inputs.membershipService) {
        // No membership service wired (e.g. minimal test fixtures) —
        // treat as not member-scoped so the template path still runs.
        return r;
    }

    // Is this conversation inside a project / organization folder?
    bool projectScoped = false;
    if (!conv.folderId.isEmpty()) {
        const auto folder = convSvc.getFolder(conv.folderId);
        if (folder.has_value()) {
            projectScoped = folder->isProject();
        }
    }
    // Surface the folder-kind decision (computed here already) so the
    // tool-scope filter can gate ProjectOnly tools.
    r.projectScoped = projectScoped;

    QString alias;
    QList<Member> roster;
    if (projectScoped) {
        // Project/org: the member entity lives on project_members.
        // Group chat → the cascade-provided responder alias.
        // 1:1 chat   → the conversations.member_alias binding stamped at
        //              direct-chat creation.
        alias = conv.isGroup ? inputs.responseMemberAlias : conv.memberAlias;
        roster = inputs.membershipService->projectMembers(conv.folderId);
    } else if (conv.isGroup) {
        // Non-project group chat: override lives on conversation_members.
        alias = inputs.responseMemberAlias;
        roster = inputs.membershipService->conversationMembers(conv.id);
    } else {
        return r;
    }

    r.memberScoped = true;
    r.alias = alias;
    if (alias.isEmpty()) {
        return r;  // member-scoped but no alias → self-heal to conv default
    }

    // Match the alias, also normalised with spaces→underscores —
    // cascade aliases arrive normalised, mirroring
    // MembershipService::findConversationMemberByAlias.
    for (const Member& m : roster) {
        const QString normalised = m.alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
        if (m.alias.compare(alias, Qt::CaseInsensitive) == 0 ||
            normalised.compare(alias, Qt::CaseInsensitive) == 0) {
            r.memberRowFound = true;
            r.modelProvider = m.modelProvider;
            r.modelName = m.modelName;
            r.allowedTools = m.allowedTools;
            return r;
        }
    }
    return r;  // memberScoped=true, memberRowFound=false → self-heal
}

}  // namespace

// ---------------------------------------------------------------------------
// assembleHistory — walk-and-pair reconstruction
// ---------------------------------------------------------------------------
//
// Why this exists: the messages table stores assistant rows and tool-result
// rows as independent records. The `tool_calls` field on an assistant row
// (the JSON array of function calls emitted by the LLM) is NOT stored on
// the messages table — it lives in the `tool_calls` side table that we
// populate from ChatController's watcher lambda. If we rebuild history
// using only the messages table, every historical assistant row that
// used tools loses its tool_calls field and every role=tool row becomes
// an orphan with no parent `tool_calls` declaration. OpenAI-compatible
// providers (Ollama, OpenAI, Anthropic-with-tool-use, Gemini) require
// the sequence
//
//   user → assistant{tool_calls:[...]} → tool{tool_call_id: matches} → ...
//
// with strict pairing. Sending a `role=tool` block that doesn't match
// any preceding `tool_calls` id is malformed. qwen3.5:9b responds to
// this by emitting a single stop token (`eval_count=1` / ~1s think).
// Other small local models exhibit similar failure modes.
//
// This helper walks the message list forward and rebuilds well-formed
// pairs from the two authoritative sources (messages table + tool_calls
// table), falling back gracefully for legacy rows that predate the
// side-table persistence.
//
QList<LlmMessage> RequestBuilder::assembleHistory(const QList<Message>& dbMessages,
                                                  bool isGroupChat,
                                                  const QString& excludeMsgId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<LlmMessage> out;
    out.reserve(dbMessages.size());

    QStringList toolCallMessageIds;
    toolCallMessageIds.reserve(dbMessages.size());
    for (const Message& m : dbMessages) {
        if (m.role == QStringLiteral("assistant") &&
            m.finishReason == QStringLiteral("tool_calls")) {
            toolCallMessageIds.append(m.id);
        }
    }
    const QHash<QString, QList<ToolCall>> toolCallsByMessage =
        toolCallMessageIds.isEmpty() ? QHash<QString, QList<ToolCall>>{}
                                     : m_msgSvc.getToolCallsForMessages(toolCallMessageIds);

    // All agent turns emit as role=assistant with "(Alias said)\n"
    // prefix in group chats. This matches small local models'
    // natural training distribution (clean user/assistant alternation);
    // emitting other agents' replies as role=user confuses qwen3.5's
    // chat template.
    //
    // The assistant-tail EOS problem (model sees last message as
    // assistant and emits a stop token) is solved separately in
    // buildRequest by appending a synthetic role=system nudge before
    // dispatch.

    auto normalizeAlias = [](const QString& a) {
        return a.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
    };

    // Defensive sanitization: strip any leading "(Alias said)\n" prefix
    // (possibly stacked multiple times) from stored content. Legacy
    // rows may contain these prefixes as literal text — stripping here
    // prevents a double prefix when we re-apply below.
    //
    // Also strips a "reframe wrapper" pattern that briefly leaked into
    // persisted assistant messages from a reverted experiment:
    //   "[Earlier this round, @<alias> replied (input only — do not
    //    imitate this voice or repeat this content):\n[body]\n
    //    — end of @<alias>'s prior turn —]"
    // The experiment is gone but the rotted DB rows survive; this
    // unwrap rescues the inner [body] so subsequent history assemblies
    // don't keep showing the wrapper to fresh cascade members and
    // teaching new models to echo it. The unwrap is idempotent — if
    // the row is already clean, it's a no-op.
    auto stripLeadingAliasPrefix = [](QString content) -> QString {
        static const QRegularExpression aliasSaidRx(
            QStringLiteral("^\\([A-Za-z0-9_ ]{1,64} said\\)\\n"));
        static const QRegularExpression reframeWrapperRx(
            QStringLiteral("^\\[Earlier this round, @[A-Za-z0-9_]{1,64} replied "
                           "\\(input only — do not imitate this voice "
                           "or repeat this content\\):\\n"
                           "([\\s\\S]*?)"
                           "\\n— end of @[A-Za-z0-9_]{1,64}'s prior "
                           "turn —\\]\\s*$"));
        while (true) {
            const auto wrapMatch = reframeWrapperRx.match(content);
            if (wrapMatch.hasMatch() && wrapMatch.capturedStart() == 0) {
                content = wrapMatch.captured(1);
                continue;
            }
            const auto aliasMatch = aliasSaidRx.match(content);
            if (aliasMatch.hasMatch() && aliasMatch.capturedStart() == 0) {
                content = content.mid(aliasMatch.capturedEnd());
                continue;
            }
            break;
        }
        return content;
    };

    // Emit an assistant row. In group chats, prepend "(Alias said)\n"
    // so the model can distinguish which teammate is speaking in each
    // assistant turn, AND set LlmMessage::speakerName so the provider
    // can emit a proper OpenAI-style {"role":"assistant","name":"Alice"}
    // envelope. The two mechanisms are complementary: providers that
    // respect `name` get a structural speaker tag, providers that
    // don't still have the content-prefix fallback. In 1:1 chats,
    // neither is applied.
    auto emitAssistant = [&](const Message& m, const QJsonArray& toolCalls) {
        LlmMessage lm;
        lm.role = QStringLiteral("assistant");
        const QString clean = stripLeadingAliasPrefix(m.content);
        if (isGroupChat && !m.memberAlias.isEmpty()) {
            const QString norm = normalizeAlias(m.memberAlias);
            lm.content = QStringLiteral("(%1 said)\n%2").arg(norm, clean);
            lm.speakerName = norm;
        } else {
            lm.content = clean;
        }
        if (!toolCalls.isEmpty()) {
            lm.toolCallsJson = toolCalls;
        }
        out.append(lm);
    };

    // Convert a ToolCall row into the OpenAI-style nested JSON object
    // that providers expect inside the `tool_calls` array.
    auto toolCallRowToJson = [](const ToolCall& tc) -> QJsonObject {
        QJsonObject func;
        func[QStringLiteral("name")] = tc.toolName;
        func[QStringLiteral("arguments")] = tc.arguments;
        QJsonObject obj;
        obj[QStringLiteral("id")] = tc.id;
        obj[QStringLiteral("type")] = QStringLiteral("function");
        obj[QStringLiteral("function")] = func;
        return obj;
    };

    const int n = dbMessages.size();
    for (int i = 0; i < n; ++i) {
        const Message& m = dbMessages.at(i);

        // Skip the streaming placeholder (not persisted; would be empty anyway).
        if (!excludeMsgId.isEmpty() && m.id == excludeMsgId)
            continue;

        // role=user / role=system → pass through unchanged.
        if (m.role == QStringLiteral("user") || m.role == QStringLiteral("system")) {
            LlmMessage lm;
            lm.role = m.role;
            lm.content = m.content;
            out.append(lm);
            continue;
        }

        if (m.role == QStringLiteral("assistant")) {
            // Assistant with tool_calls → walk-and-pair reconstruction.
            if (m.finishReason == QStringLiteral("tool_calls")) {
                const QList<ToolCall> callRows = toolCallsByMessage.value(m.id);
                if (callRows.isEmpty()) {
                    // Legacy orphan: the assistant finished with
                    // tool_calls but we have no side-table rows to
                    // prove which calls were made. Drop the
                    // tool_calls signal entirely and ALSO skip any
                    // immediately-following role=tool rows so we
                    // don't produce an unanchored tool response.
                    emitAssistant(m, QJsonArray{});
                    int j = i + 1;
                    while (j < n && dbMessages.at(j).role == QStringLiteral("tool")) {
                        ++j;
                    }
                    i = j - 1;
                    continue;
                }

                // We have authoritative tool_call rows. Look ahead
                // across the next contiguous run of role=tool
                // messages and match each by metadata.tool_call_id.
                QSet<QString> expectedIds;
                for (const ToolCall& tc : callRows) {
                    expectedIds.insert(tc.id);
                }

                /**
                 * @brief A tool_response message paired with the id
                 *        of the tool_call it answers, kept together
                 *        for ordered insertion into the prompt.
                 */
                struct PairedTool {
                    const Message* msg;
                    QString tcid;
                };
                QList<PairedTool> pairedTools;
                // Legacy DB rows may have a role=system message
                // persisted BETWEEN the assistant's tool_calls row
                // and its paired tool responses (e.g. a task_event
                // system row that landed in the middle of the
                // sequence). Collect intervening system rows so we
                // can emit them AFTER the tool pair, keeping the
                // OpenAI-style transcript well-formed instead of
                // breaking the walk on the first system row and
                // dropping the tool response as an orphan forever.
                QList<const Message*> intervening;

                int j = i + 1;
                while (j < n) {
                    const Message& next = dbMessages.at(j);
                    if (next.role == QStringLiteral("tool")) {
                        const QString tcid =
                            next.metadata[QStringLiteral("tool_call_id")].toString();
                        if (!tcid.isEmpty() && expectedIds.contains(tcid)) {
                            pairedTools.append(PairedTool{&next, tcid});
                            expectedIds.remove(tcid);
                            ++j;
                            continue;
                        }
                        // Tool row from a DIFFERENT assistant turn
                        // (stale / unmatched) — ends our window.
                        break;
                    }
                    if (next.role == QStringLiteral("system")) {
                        // Informational row interleaved into the tool
                        // window. Pass over it while hunting for our
                        // paired tool responses; emit it after the
                        // pair so the OpenAI-style transcript stays
                        // well-formed (assistant[tool_calls] → tool →
                        // system, not assistant[tool_calls] → system
                        // → tool which breaks the contract).
                        intervening.append(&next);
                        ++j;
                        continue;
                    }
                    // assistant / user / anything else ends the window.
                    break;
                }

                if (expectedIds.isEmpty() && !pairedTools.isEmpty()) {
                    // Full pairing — emit assistant with tool_calls
                    // field, the matched tool responses, then any
                    // intervening system rows we stepped past.
                    QJsonArray tcJson;
                    for (const ToolCall& tc : callRows) {
                        tcJson.append(toolCallRowToJson(tc));
                    }
                    emitAssistant(m, tcJson);
                    for (const PairedTool& pt : pairedTools) {
                        LlmMessage lm;
                        lm.role = QStringLiteral("tool");
                        lm.content = pt.msg->content;
                        lm.toolCallId = pt.tcid;
                        out.append(lm);
                    }
                    for (const Message* sys : intervening) {
                        LlmMessage lm;
                        lm.role = sys->role;
                        lm.content = sys->content;
                        out.append(lm);
                    }
                    i = j - 1;
                } else {
                    // Partial pairing — safer to emit the assistant
                    // WITHOUT its tool_calls field and drop any tool
                    // rows we were about to include. Keeps the
                    // transcript well-formed at the cost of losing
                    // tool-call history for that specific turn.
                    // Intervening system rows still deserve emission
                    // (they're independently valid history).
                    emitAssistant(m, QJsonArray{});
                    for (const Message* sys : intervening) {
                        LlmMessage lm;
                        lm.role = sys->role;
                        lm.content = sys->content;
                        out.append(lm);
                    }
                    i = j - 1;
                }
                continue;
            }

            // Plain assistant (no tool calls) → pass through.
            emitAssistant(m, QJsonArray{});
            continue;
        }

        // role=tool that wasn't consumed by an earlier look-ahead is
        // an orphan (legacy data with no matching assistant, or a
        // message out of sequence). Drop it so the history stays
        // well-formed. Logged at debug level — common on upgraded DBs.
        if (m.role == QStringLiteral("tool")) {
            qCDebug(verzetaUi) << "RequestBuilder::assembleHistory: dropping orphan "
                                  "role=tool row"
                               << m.id.left(8);
            continue;
        }

        // Unknown role: pass through as-is rather than losing the row.
        LlmMessage lm;
        lm.role = m.role;
        lm.content = m.content;
        out.append(lm);
    }

    return out;
}


namespace {

// Verbatim tool payloads (file bodies + raw results) may consume at most this
// fraction of the history budget; older groups beyond it are digested.
constexpr double kVerbatimToolShare = 0.35;
// A single tool payload over this many estimated tokens is digested even if it
// is the newest group — one huge write must never monopolize the window.
constexpr int kVerbatimPayloadCeiling = 700;
constexpr int kProtectedResultCeiling = 4000;
constexpr int kAlwaysKeepLastToolGroups = 3;

int payloadTokens(const QString& s) {
    return HistoryBudgeter::estimateTokens(s, HistoryBudgeter::ContentClass::CodeOrJson);
}

}  // namespace

void RequestBuilder::digestToolPayloads(QList<LlmMessage>& msgs, int historyBudgetTokens) {
    /**
     * @brief A tool-call group: an assistant{tool_calls} message plus its
     *        contiguous run of role=tool result messages.
     */
    struct Group {
        int asstIdx = 0;        ///< the assistant{tool_calls} message index
        int firstTool = 0;      ///< first role=tool index (asstIdx + 1)
        int lastTool = -1;      ///< last role=tool index; < firstTool if none
        int tokens = 0;         ///< total payload tokens (args + results)
        int maxPayloadTok = 0;  ///< largest single-payload tokens (ceiling)
        int maxArgsTok = 0;     ///< largest single ARGS payload (write bodies)
        int maxResultTok = 0;   ///< largest single RESULT payload (working set)
        QString hash;           ///< concatenated payload (dedup key)
    };

    QList<Group> groups;
    for (int i = 0; i < msgs.size(); ++i) {
        if (msgs.at(i).role != QStringLiteral("assistant") || msgs.at(i).toolCallsJson.isEmpty()) {
            continue;
        }
        int j = i + 1;
        while (j < msgs.size() && msgs.at(j).role == QStringLiteral("tool")) {
            ++j;
        }
        Group g;
        g.asstIdx = i;
        g.firstTool = i + 1;
        g.lastTool = j - 1;

        QString payload;
        int maxComp = 0;
        for (const QJsonValue& tcv : msgs.at(i).toolCallsJson) {
            const QJsonObject fn = tcv.toObject().value(QStringLiteral("function")).toObject();
            const QString argsStr =
                QString::fromUtf8(QJsonDocument(fn.value(QStringLiteral("arguments")).toObject())
                                      .toJson(QJsonDocument::Compact));
            payload += argsStr;
            const int argsTok = payloadTokens(argsStr);
            maxComp = qMax(maxComp, argsTok);
            g.maxArgsTok = qMax(g.maxArgsTok, argsTok);
        }
        for (int k = g.firstTool; k <= g.lastTool; ++k) {
            payload += msgs.at(k).content;
            const int resTok = payloadTokens(msgs.at(k).content);
            maxComp = qMax(maxComp, resTok);
            g.maxResultTok = qMax(g.maxResultTok, resTok);
        }
        g.tokens = payloadTokens(payload);
        g.maxPayloadTok = maxComp;
        g.hash = payload;
        groups.append(g);
    }
    if (groups.isEmpty()) {
        return;
    }

    const int shareBudget = qMax(0, static_cast<int>(historyBudgetTokens * kVerbatimToolShare));
    int verbatimUsed = 0;
    QSet<QString> keptHashes;
    QSet<int> digestGroupIdx;  // group positions to collapse to a prose record
    const int protectFrom = groups.size() - kAlwaysKeepLastToolGroups;

    // Walk newest → oldest, deciding verbatim vs digest per group.
    for (int g = groups.size() - 1; g >= 0; --g) {
        const Group& grp = groups.at(g);
        const bool overCeiling = grp.maxPayloadTok > kVerbatimPayloadCeiling;
        const bool duplicate = keptHashes.contains(grp.hash);

        bool digest;
        if (duplicate) {
            digest = true;  // same body kept by a newer group
        } else if (g >= protectFrom) {
            // Protected window (the active working set): ARGS keep the
            // strict ceiling (write bodies never replay), but RESULTS
            // the model just fetched stay verbatim up to the larger
            // bound — digesting a fresh read_canvas here forced blind
            // whole-document rewrites (the churn class).
            digest = grp.maxArgsTok > kVerbatimPayloadCeiling ||
                     grp.maxResultTok > kProtectedResultCeiling;
        } else if (!overCeiling && verbatimUsed + grp.tokens <= shareBudget) {
            digest = false;  // fits the verbatim share
            verbatimUsed += grp.tokens;
        } else {
            digest = true;
        }

        if (!digest) {
            keptHashes.insert(grp.hash);
        } else {
            digestGroupIdx.insert(g);
        }
    }
    if (digestGroupIdx.isEmpty()) {
        return;
    }

    QHash<int, int> asstToGroup;
    for (int gi = 0; gi < groups.size(); ++gi) {
        asstToGroup.insert(groups.at(gi).asstIdx, gi);
    }

    QList<LlmMessage> rebuilt;
    rebuilt.reserve(msgs.size());
    for (int i = 0; i < msgs.size();) {
        const auto it = asstToGroup.constFind(i);
        if (it == asstToGroup.constEnd()) {
            rebuilt.append(msgs.at(i));
            ++i;
            continue;
        }
        const Group& grp = groups.at(it.value());
        const int groupEnd = (grp.lastTool >= grp.firstTool) ? grp.lastTool : grp.asstIdx;

        if (!digestGroupIdx.contains(it.value())) {
            for (int k = grp.asstIdx; k <= groupEnd; ++k) {
                rebuilt.append(msgs.at(k));
            }
            i = groupEnd + 1;
            continue;
        }

        // Collapse to one role=system prose record.
        QHash<QString, QString> idToResult;
        for (int k = grp.firstTool; k <= grp.lastTool; ++k) {
            idToResult.insert(msgs.at(k).toolCallId, msgs.at(k).content);
        }
        QStringList lines;
        for (const QJsonValue& tcv : msgs.at(grp.asstIdx).toolCallsJson) {
            const QJsonObject tc = tcv.toObject();
            const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
            const QString name = fn.value(QStringLiteral("name")).toString();
            const QString id = tc.value(QStringLiteral("id")).toString();
            const QJsonObject args = fn.value(QStringLiteral("arguments")).toObject();
            lines.append(QStringLiteral("  • ") +
                         Chat::ToolPayloadDigest::summarizeCall(name, args, idToResult.value(id)));
        }
        LlmMessage rec;
        rec.role = QStringLiteral("system");
        rec.content =
            QStringLiteral("[earlier tool activity — results are saved; re-read with "
                           "read_file / read_canvas / search_messages for full content:\n") +
            lines.join(QStringLiteral("\n")) + QStringLiteral("\n]");
        rebuilt.append(rec);
        i = groupEnd + 1;
    }
    msgs = rebuilt;
}


namespace {

// Estimated tokens of an assembled message, counting tool-call args and
// tool-result bodies as dense (CodeOrJson) — estimateMessageTokens (DB-row
// shaped) does NOT see toolCallsJson, so we measure the LlmMessage directly.
int estimateLlmMessageTokens(const LlmMessage& m) {
    using CC = HistoryBudgeter::ContentClass;
    int t = 4;  // role/envelope overhead
    const bool dense = (m.role == QStringLiteral("tool"));
    t += HistoryBudgeter::estimateTokens(m.content, dense ? CC::CodeOrJson : CC::Prose);
    if (!m.toolCallsJson.isEmpty()) {
        const QString s =
            QString::fromUtf8(QJsonDocument(m.toolCallsJson).toJson(QJsonDocument::Compact));
        t += HistoryBudgeter::estimateTokens(s, CC::CodeOrJson);
    }
    if (!m.speakerName.isEmpty()) {
        t += 4;
    }
    return t;
}

int estimateAssembledTokens(const QList<LlmMessage>& msgs) {
    int t = 0;
    for (const LlmMessage& m : msgs) {
        t += estimateLlmMessageTokens(m);
    }
    return t;
}

bool isLeadingSummary(const LlmMessage& m) {
    return m.role == QStringLiteral("system") &&
           m.content.startsWith(QStringLiteral("Summary of earlier conversation"));
}

// Bound an app-generated prompt section to a token budget. A no-op when the
// text already fits; otherwise truncates (preferring a line boundary) and
// appends an explicit "(truncated)" marker. Defense-in-depth so a growing
// app-generated layer can't bloat the system prompt — the post-assembly
// shaper still guarantees the overall fit.
QString truncateToTokens(const QString& text, int capTokens) {
    if (capTokens <= 0) {
        return QString();
    }
    if (HistoryBudgeter::estimateTokens(text) <= capTokens) {
        return text;
    }
    const int maxChars = capTokens * 3;  // prose chars/token
    QString cut = text.left(maxChars);
    const int nl = cut.lastIndexOf(QLatin1Char('\n'));
    if (nl > maxChars / 2) {
        cut = cut.left(nl);
    }
    return cut + QStringLiteral("\n…(truncated)");
}

// Index of the oldest message that may be shed: not the leading summary, not
// the newest (last) message. Returns -1 when nothing is safely droppable.
int firstDroppableIndex(const QList<LlmMessage>& msgs) {
    const int last = msgs.size() - 1;
    for (int i = 0; i < last; ++i) {
        if (i == 0 && isLeadingSummary(msgs.at(i))) {
            continue;
        }
        return i;
    }
    return -1;
}

// Remove the message at @p idx and, if it is an assistant carrying tool_calls,
// its contiguous trailing role=tool result rows — so walk-and-pair stays valid.
void removeGroupAt(QList<LlmMessage>& msgs, int idx) {
    if (idx < 0 || idx >= msgs.size()) {
        return;
    }
    const bool hasTools =
        msgs.at(idx).role == QStringLiteral("assistant") && !msgs.at(idx).toolCallsJson.isEmpty();
    int end = idx;  // inclusive
    if (hasTools) {
        while (end + 1 < msgs.size() && msgs.at(end + 1).role == QStringLiteral("tool")) {
            ++end;
        }
    }
    for (int k = end; k >= idx; --k) {
        msgs.removeAt(k);
    }
}

}  // namespace

int RequestBuilder::shapeRequestToWindow(LlmRequest& req,
                                         int sysTok,
                                         int toolsTok,
                                         bool allowGrow) {
    constexpr int kMaxAutoContextWindow = 32768;
    const int floor = HistoryBudgeter::kRealOutputFloor;

    int estPrompt = sysTok + toolsTok + estimateAssembledTokens(req.messages);

    // 1. Grow the window (Ollama num_ctx) if allowed and below the ceiling.
    if (estPrompt + floor > req.config.contextWindow && allowGrow &&
        req.config.contextWindow < kMaxAutoContextWindow) {
        const int grown = qMin(kMaxAutoContextWindow, estPrompt + floor);
        if (grown > req.config.contextWindow) {
            qCDebug(verzetaUi) << "RequestBuilder::shapeRequestToWindow: grew context window"
                               << req.config.contextWindow << "->" << grown << "(estPrompt"
                               << estPrompt << "+ floor" << floor << ")";
            req.config.contextWindow = grown;
        }
    }

    // 2. Last resort: shed oldest droppable history groups until it fits.
    //    After Pillar A digest this almost never fires; when a single huge
    //    turn overflows even the grown ceiling it is logged, never silent.
    int guard = req.messages.size() + 2;
    while (estPrompt + floor > req.config.contextWindow && guard-- > 0) {
        const int dropIdx = firstDroppableIndex(req.messages);
        if (dropIdx < 0) {
            qCWarning(verzetaUi) << "RequestBuilder::shapeRequestToWindow: prompt still exceeds"
                                    " window after digest+grow+shed — sending the newest turn"
                                    " alone (estPrompt"
                                 << estPrompt << "window" << req.config.contextWindow << ")";
            break;
        }
        removeGroupAt(req.messages, dropIdx);
        estPrompt = sysTok + toolsTok + estimateAssembledTokens(req.messages);
    }

    const int ctx = qMax(1, req.config.contextWindow);
    return qBound(0, estPrompt * 100 / ctx, 100);
}

// ---------------------------------------------------------------------------
// buildRequest — full LlmRequest composition
// ---------------------------------------------------------------------------
//
// Composes:
//
//   1. Organization / project context layer (folder chain, team
//      members including @User, shared-doc list).
//   2. Agent role layer (template system prompt).
//   3. Conversation-level system-prompt override.
//   4. Group Chat Contract (anti-LARP rules, roster).
//   5. TASK SYSTEM trigger (one short universal line — open with
//      start_task, close with complete_task; gated off when an
//      active task exists — see #8).
//   6. Time layer (UTC now, prepended so models see it first).
//   7. ACTIVE TASK framing (flat, group-owned: "do the work + call
//      complete_task; anyone can close it" — NO per-step owner, NO
//      hand-off, NO status marker. ALSO reports back via
//      result.clearActiveTaskPlanId when the plan has gone
//      terminal or missing).
//   8. Cross-chat awareness ("YOUR ACTIVE / RECENTLY COMPLETED
//      TASKS"; filter excludes the current conversation so the
//      agent does not see their in-progress task duplicated here).
//   9. RAG augmentation (wraps the composed system prompt).
//  10. Tool list assembly (filters task-only tools when no active
//      task, so the LLM does not see submit_result / report_blocked
//      / approve_step etc. outside the executor context).
//  11. Conversation-level config finalisation (fallback to
//      router active model, group-chat context-window floor of
//      16384 tokens).
//  12. History loading + HistoryBudgeter + assembleHistory +
//      cascade-tail user-nudge (injected as role=system so small
//      local models don't echo it back as pseudo-user text).
//  13. Pending image / file-context attach to last user message.
//  14. Routing diagnostic log.
//
// Error paths return BuildResult::success=false with a populated
// errorReason; the caller (ChatController) emits errorOccurred +
// onRequestError. Side effects that remain caller-owned:
//   - m_inflightConvId assignment
//   - m_currentRequestId bump
//   - m_router.route(req)  (the actual network send)
//   - active-plan anchor clear (done based on
//     result.clearActiveTaskPlanId)
//   - m_pendingImages / m_pendingFileContext clear (done based on
//     result.consumedAttachments)
//
BuildResult RequestBuilder::buildRequest(const BuildRequestInputs& inputs) {
    VERZETA_ASSERT_MAIN_THREAD();

    BuildResult result;

    // Load conversation metadata (system prompt + model config).
    const auto conv = m_convSvc.getConversation(inputs.inflightConvId);
    if (!conv.has_value()) {
        result.success = false;
        result.errorReason = QStringLiteral("Active conversation not found in DB");
        return result;
    }

    // History loading is deferred until AFTER the system prompt and
    // config are fully built (see below). We need the final system
    // prompt size + tool definitions + num_ctx to compute the token
    // budget for the HistoryBudgeter.

    LlmRequest req;
    req.requestId = inputs.requestId;
    req.conversationId = inputs.inflightConvId;

    // Compose the layered system prompt (Cycle 2):
    //   1. Organization context (if conversation is inside an organization folder)
    //   2. Project context (if conversation is inside a project folder)
    //   3. Team members (agents assigned to the project/organization)
    //   4. Agent role (if primary agent set)
    //   5. Conversation-specific system prompt override
    //
    // Legacy conversations (no agent, no project/org folder) fall through to
    // just conv->systemPrompt exactly as before.
    QStringList promptLayers;

    // Walk the folder chain: innermost → outermost. Collect project/org folders.
    // For project folders, read members from project_members (not folder.agentIds).
    const QList<Folder> folderChain = m_convSvc.folderChainForConversation(inputs.activeConvId);
    for (const Folder& f : folderChain) {
        if (!f.isProject())
            continue;

        const QString kind = f.folderType == QStringLiteral("organization")
                                 ? QStringLiteral("Organization")
                                 : QStringLiteral("Project");

        QString layer = QStringLiteral("You are operating inside the %1 \"%2\".").arg(kind, f.name);
        if (!f.goal.trimmed().isEmpty()) {
            layer += QStringLiteral("\nGoal: ") + f.goal.trimmed();
        }
        if (!f.description.trimmed().isEmpty()) {
            layer += QStringLiteral("\nDescription: ") + f.description.trimmed();
        }

        // Team members from the aliased project_members table.
        //
        // Include the human user in the roster. Without this, small
        // local models treat "everyone" and "the team" as "every
        // agent except the user" — when asked to e.g. save every
        // member's favourite colour, they silently omit the user's.
        // Naming the user explicitly as @User with a short
        // description fixes the attribution.
        if (inputs.membershipService) {
            const QList<Member> members = inputs.membershipService->projectMembers(f.id);
            QStringList memberDescriptions;

            // Human user FIRST so agents read them as the primary
            // stakeholder.
            memberDescriptions.append(
                QStringLiteral("- @User (Human) — the person you are chatting with; "
                               "include them whenever the request is about the group "
                               "(\"everyone\", \"the team\", \"all of us\"). Address "
                               "them as @User unless they've introduced themselves "
                               "with a specific name, in which case use that name."));

            for (const Member& m : members) {
                QString line = QStringLiteral("- @%1").arg(
                    m.alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_')));
                if (!m.agentName.isEmpty() && m.agentName != m.alias) {
                    line += QStringLiteral(" (%1)").arg(m.agentName);
                }
                if (!m.agentDescription.trimmed().isEmpty()) {
                    line += QStringLiteral(" — ") + m.agentDescription.trimmed();
                }
                if (m.isCoordinator) {
                    line += QStringLiteral(" [coordinator]");
                }
                memberDescriptions.append(line);
            }

            layer +=
                QStringLiteral("\nTeam members:\n") + memberDescriptions.join(QLatin1Char('\n'));
        }

        // Shared project documents — listed in the prompt so agents know
        // what files exist; agents read them via the read_file tool.
        if (inputs.fileService) {
            const QStringList docs = inputs.fileService->listProjectDocuments(f.id, f.name);
            if (!docs.isEmpty()) {
                const QString dir = inputs.fileService->projectDocsDir(f.id, f.name);
                QStringList lines;
                for (const QString& name : docs) {
                    lines.append(QStringLiteral("- %1/%2").arg(dir, name));
                }
                layer += QStringLiteral("\nShared project documents (read with the read_file "
                                        "tool when relevant):\n") +
                         lines.join(QLatin1Char('\n'));
            }
        }

        promptLayers.append(layer);
    }

    // Agent role layer:
    //   - Group chat: use responseMemberAgentId (mentioned or default coordinator)
    //   - 1:1 chat: use primary_agent_id
    //
    // IMPORTANT composition order for group chats:
    //   1. Agent template system prompt (specialty knowledge, added FIRST)
    //   2. Project context (already in promptLayers above)
    //   3. Conversation override (added AFTER this block)
    //   4. Group Chat Contract (added LAST — identity, roster, rules)
    QString effectiveAgentId = inputs.responseMemberAgentId.isEmpty()
                                   ? conv->primaryAgentId
                                   : inputs.responseMemberAgentId;
    const Agent effectiveAgent = (inputs.agentRegistry && !effectiveAgentId.isEmpty())
                                     ? inputs.agentRegistry->getAgent(effectiveAgentId)
                                     : Agent{};

    const ResolvedMemberOverride memberOv = resolveMemberOverride(*conv, inputs, m_convSvc);

    if (effectiveAgent.isValid()) {
        promptLayers.append(effectiveAgent.systemPrompt);
    }

    // Conversation-level override
    if (!conv->systemPrompt.trimmed().isEmpty()) {
        promptLayers.append(conv->systemPrompt);
    }

    // Group Chat Contract — LAST layer, most influential for LLM attention
    if (conv->isGroup && effectiveAgent.isValid() && !inputs.responseMemberAlias.isEmpty() &&
        inputs.membershipService) {
        const QString myAlias =
            inputs.responseMemberAlias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));

        // Build the FULL roster (including self, marked "YOU")
        const QList<Member> members =
            inputs.membershipService->conversationMembers(inputs.activeConvId);
        QStringList rosterLines;
        QStringList teammateMentions;  // everyone except self
        for (const Member& m : members) {
            const QString normalized =
                m.alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
            const bool isMe = m.alias.compare(inputs.responseMemberAlias, Qt::CaseInsensitive) == 0;
            QString role = m.agentName;
            if (m.isCoordinator) {
                role += QStringLiteral(", coordinator");
            }
            QString line = QStringLiteral("  • @%1").arg(normalized);
            if (!role.trimmed().isEmpty()) {
                line += QStringLiteral("  (%1)").arg(role);
            }
            if (isMe) {
                // Mark self as the reader AND steer away from self-@mention:
                // small models otherwise copy their own "@alias" out of the
                // roster into their replies and appear to talk to themselves.
                line += QStringLiteral("  ← THIS IS YOU (refer to yourself as \"I\"/"
                                       "\"me\", never \"@%1\")")
                            .arg(normalized);
            }
            rosterLines.append(line);
            if (!isMe) {
                teammateMentions.append(QStringLiteral("@%1").arg(normalized));
            }
        }

        // Tight, small-model-friendly contract. Previous versions were 3KB+
        // which caused 4B-class local models to overflow, hallucinate dialog
        // transcripts, or ignore instructions entirely. This version is
        // under 1 KB and sticks to the handful of rules that actually matter.
        const QString teammateList = teammateMentions.isEmpty()
                                         ? QStringLiteral("(none — respond solo)")
                                         : teammateMentions.join(QStringLiteral(", "));

        QString contract =
            QStringLiteral("\n---\n"
                           "You are @%1 in a group chat. Speak ONLY in your own voice, "
                           "with YOUR OWN words, from YOUR own perspective. Everything "
                           "you emit must be NEW content — never a copy, paraphrase, "
                           "or recap of what a teammate just said.\n"
                           "\n"
                           "FORBIDDEN output shapes (never produce any of these):\n"
                           "  • Writing your OWN handle @%1 — you ARE @%1, so refer to "
                           "yourself as \"I\"/\"me\". @-handles address OTHER people only "
                           "(see the teammate list below); never @-mention yourself.\n"
                           "  • Copying or paraphrasing another teammate's last reply. "
                           "If Alice just introduced herself, DO NOT re-introduce "
                           "Alice — introduce YOURSELF and move on.\n"
                           "  • \"(%1 said)\" or \"(Alice said)\" or any \"(X said)\" "
                           "prefix — do NOT prefix your reply with attribution; the "
                           "app does that for you. This is the single most common "
                           "failure — do not do it.\n"
                           "  • \"(Human said) ...\" / \"(Human user said) ...\" / "
                           "\"(User said) ...\" — do NOT quote or paraphrase the human; "
                           "respond directly instead.\n"
                           "  • \"[@%1, it is your turn to respond.]\" — that is a "
                           "system-injected nudge; never echo it.\n"
                           "  • A transcript of what others said — one agent's reply "
                           "followed by another agent's reply. You produce ONE reply: "
                           "yours.\n"
                           "  • Announcing work without doing it. If you say you will "
                           "create, edit, run, or search something, call the tool IN "
                           "THIS SAME TURN.\n"
                           "  • Skipping an open poll. If a teammate opened a poll you "
                           "have not voted in, CAST YOUR VOTE (cast_vote) in this turn "
                           "— a poll is how the team decides, and an unvoted poll "
                           "blocks everyone. Never open a poll and then proceed as if "
                           "it were already decided.\n"
                           "\n"
                           "Tool calls are ALWAYS allowed and encouraged alongside (or "
                           "instead of) your text reply — when work needs doing, call "
                           "the tool.\n"
                           "\n"
                           "History format: previous replies from teammates appear as "
                           "\"(Alice said)\\n...\" in the transcript you read. That is "
                           "INPUT attribution for you — it is NOT a template for your "
                           "OUTPUT. Only `role=user` messages are from the real human.\n"
                           "\n"
                           "Teammates you may @mention: %2\n"
                           "Rules: use ONLY aliases from that list. @all broadcasts "
                           "to everyone; @owner or @User pings the human — use "
                           "sparingly. Only @mention a teammate when you genuinely "
                           "need their input; broadcast mentions (@team, @all) invite "
                           "everyone to respond and should be rare.\n"
                           "\n"
                           "Budget: up to %3 agent turns per user message, and the "
                           "conversation can continue across rounds when real work is "
                           "happening — so do not save your turn for later: use it to "
                           "PRODUCE something (a file, a search, a decision), not just "
                           "to comment.")
                .arg(myAlias, teammateList, QString::number(inputs.maxAgentCascade));

        promptLayers.append(contract);

        // Prose-only TASK SYSTEM trigger.
        //
        // Do NOT include a literal JSON example of the start_task tool
        // shape here. Small local models (qwen3-class) pattern-match
        // on JSON blocks in the system prompt and emit the example
        // VERBATIM as text content instead of calling the tool via
        // the tool_calls channel. Observed failure: the model wrote
        //   { "tool": "start_task", "arguments": {...} }
        // into its reply content, producing no plan and no work.
        const QString groupTaskSystem =
            QStringLiteral("\n---\n"
                           "=== TASK SYSTEM ===\n"
                           "\n"
                           "If you take on concrete multi-part work worth tracking — "
                           "building, writing, researching, or producing a deliverable "
                           "— call the `start_task` tool with a one-line goal to open a "
                           "tracked task, then just do the work with your tools. Emit a "
                           "real tool call via your tool-call channel; do NOT write "
                           "tool-call JSON as text. To bring a teammate in, @-mention "
                           "them (or use request_turn) — the cascade routes their turn; "
                           "never write \"(TeammateName said) ...\" as if they already "
                           "spoke. When everything the user asked for is finished, call "
                           "`complete_task` to close it — a task belongs to the whole "
                           "conversation and anyone here can close it. Do NOT call "
                           "start_task for small talk, questions, opinions, or quick "
                           "replies.");
        // Only show the MANDATORY TRIGGER block when there is NO
        // active task anchored to this conversation — otherwise the
        // model would see a "call start_task now" instruction while
        // already working on one.
        if (inputs.activeTaskPlanId.isEmpty()) {
            promptLayers.append(groupTaskSystem);
        }
    }

    // 1:1 turns (agent OR plain Direct) get the same lightweight
    // task-tracker instruction: start_task opens a tracked task,
    // complete_task closes it. No owners, no per-step delegation — a task
    // is a passive anchor, not an executor. Shown for every 1:1 (including
    // a plain non-agent Direct chat) when no task is already open.
    if (!conv->isGroup) {
        const QString oneOnOne =
            QStringLiteral("\n---\n"
                           "=== TASK SYSTEM ===\n"
                           "\n"
                           "If you take on concrete multi-part work worth tracking — "
                           "building, writing, researching, or producing a deliverable "
                           "— call the `start_task` tool with a one-line goal to open a "
                           "tracked task, then just do the work with your tools. Emit a "
                           "real tool call via your tool-call channel; do NOT write "
                           "tool-call JSON as text. When everything the user asked for "
                           "is finished, call `complete_task` to close it. Do NOT call "
                           "start_task for small talk, questions, opinions, or quick "
                           "replies.");
        if (inputs.activeTaskPlanId.isEmpty()) {
            promptLayers.append(oneOnOne);
        }
    }

    // Authoritative "now" — kept minimal. Small models drop long prompts;
    // one clear line beats a lecture. UTC is unambiguous and doesn't fake
    // a user location the app doesn't actually know.
    {
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        const QString nowLayer =
            QStringLiteral("Now (UTC): %1. Use this as today for any tool needing a date. "
                           "For location-specific tools (weather, events), ask the user "
                           "for the city if they haven't said.")
                .arg(nowUtc.toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'")));
        promptLayers.prepend(nowLayer);
    }

    if (!inputs.canvasFilename.isEmpty()) {
        const QString canvasLayer =
            QStringLiteral("=== ACTIVE CANVAS ===\n"
                           "A canvas is currently open in this conversation:\n"
                           "  filename:  %1\n"
                           "  language:  %2\n"
                           "  lines:     %3\n"
                           "  size:      %4 bytes\n"
                           "  revision:  %5\n"
                           "\n"
                           "To READ any portion of its content, call the read_canvas tool "
                           "with start_line and end_line. To MODIFY it, call edit_canvas "
                           "with the new full content (read first if you need to preserve "
                           "existing parts). Do NOT attempt to recall the content from "
                           "memory — always read first.")
                .arg(inputs.canvasFilename,
                     inputs.canvasLanguage,
                     QString::number(inputs.canvasLineCount),
                     QString::number(inputs.canvasByteSize),
                     QString::number(inputs.canvasRevision));
        promptLayers.append(canvasLayer);
    }

    // Active-task framing.
    //
    // When activeTaskPlanId is set (Start Task button or LLM-initiated
    // start_task), append a soft framing layer that tells the model
    // what it is currently working on.
    //
    // Stale-plan self-repair: if the anchor points to a plan that has
    // already transitioned to Completed or Failed, clear the prompt
    // section AND ask the caller to reset its own anchor via
    // result.clearActiveTaskPlanId. Local `effectiveActivePlanId`
    // starts as inputs.activeTaskPlanId and is cleared if the plan
    // is terminal or missing; the prompt block is skipped in that
    // case.
    QString effectiveActivePlanId = inputs.activeTaskPlanId;
    if (!effectiveActivePlanId.isEmpty() && inputs.planService) {
        auto activePlanOpt = inputs.planService->getPlan(effectiveActivePlanId);
        if (!activePlanOpt.has_value()) {
            qCDebug(verzetaUi) << "ACTIVE TASK: activeTaskPlanId" << effectiveActivePlanId
                               << "has no row; clearing.";
            effectiveActivePlanId.clear();
            result.clearActiveTaskPlanId = true;
        } else if (activePlanOpt->status == PlanStatus::Completed ||
                   activePlanOpt->status == PlanStatus::Failed) {
            qCDebug(verzetaUi) << "ACTIVE TASK: plan" << effectiveActivePlanId << "is terminal ("
                               << planStatusToString(activePlanOpt->status)
                               << "); clearing stale anchor.";
            effectiveActivePlanId.clear();
            result.clearActiveTaskPlanId = true;
            activePlanOpt.reset();
        }
        if (activePlanOpt.has_value()) {
            const AgentPlan& ap = activePlanOpt.value();

            // A task is owned by the whole conversation — there is no
            // per-step owner, no owner-aware framing, and no hand-off.
            // (Closing is attributed via complete_task → completed_by;
            // turn routing in a group is the @-mention cascade, not the
            // task.)

            // Past-work context. Prepend recent TaskObserver activity
            // so the agent sees what has already been done on this
            // plan and does not repeat tool calls.
            QString pastWorkSection;
            if (inputs.taskObserver) {
                const QStringList recent =
                    inputs.taskObserver->recentActivity(effectiveActivePlanId, 8);
                if (!recent.isEmpty()) {
                    QStringList bullets;
                    for (int i = recent.size() - 1; i >= 0; --i) {
                        bullets.append(QStringLiteral("  • ") + recent[i]);
                    }
                    pastWorkSection =
                        QStringLiteral("\n"
                                       "What's been done on this task so far:\n"
                                       "%1\n"
                                       "\n"
                                       "Do NOT repeat work that's already above — build "
                                       "on it. If a file was already written, read it "
                                       "before editing. If a search already ran, use "
                                       "the existing result.\n")
                            .arg(truncateToTokens(bullets.join(QLatin1Char('\n')), 600));
                }
            }

            // Flat, group-owned framing. No per-step owner, no critic, no
            // hand-off, no mandatory status marker: a task belongs to the
            // conversation, the model just does the work with its normal
            // tools, and anyone closes it with complete_task.
            QString taskLayer =
                QStringLiteral("---\n"
                               "=== ACTIVE TASK ===\n"
                               "\n"
                               "An active task is open in this conversation:\n"
                               "\n"
                               "  \"%1\"\n"
                               "%2"
                               "\n"
                               "Keep working toward it directly with your tools — "
                               "write_file, read_file, list_files, run_shell, search_web, "
                               "search_messages, and the others — called through your "
                               "tool-call channel (never as JSON in your text). Do "
                               "everything the user asked for: if they asked for a file "
                               "you MUST call write_file; if they asked you to look "
                               "something up, call search_web / search_messages. Do not "
                               "describe a file in text as a substitute for writing it, "
                               "and do not narrate (\"I'll create it!\") — just produce "
                               "the output.\n"
                               "\n"
                               "This task belongs to the whole conversation — there is no "
                               "per-step owner. You do NOT need to call start_task again. "
                               "When everything the user asked for actually exists, call "
                               "complete_task to close it. Anyone in the chat can close "
                               "it.")
                    .arg(ap.goal, pastWorkSection);
            // Group only: assignment-by-mention. An ownerless task in a
            // team needs the NEXT concrete piece assigned every turn or
            // responsibility diffuses into consensus talk; assignment
            // rides the existing @-mention router, not owner machinery.
            if (conv->isGroup) {
                taskLayer += QStringLiteral("\n\nEach turn, either DO the next concrete piece of "
                                            "this task yourself with a tool call, or @-mention "
                                            "exactly ONE teammate and state precisely what they "
                                            "should produce next. Never leave the next step "
                                            "unassigned, and never re-discuss a piece someone "
                                            "already delivered.");
            }
            promptLayers.append(taskLayer);
        }
    }

    if (inputs.toolService && inputs.toolsEnabled) {
        const QList<ToolSchema> toolsAvail = inputs.toolService->availableTools();
        if (!toolsAvail.isEmpty()) {
            // The full prose tool list duplicates information the
            // model already receives on the structured `tools`
            // channel (~3,000 tokens on a 41-tool roster — ~18% of a
            // 16k window). The per-conversation
            // `tools_in_system_prompt` flag (default false) reclaims
            // that budget for history; the protocol-discipline
            // instruction is kept in BOTH branches because inline-JSON
            // LARP prevention does not depend on the list itself.
            const bool fullList = LlmConfig::fromJson(conv->llmConfig).toolsInSystemPrompt;
            if (fullList) {
                QStringList lines;
                lines.reserve(toolsAvail.size());
                for (const ToolSchema& s : toolsAvail) {
                    lines.append(QStringLiteral("  - %1: %2").arg(s.name, s.description));
                }
                promptLayers.append(
                    QStringLiteral("=== TOOL CALLING ===\n"
                                   "Available tools:\n%1\n"
                                   "When you need to call a tool, USE THE FUNCTION-CALLING "
                                   "PROTOCOL provided by the model — do NOT write JSON like "
                                   "{\"tool_name\": {...}} into your message body. Inline JSON "
                                   "is treated as plain text and will NOT execute the tool. "
                                   "Only the structured tool_calls channel actually runs the "
                                   "tool. If you say you called a tool but did not use the "
                                   "protocol, nothing happened. If a tool fails, retry once; "
                                   "if it still fails, explain what went wrong in plain "
                                   "language and ask for help.\n")
                        .arg(lines.join(QLatin1Char('\n'))));
            } else {
                promptLayers.append(
                    QStringLiteral("=== TOOL CALLING ===\n"
                                   "You have tools available via the function-calling "
                                   "channel (their schemas are provided to you directly). "
                                   "When you need to call a tool, USE THE FUNCTION-CALLING "
                                   "PROTOCOL — do NOT write JSON into your message body; "
                                   "inline JSON is treated as plain text and will NOT "
                                   "execute. If you say you called a tool but did not use "
                                   "the protocol, nothing happened. If a tool fails, retry "
                                   "once; if it still fails, explain what went wrong and "
                                   "ask for help.\n"));
            }
        }
    }

    if (inputs.pollService && !inputs.activeConvId.isEmpty()) {
        const QVariantList openPolls =
            inputs.pollService->pollsForConversation(inputs.activeConvId, /*openOnly=*/true);
        if (!openPolls.isEmpty()) {
            auto estimateTokens = [](const QString& s) -> int {
                return static_cast<int>(s.toUtf8().size() / 4);
            };
            constexpr int kPollBudgetTokens = 600;

            const QString header =
                QStringLiteral("=== ACTIVE POLLS ===\n"
                               "Open polls in this conversation are listed below.\n"
                               "DO NOT call start_poll for any question that matches "
                               "an existing entry — the host rejects duplicate-"
                               "question polls. To act on an existing poll, call "
                               "cast_vote with its poll_id + option_text "
                               "(recommended) or option_id; close_poll to finalise; "
                               "get_poll_results to inspect current tallies. "
                               "start_poll is ONLY for NEW questions not already "
                               "represented here.\n");
            int spent = estimateTokens(header);
            QStringList lines;
            int dropped = 0;
            for (const QVariant& v : openPolls) {
                const QVariantMap m = v.toMap();
                const QString pid = m.value(QStringLiteral("id")).toString();
                const QString q = m.value(QStringLiteral("question")).toString();
                const QString mode = m.value(QStringLiteral("mode")).toString();
                const QString creator = m.value(QStringLiteral("creator_alias")).toString();
                const QVariantList opts = m.value(QStringLiteral("options")).toList();

                QStringList optBits;
                for (const QVariant& ov : opts) {
                    const QVariantMap om = ov.toMap();
                    optBits.append(QStringLiteral("\"%1\" (%2)")
                                       .arg(om.value(QStringLiteral("text")).toString())
                                       .arg(om.value(QStringLiteral("votes")).toInt()));
                }

                // Pull the last few votes so the agent sees who voted
                // since its previous turn (the user's tap, another
                // agent's cast_vote). Without this surface the agent
                // is blind to votes that landed between its turns —
                // it has to call get_poll_results to even notice.
                const QList<PollVote> votes = inputs.pollService->votesForPoll(pid);
                QString recentActivity;
                if (!votes.isEmpty()) {
                    QStringList lines;
                    const int show = qMin(5, votes.size());
                    for (int i = votes.size() - show; i < votes.size(); ++i) {
                        const PollVote& v = votes.at(i);
                        // Resolve option_id back to text for readability.
                        QString optText = v.optionId;
                        for (const QVariant& ov : opts) {
                            const QVariantMap om = ov.toMap();
                            if (om.value(QStringLiteral("id")).toString() == v.optionId) {
                                optText = om.value(QStringLiteral("text")).toString();
                                break;
                            }
                        }
                        lines.append(
                            QStringLiteral("    %1 voted \"%2\"").arg(v.voterAlias, optText));
                    }
                    recentActivity = QStringLiteral("  recent votes:\n%1\n").arg(lines.join('\n'));
                }

                QString line = QStringLiteral("- poll_id: %1 — Q: %2 — mode: %3 — started by %4\n"
                                              "  options: [%5]\n%6")
                                   .arg(pid,
                                        q,
                                        mode,
                                        creator,
                                        optBits.join(QStringLiteral(", ")),
                                        recentActivity);
                const int cost = estimateTokens(line);
                if (spent + cost > kPollBudgetTokens) {
                    ++dropped;
                    continue;
                }
                spent += cost;
                lines.append(line);
            }
            if (!lines.isEmpty()) {
                promptLayers.append(header + lines.join(QString()));
                if (dropped > 0) {
                    qCDebug(verzetaUi) << "RequestBuilder: ACTIVE POLLS budget exhausted —"
                                       << "injected" << lines.size() << "polls,"
                                       << "dropped" << dropped;
                }
            }
        }
    }

    if (inputs.skillService) {
        const bool hasPreferred = !inputs.resolvedPreferredSkillIds.isEmpty();

        // Build the available-tool name set ONCE per request; reused
        // for every skill's readiness check. Empty when toolService is
        // null OR toolsEnabled is false — in that case skills are
        // emitted without [READY]/[BLOCKED] annotation (we have no
        // ground truth to compare against).
        QSet<QString> availableToolNames;
        const bool toolReadinessKnown = inputs.toolService && inputs.toolsEnabled;
        if (toolReadinessKnown) {
            const QList<ToolSchema> toolsAvail = inputs.toolService->availableTools();
            for (const ToolSchema& s : toolsAvail) {
                availableToolNames.insert(s.name);
            }
        }

        if (hasPreferred) {
            const int ctxWin = (req.config.contextWindow > 0) ? req.config.contextWindow : 8192;
            const int budgetTokens = qMin(1500, qMax(200, ctxWin / 32));
            // Cheap token estimator: bytes / 4. We never load a tokenizer
            // in the request path.
            auto estimateTokens = [](const QString& s) -> int {
                return static_cast<int>(s.toUtf8().size() / 4);
            };

            QString header;
            if (inputs.skillsExposeOnly) {
                header = QStringLiteral("=== AVAILABLE SKILLS ===\n"
                                        "These are the only skills available for this scope. "
                                        "Before producing any concrete deliverable, check if "
                                        "a skill matches the user's request — if one does, "
                                        "call read_skill with its id to load the full "
                                        "instructions BEFORE you act. Skills do NOT grant "
                                        "tools; your tool list is fixed by AVAILABLE TOOLS. "
                                        "Skills whose required tools are missing on this "
                                        "platform are tagged with a missing-tools note — do "
                                        "not pick those without addressing the missing tool "
                                        "first.\n");
            } else {
                header = QStringLiteral("=== AVAILABLE SKILLS ===\n"
                                        "Skills preferred for this conversation are listed "
                                        "below. Before producing any concrete deliverable, "
                                        "check if a skill matches the user's request — if "
                                        "one does, call read_skill with its id to load the "
                                        "full instructions BEFORE you act. To enumerate "
                                        "other approved skills in the application library, "
                                        "call discover_skills. Skills do NOT grant tools; "
                                        "your tool list is fixed by AVAILABLE TOOLS. Skills "
                                        "whose required tools are missing on this platform "
                                        "are tagged with a missing-tools note — do not pick "
                                        "those without addressing the missing tool first.\n");
            }

            // Two-pass: format every skill, partition into ready/blocked,
            // then apply the greedy budget on the concatenated stream
            // (ready first, blocked after). This way the budget never
            // drops a [READY] skill in favour of a [BLOCKED] one.
            QStringList readyLines;
            QStringList blockedLines;
            for (const QString& sid : inputs.resolvedPreferredSkillIds) {
                const Skill s = inputs.skillService->skillById(sid);
                if (!s.isValid())
                    continue;

                QStringList missing;
                if (toolReadinessKnown) {
                    for (const QString& declared : s.declaredTools) {
                        if (!availableToolNames.contains(declared)) {
                            missing.append(declared);
                        }
                    }
                }

                QString line = QStringLiteral("- %1: %2").arg(s.id, s.description);
                if (toolReadinessKnown && !s.declaredTools.isEmpty()) {
                    if (missing.isEmpty()) {
                        line += QStringLiteral(" [READY]");
                    } else {
                        line += QStringLiteral(" [BLOCKED: missing %1]")
                                    .arg(missing.join(QStringLiteral(", ")));
                    }
                }
                line += QStringLiteral("\n");
                if (!s.tags.isEmpty()) {
                    line += QStringLiteral("  tags: %1\n").arg(s.tags.join(", "));
                }
                if (!s.declaredTools.isEmpty()) {
                    line +=
                        QStringLiteral("  declared tools: %1\n").arg(s.declaredTools.join(", "));
                }

                if (missing.isEmpty()) {
                    readyLines.append(line);
                } else {
                    blockedLines.append(line);
                }
            }

            QStringList orderedLines = readyLines + blockedLines;

            int spent = estimateTokens(header);
            QStringList accepted;
            int dropped = 0;
            for (const QString& line : orderedLines) {
                const int cost = estimateTokens(line);
                if (spent + cost > budgetTokens) {
                    ++dropped;
                    continue;
                }
                spent += cost;
                accepted.append(line);
            }
            if (!accepted.isEmpty()) {
                promptLayers.append(header + accepted.join(QString()));
                if (dropped > 0) {
                    qCDebug(verzetaUi) << "RequestBuilder: AVAILABLE SKILLS budget exhausted —"
                                       << "injected" << accepted.size() << "skills,"
                                       << "dropped" << dropped;
                }
            }
        } else if (!inputs.skillsExposeOnly) {
            const QVariantList approved = inputs.skillService->approvedSkills();
            if (!approved.isEmpty()) {
                promptLayers.append(
                    QStringLiteral("=== SKILLS AVAILABLE ===\n"
                                   "Your application library has %1 approved skill(s) "
                                   "installed (no preferred list is set for this "
                                   "conversation). A skill is a packaged set of "
                                   "instructions for a specific workflow (formatting "
                                   "conventions, multi-step procedures, integrations). "
                                   "If — and only if — the user's request clearly matches "
                                   "such a workflow, call discover_skills to list them and "
                                   "then read_skill on a matching id from that list. "
                                   "Otherwise, handle the request directly with your "
                                   "normal tools — you do not need a skill, and never call "
                                   "read_skill on a guessed id. Skills do NOT grant tools; "
                                   "your tool list is fixed by AVAILABLE TOOLS.\n")
                        .arg(approved.size()));
            }
        }
    }

    if (inputs.heartbeatConfigService && effectiveAgent.isValid() &&
        !inputs.activeConvId.isEmpty()) {
        const HeartbeatScopeType scope = conv->isGroup ? HeartbeatScopeType::ConversationGroup
                                                       : HeartbeatScopeType::Conversation1to1;
        const QString aliasForLookup = conv->isGroup ? inputs.responseMemberAlias : QString();

        const HeartbeatConfig existing = inputs.heartbeatConfigService->configFor(
            effectiveAgent.id, scope, inputs.activeConvId, aliasForLookup);

        // Only inject the heartbeat-awareness prompt layer when there is
        // something concrete to surface — i.e. an existing schedule for
        // this (agent, scope, alias) tuple OR the agent has heartbeat
        // defaults configured. Stuffing the routines block into every
        // foreground turn pushes smaller models away from the tool-call
        // protocol because the prompt grows past where they can keep
        // both proseand structured-output skills in focus simultaneously.
        const bool agentHasDefaults = !effectiveAgent.defaultHeartbeatGoal.isEmpty() ||
                                      !effectiveAgent.defaultHeartbeatSchedule.isEmpty() ||
                                      !effectiveAgent.defaultHeartbeatSurfaceCriteria.isEmpty();

        if (!existing.isValid() && !agentHasDefaults) {
            // No existing routine, no agent-level defaults — skip the
            // layer to keep the prompt tight. Heartbeat self-config tools
            // are still registered + invokable; the user can configure a
            // routine through the UI and the layer fires from then on.
            goto _skip_heartbeat_layer;
        }

        {
            QString hb = QStringLiteral("=== BACKGROUND ROUTINES ===\n"
                                        "You can schedule yourself to run on a recurring basis "
                                        "(hourly, daily at HH:MM, weekly, or every N>=5 minutes). "
                                        "While running as a routine you receive a private "
                                        "[BACKGROUND ACTIVITY MODE] block and your output is "
                                        "stored in the heartbeat overlay until reviewed; nothing "
                                        "is posted to the team automatically. Use these tools to "
                                        "configure your schedule for the current chat:\n"
                                        "  set_heartbeat_goal, set_heartbeat_schedule,\n"
                                        "  set_heartbeat_surface_criteria,\n"
                                        "  enable_heartbeat, disable_heartbeat.\n"
                                        "Propose a routine only when the user's request implies "
                                        "ongoing or recurring work; do NOT enable a routine "
                                        "without explicit user confirmation.\n");

            if (existing.isValid()) {
                const QString state =
                    existing.enabled ? QStringLiteral("enabled") : QStringLiteral("paused");
                hb += QStringLiteral("Current routine for this chat: %1, schedule \"%2\", "
                                     "goal \"%3\".\n")
                          .arg(state, existing.schedule, existing.goal);
            } else {
                hb += QStringLiteral("No routine is configured for this chat yet.\n");
            }
            promptLayers.append(hb);
        }
    _skip_heartbeat_layer:;
    }

    // Join layers with section separators.
    QString composedPrompt;
    if (promptLayers.size() == 1) {
        composedPrompt = promptLayers.first();
    } else if (promptLayers.size() > 1) {
        composedPrompt = promptLayers.join(QStringLiteral("\n\n--- --- ---\n\n"));
    }

    // RAG augmentation. The context was retrieved ASYNCHRONOUSLY off the turn
    // path (ConversationRun → RagService::retrieveAsync → retrievalReady) and
    // pre-formatted into inputs.ragContext; buildRequest never embeds or
    // retrieves synchronously, so it never blocks the main thread.
    req.systemPrompt = composedPrompt;
    if (!inputs.ragContext.isEmpty()) {
        req.systemPrompt += inputs.ragContext;
    }

    // Populate available tools when ToolService is attached and the
    // session-level tools toggle is on. Task-management tools
    // (submit_result / report_blocked / approve_step / ...) are
    // filtered out when no task is active — the LLM should not see
    // those as options outside an executor context.
    if (inputs.toolService && inputs.toolsEnabled) {
        const QList<ToolSchema> allTools = inputs.toolService->availableTools();
        const bool taskActive = !effectiveActivePlanId.isEmpty();

        if (taskActive) {
            req.availableTools = allTools;
        } else {
            static const QSet<QString> kTaskOnlyTools = {
                QStringLiteral("submit_result"),
                QStringLiteral("report_blocked"),
                QStringLiteral("stop_task"),
                QStringLiteral("update_plan_step"),
            };
            req.availableTools.reserve(allTools.size());
            for (const ToolSchema& t : allTools) {
                if (!kTaskOnlyTools.contains(t.name)) {
                    req.availableTools.append(t);
                }
            }
        }

        // Capability-scope filter. A single-agent / plain chat is NOT
        // offered group-coordination (cascade / polls) or project-
        // membership tools it can never use; offering them burned a large
        // share of the context window on dead schemas. The filter ONLY
        // narrows: a group chat keeps its GroupOnly tools, a project/org
        // chat keeps its ProjectOnly tools — nothing is granted. Applied
        // BEFORE the whitelist (both only narrow). The tools' own runtime
        // self-rejection stays as defense-in-depth.
        {
            const bool isGroup = conv->isGroup;
            const bool inProject = memberOv.projectScoped;
            QList<ToolSchema> scoped;
            scoped.reserve(req.availableTools.size());
            for (const ToolSchema& t : req.availableTools) {
                bool keep = true;
                switch (t.scope) {
                    case ToolScope::GroupOnly:
                        keep = isGroup;
                        break;
                    case ToolScope::ProjectOnly:
                        keep = inProject;
                        break;
                    case ToolScope::Universal:
                        keep = true;
                        break;
                }
                if (keep) {
                    scoped.append(t);
                }
            }
            req.availableTools = scoped;
        }

        QStringList toolWhitelist;
        if (memberOv.memberScoped) {
            if (memberOv.memberRowFound) {
                toolWhitelist = memberOv.allowedTools;
            }
            // memberScoped && !memberRowFound → self-heal: no whitelist.
        } else if (effectiveAgent.isValid()) {
            toolWhitelist = effectiveAgent.allowedTools;
        }
        if (!toolWhitelist.isEmpty()) {
            const QSet<QString> whitelist(toolWhitelist.cbegin(), toolWhitelist.cend());
            QList<ToolSchema> filtered;
            filtered.reserve(req.availableTools.size());
            for (const ToolSchema& t : req.availableTools) {
                if (whitelist.contains(t.name)) {
                    filtered.append(t);
                }
            }
            req.availableTools = filtered;
        }
    }

    req.turnKind = QStringLiteral("conversational");

    // Conversational turns get a "Your tasks" block listing the
    // responder's active and recently-completed plans, so an agent
    // picked up in one conversation is aware of commitments it has
    // open in other chats. Plans from the CURRENT conversation are
    // filtered out — the active-task framing block above already
    // covers those and duplicating here reads like redundant
    // status-reporting to the model.
    if (req.turnKind == QStringLiteral("conversational") && inputs.planService &&
        !inputs.responseMemberAgentId.isEmpty() && !inputs.responseMemberAlias.isEmpty()) {
        const QStringList activeStatuses = {
            QStringLiteral("planning"),
            QStringLiteral("executing"),
            QStringLiteral("critiquing"),
        };
        const QStringList doneStatuses = {
            QStringLiteral("completed"),
        };

        const QList<AgentPlan> activePlans =
            plansExcludingConversation(inputs.planService->plansByAgentIdAcrossChats(
                                           inputs.responseMemberAgentId, activeStatuses, 0),
                                       inputs.inflightConvId);
        const QList<AgentPlan> doneePlans =
            plansExcludingConversation(inputs.planService->plansByAgentIdAcrossChats(
                                           inputs.responseMemberAgentId, doneStatuses, 7),
                                       inputs.inflightConvId);

        if (!activePlans.isEmpty() || !doneePlans.isEmpty()) {
            // Slim cross-chat awareness — titles only, max 3 recent
            // completed plans. Agents retrieve past content on demand
            // via the search_messages / read_conversation tools so
            // the prompt stays compact.
            static constexpr int kMaxRecentDonePlans = 3;
            QStringList lines;

            if (!activePlans.isEmpty()) {
                lines << QStringLiteral("\n\n=== YOUR ACTIVE TASKS ===");
                for (const AgentPlan& p : activePlans) {
                    lines << QStringLiteral("- [%1] home chat: %2")
                                 .arg(p.goal.left(80), p.conversationId.left(8));
                }
            }

            if (!doneePlans.isEmpty()) {
                lines << QStringLiteral("\n\n=== YOUR RECENTLY COMPLETED TASKS (titles only) ===");
                int n = 0;
                for (const AgentPlan& p : doneePlans) {
                    if (n >= kMaxRecentDonePlans)
                        break;
                    lines << QStringLiteral("- [%1]").arg(p.goal.left(80));
                    ++n;
                }
                if (doneePlans.size() > kMaxRecentDonePlans) {
                    lines << QStringLiteral("- …and %1 more")
                                 .arg(doneePlans.size() - kMaxRecentDonePlans);
                }
                lines << QStringLiteral("If you need the content of a past task, call "
                                        "search_messages or read_conversation — do not assume "
                                        "the work is already done when the user asks for it "
                                        "again.");
            }
            req.systemPrompt += lines.join(QLatin1Char('\n'));
        }
    }

    LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
    if (cfg.providerId.isEmpty()) {
        cfg.providerId = m_router.activeProviderId();
    }
    if (cfg.modelName.isEmpty()) {
        cfg.modelName = m_router.activeModelName();
    }

    QString ovProvider;
    QString ovModel;
    QString ovSource;  // diagnostic label for the unregistered-provider warning
    if (memberOv.memberScoped) {
        if (memberOv.memberRowFound) {
            ovProvider = memberOv.modelProvider;
            ovModel = memberOv.modelName;
            ovSource = QStringLiteral("member '%1'").arg(memberOv.alias);
        }
        // memberScoped && !memberRowFound → no override; conv default stands.
    } else if (effectiveAgent.isValid()) {
        ovProvider = effectiveAgent.modelProvider;
        ovModel = effectiveAgent.modelName;
        ovSource = QStringLiteral("agent '%1'").arg(effectiveAgentId);
    }

    if (!ovProvider.isEmpty()) {
        if (m_router.providerForId(ovProvider)) {
            cfg.providerId = ovProvider;
            // The model is only meaningful paired with its provider —
            // apply it only when the provider substitution succeeded.
            if (!ovModel.isEmpty()) {
                cfg.modelName = ovModel;
            }
        } else {
            qCWarning(verzetaUi) << "RequestBuilder:" << ovSource << "names unregistered provider"
                                 << ovProvider << "— falling back to conversation default provider"
                                 << cfg.providerId;
        }
    } else if (!ovModel.isEmpty()) {
        // Model-only override: pin a specific model on whatever provider
        // the conversation already uses.
        cfg.modelName = ovModel;
    }

    // Guard: refuse to send a request with no model — Ollama returns 400.
    if (cfg.modelName.isEmpty()) {
        result.success = false;
        result.errorReason = QStringLiteral("No model selected. Please select a model first.");
        return result;
    }

    // Context-window floor.
    //
    // Group chats need at least 16K tokens because the roster +
    // group-chat contract + active-task framing eat a noticeable
    // fraction of the prompt budget before history even loads.
    //
    // 1:1 chats need the same floor as soon as ANY of these padding
    // sources are in play:
    //   - an active task is anchored (ACTIVE TASK block ≈ 1000 tok),
    //   - an active canvas is open (ACTIVE CANVAS + 3 canvas tools'
    //     descriptions ≈ 200 tok),
    //   - tools are enabled (full schema list of 20+ built-in tools
    //     descriptions ≈ 1500-2500 tok),
    //   - skills are resolved into the system prompt (AVAILABLE SKILLS
    //     layer ≈ 200-1500 tok, capped),
    //   - a heartbeat config exists for this membership (BACKGROUND
    //     ROUTINES layer ≈ 300 tok).
    // Otherwise default 8K contextWindow minus the 4K maxTokens output
    // reserve leaves a history budget that even a fresh conversation's
    // system prompt overruns the moment any of the above is present —
    // surfaced by the live-Ollama 1:1 stress run hitting "Conversation
    // too large" on every turn before a single message could be sent.
    // Capture the user's PRISTINE stored window BEFORE the internal floor
    // bump below, so model-aware resolution can tell a genuine Conversation-
    // Settings override apart from an internal bump (the bump must NOT be
    // mistaken for a user override — that would suppress model-aware sizing).
    const int userStoredContextWindow = cfg.contextWindow;

    const bool needsLargerContext = conv->isGroup || !effectiveActivePlanId.isEmpty() ||
                                    !inputs.canvasFilename.isEmpty() || inputs.toolsEnabled ||
                                    !inputs.resolvedPreferredSkillIds.isEmpty() ||
                                    (inputs.heartbeatConfigService != nullptr);
    if (needsLargerContext && cfg.contextWindow < 16384) {
        cfg.contextWindow = 16384;
    }

    // Per-(provider, model) sampling profile.  Applies workaround
    // sampling parameters (e.g. forced greedy for stock Ollama
    // qwen3.5/3.6) for known-problematic combinations.  Profile
    // fields are additive defaults that fill in slots the user has
    // not explicitly set (`-1` sentinel) — explicit user values
    // always win.  Returns nullptr for the common case where no
    // profile applies.
    if (const auto* appliedProfile = Verzeta::Models::applyProfileTo(cfg)) {
        // Log once per process per profile so operators reading the
        // logs see which workaround is active without spamming on
        // every request.  Use a static set of already-logged reasons
        // — distinct profiles get distinct entries.
        static QSet<QString> s_loggedReasons;
        if (!s_loggedReasons.contains(appliedProfile->reason)) {
            s_loggedReasons.insert(appliedProfile->reason);
            qCDebug(verzetaUi).noquote() << "Sampling profile applied:" << appliedProfile->reason;
        }
    }

    // Echo-retry sampling jitter. The anti-echo text nudge alone
    // cannot break an echo loop when sampling is near-deterministic
    // (e.g. the qwen profile's temperature=0.6 / top_k=10 / top_p=0.5
    // — chosen to prevent early-EOS truncation): the retry re-samples
    // almost the same token path and produces the SAME reply again,
    // burning all kMaxEchoRetries attempts and persisting the echo as
    // "echo-failed". Each retry therefore escalates exploration:
    //   attempt 1: temp 0.8, top_k 30, top_p 0.75, presence 0.3
    //   attempt 2: temp 0.95, top_k 50, top_p 0.9,  presence 0.6
    //   attempt 3: temp 1.1,  top_k 80, top_p 0.95, presence 0.9
    // Presence penalty is the strongest anti-echo lever — it directly
    // penalises tokens already present in the context, which is
    // precisely what an echo is. The escalation applies ONLY to the
    // retry request; the next normal turn returns to profile/user
    // values. Truncation risk from the raised values is acceptable
    // here: a truncated-but-novel retry beats a complete-but-identical
    // echo, and the per-attempt escalation means attempt 1 stays
    // close to the safe profile.
    if (inputs.echoRetryAttempt > 0) {
        const int a = qMin(inputs.echoRetryAttempt, 3);
        static constexpr double kJitterTemp[] = {0.8, 0.95, 1.1};
        static constexpr int kJitterTopK[] = {30, 50, 80};
        static constexpr double kJitterTopP[] = {0.75, 0.9, 0.95};
        static constexpr double kJitterPresence[] = {0.3, 0.6, 0.9};
        cfg.temperature = kJitterTemp[a - 1];
        cfg.topK = kJitterTopK[a - 1];
        cfg.topP = kJitterTopP[a - 1];
        cfg.presencePenalty = kJitterPresence[a - 1];
        qCDebug(verzetaUi) << "Echo-retry sampling jitter applied — attempt" << a
                           << "temp=" << cfg.temperature << "top_k=" << cfg.topK
                           << "top_p=" << cfg.topP << "presence=" << cfg.presencePenalty;
    }

    req.config = cfg;

    // --- History loading + token-budget pruning ---
    {
        const int sysTok = HistoryBudgeter::estimateTokens(req.systemPrompt);
        // Account for the FULL serialized tool schema — name + description + every
        // parameter's name/type/description — not just the description. The old
        // description-only estimate under-counted the tool block by thousands of
        // tokens, so the budgeter handed history a budget that did NOT actually
        // fit num_ctx; the provider then truncated the request server-side,
        // silently corrupting the model's view of the conversation.
        // Tool schemas serialize to JSON on the wire and tokenize denser
        // than prose; estimate them with the CodeOrJson content class so
        // the fixed cost is not under-counted (an under-count here is what
        // silently ate the output reservation on tool-heavy turns).
        using CC = HistoryBudgeter::ContentClass;
        int toolsTok = 0;
        for (const ToolSchema& ts : req.availableTools) {
            toolsTok += HistoryBudgeter::estimateTokens(ts.name, CC::CodeOrJson) +
                        HistoryBudgeter::estimateTokens(ts.description, CC::CodeOrJson) + 8;
            for (const ToolParameterSchema& p : ts.parameters) {
                toolsTok += HistoryBudgeter::estimateTokens(p.name, CC::CodeOrJson) +
                            HistoryBudgeter::estimateTokens(p.description, CC::CodeOrJson) +
                            HistoryBudgeter::estimateTokens(p.type, CC::CodeOrJson) + 6;
            }
        }
        static constexpr int kOutputReservation = 4096;

        std::optional<ConversationSummary> freshSummary;
        int summaryTok = 0;
        if (inputs.summarizer && cfg.dynamicCompactEnabled) {
            auto sOpt = inputs.summarizer->summaryFor(inputs.inflightConvId);
            if (sOpt.has_value() && sOpt->fresh()) {
                freshSummary = std::move(sOpt);
                summaryTok = freshSummary->tokenCount;
            }
        }

        // Model-aware context-window resolution (provider-layer).
        // Ask the active provider for THIS model's real maximum window and let
        // the shared policy helper decide the effective value: a user override
        // (cfg.contextWindow != default) is honoured but capped; otherwise a
        // resolved window is clamped to the sane [floor, ceiling]; an unknown
        // (0) window keeps the default. Resolution lives entirely in the
        // provider (single-flight async warm + cache) — see ILLMProvider::
        // contextWindowFor. The first turn for a never-seen model on a
        // live-source provider may resolve to the default and only warm later.
        const int rawCtx = m_router.contextWindowFor(req.config.providerId, req.config.modelName);
        // Role-aware ceiling: high-overhead turns (group / tools / canvas /
        // skills / heartbeat — the same set that triggered the floor bump
        // above) carry ~9.5k of fixed prompt cost, so a 16k window leaves them
        // starved; give them the heavier 24k ceiling. Lightweight 1:1 turns
        // keep the 16k ceiling (their overhead is small and 32k/24k would only
        // add latency).
        const int ceilingCtx =
            needsLargerContext ? LlmConfig::kSaneCeilingCtxHeavy : LlmConfig::kSaneCeilingCtx;
        // Override detection uses the PRISTINE stored value; the current
        // (possibly group-floor-bumped) cfg.contextWindow is the no-override
        // unknown fallback so an internal floor is never shrunk.
        const int resolvedCtx =
            effectiveContextWindow(rawCtx, userStoredContextWindow, cfg.contextWindow, ceilingCtx);
        if (resolvedCtx != cfg.contextWindow) {
            qCDebug(verzetaUi) << "RequestBuilder: model-aware context window" << cfg.contextWindow
                               << "->" << resolvedCtx << "(provider" << req.config.providerId
                               << "model" << req.config.modelName << "raw" << rawCtx << ")";
            cfg.contextWindow = resolvedCtx;
            req.config.contextWindow = resolvedCtx;
        }

        const int fixedTok = sysTok + toolsTok + kOutputReservation + summaryTok;
        int historyBudget = cfg.contextWindow - fixedTok;

        // Load a batch of recent messages (also used to size the newest turn).
        const int batchSize = qMax(50, (qMax(0, historyBudget) / 30) * 2);
        const QList<Message> recentDb =
            m_msgSvc.getRecentMessages(inputs.inflightConvId, batchSize);

        // The newest user message is the turn being sent — it must always fit.
        int newestUserTok = 0;
        for (int i = recentDb.size() - 1; i >= 0; --i) {
            if (recentDb.at(i).role == QStringLiteral("user")) {
                newestUserTok = HistoryBudgeter::estimateMessageTokens(recentDb.at(i));
                break;
            }
        }

        // "Conversation too large" must be UNREACHABLE. Rather than refuse the
        // turn, grow num_ctx just enough to hold the fixed cost + a minimum of
        // history (or the newest user turn, whichever is larger), capped at a
        // sane ceiling. Compaction already replaces dropped history with the
        // summary, so the live turn is all this must guarantee. Most turns never
        // grow (they fit the configured window); this only bites when the system
        // prompt + tools + a large pasted message would otherwise overflow.
        static constexpr int kMaxAutoContextWindow = 32768;
        const int requiredHistory = qMax(HistoryBudgeter::kMinHistoryBudget, newestUserTok + 64);
        if (historyBudget < requiredHistory) {
            const int grown = qMin(kMaxAutoContextWindow, fixedTok + requiredHistory);
            if (grown > cfg.contextWindow) {
                qCDebug(verzetaUi)
                    << "RequestBuilder: auto-grew context window" << cfg.contextWindow << "->"
                    << grown << "(fixed" << fixedTok << "+ required history" << requiredHistory
                    << ") to keep the turn sendable";
                cfg.contextWindow = grown;
                req.config.contextWindow = grown;
                historyBudget = cfg.contextWindow - fixedTok;
            }
        }

        // Minimum-history floor: reclaim from the output reservation (arithmetic
        // only — num_predict governs the real output cap) before letting history
        // starve. Only bites when the auto-grow ceiling above was hit.
        historyBudget =
            HistoryBudgeter::applyMinimumHistoryFloor(historyBudget, kOutputReservation);

        auto budgetResult = HistoryBudgeter::selectForBudget(
            recentDb, qMax(0, historyBudget), inputs.streamingMsgId);

        if (!budgetResult.ok) {
            // Unreachable for normal turns after the auto-grow (only a single
            // user message larger than the whole capped window lands here). NEVER
            // refuse the turn — send the newest user message alone; the summary
            // is prepended below and the provider caps its own input if needed.
            qCWarning(verzetaUi) << "RequestBuilder: newest user turn exceeds even the grown "
                                    "window — sending it alone rather than refusing the turn";
            HistoryBudgeter::Result forced;
            forced.totalDbRows = recentDb.size();
            for (int i = recentDb.size() - 1; i >= 0; --i) {
                if (recentDb.at(i).role == QStringLiteral("user")) {
                    forced.messages.append(recentDb.at(i));
                    break;
                }
            }
            forced.ok = true;
            forced.includedRows = static_cast<int>(forced.messages.size());
            forced.droppedRows = forced.totalDbRows - forced.includedRows;
            budgetResult = forced;
        }

        qCDebug(verzetaUi) << "HistoryBudgeter:"
                           << "dbBatch=" << recentDb.size() << "budget=" << historyBudget
                           << "included=" << budgetResult.includedRows
                           << "dropped=" << budgetResult.droppedRows
                           << "estTokens=" << budgetResult.estimatedTokens;

        // Context-fill indicator (chat input area, INNER gauge ring). Used
        // tokens = fixed prompt parts + the summary (when injected) + the
        // history actually selected.
        {
            const int used = sysTok + toolsTok + summaryTok + budgetResult.estimatedTokens;
            if (cfg.contextWindow > 0) {
                result.contextFillPercent = qBound(0, (used * 100) / cfg.contextWindow, 100);
            }
        }

        // Compaction-cadence indicator (OUTER gauge ring): assistant turns
        // accumulated toward the next memory refresh vs the conversation's
        // compactEveryTurns. 0 total = cadence off (the gauge then shows
        // context pressure only).
        result.compactionTurnsTotal = cfg.compactEveryTurns;
        if (inputs.summarizer && cfg.compactEveryTurns > 0) {
            result.compactionTurnsUsed = inputs.summarizer->cadenceTurnsUsed(inputs.inflightConvId);
        }

        req.messages = assembleHistory(budgetResult.messages, conv->isGroup, inputs.streamingMsgId);

        digestToolPayloads(req.messages, qMax(0, historyBudget));

        if (freshSummary.has_value()) {
            LlmMessage summaryMsg;
            summaryMsg.role = QStringLiteral("system");
            summaryMsg.content = QStringLiteral("Summary of earlier conversation (%1 messages "
                                                "compacted):\n\n%2")
                                     .arg(freshSummary->coveredCount)
                                     .arg(freshSummary->summaryText);
            req.messages.prepend(summaryMsg);
        }

        {
            const bool allowGrow = (userStoredContextWindow == LlmConfig::kDefaultContextWindow);
            result.contextFillPercent = shapeRequestToWindow(req, sysTok, toolsTok, allowGrow);
            // Keep the local cfg mirror in sync for any later reads.
            cfg.contextWindow = req.config.contextWindow;
        }

        // Auto-trigger. Fire-and-forget:
        // THIS turn proceeds with whatever the budgeter selected; the
        // generated summary benefits the NEXT turn. recentDb.size()
        // is the batch (a lower bound on the conversation total when
        // the batch filled), which is sufficient for the >30 gate
        // because batchSize >= 50. The measured fill percentage feeds
        // the PROACTIVE path (compact at amber, before any drop) and
        // cfg.compactEveryTurns feeds the per-conversation CADENCE
        // path (attention freshening) — see
        // ConversationSummarizer::shouldSummarize.
        // Decide here (we have droppedRows + fill%), but do NOT dispatch the
        // summary from the build path: that fired it concurrently with the turn
        // about to be sent and saturated a single local provider (RAGP then timed
        // out → cascade stalled). ConversationRun dispatches it in the post-cascade
        // idle gap (or immediately when context is already critical).
        if (inputs.summarizer && inputs.summarizer->shouldSummarize(inputs.inflightConvId,
                                                                    recentDb.size(),
                                                                    budgetResult.droppedRows,
                                                                    cfg.dynamicCompactEnabled,
                                                                    result.contextFillPercent,
                                                                    cfg.compactEveryTurns)) {
            result.shouldSummarize = true;
        }

        // Cascade-tail nudge — when the responder is about to speak
        // after another agent's turn, small local models need an
        // explicit cue that it is now their turn or they tend to
        // echo the previous turn back as a stop-token reply. The
        // nudge is emitted as role=system because it is an
        // app-injected routing instruction, not a human utterance;
        // role=user would cause some models to echo it back as
        // quoted text.
        if (conv->isGroup && !inputs.responseMemberAlias.isEmpty() && !req.messages.isEmpty() &&
            (req.messages.last().role == QStringLiteral("assistant") ||
             req.messages.last().role == QStringLiteral("tool"))) {
            const QString normalizedSelf =
                inputs.responseMemberAlias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));

            // Find the agent's role description for the retry-aware
            // identity refresh — the same agentRegistry lookup used
            // earlier in this function for the system prompt's role
            // layer (effectiveAgent). Reuse instead of looking up
            // again to avoid the registry round-trip.
            QString roleHint;
            if (effectiveAgent.isValid()) {
                roleHint = effectiveAgent.name.trimmed();
            }

            LlmMessage nudge;
            nudge.role = QStringLiteral("system");

            // Retry-aware nudge text. The base form is the same
            // 35-char legacy string for backward-compat with
            // models that the simple form already steers correctly;
            // each escalating retry stacks on a stronger, more-
            // specific anti-echo block. The cumulative pattern
            // exploits the model's own attention to recent system
            // messages — by retry 3 the prompt is dominated by
            // "do not copy" language which competes effectively
            // with the immediately-prior assistant message that
            // the model has been pattern-pulling on.
            QString nudgeText =
                QStringLiteral("[@%1, it is your turn to respond.]").arg(normalizedSelf);

            if (inputs.echoRetryAttempt > 0 &&
                inputs.retryReason == QStringLiteral("impersonation")) {
                nudgeText +=
                    QStringLiteral("\n\n[IDENTITY RETRY %2/3 — your previous reply was "
                                   "written in a TEAMMATE's voice, as if another member "
                                   "had said it. You are @%1 and ONLY @%1. Never write "
                                   "replies, labels, or dialogue for other members — "
                                   "they speak for themselves in their own turns. Reply "
                                   "now with @%1's OWN contribution. If you want a "
                                   "teammate to act or answer, @-mention them instead "
                                   "of speaking for them.]")
                        .arg(normalizedSelf, QString::number(qMin(inputs.echoRetryAttempt, 3)));
            } else if (inputs.echoRetryAttempt == 1) {
                nudgeText +=
                    QStringLiteral("\n\n[ECHO RETRY 1/3 — your previous reply was "
                                   "flagged as a near-verbatim copy of a teammate's "
                                   "reply, OR was empty. Speak in @%1's OWN voice as "
                                   "%2. Produce ENTIRELY new content — do not copy, "
                                   "paraphrase, or echo any teammate's phrasing. "
                                   "Begin with content that introduces or expresses "
                                   "@%1's perspective, not anyone else's.]")
                        .arg(normalizedSelf, roleHint.isEmpty() ? normalizedSelf : roleHint);
            } else if (inputs.echoRetryAttempt == 2) {
                nudgeText +=
                    QStringLiteral("\n\n[ECHO RETRY 2/3 — ESCALATING. Your previous "
                                   "TWO replies were flagged as copies or empty. "
                                   "You are @%1, %2. Discard whatever you were "
                                   "about to say. Begin a FRESH reply that:\n"
                                   "  • starts with a clear statement in @%1's "
                                   "voice (\"I'm @%1, the %2 — ...\" or similar);\n"
                                   "  • describes what @%1 uniquely contributes "
                                   "in @%1's role, not what other teammates do;\n"
                                   "  • is ENTIRELY new content with NO words or "
                                   "phrases borrowed from any teammate's reply in "
                                   "this conversation.\n"
                                   "Do NOT produce another empty reply.]")
                        .arg(normalizedSelf, roleHint.isEmpty() ? normalizedSelf : roleHint);
            } else if (inputs.echoRetryAttempt >= 3) {
                nudgeText +=
                    QStringLiteral("\n\n[ECHO RETRY 3/3 — FINAL ATTEMPT. Your prior "
                                   "THREE attempts all failed (echoed a teammate or "
                                   "produced empty content). This is your last "
                                   "chance to produce a real reply as @%1, %2.\n"
                                   "If this attempt also fails, your turn will be "
                                   "marked as ECHO-FAILED in the conversation log.\n"
                                   "Required: produce a SHORT reply (2-3 sentences) "
                                   "that is unmistakably @%1's voice. Topic: whatever "
                                   "the user just asked. Forbidden: any phrase that "
                                   "appears in another teammate's reply this round.]")
                        .arg(normalizedSelf, roleHint.isEmpty() ? normalizedSelf : roleHint);
            }

            nudge.content = nudgeText;
            req.messages.append(nudge);
        }

        if (!conv->isGroup) {
            const bool lastIsContinuation =
                !req.messages.isEmpty() &&
                (req.messages.last().role == QStringLiteral("assistant") ||
                 req.messages.last().role == QStringLiteral("tool"));
            QString contText;
            if (inputs.echoRetryAttempt > 0 && inputs.retryReason == QStringLiteral("empty") &&
                lastIsContinuation) {
                contText =
                    QStringLiteral("[CONTINUE — your previous turn produced no reply. The "
                                   "tool results above are for you to act on. Continue the "
                                   "task now: call the next tool, or write the requested "
                                   "output. Only stop when the user's request is fully done.]");
            } else if (inputs.isPostToolContinuation && inputs.echoRetryAttempt == 0 &&
                       lastIsContinuation) {
                contText =
                    QStringLiteral("[The tool result above is ready. Continue toward the "
                                   "user's request — take the next action, or give the final "
                                   "answer if the work is done. Stop only when the request is "
                                   "fully satisfied.]");
            }
            if (!contText.isEmpty()) {
                LlmMessage cont;
                cont.role = QStringLiteral("system");
                cont.content = contText;
                req.messages.append(cont);
            }
        }
    }

    if (inputs.isDeferredActionContinuation && conv) {
        LlmMessage followThrough;
        followThrough.role = QStringLiteral("user");
        if (conv->isGroup && !inputs.responseMemberAlias.isEmpty()) {
            const QString self =
                inputs.responseMemberAlias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
            followThrough.content =
                QStringLiteral("[Automated follow-through — no human typed this] "
                               "@%1: your previous reply announced work that never "
                               "appeared — a tool call that did not run, or content "
                               "(a draft, copy, code) the reply promised but stopped "
                               "before writing. Deliver it NOW, in this same turn: "
                               "emit the actual tool call, or write the promised "
                               "content in full — do not describe it again. If you "
                               "truly cannot, say why in one sentence, or hand the "
                               "work to a teammate with an @-mention.")
                    .arg(self);
        } else {
            followThrough.content =
                QStringLiteral("[Automated follow-through — no human typed this] "
                               "Your previous reply announced work that never "
                               "appeared — a tool call that did not run, or content "
                               "(a draft, copy, code) the reply promised but stopped "
                               "before writing. Deliver it NOW, in this same turn: "
                               "emit the actual tool call, or write the promised "
                               "content in full — do not describe it again. If you "
                               "truly cannot, say why in one sentence.");
        }
        req.messages.append(followThrough);
    }

    if (inputs.isAsyncArtifactContinuation && conv) {
        LlmMessage imgFollow;
        imgFollow.role = QStringLiteral("user");
        if (conv->isGroup && !inputs.responseMemberAlias.isEmpty()) {
            const QString self =
                inputs.responseMemberAlias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
            imgFollow.content =
                QStringLiteral("[Automated follow-through — no human typed this] "
                               "@%1: the image you requested has finished generating — "
                               "it is attached here and saved to the project files "
                               "(the message above has the path). Continue your work "
                               "from here: review it, take the next step, or hand off "
                               "to a teammate with an @-mention.")
                    .arg(self);
        } else {
            imgFollow.content =
                QStringLiteral("[Automated follow-through — no human typed this] "
                               "The image you requested has finished generating — it "
                               "is attached here and saved to the project files (the "
                               "message above has the path). Continue your work from "
                               "here: review it and take the next step.");
        }
        req.messages.append(imgFollow);
    }

    // Attach pending images and file content to the last user message.
    // Tells the caller to clear m_pendingImages / m_pendingFileContext
    // via result.consumedAttachments — behaviour-preserving w.r.t. the
    // original inline code which only cleared on successful attach.
    if ((!inputs.pendingImages.isEmpty() || !inputs.pendingFileContext.isEmpty()) &&
        !req.messages.isEmpty()) {
        for (int i = req.messages.size() - 1; i >= 0; --i) {
            if (req.messages[i].role == QStringLiteral("user")) {
                req.messages[i].images = inputs.pendingImages;
                if (!inputs.pendingFileContext.isEmpty()) {
                    req.messages[i].content += inputs.pendingFileContext;
                }
                result.consumedAttachments = true;
                break;
            }
        }
    }

    // Diagnostic: log request shape so we can see when a model is
    // being asked to handle an oversized prompt (common cause of
    // empty responses from smaller local models).
    qCDebug(verzetaUi) << "Routing request —"
                       << "model:" << cfg.providerId << "/" << cfg.modelName
                       << "| system prompt chars:" << req.systemPrompt.length()
                       << "| messages:" << req.messages.size()
                       << "| cascade iter:" << inputs.cascadeIterations << "| turn kind:"
                       << (req.turnKind.isEmpty() ? QStringLiteral("conversational") : req.turnKind)
                       << "| tools:"
                       << (req.availableTools.size() -
                           (req.allowedTools.isEmpty()
                                ? 0
                                : (req.availableTools.size() -
                                   std::count_if(req.availableTools.cbegin(),
                                                 req.availableTools.cend(),
                                                 [&](const ToolSchema& t) {
                                                     return req.allowedTools.contains(t.name);
                                                 }))))
                       << "| responder:"
                       << (inputs.responseMemberAlias.isEmpty() ? QStringLiteral("-")
                                                                : inputs.responseMemberAlias);

    qCDebug(verzetaUi)
        << "System prompt head:" << req.systemPrompt.left(400)
        << (req.systemPrompt.length() > 400
                ? QStringLiteral("…[%1 more chars]").arg(req.systemPrompt.length() - 400)
                : QString());

    result.request = req;
    result.success = true;
    return result;
}


BuildResult buildSurfaceReviewRequest(const SurfaceReviewInputs& inputs) {
    BuildResult out;

    if (inputs.agentId.isEmpty()) {
        out.errorReason = QStringLiteral("buildSurfaceReviewRequest: missing agentId");
        return out;
    }
    if (inputs.providerId.isEmpty() || inputs.modelName.isEmpty()) {
        out.errorReason =
            QStringLiteral("buildSurfaceReviewRequest: no active provider/model configured");
        return out;
    }
    if (inputs.reportId.isEmpty()) {
        out.errorReason = QStringLiteral("buildSurfaceReviewRequest: missing reportId");
        return out;
    }

    const QString criteria =
        inputs.surfaceCriteria.isEmpty() ? inputs.goal : inputs.surfaceCriteria;
    const QString aliasOrName = inputs.alias.isEmpty() ? inputs.agentName : inputs.alias;

    QString reviewPrompt =
        QStringLiteral("Your background activity routine just produced this report.\n"
                       "Decide whether to share it with the team RIGHT NOW.\n\n"
                       "Your goal: %1\n"
                       "Surface this report if: %2\n"
                       "(else: the user can still manually surface it from the overlay "
                       "if they disagree.)\n\n"
                       "Background report:\n"
                       "TITLE:    %3\n"
                       "RESULTS:  %4\n"
                       "SUMMARY:  %5\n\n"
                       "If you should post: you can use the SUMMARY as a starting point "
                       "and adjust to fit the conversation's flow. 1-3 sentences in your "
                       "normal voice. You may @-mention teammates to invite next steps. "
                       "Do NOT include the words [SKIP] in the post itself.\n"
                       "If you should NOT post: return exactly [SKIP].\n")
            .arg(
                inputs.goal, criteria, inputs.reportTitle, inputs.reportBody, inputs.reportSummary);

    // ---- Compose the LlmRequest ----
    LlmRequest req;
    req.requestId = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    req.conversationId = inputs.targetConvId;
    req.systemPrompt = inputs.agentSystemPrompt;
    req.turnKind = QStringLiteral("heartbeat_surface_review");

    LlmConfig cfg;
    cfg.providerId = inputs.providerId;
    cfg.modelName = inputs.modelName;
    cfg.temperature = -1;  // model defaults
    cfg.maxTokens = 512;   // 1-3 sentences cap is generous
    cfg.contextWindow = 8192;
    cfg.stream = true;
    req.config = cfg;

    // Recent conversation messages first (same role labels as
    // assembleHistory uses for chat-controller turns), then the
    // review-prompt user message at the end. The agent SEES the
    // recent context so its post can match the conversation's flow.
    req.messages = inputs.recentMessages;
    LlmMessage tail;
    tail.role = QStringLiteral("user");
    tail.content = std::move(reviewPrompt);
    req.messages.append(tail);

    qCDebug(verzetaUi) << "[heartbeat] surface-review request"
                       << "alias=" << aliasOrName << "reportId=" << inputs.reportId
                       << "targetConv=" << inputs.targetConvId
                       << "messages=" << req.messages.size();

    out.request = req;
    out.success = true;
    return out;
}

}  // namespace Chat

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file subagent-run-service.cpp
 * @brief Implementation of the sub-agent run service. See the header
 *        for the locked architectural contract.
 * @layer Service
 * @dependencies DbManager, ToolService, ModelRouter, SettingsService,
 *               ILLMProvider, Qt6::Core, Qt6::Sql.
 */

#include "subagent-run-service.h"

#include "../api/llm-interface.h"
#include "../models/db-manager.h"
#include "../utils/http-client.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "model-router.h"
#include "settings-service.h"
#include "tool-service.h"

#include <QTimer>

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {
/** Null-QString → empty-string normaliser for NOT NULL column binds.
 *  A null QString binds as SQL NULL and silently fails the
 *  constraint. */
QString nn(const QString& s) {
    return s.isNull() ? QStringLiteral("") : s;
}

QString freshId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
}  // namespace

SubagentRunService::SubagentRunService(DbManager& db,
                                       ToolService& toolSvc,
                                       ModelRouter& router,
                                       SettingsService& settings,
                                       QObject* parent)
    : QObject(parent), m_db(db), m_toolSvc(toolSvc), m_router(router), m_settings(settings) {
    qCInfo(verzetaUi) << "SubagentRunService initialized";
}

SubagentRunService::~SubagentRunService() {
    // Rule 12 — clean shutdown: abort every in-flight provider request
    // deterministically before members destruct.
    for (auto& [id, bundle] : m_runProv) {
        if (bundle.valid())
            bundle.provider->cancelRequest();
    }
    qCInfo(verzetaUi) << "SubagentRunService destroyed";
}

QStringList SubagentRunService::defaultWhitelist() {
    // Decision #7 — read-only default. write_file / edit_canvas /
    // membership / polls require explicit parent opt-in.
    return {QStringLiteral("search_web"),
            QStringLiteral("read_file"),
            QStringLiteral("list_files"),
            QStringLiteral("read_conversation"),
            QStringLiteral("get_current_time")};
}

// ---------------------------------------------------------------------------
// Spawn + queue pump
// ---------------------------------------------------------------------------

QString SubagentRunService::spawn(const QString& conversationId,
                                  const QString& parentMsgId,
                                  const QString& requesterAlias,
                                  const QString& task,
                                  const QStringList& toolsWhitelist,
                                  const QString& providerId,
                                  const QString& modelName) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (task.trimmed().isEmpty() || conversationId.isEmpty()) {
        return {};
    }

    SubagentRun run;
    run.id = freshId();
    run.conversationId = conversationId;
    run.parentMsgId = parentMsgId;
    run.requesterAlias = requesterAlias.isEmpty() ? QStringLiteral("assistant") : requesterAlias;
    run.task = task.trimmed();
    run.toolsWhitelist = toolsWhitelist.isEmpty() ? defaultWhitelist() : toolsWhitelist;
    // Decision #8 — depth 1: recursion structurally impossible.
    run.toolsWhitelist.removeAll(QStringLiteral("spawn_subagent"));
    run.toolsWhitelist.removeAll(QStringLiteral("check_subagent"));
    run.providerId = providerId.isEmpty() ? m_router.activeProviderId() : providerId;
    run.modelName = modelName.isEmpty() ? m_router.activeModelName() : modelName;
    run.createdAtMs = QDateTime::currentMSecsSinceEpoch();

    if (run.providerId.isEmpty() || run.modelName.isEmpty()) {
        qCWarning(verzetaUi) << "Subagent spawn rejected — no provider/model resolvable";
        return {};
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO subagent_runs (id, conversation_id, parent_msg_id, "
                             " requester_alias, task, tools_whitelist, provider_id, "
                             " model_name, status, created_at_ms) "
                             "VALUES (?,?,?,?,?,?,?,?,'queued',?)"));
    q.addBindValue(run.id);
    q.addBindValue(run.conversationId);
    q.addBindValue(nn(run.parentMsgId));
    q.addBindValue(run.requesterAlias);
    q.addBindValue(run.task);
    q.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(run.toolsWhitelist))
                                         .toJson(QJsonDocument::Compact)));
    q.addBindValue(run.providerId);
    q.addBindValue(run.modelName);
    q.addBindValue(run.createdAtMs);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "Subagent spawn persist failed:" << q.lastError().text();
        return {};
    }

    m_queue.enqueue(run.id);
    emit runStatusChanged(run.id, QStringLiteral("queued"));
    qCInfo(verzetaUi) << "Subagent spawned:" << run.id.left(8) << "by @" << run.requesterAlias
                      << "task:" << run.task.left(60);
    pump();
    return run.id;
}

void SubagentRunService::pump() {
    while (m_activeCount < kMaxConcurrentRuns && !m_queue.isEmpty()) {
        const QString runId = m_queue.dequeue();
        auto rOpt = run(runId);
        if (!rOpt.has_value() || rOpt->status != QStringLiteral("queued")) {
            continue;  // Cancelled while queued.
        }
        const SubagentRun r = *rOpt;

        // Per-run provider bundle. The test seam pre-fills m_prov; a
        // seeded bundle is consumed by exactly one run.
        Ragp::ClassifierProvider bundle;
        if (m_prov.valid() && m_prov.providerId == r.providerId) {
            bundle = std::move(m_prov);
            m_prov = Ragp::ClassifierProvider{};
        } else {
            QString whyNot;
            bundle = Ragp::makeClassifierProvider(r.providerId, m_settings, this, &whyNot);
            if (!bundle.valid()) {
                finalizeRun(runId,
                            QStringLiteral("failed"),
                            {},
                            QStringLiteral("provider unavailable: %1").arg(whyNot));
                continue;
            }
        }
        m_runProv.emplace(runId, std::move(bundle));

        RunState st;
        LlmMessage user;
        user.role = QStringLiteral("user");
        user.content = r.task;
        st.messages = {user};
        st.deadlineMs = QDateTime::currentMSecsSinceEpoch() + kRunTimeoutMs;
        m_states.insert(runId, st);

        ++m_activeCount;
        {
            QSqlQuery q(m_db.db());
            q.prepare(QStringLiteral("UPDATE subagent_runs SET status='running', "
                                     "started_at_ms=? WHERE id=?"));
            q.addBindValue(QDateTime::currentMSecsSinceEpoch());
            q.addBindValue(runId);
            q.exec();
        }
        emit runStatusChanged(runId, QStringLiteral("running"));
        appendTranscript(runId, QStringLiteral("system"), buildSystemPrompt(r));
        appendTranscript(runId, QStringLiteral("user"), r.task);
        dispatchTurn(runId);
    }
}

// ---------------------------------------------------------------------------
// The dispatch loop (one async model turn per call)
// ---------------------------------------------------------------------------

QString SubagentRunService::buildSystemPrompt(const SubagentRun& run) const {
    return QStringLiteral("You are a focused sub-agent working for @%1. You have ONE "
                          "task. Complete it using your tools, then produce a final "
                          "concise report as plain text (no tool call) — that report is "
                          "delivered back to @%1 and ends your run.\n"
                          "Task: %2\n"
                          "Rules: stay strictly on-task; use the function-calling "
                          "protocol for every tool use; never narrate an action without "
                          "performing it; your final reply must summarise findings and "
                          "outcomes, including anything you could not do.")
        .arg(run.requesterAlias, run.task);
}

void SubagentRunService::dispatchTurn(const QString& runId) {
    auto stIt = m_states.find(runId);
    auto pvIt = m_runProv.find(runId);
    const auto rOpt = run(runId);
    if (stIt == m_states.end() || pvIt == m_runProv.end() || !rOpt.has_value()) {
        return;
    }
    RunState& st = stIt.value();
    const SubagentRun r = *rOpt;

    if (QDateTime::currentMSecsSinceEpoch() > st.deadlineMs) {
        finalizeRun(runId, QStringLiteral("failed"), {}, QStringLiteral("time limit reached"));
        return;
    }
    if (st.toolRounds >= kMaxToolRounds) {
        finalizeRun(runId, QStringLiteral("failed"), {}, QStringLiteral("tool step limit reached"));
        return;
    }

    LlmRequest req;
    req.conversationId = QStringLiteral("subagent:%1").arg(runId);
    req.systemPrompt = buildSystemPrompt(r);
    req.messages = st.messages;
    req.config.providerId = r.providerId;
    req.config.modelName = r.modelName;
    req.config.temperature = 0.4;
    req.config.maxTokens = -1;
    req.config.stream = true;
    req.config.thinkingMode = false;
    {
        const QList<ToolSchema> all = m_toolSvc.availableTools();
        for (const ToolSchema& schema : all) {
            if (r.toolsWhitelist.contains(schema.name)) {
                req.availableTools.append(schema);
            }
        }
    }

    st.inFlight = true;

    auto completed = std::make_shared<bool>(false);
    auto buffer = std::make_shared<QString>();
    auto toolCalls = std::make_shared<QJsonArray>();
    auto conns = std::make_shared<QList<QMetaObject::Connection>>();
    QPointer<SubagentRunService> selfGuard(this);

    auto finish = [selfGuard, completed, conns, runId](const QString& finishReason,
                                                       const QString& content,
                                                       const QJsonArray& calls,
                                                       const QString& error) {
        if (*completed)
            return;
        *completed = true;
        for (const auto& c : *conns)
            QObject::disconnect(c);
        conns->clear();
        if (!selfGuard)
            return;
        selfGuard->m_states[runId].inFlight = false;

        if (!error.isEmpty()) {
            selfGuard->finalizeRun(runId, QStringLiteral("failed"), {}, error);
            return;
        }
        const auto rNow = selfGuard->run(runId);
        if (!rNow.has_value() || rNow->terminal()) {
            return;  // Cancelled mid-flight; finalize already ran.
        }

        if (finishReason == QStringLiteral("tool_calls") && !calls.isEmpty()) {
            // Record the narration (if any) + execute each whitelisted
            // tool synchronously (ToolService::invokeTool blocks; we
            // are on the main thread but sub-agent tools are the same
            // read-only set the chat path executes — acceptable for
            // V1 and bounded by kMaxToolRounds).
            RunState& st2 = selfGuard->m_states[runId];
            if (!content.trimmed().isEmpty()) {
                selfGuard->appendTranscript(runId, QStringLiteral("assistant"), content);
            }
            LlmMessage asst;
            asst.role = QStringLiteral("assistant");
            asst.content = content;
            asst.toolCallsJson = calls;
            st2.messages.append(asst);

            for (const QJsonValue& cv : calls) {
                const QJsonObject call = cv.toObject();
                const QString name = call.value(QStringLiteral("name")).toString();
                const QJsonObject args = call.value(QStringLiteral("arguments")).toObject();
                const QString callId = call.value(QStringLiteral("id")).toString();
                QJsonValue result;
                if (!rNow->toolsWhitelist.contains(name)) {
                    result = QJsonObject{{QStringLiteral("error"),
                                          QStringLiteral("tool not in whitelist: %1").arg(name)}};
                } else {
                    result = selfGuard->m_toolSvc.invokeTool(
                        name, args, rNow->conversationId, QString(), rNow->requesterAlias);
                }
                const QString resultStr = QString::fromUtf8(
                    QJsonDocument(result.isObject()
                                      ? result.toObject()
                                      : QJsonObject{{QStringLiteral("result"), result}})
                        .toJson(QJsonDocument::Compact));
                selfGuard->appendTranscript(runId, QStringLiteral("tool"), resultStr, name);
                LlmMessage toolMsg;
                toolMsg.role = QStringLiteral("tool");
                toolMsg.content = resultStr;
                toolMsg.toolCallId = callId;
                st2.messages.append(toolMsg);
            }
            ++st2.toolRounds;
            selfGuard->dispatchTurn(runId);  // Next round.
            return;
        }

        // Terminal reply — the run's final report.
        selfGuard->appendTranscript(runId, QStringLiteral("assistant"), content);
        selfGuard->finalizeRun(runId, QStringLiteral("done"), content.trimmed(), {});
    };

    ILLMProvider* provider = pvIt->second.provider.get();
    conns->append(QObject::connect(
        provider, &ILLMProvider::chunkReceived, this, [buffer, toolCalls](const LlmChunk& chunk) {
            if (!chunk.delta.isEmpty())
                buffer->append(chunk.delta);
            if (!chunk.toolCallJson.isEmpty()) {
                toolCalls->append(chunk.toolCallJson);
            }
        }));
    conns->append(QObject::connect(provider,
                                   &ILLMProvider::requestFinished,
                                   this,
                                   [buffer, toolCalls, finish](const QString& reason, int) {
                                       finish(reason, *buffer, *toolCalls, QString());
                                   }));
    conns->append(
        QObject::connect(provider, &ILLMProvider::requestError, this, [finish](const QString& msg) {
            finish(QString(), QString(), QJsonArray(), msg);
        }));

    QTimer::singleShot(kTurnTimeoutMs, nullptr, [finish, completed, selfGuard, runId]() {
        if (*completed)
            return;
        if (selfGuard) {
            auto it = selfGuard->m_runProv.find(runId);
            if (it != selfGuard->m_runProv.end() && it->second.valid()) {
                it->second.provider->cancelRequest();
            }
        }
        finish(QString(),
               QString(),
               QJsonArray(),
               QStringLiteral("the model did not respond in time"));
    });

    provider->sendRequest(req);
}

// ---------------------------------------------------------------------------
// Cancellation + finalization
// ---------------------------------------------------------------------------

bool SubagentRunService::cancel(const QString& runId) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto rOpt = run(runId);
    if (!rOpt.has_value() || rOpt->terminal())
        return false;

    auto it = m_runProv.find(runId);
    if (it != m_runProv.end() && it->second.valid()) {
        it->second.provider->cancelRequest();
    }
    finalizeRun(runId, QStringLiteral("cancelled"), {}, QStringLiteral("cancelled by user"));
    return true;
}

void SubagentRunService::finalizeRun(const QString& runId,
                                     const QString& status,
                                     const QString& resultText,
                                     const QString& failReason) {
    const auto rOpt = run(runId);
    if (!rOpt.has_value() || rOpt->terminal())
        return;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE subagent_runs SET status=?, result_text=?, "
                             "fail_reason=?, finished_at_ms=? WHERE id=?"));
    q.addBindValue(status);
    q.addBindValue(nn(resultText));
    q.addBindValue(nn(failReason));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(runId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "finalizeRun persist failed:" << q.lastError().text();
    }

    const bool wasActive = m_states.remove(runId) > 0;
    m_runProv.erase(runId);
    if (wasActive && m_activeCount > 0)
        --m_activeCount;

    qCInfo(verzetaUi) << "Subagent run" << runId.left(8) << "finished:" << status
                      << (failReason.isEmpty() ? QString()
                                               : QStringLiteral("(%1)").arg(failReason));
    emit runStatusChanged(runId, status);
    emit runFinished(runId, rOpt->conversationId, rOpt->requesterAlias, status, resultText);
    pump();  // A slot freed — start the next queued run.
}

// ---------------------------------------------------------------------------
// Persistence reads + transcript
// ---------------------------------------------------------------------------

std::optional<SubagentRun> SubagentRunService::run(const QString& runId) const {
    if (runId.isEmpty())
        return std::nullopt;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT conversation_id, parent_msg_id, requester_alias, task, "
                             " tools_whitelist, provider_id, model_name, status, "
                             " result_text, fail_reason, total_tokens, created_at_ms, "
                             " started_at_ms, finished_at_ms "
                             "FROM subagent_runs WHERE id=?"));
    q.addBindValue(runId);
    if (!q.exec() || !q.next())
        return std::nullopt;

    SubagentRun r;
    r.id = runId;
    r.conversationId = q.value(0).toString();
    r.parentMsgId = q.value(1).toString();
    r.requesterAlias = q.value(2).toString();
    r.task = q.value(3).toString();
    {
        const QJsonDocument d = QJsonDocument::fromJson(q.value(4).toString().toUtf8());
        for (const QJsonValue& v : d.array()) {
            r.toolsWhitelist.append(v.toString());
        }
    }
    r.providerId = q.value(5).toString();
    r.modelName = q.value(6).toString();
    r.status = q.value(7).toString();
    r.resultText = q.value(8).toString();
    r.failReason = q.value(9).toString();
    r.totalTokens = q.value(10).toInt();
    r.createdAtMs = q.value(11).toLongLong();
    r.startedAtMs = q.value(12).toLongLong();
    r.finishedAtMs = q.value(13).toLongLong();
    return r;
}

QList<SubagentRun> SubagentRunService::runsForConversation(const QString& conversationId) const {
    QList<SubagentRun> out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT id FROM subagent_runs WHERE conversation_id=? "
                             "ORDER BY created_at_ms DESC"));
    q.addBindValue(conversationId);
    if (!q.exec())
        return out;
    while (q.next()) {
        const auto r = run(q.value(0).toString());
        if (r.has_value())
            out.append(*r);
    }
    return out;
}

QJsonArray SubagentRunService::transcript(const QString& runId) const {
    QJsonArray out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT role, content, tool_name FROM subagent_messages "
                             "WHERE run_id=? ORDER BY seq ASC"));
    q.addBindValue(runId);
    if (!q.exec())
        return out;
    while (q.next()) {
        out.append(QJsonObject{
            {QStringLiteral("role"), q.value(0).toString()},
            {QStringLiteral("content"), q.value(1).toString()},
            {QStringLiteral("tool_name"), q.value(2).toString()},
        });
    }
    return out;
}

void SubagentRunService::appendTranscript(const QString& runId,
                                          const QString& role,
                                          const QString& content,
                                          const QString& toolName) {
    QSqlQuery seqQ(m_db.db());
    seqQ.prepare(QStringLiteral("SELECT COALESCE(MAX(seq), 0) + 1 FROM subagent_messages "
                                "WHERE run_id=?"));
    seqQ.addBindValue(runId);
    int seq = 1;
    if (seqQ.exec() && seqQ.next())
        seq = seqQ.value(0).toInt();

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO subagent_messages (id, run_id, seq, role, "
                             " content, tool_name, created_at_ms) VALUES (?,?,?,?,?,?,?)"));
    q.addBindValue(freshId());
    q.addBindValue(runId);
    q.addBindValue(seq);
    q.addBindValue(role);
    q.addBindValue(nn(content));
    q.addBindValue(nn(toolName));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qCWarning(verzetaDb) << "appendTranscript failed:" << q.lastError().text();
    }
}

void SubagentRunService::setStatus(const QString& runId,
                                   const QString& status,
                                   const QString& failReason) {
    Q_UNUSED(failReason);
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE subagent_runs SET status=? WHERE id=?"));
    q.addBindValue(status);
    q.addBindValue(runId);
    q.exec();
    emit runStatusChanged(runId, status);
}

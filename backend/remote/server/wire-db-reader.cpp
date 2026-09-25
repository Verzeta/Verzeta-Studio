// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-db-reader.cpp
 * @brief Implementation of the verzeta-remote-side read-only SQLite
 *        connection to the host's main database.
 *
 *        Every method here is a pure SELECT (or a file read of a
 *        per-row data_path / content). The connection is opened with
 *        QSqlDatabase::open() AFTER setting `?mode=ro` on the
 *        database name URI form, so even a malicious payload cannot
 *        cause a write through this connection. WAL is used by the
 *        host, and the wire reader reads through it with shared
 *        cache off (Qt's QSQLITE driver default: separate connection
 *        cache, separate page cache, no synchronisation needed).
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::Sql.
 */

#include "wire-db-reader.h"

#include "../wire-protocol.h"

#include <QTimeZone>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(wireDb, "verzeta.remote.dbreader", QtInfoMsg)

namespace Verzeta::Remote {

namespace {

/** Convert ms-since-epoch to ISO 8601 UTC. The host stores
 *  conversations.created_at / messages.created_at as INTEGER ms;
 *  Android's formatRelativeTimestamp parses ISO 8601, so emit ISO. */
QString isoFromMs(qint64 ms) {
    if (ms <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

/** Best-effort JSON parse. Returns Null on parse failure or empty
 *  string so the caller can pass it straight into a JSON object
 *  without conditionals. */
QJsonValue parseJsonOrNull(const QString& s) {
    if (s.isEmpty())
        return QJsonValue::Null;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError)
        return s;  // raw fallback
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return doc.array();
    return s;
}

/** Read llm_config JSON column and project it onto the per-conversation
 *  settings shape the wire layer uses.
 *
 *  The JSON column is written by `LlmConfig::toJson` in snake_case
 *  (`max_tokens`, `provider_id`, etc.). This function used to read
 *  camelCase keys, so every field except `temperature`, which matches
 *  by coincidence because the name is the same in both cases, was
 *  silently dropped from the wire response. Wire clients saw an empty
 *  provider and model, and default settings, for every conversation.
 *
 *  Output keys remain camelCase to match Android's `parseConvSettings`
 *  in `RemoteRepository.kt::parseConvSettings`. The mismatch was
 *  ONLY on input.
 *
 *  The per-conversation fields (`agent_pattern`, `tools_enabled`,
 *  `require_confirmation`) are projected onto camelCase output too, so
 *  the wire response carries per-conversation values. The wire-session
 *  layer reads agentPattern and requireConfirmation from here instead
 *  of its single-value cache.
 */
QJsonObject parseLlmConfig(const QString& json) {
    QJsonObject out;
    if (json.isEmpty())
        return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return out;
    const QJsonObject cfg = doc.object();
    if (cfg.contains(QStringLiteral("temperature")))
        out.insert(QStringLiteral("temperature"), cfg.value(QStringLiteral("temperature")));
    if (cfg.contains(QStringLiteral("max_tokens")))
        out.insert(QStringLiteral("maxTokens"), cfg.value(QStringLiteral("max_tokens")));
    if (cfg.contains(QStringLiteral("context_window")))
        out.insert(QStringLiteral("contextWindow"), cfg.value(QStringLiteral("context_window")));
    if (cfg.contains(QStringLiteral("stream")))
        out.insert(QStringLiteral("streaming"), cfg.value(QStringLiteral("stream")));
    if (cfg.contains(QStringLiteral("thinking_mode")))
        out.insert(QStringLiteral("thinking"), cfg.value(QStringLiteral("thinking_mode")));
    if (cfg.contains(QStringLiteral("provider_id")))
        out.insert(QStringLiteral("providerId"), cfg.value(QStringLiteral("provider_id")));
    if (cfg.contains(QStringLiteral("model_name")))
        out.insert(QStringLiteral("modelName"), cfg.value(QStringLiteral("model_name")));
    if (cfg.contains(QStringLiteral("agent_pattern")))
        out.insert(QStringLiteral("agentPattern"), cfg.value(QStringLiteral("agent_pattern")));
    if (cfg.contains(QStringLiteral("tools_enabled")))
        out.insert(QStringLiteral("toolsEnabled"), cfg.value(QStringLiteral("tools_enabled")));
    if (cfg.contains(QStringLiteral("require_confirmation")))
        out.insert(QStringLiteral("requireConfirmation"),
                   cfg.value(QStringLiteral("require_confirmation")));
    if (cfg.contains(QStringLiteral("top_k")))
        out.insert(QStringLiteral("topK"), cfg.value(QStringLiteral("top_k")));
    if (cfg.contains(QStringLiteral("top_p")))
        out.insert(QStringLiteral("topP"), cfg.value(QStringLiteral("top_p")));
    if (cfg.contains(QStringLiteral("repeat_penalty")))
        out.insert(QStringLiteral("repeatPenalty"), cfg.value(QStringLiteral("repeat_penalty")));
    if (cfg.contains(QStringLiteral("presence_penalty")))
        out.insert(QStringLiteral("presencePenalty"),
                   cfg.value(QStringLiteral("presence_penalty")));
    if (cfg.contains(QStringLiteral("frequency_penalty")))
        out.insert(QStringLiteral("frequencyPenalty"),
                   cfg.value(QStringLiteral("frequency_penalty")));
    if (cfg.contains(QStringLiteral("force_app_sampling")))
        out.insert(QStringLiteral("forceAppSampling"),
                   cfg.value(QStringLiteral("force_app_sampling")));
    if (cfg.contains(QStringLiteral("tools_in_system_prompt")))
        out.insert(QStringLiteral("toolsInSystemPrompt"),
                   cfg.value(QStringLiteral("tools_in_system_prompt")));
    if (cfg.contains(QStringLiteral("dynamic_compact_enabled")))
        out.insert(QStringLiteral("dynamicCompactEnabled"),
                   cfg.value(QStringLiteral("dynamic_compact_enabled")));
    if (cfg.contains(QStringLiteral("implicit_task_completion")))
        out.insert(QStringLiteral("implicitTaskCompletion"),
                   cfg.value(QStringLiteral("implicit_task_completion")));
    if (cfg.contains(QStringLiteral("compact_every_turns")))
        out.insert(QStringLiteral("compactEveryTurns"),
                   cfg.value(QStringLiteral("compact_every_turns")));
    if (cfg.contains(QStringLiteral("rag_enabled")))
        out.insert(QStringLiteral("ragEnabled"), cfg.value(QStringLiteral("rag_enabled")));
    if (cfg.contains(QStringLiteral("aim_enabled")))
        out.insert(QStringLiteral("aimEnabled"), cfg.value(QStringLiteral("aim_enabled")));
    if (cfg.contains(QStringLiteral("acn_enabled")))
        out.insert(QStringLiteral("acnEnabled"), cfg.value(QStringLiteral("acn_enabled")));
    if (cfg.contains(QStringLiteral("max_auto_rounds")))
        out.insert(QStringLiteral("maxAutoRounds"), cfg.value(QStringLiteral("max_auto_rounds")));
    return out;
}

}  // namespace

WireDbReader::WireDbReader(QObject* parent) : QObject(parent) {
    m_connectionName =
        QStringLiteral("verzeta-remote-reader-%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
}

WireDbReader::~WireDbReader() {
    close();
}

bool WireDbReader::open() {
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dbPath = dataDir + QStringLiteral("/verzeta-studio.db");
    if (!QFileInfo::exists(dbPath)) {
        qCWarning(wireDb) << "WireDbReader::open: host db not found at" << dbPath
                          << "(host not started yet — SQL ops will return 'host_offline'"
                             " until the file appears)";
        return false;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    // QSQLITE accepts "QSQLITE_OPEN_READONLY" via setConnectOptions for
    // strict read-only mode. WAL allows concurrent readers without
    // blocking the host writer.
    m_db.setDatabaseName(dbPath);
    m_db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!m_db.open()) {
        qCCritical(wireDb) << "WireDbReader::open: cannot open" << dbPath << ":"
                           << m_db.lastError().text();
        return false;
    }
    qCInfo(wireDb) << "WireDbReader::open: read-only on" << dbPath;
    return true;
}

void WireDbReader::close() {
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

// ---------------------------------------------------------------------------
// Conversation reads
// ---------------------------------------------------------------------------

QJsonArray WireDbReader::listConversations() {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT id, title, folder_id, created_at, updated_at, "
                               "       system_prompt, token_total, primary_agent_id, "
                               "       is_group, group_agent_ids, is_pinned, member_alias "
                               "FROM conversations "
                               "ORDER BY updated_at DESC"))) {
        qCWarning(wireDb) << "listConversations:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(QJsonObject{
            {QStringLiteral("id"), q.value(0).toString()},
            {QStringLiteral("title"), q.value(1).toString()},
            {QStringLiteral("folder_id"), q.value(2).toString()},
            {QStringLiteral("created_at"), isoFromMs(q.value(3).toLongLong())},
            {QStringLiteral("updated_at"), isoFromMs(q.value(4).toLongLong())},
            {QStringLiteral("system_prompt"), q.value(5).toString()},
            {QStringLiteral("token_total"), q.value(6).toLongLong()},
            {QStringLiteral("primary_agent_id"), q.value(7).toString()},
            {QStringLiteral("is_group"), q.value(8).toBool()},
            {QStringLiteral("group_agent_ids"), parseJsonOrNull(q.value(9).toString())},
            {QStringLiteral("is_pinned"), q.value(10).toBool()},
            {QStringLiteral("member_alias"), q.value(11).toString()},
        });
    }
    return out;
}

QJsonObject WireDbReader::conversationById(const QString& id) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, title, folder_id, created_at, updated_at, "
                             "       system_prompt, llm_config, token_total, "
                             "       primary_agent_id, is_group, group_agent_ids, is_pinned, "
                             "       member_alias "
                             "FROM conversations WHERE id=? LIMIT 1"));
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return out;
    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toString()},
        {QStringLiteral("title"), q.value(1).toString()},
        {QStringLiteral("folder_id"), q.value(2).toString()},
        {QStringLiteral("created_at"), q.value(3).toLongLong()},
        {QStringLiteral("updated_at"), q.value(4).toLongLong()},
        {QStringLiteral("system_prompt"), q.value(5).toString()},
        {QStringLiteral("llm_config"), parseJsonOrNull(q.value(6).toString())},
        {QStringLiteral("token_total"), q.value(7).toLongLong()},
        {QStringLiteral("primary_agent_id"), q.value(8).toString()},
        {QStringLiteral("is_group"), q.value(9).toBool()},
        {QStringLiteral("group_agent_ids"), parseJsonOrNull(q.value(10).toString())},
        {QStringLiteral("is_pinned"), q.value(11).toBool()},
        {QStringLiteral("member_alias"), q.value(12).toString()},
    };
}

QJsonObject WireDbReader::messageById(const QString& msgId) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    // `thinking_content` is the schema-v16 sidecar column carrying any
    // captured reasoning for an assistant row.  Surfacing it on the wire
    // here lets paired Android clients render the same render-only
    // disclosure the desktop bubble provides — additive payload field,
    // older Android clients silently drop it via
    // `RemoteJson { ignoreUnknownKeys = true }`.  Display-only by
    // contract: no inbound wire op accepts the field, so it never
    // round-trips into a subsequent prompt.
    q.prepare(QStringLiteral("SELECT id, conversation_id, role, content, created_at, "
                             "       finish_reason, agent_id, member_alias, turn_id, "
                             "       token_count, model_used, thinking_content "
                             "FROM messages WHERE id=? LIMIT 1"));
    q.addBindValue(msgId);
    if (!q.exec() || !q.next())
        return out;
    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toString()},
        {QStringLiteral("conversation_id"), q.value(1).toString()},
        {QStringLiteral("role"), q.value(2).toString()},
        {QStringLiteral("content"), q.value(3).toString()},
        {QStringLiteral("created_at"), isoFromMs(q.value(4).toLongLong())},
        {QStringLiteral("finish_reason"), q.value(5).toString()},
        {QStringLiteral("agent_id"), q.value(6).toString()},
        {QStringLiteral("member_alias"), q.value(7).toString()},
        {QStringLiteral("turn_id"), q.value(8).toString()},
        {QStringLiteral("token_count"), q.value(9).toInt()},
        {QStringLiteral("model_used"), q.value(10).toString()},
        {QStringLiteral("thinking_content"), q.value(11).toString()},
    };
}

QJsonArray WireDbReader::messagesForConversation(const QString& convId) {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    // `thinking_content` added per the same contract documented on
    // `messageById` above — additive wire field, never round-trips.
    q.prepare(QStringLiteral("SELECT id, conversation_id, role, content, created_at, "
                             "       finish_reason, agent_id, member_alias, turn_id, "
                             "       token_count, model_used, thinking_content, metadata "
                             "FROM messages WHERE conversation_id=? ORDER BY created_at ASC"));
    q.addBindValue(convId);
    if (!q.exec()) {
        qCWarning(wireDb) << "messagesForConversation:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        const QJsonObject metadata = parseJsonOrNull(q.value(12).toString()).toObject();
        out.append(QJsonObject{
            {QStringLiteral("id"), q.value(0).toString()},
            {QStringLiteral("conversation_id"), q.value(1).toString()},
            {QStringLiteral("role"), q.value(2).toString()},
            {QStringLiteral("content"), q.value(3).toString()},
            {QStringLiteral("created_at"), isoFromMs(q.value(4).toLongLong())},
            {QStringLiteral("finish_reason"), q.value(5).toString()},
            {QStringLiteral("agent_id"), q.value(6).toString()},
            {QStringLiteral("member_alias"), q.value(7).toString()},
            {QStringLiteral("turn_id"), q.value(8).toString()},
            {QStringLiteral("token_count"), q.value(9).toInt()},
            {QStringLiteral("model_used"), q.value(10).toString()},
            {QStringLiteral("thinking_content"), q.value(11).toString()},
            {QStringLiteral("metadata"), metadata},
        });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Folder reads
// ---------------------------------------------------------------------------

QJsonArray WireDbReader::listAllFolders() {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT id, name, parent_id, folder_type, goal, description, "
                               "       agent_ids, created_at "
                               "FROM folders ORDER BY name ASC"))) {
        qCWarning(wireDb) << "listAllFolders:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(QJsonObject{
            {QStringLiteral("id"), q.value(0).toString()},
            {QStringLiteral("name"), q.value(1).toString()},
            {QStringLiteral("parent_id"), q.value(2).toString()},
            {QStringLiteral("folder_type"), q.value(3).toString()},
            {QStringLiteral("goal"), q.value(4).toString()},
            {QStringLiteral("description"), q.value(5).toString()},
            {QStringLiteral("agent_ids"), parseJsonOrNull(q.value(6).toString())},
            {QStringLiteral("created_at"), isoFromMs(q.value(7).toLongLong())},
        });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Per-conversation settings
// ---------------------------------------------------------------------------

QJsonObject WireDbReader::conversationSettings(const QString& convId) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT system_prompt, llm_config, primary_agent_id, "
                             "       is_group, folder_id "
                             "FROM conversations WHERE id=? LIMIT 1"));
    q.addBindValue(convId);
    if (!q.exec() || !q.next())
        return out;

    // camelCase field names match Android's parseConvSettings.
    out.insert(QStringLiteral("systemPrompt"), q.value(0).toString());
    out.insert(QStringLiteral("primaryAgentId"), q.value(2).toString());
    out.insert(QStringLiteral("isGroup"), q.value(3).toBool());
    out.insert(QStringLiteral("folderId"), q.value(4).toString());
    const QJsonObject llm = parseLlmConfig(q.value(1).toString());
    for (auto it = llm.begin(); it != llm.end(); ++it) {
        out.insert(it.key(), it.value());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Plans / tasks
// ---------------------------------------------------------------------------

namespace {

QJsonArray fetchStepsForPlan(QSqlDatabase& db, const QString& planId) {
    QJsonArray steps;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT id, plan_id, ordering, title, description, owner_alias, "
                             "       acceptance_criteria, status, rejection_count, "
                             "       tool_retry_count, executor_turns_used, "
                             "       last_rejection_reason, created_at, updated_at "
                             "FROM plan_steps WHERE plan_id=? ORDER BY ordering ASC"));
    q.addBindValue(planId);
    if (!q.exec())
        return steps;
    while (q.next()) {
        steps.append(QJsonObject{
            {QStringLiteral("id"), q.value(0).toString()},
            {QStringLiteral("plan_id"), q.value(1).toString()},
            {QStringLiteral("ordering"), q.value(2).toInt()},
            {QStringLiteral("title"), q.value(3).toString()},
            {QStringLiteral("description"), q.value(4).toString()},
            {QStringLiteral("owner_alias"), q.value(5).toString()},
            {QStringLiteral("acceptance_criteria"), q.value(6).toString()},
            {QStringLiteral("status"), q.value(7).toString()},
            {QStringLiteral("rejection_count"), q.value(8).toInt()},
            {QStringLiteral("tool_retry_count"), q.value(9).toInt()},
            {QStringLiteral("executor_turns_used"), q.value(10).toInt()},
            {QStringLiteral("last_rejection_reason"), q.value(11).toString()},
            {QStringLiteral("created_at"), q.value(12).toLongLong()},
            {QStringLiteral("updated_at"), q.value(13).toLongLong()},
        });
    }
    return steps;
}

QJsonObject fetchPlanRow(QSqlQuery& q) {
    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toString()},
        {QStringLiteral("conversation_id"), q.value(1).toString()},
        {QStringLiteral("project_folder_id"), q.value(2).toString()},
        {QStringLiteral("organization_folder_id"), q.value(3).toString()},
        {QStringLiteral("goal"), q.value(4).toString()},
        {QStringLiteral("status"), q.value(5).toString()},
        {QStringLiteral("started_by"), q.value(6).toString()},
        {QStringLiteral("created_at"), q.value(7).toLongLong()},
        {QStringLiteral("updated_at"), q.value(8).toLongLong()},
        {QStringLiteral("turns_used"), q.value(9).toInt()},
        {QStringLiteral("heartbeats_used"), q.value(10).toInt()},
    };
}

}  // namespace

QJsonArray WireDbReader::plansForConversation(const QString& convId) {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(
        QStringLiteral("SELECT id, conversation_id, project_folder_id, organization_folder_id, "
                       "       goal, status, started_by, created_at, updated_at, "
                       "       turns_used, heartbeats_used "
                       "FROM agent_plans WHERE conversation_id=? "
                       "ORDER BY created_at DESC"));
    q.addBindValue(convId);
    if (!q.exec()) {
        qCWarning(wireDb) << "plansForConversation:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        QJsonObject plan = fetchPlanRow(q);
        plan.insert(QStringLiteral("steps"), fetchStepsForPlan(m_db, q.value(0).toString()));
        out.append(plan);
    }
    return out;
}

QString WireDbReader::planIdForStep(const QString& stepId) {
    if (!m_db.isOpen())
        return {};
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT plan_id FROM plan_steps WHERE id=? LIMIT 1"));
    q.addBindValue(stepId);
    if (!q.exec() || !q.next())
        return {};
    return q.value(0).toString();
}

QJsonObject WireDbReader::planById(const QString& planId) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(
        QStringLiteral("SELECT id, conversation_id, project_folder_id, organization_folder_id, "
                       "       goal, status, started_by, created_at, updated_at, "
                       "       turns_used, heartbeats_used "
                       "FROM agent_plans WHERE id=? LIMIT 1"));
    q.addBindValue(planId);
    if (!q.exec() || !q.next())
        return out;
    out = fetchPlanRow(q);
    out.insert(QStringLiteral("steps"), fetchStepsForPlan(m_db, planId));
    return out;
}

// ---------------------------------------------------------------------------
// Tool-call activity
// ---------------------------------------------------------------------------

namespace {

QJsonObject toolCallRow(QSqlQuery& q) {
    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toString()},
        {QStringLiteral("message_id"), q.value(1).toString()},
        {QStringLiteral("tool_name"), q.value(2).toString()},
        {QStringLiteral("arguments"), parseJsonOrNull(q.value(3).toString())},
        {QStringLiteral("result"), parseJsonOrNull(q.value(4).toString())},
        {QStringLiteral("status"), q.value(5).toString()},
        {QStringLiteral("started_at"), q.value(6).toLongLong()},
        {QStringLiteral("completed_at"), q.value(7).toLongLong()},
        {QStringLiteral("plan_step_id"), q.value(8).toString()},
        {QStringLiteral("conversation_id"), q.value(9).toString()},
    };
}

}  // namespace

QJsonArray WireDbReader::toolCallsForConversation(const QString& convId) {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT tc.id, tc.message_id, tc.tool_name, tc.arguments, tc.result, "
                             "       tc.status, tc.started_at, tc.completed_at, tc.plan_step_id, "
                             "       m.conversation_id "
                             "FROM tool_calls tc "
                             "JOIN messages m ON m.id = tc.message_id "
                             "WHERE m.conversation_id=? "
                             "ORDER BY m.created_at ASC, tc.started_at ASC"));
    q.addBindValue(convId);
    if (!q.exec()) {
        qCWarning(wireDb) << "toolCallsForConversation:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(toolCallRow(q));
    return out;
}

QJsonArray WireDbReader::toolCallsForMessage(const QString& messageId) {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT tc.id, tc.message_id, tc.tool_name, tc.arguments, tc.result, "
                             "       tc.status, tc.started_at, tc.completed_at, tc.plan_step_id, "
                             "       m.conversation_id "
                             "FROM tool_calls tc "
                             "JOIN messages m ON m.id = tc.message_id "
                             "WHERE tc.message_id=? "
                             "ORDER BY tc.started_at ASC"));
    q.addBindValue(messageId);
    if (!q.exec()) {
        qCWarning(wireDb) << "toolCallsForMessage:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(toolCallRow(q));
    return out;
}

QJsonArray WireDbReader::toolCallsForTurn(const QString& turnMsgId, const QString& agentId) {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    if (agentId.isEmpty()) {
        q.prepare(
            QStringLiteral("SELECT tc.id, tc.message_id, tc.tool_name, tc.arguments, tc.result, "
                           "       tc.status, tc.started_at, tc.completed_at, tc.plan_step_id, "
                           "       m.conversation_id "
                           "FROM tool_calls tc "
                           "JOIN messages m ON m.id = tc.message_id "
                           "WHERE m.turn_id=? "
                           "ORDER BY m.created_at ASC, tc.started_at ASC"));
        q.addBindValue(turnMsgId);
    } else {
        q.prepare(
            QStringLiteral("SELECT tc.id, tc.message_id, tc.tool_name, tc.arguments, tc.result, "
                           "       tc.status, tc.started_at, tc.completed_at, tc.plan_step_id, "
                           "       m.conversation_id "
                           "FROM tool_calls tc "
                           "JOIN messages m ON m.id = tc.message_id "
                           "WHERE m.turn_id=? AND m.agent_id=? "
                           "ORDER BY m.created_at ASC, tc.started_at ASC"));
        q.addBindValue(turnMsgId);
        q.addBindValue(agentId);
    }
    if (!q.exec()) {
        qCWarning(wireDb) << "toolCallsForTurn:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(toolCallRow(q));
    return out;
}

QJsonObject WireDbReader::toolCallById(const QString& callId) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT tc.id, tc.message_id, tc.tool_name, tc.arguments, tc.result, "
                             "       tc.status, tc.started_at, tc.completed_at, tc.plan_step_id, "
                             "       m.conversation_id "
                             "FROM tool_calls tc "
                             "JOIN messages m ON m.id = tc.message_id "
                             "WHERE tc.id=? LIMIT 1"));
    q.addBindValue(callId);
    if (!q.exec() || !q.next())
        return out;
    return toolCallRow(q);
}

// ---------------------------------------------------------------------------
// Attachments
// ---------------------------------------------------------------------------

QJsonArray WireDbReader::attachmentsForMessage(const QString& messageId) {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, message_id, type, filename, mime_type, data_path, "
                             "       LENGTH(COALESCE(data_inline, '')) AS inline_size, "
                             "       created_at "
                             "FROM attachments WHERE message_id=? ORDER BY created_at ASC"));
    q.addBindValue(messageId);
    if (!q.exec()) {
        qCWarning(wireDb) << "attachmentsForMessage:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        const QString dataPath = q.value(5).toString();
        const qint64 inlineSize = q.value(6).toLongLong();
        qint64 size = inlineSize;
        if (size == 0 && !dataPath.isEmpty()) {
            QFileInfo fi(dataPath);
            size = fi.exists() ? fi.size() : 0;
        }
        out.append(QJsonObject{
            {QStringLiteral("id"), q.value(0).toString()},
            {QStringLiteral("message_id"), q.value(1).toString()},
            {QStringLiteral("type"), q.value(2).toString()},
            {QStringLiteral("file_name"), q.value(3).toString()},
            {QStringLiteral("mime_type"), q.value(4).toString()},
            {QStringLiteral("has_inline_data"), inlineSize > 0},
            {QStringLiteral("has_path"), !dataPath.isEmpty()},
            {QStringLiteral("total_bytes"), size},
            {QStringLiteral("created_at"), q.value(7).toLongLong()},
        });
    }
    return out;
}

QJsonObject WireDbReader::attachmentBytes(const QString& attachmentId) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, filename, mime_type, data_path, data_inline "
                             "FROM attachments WHERE id=? LIMIT 1"));
    q.addBindValue(attachmentId);
    if (!q.exec() || !q.next())
        return out;

    const QString filename = q.value(1).toString();
    const QString mime = q.value(2).toString();
    const QString dataPath = q.value(3).toString();
    const QByteArray inlineBytes = q.value(4).toByteArray();

    QByteArray bytes;
    if (!inlineBytes.isEmpty()) {
        bytes = inlineBytes;
    } else if (!dataPath.isEmpty()) {
        QFile f(dataPath);
        if (f.open(QIODevice::ReadOnly)) {
            // Refuse oversized payloads. One content cap governs every
            // decoded payload on the wire (kMaxContentBytes, sized so the
            // base64-encoded frame stays under kMaxWireFrameBytes); see
            // wire-protocol.h.
            if (f.size() > kMaxContentBytes) {
                qCWarning(wireDb) << "attachmentBytes: refusing oversized file" << f.size() << ">"
                                  << kMaxContentBytes;
                return out;
            }
            bytes = f.readAll();
        }
    }
    if (bytes.isEmpty())
        return out;

    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toString()},
        {QStringLiteral("file_name"), filename},
        {QStringLiteral("mime_type"), mime},
        {QStringLiteral("total_bytes"), bytes.size()},
        {QStringLiteral("truncated"), false},
        {QStringLiteral("content_base64"), QString::fromLatin1(bytes.toBase64())},
    };
}

// ---------------------------------------------------------------------------
// Artifacts (canvas_artifacts)
// ---------------------------------------------------------------------------

QJsonArray WireDbReader::artifactsForConversation(const QString& convId) {
    QJsonArray out;
    if (!m_db.isOpen())
        return out;

    // -- Sources 1 + 2: messages-derived artifacts ---------------------
    {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("SELECT id, role, content, finish_reason, metadata, created_at "
                                 "FROM messages "
                                 "WHERE conversation_id=? "
                                 "  AND ( role = 'tool' "
                                 "        OR (role = 'assistant' AND finish_reason = 'artifact') )"
                                 "ORDER BY created_at ASC"));
        q.addBindValue(convId);
        if (q.exec()) {
            while (q.next()) {
                const QString msgId = q.value(0).toString();
                const QString role = q.value(1).toString();
                const QString content = q.value(2).toString();
                const QString finish = q.value(3).toString();
                const QJsonObject metadata = parseJsonOrNull(q.value(4).toString()).toObject();
                const qint64 createdAtMs = q.value(5).toLongLong();
                const QString createdIso = isoFromMs(createdAtMs);

                if (role == QStringLiteral("tool")) {
                    QJsonParseError err;
                    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8(), &err);
                    if (err.error != QJsonParseError::NoError)
                        continue;
                    const QJsonObject obj = doc.object();
                    const QString path = obj.value(QStringLiteral("path")).toString();
                    if (path.isEmpty())
                        continue;
                    const QString fileName = QFileInfo(path).fileName();
                    out.append(QJsonObject{
                        {QStringLiteral("source_msg_id"), msgId},
                        {QStringLiteral("conversation_id"), convId},
                        {QStringLiteral("path"), path},
                        {QStringLiteral("file_name"), fileName},
                        {QStringLiteral("tool_name"),
                         metadata.value(QStringLiteral("tool_name")).toString()},
                        {QStringLiteral("plan_id"), QString()},
                        {QStringLiteral("step_id"), QString()},
                        {QStringLiteral("step_title"), QString()},
                        {QStringLiteral("plan_goal"), QString()},
                        {QStringLiteral("submitted_by"), QString()},
                        {QStringLiteral("created_at"), createdIso},
                        {QStringLiteral("updated_at"), createdIso},
                    });
                } else {
                    // role='assistant' + finish_reason='artifact'
                    Q_UNUSED(finish);
                    const QString summary = metadata.value(QStringLiteral("summary")).toString();
                    out.append(QJsonObject{
                        {QStringLiteral("source_msg_id"), msgId},
                        {QStringLiteral("conversation_id"), convId},
                        {QStringLiteral("path"), QString()},
                        {QStringLiteral("file_name"), summary},
                        {QStringLiteral("tool_name"), QStringLiteral("submit_result")},
                        {QStringLiteral("plan_id"),
                         metadata.value(QStringLiteral("plan_id")).toString()},
                        {QStringLiteral("step_id"),
                         metadata.value(QStringLiteral("step_id")).toString()},
                        {QStringLiteral("step_title"),
                         metadata.value(QStringLiteral("step_title")).toString()},
                        {QStringLiteral("plan_goal"),
                         metadata.value(QStringLiteral("plan_goal")).toString()},
                        {QStringLiteral("submitted_by"),
                         metadata.value(QStringLiteral("submitted_by_alias")).toString()},
                        {QStringLiteral("created_at"), createdIso},
                        {QStringLiteral("updated_at"), createdIso},
                    });
                }
            }
        }
    }

    // -- Source 3: canvas_artifacts table (content lives inline) -----
    {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("SELECT id, conversation_id, filename, language, revision, "
                                 "       is_archived, source_msg_id, created_at, updated_at, "
                                 "       LENGTH(content) AS content_size "
                                 "FROM canvas_artifacts WHERE conversation_id=? "
                                 "ORDER BY updated_at DESC"));
        q.addBindValue(convId);
        if (q.exec()) {
            while (q.next()) {
                out.append(QJsonObject{
                    {QStringLiteral("id"), q.value(0).toString()},
                    {QStringLiteral("conversation_id"), q.value(1).toString()},
                    {QStringLiteral("file_name"), q.value(2).toString()},
                    {QStringLiteral("language"), q.value(3).toString()},
                    {QStringLiteral("revision"), q.value(4).toInt()},
                    {QStringLiteral("is_archived"), q.value(5).toBool()},
                    {QStringLiteral("source_msg_id"), q.value(6).toString()},
                    {QStringLiteral("path"), QString()},
                    {QStringLiteral("tool_name"), QString()},
                    {QStringLiteral("plan_id"), QString()},
                    {QStringLiteral("step_id"), QString()},
                    {QStringLiteral("step_title"), QString()},
                    {QStringLiteral("plan_goal"), QString()},
                    {QStringLiteral("submitted_by"), QString()},
                    {QStringLiteral("created_at"), q.value(7).toString()},
                    {QStringLiteral("updated_at"), q.value(8).toString()},
                    {QStringLiteral("content_size"), q.value(9).toLongLong()},
                });
            }
        }
    }
    return out;
}

QJsonObject WireDbReader::canvasArtifactById(const QString& canvasId) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    // SELECT mirrors `CanvasService::activeCanvasFor` so the column
    // order + casts stay aligned with the host's QVariantMap projection
    // that Android already knows how to parse via parseCanvasObject.
    q.prepare(QStringLiteral("SELECT id, conversation_id, filename, language, content, "
                             "       revision, is_archived, source_msg_id, created_at, updated_at "
                             "FROM canvas_artifacts WHERE id=? LIMIT 1"));
    q.addBindValue(canvasId);
    if (!q.exec() || !q.next())
        return out;

    const QString content = q.value(4).toString();
    const int lineCount = content.isEmpty() ? 0 : (content.count(QLatin1Char('\n')) + 1);
    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toString()},
        {QStringLiteral("conversationId"), q.value(1).toString()},
        {QStringLiteral("filename"), q.value(2).toString()},
        {QStringLiteral("language"), q.value(3).toString()},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), q.value(5).toInt()},
        {QStringLiteral("isArchived"), q.value(6).toInt() != 0},
        {QStringLiteral("sourceMsgId"), q.value(7).toString()},
        {QStringLiteral("createdAt"), q.value(8).toString()},
        {QStringLiteral("updatedAt"), q.value(9).toString()},
        {QStringLiteral("byteSize"), content.size()},
        {QStringLiteral("lineCount"), lineCount},
    };
}

QJsonObject WireDbReader::artifactBytes(const QString& convId, const QString& filename) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, filename, language, content "
                             "FROM canvas_artifacts "
                             "WHERE conversation_id=? AND filename=? AND is_archived=0 "
                             "ORDER BY updated_at DESC LIMIT 1"));
    q.addBindValue(convId);
    q.addBindValue(filename);
    if (!q.exec() || !q.next()) {
        // Fall back to archived rows if no live row exists.
        QSqlQuery q2(m_db);
        q2.prepare(QStringLiteral("SELECT id, filename, language, content "
                                  "FROM canvas_artifacts "
                                  "WHERE conversation_id=? AND filename=? "
                                  "ORDER BY updated_at DESC LIMIT 1"));
        q2.addBindValue(convId);
        q2.addBindValue(filename);
        if (!q2.exec() || !q2.next())
            return out;
        const QByteArray bytes = q2.value(3).toString().toUtf8();
        return QJsonObject{
            {QStringLiteral("id"), q2.value(0).toString()},
            {QStringLiteral("file_name"), q2.value(1).toString()},
            {QStringLiteral("mime_type"), QStringLiteral("text/plain")},
            {QStringLiteral("language"), q2.value(2).toString()},
            {QStringLiteral("total_bytes"), bytes.size()},
            {QStringLiteral("truncated"), false},
            {QStringLiteral("content_base64"), QString::fromLatin1(bytes.toBase64())},
        };
    }
    const QByteArray bytes = q.value(3).toString().toUtf8();
    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toString()},
        {QStringLiteral("file_name"), q.value(1).toString()},
        {QStringLiteral("mime_type"), QStringLiteral("text/plain")},
        {QStringLiteral("language"), q.value(2).toString()},
        {QStringLiteral("total_bytes"), bytes.size()},
        {QStringLiteral("truncated"), false},
        {QStringLiteral("content_base64"), QString::fromLatin1(bytes.toBase64())},
    };
}

// ---------------------------------------------------------------------------
// Settings table (raw key-value reads)
// ---------------------------------------------------------------------------

QJsonObject WireDbReader::heartbeatConfigById(const QString& configId) {
    QJsonObject out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, agent_id, scope_type, scope_id, alias, enabled, "
                             "       schedule, goal, surface_criteria, max_runs_per_day, "
                             "       auto_surface_target_conversation_id, self_config_allowed, "
                             "       last_fire_at, last_fire_outcome "
                             "FROM heartbeat_configs WHERE id=? LIMIT 1"));
    q.addBindValue(configId);
    if (!q.exec() || !q.next())
        return out;
    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toString()},
        {QStringLiteral("agentId"), q.value(1).toString()},
        {QStringLiteral("scopeType"), q.value(2).toString()},
        {QStringLiteral("scopeId"), q.value(3).toString()},
        {QStringLiteral("alias"), q.value(4).toString()},
        {QStringLiteral("enabled"), q.value(5).toBool()},
        {QStringLiteral("schedule"), q.value(6).toString()},
        {QStringLiteral("goal"), q.value(7).toString()},
        {QStringLiteral("surfaceCriteria"), q.value(8).toString()},
        {QStringLiteral("maxRunsPerDay"), q.value(9).toInt()},
        {QStringLiteral("autoSurfaceTargetConversationId"), q.value(10).toString()},
        {QStringLiteral("selfConfigAllowed"), q.value(11).toBool()},
        {QStringLiteral("lastFireAt"), q.value(12).toString()},
        {QStringLiteral("lastFireOutcome"), q.value(13).toString()},
    };
}

QString WireDbReader::settingsValue(const QString& key, const QString& def) {
    if (!m_db.isOpen())
        return def;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key=? LIMIT 1"));
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return def;
    return q.value(0).toString();
}

}  // namespace Verzeta::Remote

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent-registry.cpp
 * @brief AgentRegistry implementation: DB CRUD, QML API, templates.
 * @layer Service
 * @dependencies DbManager, Qt6::Core, Qt6::Sql.
 */

#include "agent-registry.h"

#include "../models/db-manager.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

// ---------------------------------------------------------------------------
// Constructor / lifecycle
// ---------------------------------------------------------------------------

AgentRegistry::AgentRegistry(DbManager& db, QObject* parent) : QObject(parent), m_db(db) {}

AgentRegistry::~AgentRegistry() = default;

void AgentRegistry::initialize() {
    VERZETA_ASSERT_MAIN_THREAD();
    seedBuiltInsIfEmpty();
    const int pruned = pruneOrphanBuiltinDuplicates();
    if (pruned > 0) {
        qCInfo(verzetaDb) << "AgentRegistry: pruned" << pruned
                          << "orphaned built-in template duplicates";
    }
}

// ---------------------------------------------------------------------------
// Internal: row <-> struct
// ---------------------------------------------------------------------------

/**
 * @brief Builds an Agent from one agents table row.
 * @param rec Row read from the agents table.
 * @returns The populated Agent.
 */
static Agent agentFromRecord(const QSqlRecord& rec) {
    Agent a;
    a.id = rec.value(QStringLiteral("id")).toString();
    a.name = rec.value(QStringLiteral("name")).toString();
    a.description = rec.value(QStringLiteral("description")).toString();
    a.iconName = rec.value(QStringLiteral("icon_name")).toString();
    a.systemPrompt = rec.value(QStringLiteral("system_prompt")).toString();
    a.defaultPattern = rec.value(QStringLiteral("default_pattern")).toString();
    a.modelProvider = rec.value(QStringLiteral("model_provider")).toString();
    a.modelName = rec.value(QStringLiteral("model_name")).toString();
    a.isBuiltIn = rec.value(QStringLiteral("is_builtin")).toInt() != 0;
    if (rec.contains(QStringLiteral("is_coordinator"))) {
        a.isCoordinator = rec.value(QStringLiteral("is_coordinator")).toInt() != 0;
    }
    a.createdAt =
        QDateTime::fromMSecsSinceEpoch(rec.value(QStringLiteral("created_at")).toLongLong());

    const QString toolsJson = rec.value(QStringLiteral("allowed_tools")).toString();
    if (!toolsJson.isEmpty()) {
        const QJsonArray arr = QJsonDocument::fromJson(toolsJson.toUtf8()).array();
        for (const QJsonValue& v : arr)
            a.allowedTools.append(v.toString());
    }

    if (rec.contains(QStringLiteral("default_heartbeat_goal"))) {
        a.defaultHeartbeatGoal = rec.value(QStringLiteral("default_heartbeat_goal")).toString();
    }
    if (rec.contains(QStringLiteral("default_heartbeat_schedule"))) {
        a.defaultHeartbeatSchedule =
            rec.value(QStringLiteral("default_heartbeat_schedule")).toString();
    }
    if (rec.contains(QStringLiteral("default_heartbeat_surface_criteria"))) {
        a.defaultHeartbeatSurfaceCriteria =
            rec.value(QStringLiteral("default_heartbeat_surface_criteria")).toString();
    }

    return a;
}

// ---------------------------------------------------------------------------
// C++ API
// ---------------------------------------------------------------------------

QList<Agent> AgentRegistry::allAgents() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<Agent> result;
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral("SELECT * FROM agents ORDER BY is_builtin DESC, name ASC"))) {
        qCWarning(verzetaDb) << "AgentRegistry::allAgents failed:" << q.lastError().text();
        return result;
    }
    while (q.next()) {
        VERZETA_ASSERT_MAIN_THREAD();
        result.append(agentFromRecord(q.record()));
    }
    return result;
}

Agent AgentRegistry::getAgent(const QString& id) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (id.isEmpty())
        return {};
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM agents WHERE id = ? LIMIT 1"));
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return {};
    return agentFromRecord(q.record());
}

Agent AgentRegistry::getAgentByName(const QString& name) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (name.isEmpty())
        return {};
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM agents WHERE name = ? LIMIT 1"));
    q.addBindValue(name);
    if (!q.exec() || !q.next())
        return {};
    return agentFromRecord(q.record());
}

QString AgentRegistry::createAgent(const Agent& agent) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (agent.name.isEmpty() || agent.systemPrompt.isEmpty()) {
        qCWarning(verzetaDb) << "AgentRegistry: cannot create agent with empty name or prompt";
        return {};
    }

    Agent a = agent;
    if (a.id.isEmpty()) {
        a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!a.createdAt.isValid()) {
        a.createdAt = QDateTime::currentDateTimeUtc();
    }

    QJsonArray toolsArr;
    for (const QString& t : a.allowedTools)
        toolsArr.append(t);
    const QString toolsJson =
        a.allowedTools.isEmpty()
            ? QString()
            : QString::fromUtf8(QJsonDocument(toolsArr).toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO agents(id, name, description, icon_name, system_prompt, "
                             "default_pattern, model_provider, model_name, allowed_tools, "
                             "is_builtin, is_coordinator, created_at, "
                             "default_heartbeat_goal, default_heartbeat_schedule, "
                             "default_heartbeat_surface_criteria) "
                             "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(a.id);
    q.addBindValue(a.name);
    q.addBindValue(a.description.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                           : QVariant(a.description));
    q.addBindValue(a.iconName.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                        : QVariant(a.iconName));
    q.addBindValue(a.systemPrompt);
    q.addBindValue(a.defaultPattern);
    q.addBindValue(a.modelProvider.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                             : QVariant(a.modelProvider));
    q.addBindValue(a.modelName.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                         : QVariant(a.modelName));
    q.addBindValue(toolsJson.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                       : QVariant(toolsJson));
    q.addBindValue(a.isBuiltIn ? 1 : 0);
    q.addBindValue(a.isCoordinator ? 1 : 0);
    q.addBindValue(a.createdAt.toMSecsSinceEpoch());
    q.addBindValue(a.defaultHeartbeatGoal.isNull() ? QStringLiteral("") : a.defaultHeartbeatGoal);
    q.addBindValue(a.defaultHeartbeatSchedule.isNull() ? QStringLiteral("")
                                                       : a.defaultHeartbeatSchedule);
    q.addBindValue(a.defaultHeartbeatSurfaceCriteria.isNull() ? QStringLiteral("")
                                                              : a.defaultHeartbeatSurfaceCriteria);

    if (!q.exec()) {
        qCWarning(verzetaDb) << "AgentRegistry::createAgent failed:" << q.lastError().text();
        return {};
    }

    emit agentsChanged();
    return a.id;
}

bool AgentRegistry::updateAgent(const Agent& agent) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (agent.id.isEmpty())
        return false;

    QJsonArray toolsArr;
    for (const QString& t : agent.allowedTools)
        toolsArr.append(t);
    const QString toolsJson =
        agent.allowedTools.isEmpty()
            ? QString()
            : QString::fromUtf8(QJsonDocument(toolsArr).toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("UPDATE agents SET name=?, description=?, icon_name=?, system_prompt=?, "
                       "default_pattern=?, model_provider=?, model_name=?, allowed_tools=?, "
                       "is_coordinator=?, "
                       "default_heartbeat_goal=?, default_heartbeat_schedule=?, "
                       "default_heartbeat_surface_criteria=? "
                       "WHERE id=?"));
    q.addBindValue(agent.name);
    q.addBindValue(agent.description.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                               : QVariant(agent.description));
    q.addBindValue(agent.iconName.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                            : QVariant(agent.iconName));
    q.addBindValue(agent.systemPrompt);
    q.addBindValue(agent.defaultPattern);
    q.addBindValue(agent.modelProvider.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                                 : QVariant(agent.modelProvider));
    q.addBindValue(agent.modelName.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                             : QVariant(agent.modelName));
    q.addBindValue(toolsJson.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                       : QVariant(toolsJson));
    q.addBindValue(agent.isCoordinator ? 1 : 0);
    q.addBindValue(agent.defaultHeartbeatGoal.isNull() ? QStringLiteral("")
                                                       : agent.defaultHeartbeatGoal);
    q.addBindValue(agent.defaultHeartbeatSchedule.isNull() ? QStringLiteral("")
                                                           : agent.defaultHeartbeatSchedule);
    q.addBindValue(agent.defaultHeartbeatSurfaceCriteria.isNull()
                       ? QStringLiteral("")
                       : agent.defaultHeartbeatSurfaceCriteria);
    q.addBindValue(agent.id);

    if (!q.exec()) {
        qCWarning(verzetaDb) << "AgentRegistry::updateAgent failed:" << q.lastError().text();
        return false;
    }

    emit agentsChanged();
    return true;
}

bool AgentRegistry::deleteAgent(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (id.isEmpty())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM agents WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "AgentRegistry::deleteAgent failed:" << q.lastError().text();
        return false;
    }
    const bool removed = q.numRowsAffected() > 0;
    if (removed) {
        emit agentDeleted(id);  // let per-agent memory purge agent:<id>
        emit agentsChanged();
    }
    return removed;
}

// ---------------------------------------------------------------------------
// QML API
// ---------------------------------------------------------------------------

/**
 * @brief Converts an Agent to the map shape QML reads.
 * @param a Agent to convert.
 * @returns Map keyed by the agent's field names.
 */
static QVariantMap agentToVariantMap(const Agent& a) {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantMap m;
    m[QStringLiteral("id")] = a.id;
    m[QStringLiteral("name")] = a.name;
    m[QStringLiteral("description")] = a.description;
    m[QStringLiteral("iconName")] = a.iconName;
    m[QStringLiteral("systemPrompt")] = a.systemPrompt;
    m[QStringLiteral("defaultPattern")] = a.defaultPattern;
    m[QStringLiteral("modelProvider")] = a.modelProvider;
    m[QStringLiteral("modelName")] = a.modelName;
    m[QStringLiteral("allowedTools")] = a.allowedTools;
    m[QStringLiteral("isBuiltIn")] = a.isBuiltIn;
    m[QStringLiteral("isCoordinator")] = a.isCoordinator;
    m[QStringLiteral("createdAt")] = a.createdAt;
    m[QStringLiteral("defaultHeartbeatGoal")] = a.defaultHeartbeatGoal;
    m[QStringLiteral("defaultHeartbeatSchedule")] = a.defaultHeartbeatSchedule;
    m[QStringLiteral("defaultHeartbeatSurfaceCriteria")] = a.defaultHeartbeatSurfaceCriteria;
    return m;
}

QVariantList AgentRegistry::agentList() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList list;
    const QList<Agent> agents = allAgents();
    for (const Agent& a : agents) {
        list.append(agentToVariantMap(a));
    }
    return list;
}

QString AgentRegistry::saveAgent(const QVariantMap& map) {
    VERZETA_ASSERT_MAIN_THREAD();
    Agent a;
    a.id = map.value(QStringLiteral("id")).toString();
    a.name = map.value(QStringLiteral("name")).toString().trimmed();
    a.description = map.value(QStringLiteral("description")).toString();
    a.iconName = map.value(QStringLiteral("iconName")).toString();
    a.systemPrompt = map.value(QStringLiteral("systemPrompt")).toString();
    a.defaultPattern = map.value(QStringLiteral("defaultPattern")).toString();
    if (a.defaultPattern.isEmpty())
        a.defaultPattern = QStringLiteral("direct");
    a.modelProvider = map.value(QStringLiteral("modelProvider")).toString();
    a.modelName = map.value(QStringLiteral("modelName")).toString();
    a.allowedTools = map.value(QStringLiteral("allowedTools")).toStringList();
    a.isCoordinator = map.value(QStringLiteral("isCoordinator")).toBool();
    a.defaultHeartbeatGoal = map.value(QStringLiteral("defaultHeartbeatGoal")).toString();
    a.defaultHeartbeatSchedule = map.value(QStringLiteral("defaultHeartbeatSchedule")).toString();
    a.defaultHeartbeatSurfaceCriteria =
        map.value(QStringLiteral("defaultHeartbeatSurfaceCriteria")).toString();

    if (a.name.isEmpty() || a.systemPrompt.isEmpty())
        return {};

    if (!a.id.isEmpty() && getAgent(a.id).isValid()) {
        return updateAgent(a) ? a.id : QString();
    }
    return createAgent(a);
}

bool AgentRegistry::removeAgent(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    return deleteAgent(id);
}

QVariantMap AgentRegistry::agentInfo(const QString& id) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const Agent a = getAgent(id);
    if (!a.isValid())
        return {};
    return agentToVariantMap(a);
}

QVariantList AgentRegistry::builtInTemplates() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList list;
    const QList<Agent> templates = builtInAgentTemplates();
    for (const Agent& a : templates) {
        list.append(agentToVariantMap(a));
    }
    return list;
}

QString AgentRegistry::getOrCreateBuiltinByName(const QString& name) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (name.isEmpty())
        return {};

    const Agent existing = getAgentByName(name);
    if (!existing.id.isEmpty()) {
        return existing.id;
    }

    const QList<Agent> templates = builtInAgentTemplates();
    for (Agent a : templates) {
        if (a.name == name) {
            a.id.clear();
            a.isBuiltIn = true;
            return createAgent(a);
        }
    }
    return {};
}

QString AgentRegistry::createFromTemplate(const QString& templateName) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QList<Agent> templates = builtInAgentTemplates();
    for (Agent a : templates) {
        if (a.name == templateName) {
            a.id.clear();  // force new ID
            a.isBuiltIn = true;
            // Auto-disambiguate the name when one already exists. The
            // agents.name UNIQUE constraint would otherwise reject a
            // second create-from-template click on the same template
            // — surfaced as a generic SQLite error in the logs while
            // the AgentsPage UI just appeared not to respond. With
            // suffixing the user can click a template multiple times
            // to create independent copies (e.g. "Researcher",
            // "Researcher 2", "Researcher 3").
            const QString baseName = a.name;
            int suffix = 2;
            while (!getAgentByName(a.name).id.isEmpty()) {
                a.name = QStringLiteral("%1 %2").arg(baseName).arg(suffix++);
                if (suffix > 999) {
                    qCWarning(verzetaDb)
                        << "createFromTemplate: 999 disambiguations exhausted for" << baseName;
                    return {};
                }
            }
            return createAgent(a);
        }
    }
    return {};
}

int AgentRegistry::pruneOrphanBuiltinDuplicates() {
    VERZETA_ASSERT_MAIN_THREAD();

    const QList<Agent> templates = builtInAgentTemplates();

    static const QRegularExpression suffixRx(QStringLiteral("^.+ ([0-9]+)$"));

    int deleted = 0;

    if (!m_db.transaction()) {
        qCWarning(verzetaDb) << "pruneOrphanBuiltinDuplicates: transaction begin failed";
        return 0;
    }

    for (const Agent& tmpl : templates) {
        const QString canonical = tmpl.name;

        QSqlQuery selectCanonical(m_db.db());
        selectCanonical.prepare(
            QStringLiteral("SELECT id FROM agents WHERE is_built_in = 1 AND name = ? LIMIT 1"));
        selectCanonical.addBindValue(canonical);
        if (!selectCanonical.exec() || !selectCanonical.next()) {
            continue;
        }
        const QString canonicalId = selectCanonical.value(0).toString();
        if (canonicalId.isEmpty())
            continue;

        QSqlQuery selectDups(m_db.db());
        selectDups.prepare(QStringLiteral("SELECT id, name FROM agents "
                                          "WHERE is_built_in = 1 AND name LIKE ? AND name != ?"));
        selectDups.addBindValue(canonical + QStringLiteral(" %"));
        selectDups.addBindValue(canonical);
        if (!selectDups.exec())
            continue;

        QStringList dupIds;
        while (selectDups.next()) {
            const QString id = selectDups.value(0).toString();
            const QString name = selectDups.value(1).toString();
            if (!suffixRx.match(name).hasMatch())
                continue;
            if (!name.startsWith(canonical + QLatin1Char(' ')))
                continue;
            dupIds.append(id);
        }

        for (const QString& dupId : dupIds) {
            const struct {
                QString sql;
            } rebinds[] = {
                {QStringLiteral("UPDATE conversations SET primary_agent_id = ? "
                                "WHERE primary_agent_id = ?")},
            };
            for (const auto& r : rebinds) {
                QSqlQuery up(m_db.db());
                up.prepare(r.sql);
                up.addBindValue(canonicalId);
                up.addBindValue(dupId);
                if (!up.exec()) {
                    qCWarning(verzetaDb)
                        << "pruneOrphanBuiltinDuplicates: rebind failed:" << up.lastError().text();
                    m_db.rollback();
                    return 0;
                }
            }

            const QStringList rebindOrDelete{
                QStringLiteral("UPDATE OR IGNORE conversation_members SET agent_id = ? "
                               "WHERE agent_id = ?"),
                QStringLiteral("UPDATE OR IGNORE project_members SET agent_id = ? "
                               "WHERE agent_id = ?"),
                QStringLiteral("UPDATE OR IGNORE heartbeat_configs SET agent_id = ? "
                               "WHERE agent_id = ?"),
            };
            for (const QString& sql : rebindOrDelete) {
                QSqlQuery up(m_db.db());
                up.prepare(sql);
                up.addBindValue(canonicalId);
                up.addBindValue(dupId);
                if (!up.exec()) {
                    qCWarning(verzetaDb)
                        << "pruneOrphanBuiltinDuplicates: rebind failed:" << up.lastError().text();
                    m_db.rollback();
                    return 0;
                }
            }

            const QStringList sweepLeftovers{
                QStringLiteral("DELETE FROM conversation_members WHERE agent_id = ?"),
                QStringLiteral("DELETE FROM project_members WHERE agent_id = ?"),
                QStringLiteral("DELETE FROM heartbeat_configs WHERE agent_id = ?"),
            };
            for (const QString& sql : sweepLeftovers) {
                QSqlQuery del(m_db.db());
                del.prepare(sql);
                del.addBindValue(dupId);
                del.exec();
            }

            QSqlQuery delAgent(m_db.db());
            delAgent.prepare(QStringLiteral("DELETE FROM agents WHERE id = ?"));
            delAgent.addBindValue(dupId);
            if (!delAgent.exec()) {
                qCWarning(verzetaDb) << "pruneOrphanBuiltinDuplicates: delete agent failed:"
                                     << delAgent.lastError().text();
                m_db.rollback();
                return 0;
            }
            if (delAgent.numRowsAffected() > 0)
                ++deleted;
        }
    }

    if (!m_db.commit()) {
        qCWarning(verzetaDb) << "pruneOrphanBuiltinDuplicates: commit failed";
        m_db.rollback();
        return 0;
    }

    if (deleted > 0)
        emit agentsChanged();
    return deleted;
}

// ---------------------------------------------------------------------------
// Built-in seeding
// ---------------------------------------------------------------------------

void AgentRegistry::seedBuiltInsIfEmpty() {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM agents"))) {
        qCWarning(verzetaDb) << "AgentRegistry::seedBuiltInsIfEmpty: count failed";
        return;
    }
    if (!q.next())
        return;
    const int count = q.value(0).toInt();
    if (count > 0)
        return;

    qCInfo(verzetaDb) << "AgentRegistry: seeding built-in agent templates";
    const QList<Agent> templates = builtInAgentTemplates();
    for (Agent a : templates) {
        VERZETA_ASSERT_MAIN_THREAD();
        a.isBuiltIn = true;
        createAgent(a);
    }
}

QList<Agent> AgentRegistry::builtInAgentTemplates() {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<Agent> list;

    auto mk = [](const QString& name,
                 const QString& desc,
                 const QString& icon,
                 const QString& prompt,
                 const QString& pattern,
                 bool coordinator = false) {
        Agent a;
        a.name = name;
        a.description = desc;
        a.iconName = icon;
        a.systemPrompt = prompt;
        a.defaultPattern = pattern;
        a.isCoordinator = coordinator;
        return a;
    };

    list.append(mk(QStringLiteral("Assistant"),
                   QStringLiteral("General-purpose helpful assistant"),
                   QStringLiteral("face-smile"),
                   QStringLiteral("You are a helpful, accurate, and concise assistant. "
                                  "You explain things clearly and admit when you don't know."),
                   QStringLiteral("direct")));

    list.append(
        mk(QStringLiteral("Engineer"),
           QStringLiteral("Writes, reviews and debugs code"),
           QStringLiteral("applications-development"),
           QStringLiteral("You are a senior software engineer. You write clean, correct, "
                          "well-tested code. You explain your design choices and call out "
                          "edge cases. When asked to fix bugs, you identify the root cause, "
                          "not just the symptom. You use tools to read files, run commands, "
                          "and write code when needed."),
           QStringLiteral("react")));

    list.append(mk(QStringLiteral("Researcher"),
                   QStringLiteral("Gathers information, cites sources and summarizes findings"),
                   QStringLiteral("applications-science"),
                   QStringLiteral("You are a thorough researcher. You gather information from "
                                  "multiple sources, cite where each fact came from, and present "
                                  "balanced summaries. You use web search and document retrieval "
                                  "tools. When uncertain, you say so."),
                   QStringLiteral("react")));

    list.append(mk(QStringLiteral("Writer"),
                   QStringLiteral("Creative and technical writing specialist"),
                   QStringLiteral("accessories-text-editor"),
                   QStringLiteral("You are a skilled writer who adapts tone and style to the "
                                  "audience. You produce clear, engaging prose, whether "
                                  "technical documentation, marketing copy, or creative work. "
                                  "You ask for clarification when the brief is ambiguous."),
                   QStringLiteral("direct")));

    list.append(mk(QStringLiteral("Code Reviewer"),
                   QStringLiteral("Reviews code for bugs, style, and best practices"),
                   QStringLiteral("text-x-changelog"),
                   QStringLiteral("You are an experienced code reviewer. You read code carefully "
                                  "and flag correctness issues, security problems, performance "
                                  "concerns, and style violations. You prioritize by impact and "
                                  "suggest concrete fixes. You are direct but constructive."),
                   QStringLiteral("direct")));

    list.append(
        mk(QStringLiteral("Project Manager"),
           QStringLiteral("Breaks goals into tasks and coordinates execution"),
           QStringLiteral("view-calendar-tasks"),
           QStringLiteral("You are a project manager. You take high-level goals and "
                          "break them into concrete, actionable tasks with clear owners "
                          "and deadlines. You track progress, surface risks, and "
                          "coordinate between team members when multiple agents are available."),
           QStringLiteral("planner"),
           /*coordinator=*/true));

    // --- Coordinator / management templates ---

    list.append(mk(QStringLiteral("Team Lead"),
                   QStringLiteral("Coordinates a small team and routes work to specialists"),
                   QStringLiteral("preferences-system-users"),
                   QStringLiteral("You are a team lead. When the user asks a question in a "
                                  "group chat, you decide whether to answer directly or delegate "
                                  "to a teammate. You synthesize replies from multiple teammates "
                                  "into a coherent final answer, track what each person is working "
                                  "on, and unblock them when they need input."),
                   QStringLiteral("planner"),
                   /*coordinator=*/true));

    list.append(
        mk(QStringLiteral("Engineering Manager"),
           QStringLiteral("Runs the engineering team, delegating and reviewing technical work"),
           QStringLiteral("applications-development"),
           QStringLiteral("You are an engineering manager. You understand the technical "
                          "work deeply enough to route tasks to the right engineers, review "
                          "architectural decisions, and unblock your team. You coordinate "
                          "between engineers, reviewers, and other stakeholders in this group."),
           QStringLiteral("planner"),
           /*coordinator=*/true));

    list.append(
        mk(QStringLiteral("Product Manager"),
           QStringLiteral("Owns product strategy and coordinates cross-functional work"),
           QStringLiteral("office-chart-line"),
           QStringLiteral("You are a product manager. You own the product strategy and "
                          "coordinate between engineering, design, and business stakeholders. "
                          "When a request comes in, you decide who on the team should handle "
                          "each part and you synthesize their inputs into a cohesive plan."),
           QStringLiteral("planner"),
           /*coordinator=*/true));

    list.append(
        mk(QStringLiteral("Executive"),
           QStringLiteral("Sets direction and makes the final calls"),
           QStringLiteral("preferences-contact-list"),
           QStringLiteral("You are a senior executive. You set high-level direction, "
                          "make final decisions on strategic questions, and coordinate "
                          "across the entire organization. You delegate operational work "
                          "to your managers and synthesize their reports into clear decisions."),
           QStringLiteral("planner"),
           /*coordinator=*/true));

    list.append(mk(QStringLiteral("Analyst"),
                   QStringLiteral("Analyzes data, builds models and extracts insights"),
                   QStringLiteral("office-chart-bar"),
                   QStringLiteral("You are a data analyst. You examine datasets carefully, "
                                  "apply appropriate statistical methods, and present findings "
                                  "with clear visualizations and caveats. You distinguish "
                                  "correlation from causation and explicitly state assumptions."),
                   QStringLiteral("react")));

    list.append(mk(QStringLiteral("Designer"),
                   QStringLiteral("UI/UX and visual design specialist"),
                   QStringLiteral("applications-graphics"),
                   QStringLiteral("You are a UI/UX designer. You think about users first: "
                                  "their goals, mental models, and constraints. You propose "
                                  "designs grounded in established patterns and accessibility "
                                  "standards. You explain tradeoffs clearly."),
                   QStringLiteral("direct")));

    list.append(mk(QStringLiteral("Sales"),
                   QStringLiteral("Customer-facing sales and outreach specialist"),
                   QStringLiteral("preferences-contact-list"),
                   QStringLiteral("You are a sales specialist. You understand customer needs, "
                                  "position products clearly, and handle objections with honesty. "
                                  "You write persuasive but truthful outreach emails, proposals, "
                                  "and follow-ups. You focus on building long-term relationships."),
                   QStringLiteral("direct")));

    return list;
}

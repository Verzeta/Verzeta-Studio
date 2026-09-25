// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-config-service.cpp
 * @brief Implementation of `heartbeat_configs` CRUD + scheduler-
 *        targeted lookups.
 * @layer Service
 * @dependencies DbManager, HeartbeatConfig POD, Qt6::Sql.
 */

#include "heartbeat-config-service.h"

#include "../utils/heartbeat-schedule.h"
#include "../utils/logger.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

namespace {

/**
 * @brief Convert a HeartbeatConfig POD to the QVariantMap shape
 *        consumed by QML. Keep the key-set narrow; QML only needs
 *        the columns the UI binds to.
 */
QVariantMap configMapFromConfig(const HeartbeatConfig& cfg) {
    QVariantMap m;
    m[QStringLiteral("id")] = cfg.id;
    m[QStringLiteral("agentId")] = cfg.agentId;
    m[QStringLiteral("scopeType")] = heartbeatScopeTypeToString(cfg.scopeType);
    m[QStringLiteral("scopeId")] = cfg.scopeId;
    m[QStringLiteral("alias")] = cfg.alias;
    m[QStringLiteral("enabled")] = cfg.enabled;
    m[QStringLiteral("schedule")] = cfg.schedule;
    m[QStringLiteral("goal")] = cfg.goal;
    m[QStringLiteral("surfaceCriteria")] = cfg.surfaceCriteria;
    m[QStringLiteral("maxRunsPerDay")] = cfg.maxRunsPerDay;
    m[QStringLiteral("autoSurfaceTargetConversationId")] = cfg.autoSurfaceTargetConversationId;
    m[QStringLiteral("selfConfigAllowed")] = cfg.selfConfigAllowed;
    m[QStringLiteral("lastFireAt")] = cfg.lastFireAt;
    m[QStringLiteral("lastFireOutcome")] = cfg.lastFireOutcome;
    return m;
}

}  // namespace

HeartbeatConfigService::HeartbeatConfigService(DbManager& db, QObject* parent)
    : QObject(parent), m_db(db) {}

HeartbeatConfigService::~HeartbeatConfigService() = default;

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

QString HeartbeatConfigService::upsertConfig(HeartbeatConfig cfg) {
    if (cfg.id.isEmpty() || cfg.agentId.isEmpty() || cfg.scopeId.isEmpty()) {
        qCWarning(verzetaUi) << "HeartbeatConfigService::upsertConfig — invalid POD"
                             << "id=" << cfg.id << "agentId=" << cfg.agentId
                             << "scopeId=" << cfg.scopeId;
        return {};
    }
    const QDateTime nowDt = QDateTime::currentDateTimeUtc();
    if (!cfg.createdAt.isValid()) {
        cfg.createdAt = nowDt;
    }
    cfg.updatedAt = nowDt;

    // Coerce null QStrings to empty strings before binding. Qt's
    // QSqlQuery binds a null QString (isNull() == true) as SQL NULL,
    // which collides with our NOT NULL TEXT columns even though the
    // schema declares DEFAULT '' — DEFAULT only applies when the
    // column is OMITTED from the INSERT, not when explicitly bound
    // to a null value. Every text column in heartbeat_configs goes
    // through this coercion.
    auto bindStr = [](const QString& s) -> QString { return s.isNull() ? QStringLiteral("") : s; };

    QSqlQuery q(m_db.db());
    // Real upsert: INSERT and on PK conflict update the mutable
    // columns. INSERT OR REPLACE is wrong here — it DELETEs the
    // existing row and re-inserts, which triggers FK CASCADE on
    // heartbeat_reports + heartbeat_config_changes, wiping every
    // run / audit row for the config. ON CONFLICT(id) DO UPDATE
    // preserves dependent rows.
    q.prepare(QStringLiteral("INSERT INTO heartbeat_configs ("
                             "  id, agent_id, scope_type, scope_id, alias,"
                             "  enabled, schedule, goal, surface_criteria, max_runs_per_day,"
                             "  auto_surface_target_conversation_id, self_config_allowed,"
                             "  last_fire_at, last_fire_outcome,"
                             "  created_at, updated_at"
                             ") VALUES ("
                             "  ?, ?, ?, ?, ?,"
                             "  ?, ?, ?, ?, ?,"
                             "  ?, ?,"
                             "  ?, ?,"
                             "  ?, ?"
                             ") ON CONFLICT(id) DO UPDATE SET "
                             "  enabled = excluded.enabled,"
                             "  schedule = excluded.schedule,"
                             "  goal = excluded.goal,"
                             "  surface_criteria = excluded.surface_criteria,"
                             "  max_runs_per_day = excluded.max_runs_per_day,"
                             "  auto_surface_target_conversation_id = "
                             "      excluded.auto_surface_target_conversation_id,"
                             "  self_config_allowed = excluded.self_config_allowed,"
                             "  updated_at = excluded.updated_at"));
    q.addBindValue(bindStr(cfg.id));
    q.addBindValue(bindStr(cfg.agentId));
    q.addBindValue(heartbeatScopeTypeToString(cfg.scopeType));
    q.addBindValue(bindStr(cfg.scopeId));
    q.addBindValue(bindStr(cfg.alias));
    q.addBindValue(cfg.enabled ? 1 : 0);
    q.addBindValue(bindStr(cfg.schedule));
    q.addBindValue(bindStr(cfg.goal));
    q.addBindValue(bindStr(cfg.surfaceCriteria));
    q.addBindValue(cfg.maxRunsPerDay);
    q.addBindValue(bindStr(cfg.autoSurfaceTargetConversationId));
    q.addBindValue(cfg.selfConfigAllowed ? 1 : 0);
    // last_fire_at + last_fire_outcome are nullable in spirit but
    // declared TEXT (not NULL constraint). Empty string = "never fired"
    // sentinel, treated by the parser as "no anchor".
    q.addBindValue(cfg.lastFireAt.isNull() ? QVariant(QString()) : QVariant(cfg.lastFireAt));
    q.addBindValue(cfg.lastFireOutcome.isNull() ? QVariant(QString())
                                                : QVariant(cfg.lastFireOutcome));
    q.addBindValue(cfg.createdAt.toString(Qt::ISODateWithMs));
    q.addBindValue(cfg.updatedAt.toString(Qt::ISODateWithMs));

    if (!q.exec()) {
        qCWarning(verzetaUi) << "HeartbeatConfigService::upsertConfig failed:"
                             << q.lastError().text();
        return {};
    }
    emit configChanged(cfg.id);
    return cfg.id;
}

HeartbeatConfig HeartbeatConfigService::configById(const QString& id) const {
    if (id.isEmpty())
        return {};
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM heartbeat_configs WHERE id = ? LIMIT 1"));
    q.addBindValue(id);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "configById query failed:" << q.lastError().text();
        return {};
    }
    if (!q.next())
        return {};
    return HeartbeatConfig::fromSqlRecord(q.record());
}

HeartbeatConfig HeartbeatConfigService::configFor(const QString& agentId,
                                                  HeartbeatScopeType scopeType,
                                                  const QString& scopeId,
                                                  const QString& alias) const {
    if (agentId.isEmpty() || scopeId.isEmpty())
        return {};
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM heartbeat_configs "
                             "WHERE agent_id = ? AND scope_type = ? AND scope_id = ? AND alias = ? "
                             "LIMIT 1"));
    q.addBindValue(agentId);
    q.addBindValue(heartbeatScopeTypeToString(scopeType));
    q.addBindValue(scopeId);
    // The alias column is NOT NULL DEFAULT '' on the schema. Qt
    // binds a default-constructed QString as SQL NULL, which never
    // matches an '' value via `=` (three-valued logic). Coerce.
    q.addBindValue(alias.isNull() ? QStringLiteral("") : alias);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "configFor query failed:" << q.lastError().text();
        return {};
    }
    if (!q.next())
        return {};
    return HeartbeatConfig::fromSqlRecord(q.record());
}

bool HeartbeatConfigService::updateConfig(const HeartbeatConfig& cfg) {
    if (!cfg.isValid())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE heartbeat_configs SET "
                             "  enabled = ?, schedule = ?, goal = ?, surface_criteria = ?,"
                             "  max_runs_per_day = ?,"
                             "  auto_surface_target_conversation_id = ?,"
                             "  self_config_allowed = ?,"
                             "  updated_at = ?"
                             " WHERE id = ?"));
    q.addBindValue(cfg.enabled ? 1 : 0);
    q.addBindValue(cfg.schedule);
    q.addBindValue(cfg.goal);
    q.addBindValue(cfg.surfaceCriteria);
    q.addBindValue(cfg.maxRunsPerDay);
    q.addBindValue(cfg.autoSurfaceTargetConversationId);
    q.addBindValue(cfg.selfConfigAllowed ? 1 : 0);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    q.addBindValue(cfg.id);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "updateConfig failed:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0)
        return false;
    emit configChanged(cfg.id);
    return true;
}

bool HeartbeatConfigService::deleteConfig(const QString& id) {
    if (id.isEmpty())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM heartbeat_configs WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "deleteConfig failed:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0)
        return false;
    emit configRemoved(id);
    return true;
}

// ---------------------------------------------------------------------------
// Scheduler-targeted lookups
// ---------------------------------------------------------------------------

QList<HeartbeatConfig> HeartbeatConfigService::enabledConfigs() const {
    QList<HeartbeatConfig> out;
    QSqlQuery q(m_db.db());
    // ORDER BY last_fire_at IS NULL DESC, last_fire_at ASC
    //   means: configs that have NEVER fired come first (NULLs first),
    //   then by oldest fire ASC. This is the fairness ordering for
    //   the queue's tick — never-fired and longest-stale configs win.
    if (!q.exec(QStringLiteral("SELECT * FROM heartbeat_configs "
                               "WHERE enabled = 1 "
                               "ORDER BY last_fire_at IS NULL DESC, last_fire_at ASC, id ASC"))) {
        qCWarning(verzetaUi) << "enabledConfigs failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(HeartbeatConfig::fromSqlRecord(q.record()));
    }
    return out;
}

QList<HeartbeatConfig> HeartbeatConfigService::configsInScope(HeartbeatScopeType scopeType,
                                                              const QString& scopeId) const {
    QList<HeartbeatConfig> out;
    if (scopeId.isEmpty())
        return out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM heartbeat_configs "
                             "WHERE scope_type = ? AND scope_id = ? "
                             "ORDER BY alias ASC, id ASC"));
    q.addBindValue(heartbeatScopeTypeToString(scopeType));
    q.addBindValue(scopeId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "configsInScope failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(HeartbeatConfig::fromSqlRecord(q.record()));
    }
    return out;
}

bool HeartbeatConfigService::updateLastFire(const QString& configId,
                                            const QString& at,
                                            const QString& outcome) {
    if (configId.isEmpty())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE heartbeat_configs "
                             "SET last_fire_at = ?, last_fire_outcome = ?, updated_at = ? "
                             "WHERE id = ?"));
    q.addBindValue(at);
    q.addBindValue(outcome);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    q.addBindValue(configId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "updateLastFire failed:" << q.lastError().text();
        return false;
    }
    return q.numRowsAffected() > 0;
}

// ---------------------------------------------------------------------------
// App-layer cleanup hooks
// ---------------------------------------------------------------------------

int HeartbeatConfigService::onConversationDeleted(const QString& conversationId) {
    if (conversationId.isEmpty())
        return 0;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM heartbeat_configs "
                             "WHERE scope_id = ? "
                             "  AND scope_type IN ('conversation_1to1', 'conversation_group')"));
    q.addBindValue(conversationId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "onConversationDeleted cleanup failed:" << q.lastError().text();
        return 0;
    }
    return q.numRowsAffected();
}

int HeartbeatConfigService::onFolderDeleted(const QString& folderId) {
    if (folderId.isEmpty())
        return 0;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM heartbeat_configs "
                             "WHERE scope_id = ? AND scope_type = 'folder'"));
    q.addBindValue(folderId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "onFolderDeleted cleanup failed:" << q.lastError().text();
        return 0;
    }
    return q.numRowsAffected();
}


QVariantList HeartbeatConfigService::configsForConversation(const QString& conversationId) const {
    QVariantList out;
    if (conversationId.isEmpty())
        return out;
    const QList<HeartbeatConfig> oneToOne =
        configsInScope(HeartbeatScopeType::Conversation1to1, conversationId);
    for (const HeartbeatConfig& c : oneToOne) {
        out.append(configMapFromConfig(c));
    }
    const QList<HeartbeatConfig> group =
        configsInScope(HeartbeatScopeType::ConversationGroup, conversationId);
    for (const HeartbeatConfig& c : group) {
        out.append(configMapFromConfig(c));
    }
    return out;
}

QVariantMap HeartbeatConfigService::configByIdMap(const QString& id) const {
    if (id.isEmpty())
        return {};
    const HeartbeatConfig cfg = configById(id);
    if (cfg.id.isEmpty())
        return {};
    return configMapFromConfig(cfg);
}

QVariantMap HeartbeatConfigService::configForMap(const QString& agentId,
                                                 const QString& scopeTypeStr,
                                                 const QString& scopeId,
                                                 const QString& alias) const {
    if (agentId.isEmpty() || scopeId.isEmpty())
        return {};
    const HeartbeatConfig cfg =
        configFor(agentId, heartbeatScopeTypeFromString(scopeTypeStr), scopeId, alias);
    if (cfg.id.isEmpty())
        return {};
    return configMapFromConfig(cfg);
}

QString HeartbeatConfigService::upsertConfigMap(const QVariantMap& fields) {
    HeartbeatConfig cfg;
    cfg.id = fields.value(QStringLiteral("id")).toString();
    cfg.agentId = fields.value(QStringLiteral("agentId")).toString();
    cfg.scopeType =
        heartbeatScopeTypeFromString(fields.value(QStringLiteral("scopeType")).toString());
    cfg.scopeId = fields.value(QStringLiteral("scopeId")).toString();
    cfg.alias = fields.value(QStringLiteral("alias"), QString()).toString();

    cfg.enabled = fields.value(QStringLiteral("enabled"), false).toBool();
    cfg.schedule = fields.value(QStringLiteral("schedule"), QString()).toString();
    cfg.goal = fields.value(QStringLiteral("goal"), QString()).toString();
    cfg.surfaceCriteria = fields.value(QStringLiteral("surfaceCriteria"), QString()).toString();
    cfg.maxRunsPerDay = fields.value(QStringLiteral("maxRunsPerDay"), 24).toInt();
    cfg.autoSurfaceTargetConversationId =
        fields.value(QStringLiteral("autoSurfaceTargetConversationId"), QString()).toString();
    cfg.selfConfigAllowed = fields.value(QStringLiteral("selfConfigAllowed"), false).toBool();

    // Generate a UUID for fresh inserts. Existing rows keep their id.
    if (cfg.id.isEmpty()) {
        cfg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    return upsertConfig(cfg);
}

bool HeartbeatConfigService::removeConfig(const QString& id) {
    return deleteConfig(id);
}

int HeartbeatConfigService::suggestedAutoSurfaceCap(const QString& conversationId) const {
    if (conversationId.isEmpty())
        return 1;

    int total = 0;
    const auto accumulate = [&](const QList<HeartbeatConfig>& rows) {
        for (const HeartbeatConfig& c : rows) {
            if (!c.enabled)
                continue;
            const HeartbeatSchedule s = parseHeartbeatSchedule(c.schedule);
            if (!s.valid)
                continue;
            const int per = std::min(firesIn24h(s), c.maxRunsPerDay);
            total += per;
        }
    };
    accumulate(configsInScope(HeartbeatScopeType::Conversation1to1, conversationId));
    accumulate(configsInScope(HeartbeatScopeType::ConversationGroup, conversationId));
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("SELECT * FROM heartbeat_configs "
                                 "WHERE scope_type = 'folder' "
                                 "  AND auto_surface_target_conversation_id = ? "
                                 "  AND enabled = 1"));
        q.addBindValue(conversationId);
        if (q.exec()) {
            QList<HeartbeatConfig> folderTargets;
            while (q.next()) {
                folderTargets.append(HeartbeatConfig::fromSqlRecord(q.record()));
            }
            accumulate(folderTargets);
        }
    }

    if (total <= 0)
        return 1;
    if (total > 24)
        return 24;  // ceiling
    return total;
}

QVariantList HeartbeatConfigService::configsForFolder(const QString& folderId) const {
    QVariantList out;
    if (folderId.isEmpty())
        return out;
    const QList<HeartbeatConfig> rows = configsInScope(HeartbeatScopeType::Folder, folderId);
    for (const HeartbeatConfig& c : rows) {
        out.append(configMapFromConfig(c));
    }
    return out;
}

int HeartbeatConfigService::suggestedAutoSurfaceCapForFolder(const QString& folderId,
                                                             const QString& targetConvId) const {
    if (folderId.isEmpty() || targetConvId.isEmpty())
        return 1;

    int total = 0;
    const QList<HeartbeatConfig> rows = configsInScope(HeartbeatScopeType::Folder, folderId);
    for (const HeartbeatConfig& c : rows) {
        if (!c.enabled)
            continue;
        if (c.autoSurfaceTargetConversationId != targetConvId)
            continue;
        const HeartbeatSchedule s = parseHeartbeatSchedule(c.schedule);
        if (!s.valid)
            continue;
        total += std::min(firesIn24h(s), c.maxRunsPerDay);
    }
    if (total <= 0)
        return 1;
    if (total > 24)
        return 24;
    return total;
}


bool HeartbeatConfigService::recordChange(const QString& configId,
                                          const QString& field,
                                          const QString& oldValue,
                                          const QString& newValue,
                                          const QString& source) {
    if (configId.isEmpty() || field.isEmpty())
        return false;
    if (source != QStringLiteral("agent") && source != QStringLiteral("user")) {
        qCWarning(verzetaUi) << "recordChange: invalid source" << source
                             << "(expected 'agent' or 'user')";
        return false;
    }

    auto bindStr = [](const QString& s) -> QString { return s.isNull() ? QStringLiteral("") : s; };

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO heartbeat_config_changes "
                             "(id, config_id, changed_at, field, old_value, new_value, source) "
                             "VALUES (?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
    q.addBindValue(configId);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    q.addBindValue(field);
    q.addBindValue(bindStr(oldValue));
    q.addBindValue(bindStr(newValue));
    q.addBindValue(source);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "recordChange INSERT failed:" << q.lastError().text();
        return false;
    }
    return true;
}

QVariantList HeartbeatConfigService::recentConfigChanges(int limit) const {
    if (limit < 1)
        limit = 1;
    if (limit > 500)
        limit = 500;

    QVariantList out;
    QSqlQuery q(m_db.db());
    // Join with heartbeat_configs so the audit row carries enough
    // context for the diagnostics tab without QML doing a second
    // lookup. agent_name is resolved at the QML layer (the audit
    // service has no AgentRegistry dep — the diagnostics caller
    // resolves the agent name from cfg.agent_id).
    q.prepare(QStringLiteral("SELECT c.id, c.config_id, c.changed_at, c.field, c.old_value,"
                             "       c.new_value, c.source, h.alias, h.agent_id "
                             "FROM heartbeat_config_changes c "
                             "LEFT JOIN heartbeat_configs h ON h.id = c.config_id "
                             "ORDER BY c.changed_at DESC LIMIT ?"));
    q.addBindValue(limit);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "recentConfigChanges query failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        QVariantMap m;
        const QDateTime changedAt = QDateTime::fromString(q.value(2).toString(), Qt::ISODateWithMs);
        m.insert(QStringLiteral("id"), q.value(0).toString());
        m.insert(QStringLiteral("configId"), q.value(1).toString());
        m.insert(QStringLiteral("changedAtMs"),
                 changedAt.isValid() ? QVariant(changedAt.toMSecsSinceEpoch())
                                     : QVariant(qint64(-1)));
        m.insert(QStringLiteral("field"), q.value(3).toString());
        m.insert(QStringLiteral("oldValue"), q.value(4).toString());
        m.insert(QStringLiteral("newValue"), q.value(5).toString());
        m.insert(QStringLiteral("source"), q.value(6).toString());
        m.insert(QStringLiteral("alias"), q.value(7).toString());
        m.insert(QStringLiteral("agentId"), q.value(8).toString());
        out.append(m);
    }
    return out;
}

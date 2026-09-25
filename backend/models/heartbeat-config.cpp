// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file heartbeat-config.cpp
 * @brief Implementation of the HeartbeatConfig POD's SQL-record
 *        deserialiser and the scope-type enum<->string conversions.
 * @layer Data Model
 * @dependencies Qt6::Sql.
 */


#include "heartbeat-config.h"

#include <QSqlRecord>
#include <QVariant>

QString heartbeatScopeTypeToString(HeartbeatScopeType t) {
    switch (t) {
        case HeartbeatScopeType::Conversation1to1:
            return QStringLiteral("conversation_1to1");
        case HeartbeatScopeType::ConversationGroup:
            return QStringLiteral("conversation_group");
        case HeartbeatScopeType::Folder:
            return QStringLiteral("folder");
    }
    // Defensive fallback. The enum is closed; this branch is unreachable
    // unless someone adds a new value without updating this switch.
    return QStringLiteral("conversation_1to1");
}

HeartbeatScopeType heartbeatScopeTypeFromString(const QString& s) {
    if (s == QStringLiteral("conversation_group"))
        return HeartbeatScopeType::ConversationGroup;
    if (s == QStringLiteral("folder"))
        return HeartbeatScopeType::Folder;
    // Includes 'conversation_1to1' and any unknown string. The CHECK
    // constraint in applySchemaV10 should make unknown strings
    // impossible at the DB layer; this default keeps the function
    // total-typed for callers.
    return HeartbeatScopeType::Conversation1to1;
}

/**
 * @brief Constructs a HeartbeatConfig from a SELECT * row.
 *
 * Fields whose schema columns have NOT NULL DEFAULT '' / 0 are read
 * via QVariant::toString() / toInt() / toBool(). Those calls return
 * the appropriate zero value if the column is somehow null (defensive
 * for forward-compatibility with schemas where a column might be
 * relaxed to nullable).
 */
HeartbeatConfig HeartbeatConfig::fromSqlRecord(const QSqlRecord& record) {
    HeartbeatConfig cfg;
    cfg.id = record.value(QStringLiteral("id")).toString();
    cfg.agentId = record.value(QStringLiteral("agent_id")).toString();
    cfg.scopeType =
        heartbeatScopeTypeFromString(record.value(QStringLiteral("scope_type")).toString());
    cfg.scopeId = record.value(QStringLiteral("scope_id")).toString();
    cfg.alias = record.value(QStringLiteral("alias")).toString();

    cfg.enabled = record.value(QStringLiteral("enabled")).toBool();
    cfg.schedule = record.value(QStringLiteral("schedule")).toString();
    cfg.goal = record.value(QStringLiteral("goal")).toString();
    cfg.surfaceCriteria = record.value(QStringLiteral("surface_criteria")).toString();
    cfg.maxRunsPerDay = record.value(QStringLiteral("max_runs_per_day")).toInt();
    cfg.autoSurfaceTargetConversationId =
        record.value(QStringLiteral("auto_surface_target_conversation_id")).toString();
    cfg.selfConfigAllowed = record.value(QStringLiteral("self_config_allowed")).toBool();

    cfg.lastFireAt = record.value(QStringLiteral("last_fire_at")).toString();
    cfg.lastFireOutcome = record.value(QStringLiteral("last_fire_outcome")).toString();

    // created_at / updated_at are stored as Qt::ISODateWithMs strings
    // in the schema (consistent with conversations / messages). Parse
    // back into QDateTime; on parse failure leave the field default.
    {
        const QString s = record.value(QStringLiteral("created_at")).toString();
        if (!s.isEmpty()) {
            cfg.createdAt = QDateTime::fromString(s, Qt::ISODateWithMs);
        }
    }
    {
        const QString s = record.value(QStringLiteral("updated_at")).toString();
        if (!s.isEmpty()) {
            cfg.updatedAt = QDateTime::fromString(s, Qt::ISODateWithMs);
        }
    }
    return cfg;
}

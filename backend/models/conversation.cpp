// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation.cpp
 * @brief JSON and SQL serialization for the Conversation data model.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#include "conversation.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlRecord>

/**
 * @brief Serializes this conversation to a JSON object.
 * @return QJsonObject with all fields; timestamps stored as Unix ms integers.
 */
QJsonObject Conversation::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("title")] = title;
    obj[QStringLiteral("folder_id")] = folderId;
    obj[QStringLiteral("created_at")] = createdAt.toMSecsSinceEpoch();
    obj[QStringLiteral("updated_at")] = updatedAt.toMSecsSinceEpoch();
    obj[QStringLiteral("system_prompt")] = systemPrompt;
    obj[QStringLiteral("llm_config")] = llmConfig;
    obj[QStringLiteral("token_total")] = tokenTotal;
    obj[QStringLiteral("primary_agent_id")] = primaryAgentId;
    obj[QStringLiteral("is_group")] = isGroup;
    QJsonArray arr;
    for (const QString& a : groupAgentIds)
        arr.append(a);
    obj[QStringLiteral("group_agent_ids")] = arr;
    obj[QStringLiteral("is_pinned")] = isPinned;
    obj[QStringLiteral("member_alias")] = memberAlias;
    return obj;
}

/*
 * @brief Deserializes a conversation from a JSON object.
 * @param json JSON object with conversation fields.
 * @return Populated Conversation struct.
 */
Conversation Conversation::fromJson(const QJsonObject& json) {
    Conversation c;
    c.id = json[QStringLiteral("id")].toString();
    c.title = json[QStringLiteral("title")].toString();
    c.folderId = json[QStringLiteral("folder_id")].toString();
    c.createdAt = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(json[QStringLiteral("created_at")].toDouble()));
    c.updatedAt = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(json[QStringLiteral("updated_at")].toDouble()));
    c.systemPrompt = json[QStringLiteral("system_prompt")].toString();
    c.llmConfig = json[QStringLiteral("llm_config")].toObject();
    c.tokenTotal = json[QStringLiteral("token_total")].toInt();
    c.primaryAgentId = json[QStringLiteral("primary_agent_id")].toString();
    c.isGroup = json[QStringLiteral("is_group")].toBool();
    const QJsonArray arr = json[QStringLiteral("group_agent_ids")].toArray();
    for (const QJsonValue& v : arr)
        c.groupAgentIds.append(v.toString());
    c.isPinned = json[QStringLiteral("is_pinned")].toBool();
    c.memberAlias = json[QStringLiteral("member_alias")].toString();
    return c;
}

/*
 * @brief Constructs a Conversation from a QSqlRecord row.
 * @param record SQL record from SELECT on the conversations table.
 * @return Populated Conversation struct.
 */
Conversation Conversation::fromSqlRecord(const QSqlRecord& record) {
    Conversation c;
    c.id = record.value(QStringLiteral("id")).toString();
    c.title = record.value(QStringLiteral("title")).toString();
    c.folderId = record.value(QStringLiteral("folder_id")).toString();
    c.createdAt =
        QDateTime::fromMSecsSinceEpoch(record.value(QStringLiteral("created_at")).toLongLong());
    c.updatedAt =
        QDateTime::fromMSecsSinceEpoch(record.value(QStringLiteral("updated_at")).toLongLong());
    c.systemPrompt = record.value(QStringLiteral("system_prompt")).toString();

    // llm_config stored as JSON text in the database
    const QString llmConfigStr = record.value(QStringLiteral("llm_config")).toString();
    if (!llmConfigStr.isEmpty()) {
        c.llmConfig = QJsonDocument::fromJson(llmConfigStr.toUtf8()).object();
    }

    c.tokenTotal = record.value(QStringLiteral("token_total")).toInt();

    // Agent fields (schema v2+; may be absent on very old DBs)
    if (record.contains(QStringLiteral("primary_agent_id"))) {
        c.primaryAgentId = record.value(QStringLiteral("primary_agent_id")).toString();
    }
    if (record.contains(QStringLiteral("is_group"))) {
        c.isGroup = record.value(QStringLiteral("is_group")).toInt() != 0;
    }
    // Group chat fields (schema v3+)
    if (record.contains(QStringLiteral("group_agent_ids"))) {
        const QString gaJson = record.value(QStringLiteral("group_agent_ids")).toString();
        if (!gaJson.isEmpty()) {
            const QJsonArray arr = QJsonDocument::fromJson(gaJson.toUtf8()).array();
            for (const QJsonValue& v : arr)
                c.groupAgentIds.append(v.toString());
        }
    }
    // Pin flag (schema v9+) — guard for older DBs that haven't migrated
    // yet. INTEGER NOT NULL DEFAULT 0 means a non-empty value is always
    // present once the migration runs; the contains() guard only matters
    // if a SELECT happens against a v8-or-older snapshot in tests.
    if (record.contains(QStringLiteral("is_pinned"))) {
        c.isPinned = record.value(QStringLiteral("is_pinned")).toInt() != 0;
    }
    // member_alias (schema v14+) — nullable; contains() guard for
    // SELECTs against a pre-v14 snapshot in tests.
    if (record.contains(QStringLiteral("member_alias"))) {
        c.memberAlias = record.value(QStringLiteral("member_alias")).toString();
    }
    return c;
}


bool Conversation::allowHeartbeatAutoSurface() const {
    return LlmConfig::fromJson(llmConfig).allowHeartbeatAutoSurface;
}

int Conversation::heartbeatAutoSurfaceMaxPerDay() const {
    return LlmConfig::fromJson(llmConfig).autoSurfaceMaxPerDay;
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file message.cpp
 * @brief JSON and SQL serialization for the Message data model.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#include "message.h"

#include <QJsonDocument>
#include <QSet>
#include <QSqlRecord>

/**
 * @brief Serializes this message to a JSON object.
 * @return QJsonObject with all fields; timestamps as Unix ms integers.
 */
QJsonObject Message::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("conversation_id")] = conversationId;
    obj[QStringLiteral("role")] = role;
    obj[QStringLiteral("content")] = content;
    obj[QStringLiteral("content_html")] = contentHtml;
    obj[QStringLiteral("created_at")] = createdAt.toMSecsSinceEpoch();
    obj[QStringLiteral("token_count")] = tokenCount;
    obj[QStringLiteral("model_used")] = modelUsed;
    obj[QStringLiteral("finish_reason")] = finishReason;
    obj[QStringLiteral("metadata")] = metadata;
    obj[QStringLiteral("agent_id")] = agentId;
    obj[QStringLiteral("member_alias")] = memberAlias;
    obj[QStringLiteral("turn_id")] = turnId;
    obj[QStringLiteral("thinking_content")] = thinkingContent;
    return obj;
}

/**
 * @brief Deserializes a message from a JSON object.
 * @param json JSON object with message fields.
 * @return Populated Message struct.
 */
Message Message::fromJson(const QJsonObject& json) {
    Message m;
    m.id = json[QStringLiteral("id")].toString();
    m.conversationId = json[QStringLiteral("conversation_id")].toString();
    m.role = json[QStringLiteral("role")].toString();
    m.content = json[QStringLiteral("content")].toString();
    m.contentHtml = json[QStringLiteral("content_html")].toString();
    m.createdAt = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(json[QStringLiteral("created_at")].toDouble()));
    m.tokenCount = json[QStringLiteral("token_count")].toInt();
    m.modelUsed = json[QStringLiteral("model_used")].toString();
    m.finishReason = json[QStringLiteral("finish_reason")].toString();
    m.metadata = json[QStringLiteral("metadata")].toObject();
    m.agentId = json[QStringLiteral("agent_id")].toString();
    m.memberAlias = json[QStringLiteral("member_alias")].toString();
    m.turnId = json[QStringLiteral("turn_id")].toString();
    m.thinkingContent = json[QStringLiteral("thinking_content")].toString();
    return m;
}

/**
 * @brief Constructs a Message from a QSqlRecord row.
 * @param record SQL record from SELECT on the messages table.
 * @return Populated Message struct.
 */
Message Message::fromSqlRecord(const QSqlRecord& record) {
    Message m;
    m.id = record.value(QStringLiteral("id")).toString();
    m.conversationId = record.value(QStringLiteral("conversation_id")).toString();
    m.role = record.value(QStringLiteral("role")).toString();
    m.content = record.value(QStringLiteral("content")).toString();
    m.contentHtml = record.value(QStringLiteral("content_html")).toString();
    m.createdAt =
        QDateTime::fromMSecsSinceEpoch(record.value(QStringLiteral("created_at")).toLongLong());
    m.tokenCount = record.value(QStringLiteral("token_count")).toInt();
    m.modelUsed = record.value(QStringLiteral("model_used")).toString();
    m.finishReason = record.value(QStringLiteral("finish_reason")).toString();

    const QString metaStr = record.value(QStringLiteral("metadata")).toString();
    if (!metaStr.isEmpty()) {
        m.metadata = QJsonDocument::fromJson(metaStr.toUtf8()).object();
    }

    // Schema v2+ field — guard in case of older schemas
    if (record.contains(QStringLiteral("agent_id"))) {
        m.agentId = record.value(QStringLiteral("agent_id")).toString();
    }
    // Schema v4+ field
    if (record.contains(QStringLiteral("member_alias"))) {
        m.memberAlias = record.value(QStringLiteral("member_alias")).toString();
    }
    // Schema v6+ field — null on legacy rows that the v6 migration
    // hasn't touched yet, populated otherwise.
    if (record.contains(QStringLiteral("turn_id"))) {
        m.turnId = record.value(QStringLiteral("turn_id")).toString();
    }
    // Schema v16+ field — added for the thinking-content sidecar.
    // Defaults to empty string on every row at migration time so the
    // NOT NULL constraint is satisfied; explicit projection here so
    // SELECT * paths that build a Message via this converter still
    // populate the field without relying on column ordering.
    if (record.contains(QStringLiteral("thinking_content"))) {
        m.thinkingContent = record.value(QStringLiteral("thinking_content")).toString();
    }

    return m;
}

/**
 * @brief Returns true if this message has a valid ID and a recognized role.
 * @return true if id is non-empty and role is one of the allowed values.
 */
bool Message::isValid() const {
    static const QSet<QString> validRoles = {QStringLiteral("user"),
                                             QStringLiteral("assistant"),
                                             QStringLiteral("system"),
                                             QStringLiteral("tool")};
    return !id.isEmpty() && validRoles.contains(role);
}

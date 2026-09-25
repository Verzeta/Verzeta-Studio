// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-call.cpp
 * @brief JSON and SQL serialization for the ToolCall data model.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#include "tool-call.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSqlRecord>

/**
 * @brief Serializes this tool call to a JSON object.
 * @return QJsonObject with all fields; timestamps as Unix ms.
 */
QJsonObject ToolCall::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("message_id")] = messageId;
    obj[QStringLiteral("tool_name")] = toolName;
    obj[QStringLiteral("arguments")] = arguments;
    obj[QStringLiteral("result")] = result;
    obj[QStringLiteral("status")] = status;
    obj[QStringLiteral("started_at")] = startedAt.isValid()
                                            ? QJsonValue(startedAt.toMSecsSinceEpoch())
                                            : QJsonValue(QJsonValue::Null);
    obj[QStringLiteral("completed_at")] = completedAt.isValid()
                                              ? QJsonValue(completedAt.toMSecsSinceEpoch())
                                              : QJsonValue(QJsonValue::Null);
    return obj;
}

/**
 * @brief Deserializes a tool call from a JSON object.
 * @param json JSON object with tool call fields.
 * @return Populated ToolCall struct.
 */
ToolCall ToolCall::fromJson(const QJsonObject& json) {
    ToolCall tc;
    tc.id = json[QStringLiteral("id")].toString();
    tc.messageId = json[QStringLiteral("message_id")].toString();
    tc.toolName = json[QStringLiteral("tool_name")].toString();
    tc.arguments = json[QStringLiteral("arguments")].toObject();
    tc.result = json[QStringLiteral("result")];
    tc.status = json[QStringLiteral("status")].toString();

    const auto startedVal = json[QStringLiteral("started_at")];
    if (!startedVal.isNull()) {
        tc.startedAt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(startedVal.toDouble()));
    }
    const auto completedVal = json[QStringLiteral("completed_at")];
    if (!completedVal.isNull()) {
        tc.completedAt =
            QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(completedVal.toDouble()));
    }
    return tc;
}

/**
 * @brief Constructs a ToolCall from a QSqlRecord row.
 * @param record SQL record from SELECT on the tool_calls table.
 * @return Populated ToolCall struct.
 */
ToolCall ToolCall::fromSqlRecord(const QSqlRecord& record) {
    ToolCall tc;
    tc.id = record.value(QStringLiteral("id")).toString();
    tc.messageId = record.value(QStringLiteral("message_id")).toString();
    tc.toolName = record.value(QStringLiteral("tool_name")).toString();

    const QString argsStr = record.value(QStringLiteral("arguments")).toString();
    if (!argsStr.isEmpty()) {
        tc.arguments = QJsonDocument::fromJson(argsStr.toUtf8()).object();
    }

    const QString resultStr = record.value(QStringLiteral("result")).toString();
    if (!resultStr.isEmpty()) {
        const QJsonDocument resultDoc = QJsonDocument::fromJson(resultStr.toUtf8());
        if (resultDoc.isObject()) {
            tc.result = resultDoc.object();
        } else if (resultDoc.isArray()) {
            tc.result = resultDoc.array();
        } else {
            tc.result = resultStr;  // Treat as plain string if not valid JSON
        }
    }

    tc.status = record.value(QStringLiteral("status")).toString();

    const QVariant startedVal = record.value(QStringLiteral("started_at"));
    if (!startedVal.isNull()) {
        tc.startedAt = QDateTime::fromMSecsSinceEpoch(startedVal.toLongLong());
    }
    const QVariant completedVal = record.value(QStringLiteral("completed_at"));
    if (!completedVal.isNull()) {
        tc.completedAt = QDateTime::fromMSecsSinceEpoch(completedVal.toLongLong());
    }
    // plan_step_id is an optional v7 column. indexOf returns -1 on
    // older rows (before ALTER TABLE) — treat that as NULL.
    if (record.indexOf(QStringLiteral("plan_step_id")) >= 0) {
        tc.planStepId = record.value(QStringLiteral("plan_step_id")).toString();
    }
    return tc;
}

/**
 * @brief Returns true if this tool call has a valid ID and a recognized status.
 * @return true if id is non-empty and status is one of the allowed values.
 */
bool ToolCall::isValid() const {
    static const QSet<QString> validStatuses = {QStringLiteral("pending"),
                                                QStringLiteral("running"),
                                                QStringLiteral("success"),
                                                QStringLiteral("error")};
    return !id.isEmpty() && validStatuses.contains(status);
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-call.h
 * @brief Data model for LLM tool invocations linked to messages.
 *        Records tool name, arguments, result, and execution status.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

class QSqlRecord;

/**
 * @brief Data model representing a single tool call made by an LLM.
 *
 * Tool calls are stored linked to assistant messages. When the LLM emits a
 * `tool_calls` finish reason, each tool call is written to the database with
 * status "pending". After execution, status transitions to "success" or "error".
 *
 * Status lifecycle: pending → running → success | error
 */
struct ToolCall {
    QString id;             ///< UUID v4 primary key
    QString messageId;      ///< References messages.id
    QString toolName;       ///< Name of the tool (e.g., "run_shell", "read_file")
    QJsonObject arguments;  ///< JSON arguments object
    QJsonValue result;      ///< JSON result (null if pending/running)
    QString status;         ///< "pending" | "running" | "success" | "error"
    QDateTime startedAt;    ///< UTC timestamp when execution began
    QDateTime completedAt;  ///< UTC timestamp when execution finished
    QString planStepId;  ///< Nullable: plan step id if this tool was run during an active task step

    /**
     * @brief Serializes this tool call to a JSON object.
     * @return QJsonObject with all fields.
     */
    QJsonObject toJson() const;

    /**
     * @brief Deserializes a tool call from a JSON object.
     * @param json JSON object with tool call fields.
     * @return Populated ToolCall struct.
     */
    static ToolCall fromJson(const QJsonObject& json);

    /**
     * @brief Constructs a ToolCall from a QSqlRecord row.
     * @param record SQL record from SELECT on the tool_calls table.
     * @return Populated ToolCall struct.
     */
    static ToolCall fromSqlRecord(const QSqlRecord& record);

    /**
     * @brief Returns true if this tool call has a valid ID and status.
     * @return true if id and status are non-empty.
     */
    bool isValid() const;
};

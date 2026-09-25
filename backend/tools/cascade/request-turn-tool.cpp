// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file request-turn-tool.cpp
 * @brief Implementation of Tools::RequestTurnTool.
 * @layer Service (Tool subsystem)
 * @dependencies api/tool-calling-schema.h.
 */

#include "request-turn-tool.h"

#include <QJsonObject>

namespace Tools {

RequestTurnTool::RequestTurnTool() = default;

QString RequestTurnTool::name() const {
    return QStringLiteral("request_turn");
}

QString RequestTurnTool::description() const {
    return QStringLiteral("Request that a teammate (or all teammates) takes the next "
                          "turn in the conversation. Use this when you want another "
                          "agent to respond. Supports \"all\" to broadcast to everyone. "
                          "Do NOT use @mentions in your text to trigger responses — "
                          "text mentions are for attribution only and do not route.");
}

QList<ToolParameterSchema> RequestTurnTool::parameters() const {
    ToolParameterSchema alias;
    alias.name = QStringLiteral("alias");
    alias.type = QStringLiteral("string");
    alias.description = QStringLiteral("The teammate's alias (e.g. \"Engineer\", \"PM_Alice\") "
                                       "or \"all\" to broadcast to everyone.");
    alias.required = true;

    ToolParameterSchema context;
    context.name = QStringLiteral("context");
    context.type = QStringLiteral("string");
    context.description = QStringLiteral("Optional brief reason for requesting their turn.");
    context.required = false;

    return {alias, context};
}

bool RequestTurnTool::runsOnMainThread() const {
    return false;
}

QJsonValue RequestTurnTool::invoke(const QJsonObject& args) {
    QString target = args[QStringLiteral("alias")].toString().trimmed();
    if (target.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("alias is required")}};
    }
    if (target.startsWith(QLatin1Char('@'))) {
        target = target.mid(1).trimmed();
    }
    QJsonObject result;
    result[QStringLiteral("requested")] = target;
    result[QStringLiteral("status")] = QStringLiteral("queued");
    return result;
}

}  // namespace Tools

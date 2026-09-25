// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file remember-tool.cpp
 * @brief Implementation of RememberTool, the memory SAVE tool.
 * @layer Service (Tool subsystem)
 * @dependencies AgentMemoryService
 */

#include "remember-tool.h"

#include "services/agent-memory-service.h"

namespace Tools {

RememberTool::RememberTool(AgentMemoryService& memory) : m_memory(memory) {}

QString RememberTool::name() const {
    return QStringLiteral("remember");
}

QString RememberTool::description() const {
    return QStringLiteral("Save a durable fact to your long-term memory so you recall it in future "
                          "turns and conversations (e.g. a user preference, a decision, a stable "
                          "detail). Put the fact to remember in `text`. You don't need to recall "
                          "explicitly — relevant memories are surfaced to you automatically.");
}

QList<ToolParameterSchema> RememberTool::parameters() const {
    ToolParameterSchema text;
    text.name = QStringLiteral("text");
    text.type = QStringLiteral("string");
    text.description = QStringLiteral("The durable fact to remember.");
    text.required = true;
    return {text};
}

bool RememberTool::runsOnMainThread() const {
    return true;  // AgentMemoryService touches SQLite (per-thread).
}

QJsonValue RememberTool::invoke(const QJsonObject& args) {
    const QString text = args.value(QStringLiteral("text")).toString().trimmed();

    // Scope from the calling turn's identity: the responder agent (shared
    // per-agent memory) or, in an agentless direct chat, the conversation.
    // Never the active VIEW (a turn may run for a background conversation).
    const QString callerAgentId = args.value(QStringLiteral("__caller_agent_id")).toString();
    const QString callerConvId = args.value(QStringLiteral("__caller_conv_id")).toString();
    QString scope;
    if (!callerAgentId.isEmpty()) {
        scope = QStringLiteral("agent:%1").arg(callerAgentId);
    } else if (!callerConvId.isEmpty()) {
        scope = QStringLiteral("conversation:%1").arg(callerConvId);
    }
    if (scope.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"),
                            QStringLiteral("Could not resolve who is calling — no memory "
                                           "scope available.")}};
    }
    if (text.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("Nothing to remember: `text` is empty.")}};
    }

    const QString id = m_memory.save(text, scope, callerConvId);
    if (id.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("Failed to save the memory.")}};
    }
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), id}};
}

}  // namespace Tools

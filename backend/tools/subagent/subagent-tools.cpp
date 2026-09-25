// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file subagent-tools.cpp
 * @brief Implementation of spawn_subagent / check_subagent. See the
 *        header + SubagentRunService for the locked contract.
 * @layer Tool
 * @dependencies SubagentRunService, Qt6::Core.
 */

#include "subagent-tools.h"

#include "../../api/llm-interface.h"
#include "../../services/subagent-run-service.h"
#include "../../utils/http-client.h"

#include <QJsonArray>
#include <QJsonObject>

namespace Tools {

// ---------------------------------------------------------------------------
// spawn_subagent
// ---------------------------------------------------------------------------

QString SpawnSubagentTool::name() const {
    return QStringLiteral("spawn_subagent");
}

QString SpawnSubagentTool::description() const {
    return QStringLiteral("Delegate a self-contained task to a private sub-agent that "
                          "works in the background with its own tools and reports back "
                          "when finished. Returns immediately with a run_id; the result "
                          "arrives in the conversation automatically — do NOT busy-poll. "
                          "Use for research, file reading/summarising, or any scoped "
                          "side-quest you don't need to do yourself. The sub-agent "
                          "cannot see this conversation; put everything it needs into "
                          "the task text.");
}

QList<ToolParameterSchema> SpawnSubagentTool::parameters() const {
    ToolParameterSchema task;
    task.name = QStringLiteral("task");
    task.type = QStringLiteral("string");
    task.description =
        QStringLiteral("Complete, self-contained task description. Include all "
                       "context the worker needs — it cannot see this conversation.");
    task.required = true;

    ToolParameterSchema tools;
    tools.name = QStringLiteral("tools");
    tools.type = QStringLiteral("array");
    tools.description = QStringLiteral("Optional tool whitelist for the sub-agent. Default: "
                                       "read-only set (search_web, read_file, list_files, "
                                       "read_conversation, get_current_time). Add write_file "
                                       "explicitly only when the task requires writing.");
    tools.required = false;
    tools.additionalProps = QJsonObject{
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}};

    return {task, tools};
}

QJsonValue SpawnSubagentTool::invoke(const QJsonObject& args) {
    const QString task = args.value(QStringLiteral("task")).toString();
    if (task.trimmed().isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("task is required")}};
    }
    QStringList whitelist;
    for (const QJsonValue& v : args.value(QStringLiteral("tools")).toArray()) {
        const QString t = v.toString().trimmed();
        if (!t.isEmpty())
            whitelist.append(t);
    }
    // Host-injected caller identity (same contract as the file tools).
    const QString convId = args.value(QStringLiteral("__caller_conv_id")).toString();
    const QString alias = args.value(QStringLiteral("__caller_agent_alias")).toString();

    if (convId.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"),
                            QStringLiteral("no conversation context — sub-agents "
                                           "can only be spawned from a chat turn")}};
    }

    const QString runId =
        m_service.spawn(convId, /*parentMsgId*/ QString(), alias, task, whitelist);
    if (runId.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"),
                            QStringLiteral("spawn failed — no provider/model "
                                           "available for the sub-agent")}};
    }
    const auto r = m_service.run(runId);
    return QJsonObject{
        {QStringLiteral("run_id"), runId},
        {QStringLiteral("status"), r.has_value() ? r->status : QStringLiteral("queued")},
        {QStringLiteral("note"),
         QStringLiteral("Sub-agent started. Its report will be "
                        "posted to this conversation when it "
                        "finishes — continue with other work.")},
    };
}

// ---------------------------------------------------------------------------
// check_subagent
// ---------------------------------------------------------------------------

QString CheckSubagentTool::name() const {
    return QStringLiteral("check_subagent");
}

QString CheckSubagentTool::description() const {
    return QStringLiteral("Check the status of a sub-agent run you spawned earlier. "
                          "Returns status (queued | running | done | failed | "
                          "cancelled) and, when finished, the result. Normally "
                          "unnecessary — results are delivered automatically.");
}

QList<ToolParameterSchema> CheckSubagentTool::parameters() const {
    ToolParameterSchema id;
    id.name = QStringLiteral("run_id");
    id.type = QStringLiteral("string");
    id.description = QStringLiteral("The run_id from spawn_subagent.");
    id.required = true;
    return {id};
}

QJsonValue CheckSubagentTool::invoke(const QJsonObject& args) {
    const QString runId = args.value(QStringLiteral("run_id")).toString();
    const auto r = m_service.run(runId);
    if (!r.has_value()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("unknown run_id")}};
    }
    QJsonObject out{
        {QStringLiteral("run_id"), runId},
        {QStringLiteral("status"), r->status},
        {QStringLiteral("task"), r->task},
    };
    if (r->status == QStringLiteral("done")) {
        out[QStringLiteral("result")] = r->resultText;
    } else if (!r->failReason.isEmpty()) {
        out[QStringLiteral("fail_reason")] = r->failReason;
    }
    return out;
}

}  // namespace Tools

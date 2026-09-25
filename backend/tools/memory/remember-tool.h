// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file remember-tool.h
 * @brief The agent-facing memory SAVE tool: store a durable fact. Recall is not
 *        a tool; it is surfaced automatically pre-turn by MemoryRetriever.
 * @layer Service (Tool subsystem)
 * @dependencies AgentMemoryService
 */

#pragma once

#include "tools/itool.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>

class AgentMemoryService;

namespace Tools {

/**
 * @brief `remember(text)`: saves a durable fact to the responder's long-term
 *        memory.
 *
 * A single-purpose save tool (one `text` parameter) keeps the agent tool surface
 * minimal. Recall is NOT a tool: relevant memories are injected automatically
 * before each turn by the MemoryRetriever coordinator (a synchronous tool can't
 * do semantic recall without blocking the UI). Saving is gated per agent by the
 * normal `allowed_tools` whitelist, like every other tool; the entry is scoped
 * `agent:<callerAgentId>` (the agent's own memory) or, for an agentless direct
 * chat, `conversation:<callerConvId>`, resolved from the dispatcher-injected
 * caller identity. Runs on the main thread (SQLite); the embed is async.
 */
class RememberTool : public ITool {
  public:
    /**
     * @brief Constructs the tool over the agent-memory service.
     * @param memory Reference to the per-agent memory service (outlives the tool).
     */
    explicit RememberTool(AgentMemoryService& memory);

    /** @brief The tool name exposed to the model. @returns "remember". */
    QString name() const override;

    /** @brief Model-facing description. @returns the description. */
    QString description() const override;

    /** @brief Parameter schema (just `text`). @returns the params. */
    QList<ToolParameterSchema> parameters() const override;

    /** @brief Memory touches SQLite. @returns true (must run on the main thread). */
    bool runsOnMainThread() const override;

    /**
     * @brief Saves `text` as a durable memory scoped to the calling responder.
     * @param args JSON args: `text`, plus the dispatcher-injected
     *             `__caller_agent_id` / `__caller_conv_id` used to derive scope.
     * @returns `{ "ok": true, "id": \<id\> }` on success; otherwise an object with
     *          an `"error"` string (no caller identity / empty text / failure).
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    AgentMemoryService& m_memory;
};

}  // namespace Tools

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file subagent-tools.h
 * @brief The agent-visible sub-agent tool family:
 *        `spawn_subagent` (delegate a scoped task to a private worker)
 *        and `check_subagent` (poll a run's status/result mid-turn).
 * @layer Tool
 * @dependencies ITool, SubagentRunService.
 *
 * Terminology pin: agents see ONLY these two
 * tool names. Service/table/signal names never reach the agent.
 */

#pragma once

#include "../itool.h"

class SubagentRunService;

namespace Tools {

/** @brief ITool implementation for the `spawn_subagent` built-in. */
class SpawnSubagentTool : public ITool {
  public:
    /**
     * @brief Constructs the tool over the shared run service.
     * @param service Run orchestrator; non-owning (AppController owns).
     */
    explicit SpawnSubagentTool(SubagentRunService& service) : m_service(service) {}

    /**
     * @brief Tool identifier exposed to the model.
     * @returns The literal string "spawn_subagent".
     */
    QString name() const override;

    /**
     * @brief Usage description embedded in the tool prompt.
     * @returns Prose explaining delegation semantics (async spawn,
     *          report delivered to the conversation, no busy-polling).
     */
    QString description() const override;

    /**
     * @brief Declares the tool-call argument schema.
     * @returns Schema for `task` (required) and `tools` (optional
     *          whitelist array).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread affinity. The run service is main-thread
     *        orchestrated.
     * @returns Always true.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Validates arguments and queues a new sub-agent run.
     * @param args Tool-call arguments plus host-injected
     *             `__caller_conv_id` / `__caller_agent_alias`.
     * @returns JSON object with run_id + status on success, or an
     *          object with an `error` key on validation/spawn failure.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    SubagentRunService& m_service;  ///< Non-owning; AppController owns.
};

/** @brief ITool implementation for the `check_subagent` built-in. */
class CheckSubagentTool : public ITool {
  public:
    /**
     * @brief Constructs the tool over the shared run service.
     * @param service Run orchestrator; non-owning (AppController owns).
     */
    explicit CheckSubagentTool(SubagentRunService& service) : m_service(service) {}

    /**
     * @brief Tool identifier exposed to the model.
     * @returns The literal string "check_subagent".
     */
    QString name() const override;

    /**
     * @brief Usage description embedded in the tool prompt.
     * @returns Prose explaining when to poll a run's status.
     */
    QString description() const override;

    /**
     * @brief Declares the tool-call argument schema.
     * @returns Schema for `run_id` (required).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread affinity. The run service is main-thread
     *        orchestrated.
     * @returns Always true.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Looks up a run and reports its current status/result.
     * @param args Tool-call arguments; requires `run_id`.
     * @returns JSON object with status (+ result when terminal), or
     *          an object with an `error` key for unknown ids.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    SubagentRunService& m_service;  ///< Non-owning; AppController owns.
};

}  // namespace Tools

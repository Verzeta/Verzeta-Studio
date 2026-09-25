// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file multi-agent-coordinator.h
 * @brief Coordinates sequential sub-agent execution for the Multi-Agent
 *        pattern. Presents a parallel-execution interface (runParallel) while
 *        managing sub-agents via a shared ModelRouter to avoid thread conflicts.
 * @layer Worker
 * @dependencies AgentService (Service), Qt6::Core
 */


#pragma once

#include "../api/llm-interface.h"
#include "../services/agent-service.h"

#include <atomic>
#include <QList>
#include <QObject>
#include <QString>

// Forward declarations
class ModelRouter;
class ToolService;

/**
 * @brief Coordinates multiple sub-agent tasks for the Multi-Agent pattern.
 *
 * ## Usage
 *
 * 1. Construct with optional parent.
 * 2. Connect signals (subAgentStarted, subAgentCompleted, allCompleted).
 * 3. Call runParallel() with the list of sub-tasks.
 * 4. allCompleted() fires when all sub-agents finish.
 *
 * ## Threading
 *
 * All sub-agents run on the MAIN THREAD through the shared ModelRouter.
 * Sub-agents are dispatched sequentially. The runParallel() name reflects
 * the intended multi-agent interface; sequential execution is a known limitation
 * of sharing a single ModelRouter instance.
 */
class MultiAgentCoordinator : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief A single sub-agent task description.
     */
    struct SubAgentTask {
        QString taskId;       ///< Unique identifier for this task
        QString description;  ///< Human-readable description
        LlmRequest request;   ///< LLM request for this sub-agent
        AgentConfig config;   ///< Agent config (typically Direct pattern)
    };

    /**
     * @brief Result from a completed sub-agent.
     */
    struct SubAgentResult {
        QString taskId;        ///< Matches SubAgentTask::taskId
        QString description;   ///< Matches SubAgentTask::description
        QString result;        ///< Final answer from the sub-agent
        bool success = false;  ///< True when the sub-agent finished without error.
        int tokens = 0;        ///< Not currently set, so always 0.
    };

    /**
     * @brief Constructs MultiAgentCoordinator.
     * @param parent Optional Qt parent.
     */
    explicit MultiAgentCoordinator(QObject* parent = nullptr);

    /**
     * @brief Destroys the coordinator. Cancels any running sub-agents.
     */
    ~MultiAgentCoordinator() override;

    /**
     * @brief Runs sub-agent tasks, dispatching them through the provided services.
     *
     * Sub-agents run sequentially through the shared ModelRouter. Up to maxParallel
     * is respected as a future concurrency hint; currently all tasks run in order.
     *
     * @param tasks      List of sub-agent tasks to execute.
     * @param router     ModelRouter for LLM dispatch.
     * @param tools      ToolService for tool invocation.
     * @param rag        RagService for memory (if MemoryAugmented sub-agents).
     * @param maxParallel Maximum concurrent sub-agents (currently advisory only).
     * @sideeffects Creates temporary AgentService instances per sub-task.
     *              Emits subAgentStarted / subAgentCompleted for each task.
     *              Emits allCompleted when all tasks finish.
     *
     * Returns after starting the first sub-agent; each remaining one starts
     * when the previous completes. An empty task list emits allCompleted()
     * immediately with no results.
     */
    void runParallel(const QList<SubAgentTask>& tasks,
                     ModelRouter& router,
                     ToolService& tools,
                     RagService& rag,
                     int maxParallel = 3);

    /**
     * @brief Cancels all pending sub-agents.
     * @sideeffects Sets cancel flag. Current sub-agent will be cancelled at its
     *              next iteration. Remaining tasks are skipped. allCompleted()
     *              is NOT emitted after cancel.
     */
    void cancelAll();

  signals:
    /**
     * @brief Emitted when a sub-agent task starts.
     * @param taskId      Task identifier.
     * @param description Task description.
     */
    void subAgentStarted(const QString& taskId, const QString& description);

    /**
     * @brief Emitted when a sub-agent task completes.
     * @param result Completion result.
     */
    void subAgentCompleted(const SubAgentResult& result);

    /**
     * @brief Emitted when all sub-agent tasks have completed.
     * @param results List of all results in execution order.
     */
    void allCompleted(const QList<SubAgentResult>& results);

  private:
    std::atomic<bool> m_cancel{false};
    QList<SubAgentTask> m_tasks;
    QList<SubAgentResult> m_results;
    int m_currentIdx = 0;

    // Non-owning references to services (set in runParallel)
    ModelRouter* m_router = nullptr;
    ToolService* m_tools = nullptr;
    RagService* m_rag = nullptr;

    /**
     * @brief Starts the sub-agent at m_currentIdx.
     */
    void runNext();

    /**
     * @brief Returns true if the coordinator has not been cancelled and is active.
     * @return true if cancel not set and services are wired.
     */
    bool m_isRunning() const;
};

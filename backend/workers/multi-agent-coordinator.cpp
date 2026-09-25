// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file multi-agent-coordinator.cpp
 * @brief Implementation of MultiAgentCoordinator. Runs sub-agent tasks
 *        sequentially through a shared ModelRouter, collecting results and emitting
 *        allCompleted() when all tasks finish.
 * @layer Worker
 * @dependencies AgentService (Service), ModelRouter (Service), Qt6::Core
 */


#include "multi-agent-coordinator.h"

#include "../services/agent-service.h"
#include "../services/model-router.h"
#include "../services/rag-service.h"
#include "../services/tool-service.h"
#include "../utils/logger.h"

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

/**
 * @brief Constructs MultiAgentCoordinator.
 * @param parent Optional Qt parent.
 */
MultiAgentCoordinator::MultiAgentCoordinator(QObject* parent) : QObject(parent) {}

/**
 * @brief Destroys MultiAgentCoordinator.
 */
MultiAgentCoordinator::~MultiAgentCoordinator() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/*
 * @brief Runs sub-agent tasks sequentially through the provided services.
 * @param tasks      List of sub-agent tasks to execute.
 * @param router     ModelRouter for LLM dispatch.
 * @param tools      ToolService for tool invocation.
 * @param rag        RagService for memory-augmented sub-agents.
 * @param maxParallel Advisory concurrency limit (not currently enforced).
 * @sideeffects Resets state, stores references, starts first sub-agent.
 */
void MultiAgentCoordinator::runParallel(const QList<SubAgentTask>& tasks,
                                        ModelRouter& router,
                                        ToolService& tools,
                                        RagService& rag,
                                        int maxParallel) {
    Q_UNUSED(maxParallel)  // Sequential implementation; max parallel is advisory

    if (tasks.isEmpty()) {
        emit allCompleted({});
        return;
    }

    m_cancel.store(false);
    m_tasks = tasks;
    m_results.clear();
    m_results.reserve(tasks.size());
    m_currentIdx = 0;
    m_router = &router;
    m_tools = &tools;
    m_rag = &rag;

    qCInfo(verzetaUi) << "MultiAgentCoordinator: starting" << tasks.size() << "sub-agents";

    runNext();
}

/**
 * @brief Cancels all pending sub-agents.
 * @sideeffects Sets cancel flag; current sub-agent stops at next safe point.
 */
void MultiAgentCoordinator::cancelAll() {
    m_cancel.store(true);
    qCInfo(verzetaUi) << "MultiAgentCoordinator: cancel requested";
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Starts the sub-agent at m_currentIdx.
 * @sideeffects Creates a temporary AgentService child, connects its signals,
 *              and calls execute(). When the sub-agent finishes, advances to the
 *              next task or emits allCompleted().
 */
void MultiAgentCoordinator::runNext() {
    if (m_cancel.load() || m_currentIdx >= m_tasks.size()) {
        if (!m_cancel.load()) {
            emit allCompleted(m_results);
            qCInfo(verzetaUi) << "MultiAgentCoordinator: all sub-agents complete";
        }
        return;
    }

    const SubAgentTask& task = m_tasks[m_currentIdx];
    emit subAgentStarted(task.taskId, task.description);

    qCInfo(verzetaUi) << "MultiAgentCoordinator: starting sub-agent" << task.taskId << "-"
                      << task.description;

    // Create a temporary AgentService for this sub-task.
    // Parented to 'this' so it is cleaned up if the coordinator is destroyed.
    auto* agent = new AgentService(*m_router, *m_tools, *m_rag, this);

    const int capturedIdx = m_currentIdx;
    const SubAgentTask capturedTask = task;

    // Sub-agent finished successfully
    connect(agent,
            &AgentService::finished,
            this,
            [this, agent, capturedTask, capturedIdx](const QString& result) {
                agent->deleteLater();
                if (!m_isRunning())
                    return;

                SubAgentResult r;
                r.taskId = capturedTask.taskId;
                r.description = capturedTask.description;
                r.result = result;
                r.success = true;
                m_results.append(r);

                emit subAgentCompleted(r);

                m_currentIdx = capturedIdx + 1;
                runNext();
            });

    // Sub-agent encountered an error
    connect(agent,
            &AgentService::errorOccurred,
            this,
            [this, agent, capturedTask, capturedIdx](const QString& error) {
                agent->deleteLater();
                if (!m_isRunning())
                    return;

                SubAgentResult r;
                r.taskId = capturedTask.taskId;
                r.description = capturedTask.description;
                r.result = QStringLiteral("Error: ") + error;
                r.success = false;
                m_results.append(r);

                emit subAgentCompleted(r);

                m_currentIdx = capturedIdx + 1;
                runNext();
            });

    agent->execute(capturedTask.request, capturedTask.config);
}

/**
 * @brief Returns true if the coordinator is actively running sub-agents.
 * @return true if not cancelled and m_currentIdx < m_tasks.size().
 */
bool MultiAgentCoordinator::m_isRunning() const {
    return !m_cancel.load() && m_router != nullptr;
}

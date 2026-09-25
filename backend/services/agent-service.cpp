// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent-service.cpp
 * @brief Implementation of AgentService, an async signal-driven state machine
 *        that orchestrates five agent patterns (ReAct, Planner-Executor, Router,
 *        Multi-Agent, Memory-Augmented) over ModelRouter LLM calls.
 * @layer Service
 * @dependencies ModelRouter, ToolService, RagService, Qt6::Core, Qt6::Concurrent
 */


#include "agent-service.h"

#include "../services/model-router.h"
#include "../services/rag-service.h"
#include "../services/tool-service.h"
#include "../utils/logger.h"

#include <QtConcurrent/QtConcurrent>
#include <QThread>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace {

/**
 * @brief Set a tool-invocation future on @p watcher honouring the tool's
 *        thread residency, scoped to the calling conversation.
 *
 * Mirrors Chat::ToolDispatcher: a tool that declares runsOnMainThread()
 * touches SQLite / main-thread-owned state and MUST execute on the main
 * thread. AgentService historically ran EVERY tool on a QtConcurrent pool
 * thread, so a main-thread tool (e.g. read_canvas -> CanvasService SQLite)
 * invoked via the agent/task loop ran off the main thread and tripped the
 * thread-discipline qFatal ("This will corrupt SQLite state"). Here,
 * main-thread tools run on the main thread (inline when the caller is
 * already there, else marshalled with a blocking queued call so the result
 * is still returned synchronously); worker-safe tools keep running on the
 * pool. Either way the result is delivered through the caller's existing
 * QFutureWatcher, so the surrounding continuation logic is unchanged.
 *
 * @p callerConvId is the conversation this agent turn belongs to. It is
 * forwarded as the tool's caller conversation id so conv-scoped tools
 * (canvas read/edit/open, memory lookups, file tools) resolve to the
 * agent's OWN conversation instead of falling back to ChatController's
 * active-view getter. Before this, the agent path passed no caller id, so
 * a canvas edit ran against whatever conversation the user happened to be
 * viewing, so content from one chat leaked into another.
 */
void setResidencyAwareToolFuture(QFutureWatcher<QJsonValue>* watcher,
                                 ToolService* toolSvc,
                                 const QString& toolName,
                                 const QJsonObject& args,
                                 const QString& callerConvId,
                                 const QString& callerAgentId,
                                 const QString& callerAgentAlias) {
    if (!toolSvc->runsOnMainThread(toolName)) {
        watcher->setFuture(QtConcurrent::run(
            [toolSvc, toolName, args, callerConvId, callerAgentId, callerAgentAlias]()
                -> QJsonValue {
                return toolSvc->invokeTool(
                    toolName, args, callerConvId, callerAgentId, callerAgentAlias);
            }));
        return;
    }

    QCoreApplication* app = QCoreApplication::instance();
    QJsonValue syncResult;
    if (app && QThread::currentThread() != app->thread()) {
        // Caller is off the main thread — marshal the SQLite-touching tool
        // onto the main thread and block until it returns. Only reached if
        // a future refactor moves the agent loop off-thread; the normal
        // agent flow already runs these methods on the main thread, so the
        // inline branch below is what executes today (no deadlock risk).
        QMetaObject::invokeMethod(
            app,
            [toolSvc,
             &toolName,
             &args,
             &callerConvId,
             &callerAgentId,
             &callerAgentAlias,
             &syncResult]() {
                syncResult = toolSvc->invokeTool(
                    toolName, args, callerConvId, callerAgentId, callerAgentAlias);
            },
            Qt::BlockingQueuedConnection);
    } else {
        syncResult =
            toolSvc->invokeTool(toolName, args, callerConvId, callerAgentId, callerAgentAlias);
    }
    // Feed the already-computed result through the same QFutureWatcher the
    // worker path uses so the caller's finished-handler is identical.
    watcher->setFuture(QtConcurrent::run([syncResult]() -> QJsonValue { return syncResult; }));
}

}  // namespace

quint64 AgentService::nextStepRequestId() {
    // Delegate to ModelRouter's single process-wide foreground id source so
    // an agent step id can never collide with a direct-path turn id (the
    // direct path mints ids from the same source). Distinct counters would
    // overlap (both start at 1) and let a concurrent direct turn's
    // requestId-gated handler mis-consume this step's terminal signal.
    return ModelRouter::nextRequestId();
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

/*
 * @brief Constructs AgentService.
 * @param router ModelRouter for LLM dispatch.
 * @param tools  ToolService for tool invocation.
 * @param rag    RagService for memory retrieval.
 * @param parent Optional Qt parent.
 */
AgentService::AgentService(ModelRouter& router,
                           ToolService& tools,
                           RagService& rag,
                           QObject* parent)
    : QObject(parent), m_router(router), m_tools(tools), m_rag(rag) {
    // MemoryAugmented no longer retrieves here — pre-turn recall is unified in
    // the MemoryRetriever coordinator (forced on for this pattern), so the
    // pattern just executes as ReAct over the already-augmented request.
    qCInfo(verzetaUi) << "AgentService initialized";
}

/**
 * @brief Destroys AgentService. Cancels execution if running.
 */
AgentService::~AgentService() {
    if (m_isRunning) {
        m_cancel.store(true);
        disconnectRouter();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/*
 * @brief Starts an agent execution asynchronously.
 * @param request Base LLM request (messages, config, systemPrompt).
 * @param config  Agent pattern and behaviour configuration.
 * @sideeffects Connects to ModelRouter signals, starts pattern execution.
 *              No-op if already running.
 */
void AgentService::execute(const LlmRequest& request, const AgentConfig& config) {
    if (m_isRunning) {
        qCWarning(verzetaUi) << "AgentService::execute called while already running";
        return;
    }

    m_cancel.store(false);
    m_isRunning = true;
    m_currentIteration = 0;
    m_config = config;
    m_baseRequest = request;
    m_messages = request.messages;

    emit isRunningChanged();
    connectRouter();

    switch (config.pattern) {
        case AgentPattern::ReAct:
            executeReAct(request, config);
            break;
        case AgentPattern::PlannerExecutor:
            executePlannerExecutor(request, config);
            break;
        case AgentPattern::Router:
            executeRouter(request, config);
            break;
        case AgentPattern::MultiAgent:
            executeMultiAgent(request, config);
            break;
        case AgentPattern::MemoryAugmented:
            executeMemoryAugmented(request, config);
            break;
        default:
            // Direct pattern — single LLM call, no loop
            m_phase = AgentPhase::React_LLM;
            ++m_currentIteration;
            emit iterationChanged();
            emit agentStepStarted(m_currentIteration, QStringLiteral("Processing..."));
            routeToLLM(request);
            break;
    }
}

/**
 * @brief Cancels the current agent execution.
 * @sideeffects Sets cancel flag, cancels ModelRouter request, emits finished.
 */
void AgentService::cancel() {
    if (!m_isRunning)
        return;
    m_cancel.store(true);
    if (m_activeRequestId != 0) {
        m_router.cancelRequest(m_activeRequestId);
    }
    finishExecution(m_stepText);
}

/*
 * @brief Approves the pending tool call and continues execution.
 * @param callId UUID of the tool call to approve.
 */
void AgentService::approveToolCall(const QString& callId) {
    // Relay facade: this instance never executes, so forward to every
    // registered per-run source. Each source self-filters on its own
    // pending-approval id below, so only the owner acts.
    if (!m_relaySources.isEmpty()) {
        const QList<AgentService*> sources = m_relaySources.keys();
        for (AgentService* src : sources) {
            if (src)
                src->approveToolCall(callId);
        }
        return;
    }

    if (!m_isRunning || m_pendingApproval.id != callId)
        return;

    qCInfo(verzetaUi) << "AgentService: tool call approved:" << callId;

    // Determine the phase to restore after all tools complete
    AgentPhase afterTools = (m_phase == AgentPhase::React_Tools)
                                ? AgentPhase::React_LLM
                                : AgentPhase::React_LLM;  // default

    // Execute the approved tool
    ToolCall call = m_pendingApproval;
    m_pendingApproval = {};

    ToolService* toolSvc = &m_tools;
    auto* watcher = new QFutureWatcher<QJsonValue>(this);
    connect(
        watcher, &QFutureWatcher<QJsonValue>::finished, this, [this, watcher, call, afterTools]() {
            watcher->deleteLater();
            const QJsonValue result = watcher->result();

            ToolCall completed = call;
            completed.result = result;
            completed.status = QStringLiteral("success");
            emit toolCallCompleted(completed);

            appendToolResultMessage(call.id, result);
            emit agentStepCompleted(m_currentIteration, call.toolName, true);

            ++m_pendingToolIdx;
            executeNextTool(afterTools);
        });
    setResidencyAwareToolFuture(watcher,
                                toolSvc,
                                call.toolName,
                                call.arguments,
                                m_baseRequest.conversationId,
                                m_config.callerAgentId,
                                m_config.callerAgentAlias);
}

/*
 * @brief Denies the pending tool call and continues execution (skipping the tool).
 * @param callId UUID of the tool call to deny.
 */
void AgentService::denyToolCall(const QString& callId) {
    // Relay facade: forward to every registered per-run source (see
    // approveToolCall). Each source self-filters on its pending id.
    if (!m_relaySources.isEmpty()) {
        const QList<AgentService*> sources = m_relaySources.keys();
        for (AgentService* src : sources) {
            if (src)
                src->denyToolCall(callId);
        }
        return;
    }

    if (!m_isRunning || m_pendingApproval.id != callId)
        return;

    qCInfo(verzetaUi) << "AgentService: tool call denied:" << callId;

    ToolCall call = m_pendingApproval;
    m_pendingApproval = {};

    // Append a "denied" tool result so the LLM knows the call was skipped
    const QJsonValue denied = QJsonObject{{QStringLiteral("denied"), true}};
    appendToolResultMessage(call.id, denied);
    emit agentStepCompleted(m_currentIteration, call.toolName, false);

    AgentPhase afterTools = AgentPhase::React_LLM;
    ++m_pendingToolIdx;
    executeNextTool(afterTools);
}

bool AgentService::isRunning() const {
    if (m_isRunning)
        return true;
    // Relay facade: running iff any registered per-run source is running.
    for (auto it = m_relaySources.cbegin(); it != m_relaySources.cend(); ++it) {
        if (it.key() && it.key()->isRunning())
            return true;
    }
    return false;
}

int AgentService::currentIteration() const {
    if (m_isRunning)
        return m_currentIteration;
    // Relay facade: report the first running source's iteration.
    for (auto it = m_relaySources.cbegin(); it != m_relaySources.cend(); ++it) {
        if (it.key() && it.key()->isRunning())
            return it.key()->currentIteration();
    }
    return m_currentIteration;
}

// ---------------------------------------------------------------------------
// Relay facade registration
// ---------------------------------------------------------------------------

/*
 * @brief Registers a per-conversation AgentService as a relay source.
 * @param source
 *     The per-run AgentService whose UI-facing signals are forwarded
 *     through this facade. Ignored if null, equal to this, or already
 *     registered.
 */
void AgentService::registerRelaySource(AgentService* source) {
    if (!source || source == this || m_relaySources.contains(source))
        return;

    QList<QMetaObject::Connection> conns;
    // Forward the UI-facing progress signals (signal -> signal) so the
    // QML AgentProgressPanel / ToolConfirmationDialog and the remote
    // WireHostBridge observe this single facade exactly as before.
    conns.append(
        connect(source, &AgentService::agentStepStarted, this, &AgentService::agentStepStarted));
    conns.append(connect(
        source, &AgentService::agentStepCompleted, this, &AgentService::agentStepCompleted));
    conns.append(
        connect(source, &AgentService::toolCallRequested, this, &AgentService::toolCallRequested));
    conns.append(
        connect(source, &AgentService::toolCallCompleted, this, &AgentService::toolCallCompleted));
    conns.append(connect(source, &AgentService::planReady, this, &AgentService::planReady));
    // NOTIFY relays: the facade's isRunning()/currentIteration() aggregate
    // over sources, so re-emitting the parameterless NOTIFY makes bound
    // QML re-read the aggregate.
    conns.append(
        connect(source, &AgentService::isRunningChanged, this, &AgentService::isRunningChanged));
    conns.append(
        connect(source, &AgentService::iterationChanged, this, &AgentService::iterationChanged));
    // Auto-unregister when the source run is destroyed so a stale pointer
    // can never be dereferenced from isRunning()/approveToolCall().
    conns.append(connect(
        source, &QObject::destroyed, this, [this, source]() { m_relaySources.remove(source); }));

    m_relaySources.insert(source, conns);
}

/*
 * @brief Unregisters a relay source and tears down its connections.
 * @param source
 *     The per-run AgentService to stop relaying. Ignored if null or not
 *     currently registered.
 */
void AgentService::unregisterRelaySource(AgentService* source) {
    if (!source)
        return;
    const auto it = m_relaySources.constFind(source);
    if (it == m_relaySources.cend())
        return;
    for (const QMetaObject::Connection& c : it.value()) {
        disconnect(c);
    }
    m_relaySources.erase(it);
}

// ---------------------------------------------------------------------------
// Private slots — ModelRouter signal handlers
// ---------------------------------------------------------------------------

/**
 * @brief Accumulates streaming text and tool call data from ModelRouter.
 * @param chunk Partial text or tool-call JSON from the active LLM request.
 */
void AgentService::onChunkReceived(const LlmChunk& chunk) {
    if (!m_isRunning)
        return;

    if (!chunk.delta.isEmpty()) {
        m_stepText += chunk.delta;
    }

    // Accumulate tool call data
    if (!chunk.toolCallJson.isEmpty()) {
        ToolCall tc = toolCallFromJson(chunk.toolCallJson);
        if (!tc.toolName.isEmpty()) {
            m_stepToolCalls.append(tc);
        }
    }

    emit chunkReceived(chunk);
}

/**
 * @brief Drives the state machine forward when an LLM step completes.
 * @param finishReason "stop" | "tool_calls" | "length" | "user_interrupted"
 * @param totalTokens  Token count for this step.
 */
void AgentService::onStepFinished(const QString& finishReason, int totalTokens) {
    if (!m_isRunning)
        return;

    m_stepTokens = totalTokens;

    if (m_cancel.load()) {
        finishExecution(m_stepText);
        return;
    }

    switch (m_phase) {
        case AgentPhase::React_LLM:
        case AgentPhase::Router_Execute:
        case AgentPhase::Memory_Execute:
            handleReActLLMFinished(finishReason);
            break;

        case AgentPhase::Planner_Plan:
            handlePlannerPlanFinished(finishReason);
            break;

        case AgentPhase::Planner_Synthesize:
            handlePlannerSynthesizeFinished(finishReason);
            break;

        case AgentPhase::Router_Classify:
            handleRouterClassifyFinished(finishReason);
            break;

        case AgentPhase::MultiAgent_Decompose:
            handleMultiAgentDecomposeFinished(finishReason);
            break;

        case AgentPhase::MultiAgent_Synthesize:
            handleMultiAgentSynthesizeFinished(finishReason);
            break;

        default:
            // Sub-agent step in MultiAgent
            if (m_phase == AgentPhase::MultiAgent_SubTasks) {
                const QString result = m_stepText;
                if (m_subTaskIdx < m_subTasks.size()) {
                    m_subTaskResults.append({m_subTasks[m_subTaskIdx].description, result});
                    emit agentStepCompleted(
                        m_currentIteration, m_subTasks[m_subTaskIdx].description, true);
                }
                ++m_subTaskIdx;
                runNextSubAgent();
            }
            break;
    }
}

/**
 * @brief Handles LLM errors during agent execution.
 * @param errorMessage Human-readable error.
 */
void AgentService::onStepError(const QString& errorMessage) {
    if (!m_isRunning)
        return;
    qCWarning(verzetaUi) << "AgentService error:" << errorMessage;
    failExecution(errorMessage);
}

// ---------------------------------------------------------------------------
// Pattern entry points
// ---------------------------------------------------------------------------

/**
 * @brief Starts ReAct pattern execution.
 * @param req Base LLM request.
 * @param cfg Agent configuration.
 * @sideeffects Sets phase to React_LLM, routes first request to LLM.
 */
void AgentService::executeReAct(const LlmRequest& req, const AgentConfig& cfg) {
    Q_UNUSED(cfg)
    ++m_currentIteration;
    emit iterationChanged();
    m_phase = AgentPhase::React_LLM;
    emit agentStepStarted(m_currentIteration, QStringLiteral("Reasoning..."));
    routeToLLM(buildIterationRequest());
}

/**
 * @brief Starts Planner-Executor pattern.
 * Enters AgentPhase::Planner_Plan: asks the LLM for a JSON step plan.
 */
void AgentService::executePlannerExecutor(const LlmRequest& req, const AgentConfig& cfg) {
    Q_UNUSED(cfg)
    m_planSteps.clear();
    m_planResults.clear();
    m_planStepIdx = 0;

    static const QString kPlanningPrompt =
        QStringLiteral("\n\nAnalyze the user's request and create a step-by-step execution plan. "
                       "Output ONLY a valid JSON array of steps with this exact format:\n"
                       "[{\"step_id\":1,\"description\":\"What this step does\","
                       "\"tool_name\":\"name_of_tool_or_null\","
                       "\"arguments\":{\"arg1\":\"value1\"}}]\n"
                       "Use null for tool_name if no tool is needed. "
                       "Only use tools from the available tools list.");

    emit agentStepStarted(0, QStringLiteral("Generating plan..."));

    m_phase = AgentPhase::Planner_Plan;
    LlmRequest planReq = req;
    planReq.systemPrompt += kPlanningPrompt;
    routeToLLM(planReq);
}

/**
 * @brief Starts Router pattern.
 * Enters AgentPhase::Router_Classify: classifies intent with a fast model.
 */
void AgentService::executeRouter(const LlmRequest& req, const AgentConfig& cfg) {
    emit agentStepStarted(0, QStringLiteral("Classifying intent..."));
    m_phase = AgentPhase::Router_Classify;
    m_routerIntent.clear();

    static const QString kClassifyPrompt =
        QStringLiteral("Classify the user's intent into exactly one of these categories "
                       "(respond with ONLY the category name, nothing else):\n"
                       "- code: Programming, debugging, code generation\n"
                       "- search: Questions requiring web search or current information\n"
                       "- math: Mathematical calculations or analysis\n"
                       "- creative: Creative writing, brainstorming\n"
                       "- general: General knowledge questions");

    LlmRequest classifyReq = req;
    classifyReq.systemPrompt = kClassifyPrompt;
    // Use routerModel if specified
    if (!cfg.routerModel.isEmpty()) {
        classifyReq.config.modelName = cfg.routerModel;
    }
    routeToLLM(classifyReq);
}

/**
 * @brief Starts Multi-Agent pattern.
 * Enters AgentPhase::MultiAgent_Decompose: splits the task into
 * independent sub-tasks.
 */
void AgentService::executeMultiAgent(const LlmRequest& req, const AgentConfig& cfg) {
    Q_UNUSED(cfg)
    m_subTasks.clear();
    m_subTaskResults.clear();
    m_subTaskIdx = 0;

    static const QString kDecomposePrompt =
        QStringLiteral("\n\nBreak the user's request into independent sub-tasks that can be "
                       "handled separately. Output ONLY a valid JSON array:\n"
                       "[{\"task_id\":\"1\",\"description\":\"Task description\","
                       "\"prompt\":\"Specific prompt for this sub-task\"}]\n"
                       "Each sub-task should be self-contained and address a distinct aspect.");

    emit agentStepStarted(0, QStringLiteral("Decomposing into sub-tasks..."));
    m_phase = AgentPhase::MultiAgent_Decompose;

    LlmRequest decomposeReq = req;
    decomposeReq.systemPrompt += kDecomposePrompt;
    routeToLLM(decomposeReq);
}

/**
 * @brief Starts the Memory-Augmented pattern.
 *
 * Memory augmentation now happens uniformly PRE-TURN through the MemoryRetriever
 * coordinator (AIM + RAG + ACN per their gates), which ConversationRun forces ON
 * for this pattern (forceMemory) so it always augments. The injected context is
 * already in `req.systemPrompt` by the time we get here, so this pattern simply
 * executes as ReAct over it. There is exactly ONE retrieval per turn (the
 * coordinator's), never a second one here.
 */
void AgentService::executeMemoryAugmented(const LlmRequest& req, const AgentConfig& cfg) {
    Q_UNUSED(cfg)
    m_baseRequest = req;
    m_messages = req.messages;
    m_phase = AgentPhase::Memory_Execute;
    ++m_currentIteration;
    emit iterationChanged();
    emit agentStepStarted(m_currentIteration, QStringLiteral("Reasoning..."));
    routeToLLM(buildIterationRequest());
}

// ---------------------------------------------------------------------------
// Pattern continuation handlers
// ---------------------------------------------------------------------------

/**
 * @brief Handles LLM response completion for ReAct (and Router/Memory Execute phase).
 * @param finishReason LLM finish reason.
 */
void AgentService::handleReActLLMFinished(const QString& finishReason) {
    // Check for tool calls
    if (finishReason == QStringLiteral("tool_calls") && !m_stepToolCalls.isEmpty()) {
        // Append assistant message with tool call intent
        QJsonArray tcArray;
        for (const ToolCall& tc : m_stepToolCalls) {
            QJsonObject obj;
            obj[QStringLiteral("id")] = tc.id;
            obj[QStringLiteral("name")] = tc.toolName;
            obj[QStringLiteral("arguments")] = tc.arguments;
            tcArray.append(obj);
        }
        appendAssistantMessage(m_stepText, tcArray);

        emit agentStepCompleted(m_currentIteration, QStringLiteral("Reasoning complete"), true);

        // Start executing tools
        startToolExecution(AgentPhase::React_LLM);
    } else {
        // Final answer — LLM decided to stop
        emit agentStepCompleted(m_currentIteration, QStringLiteral("Final answer"), true);
        finishExecution(m_stepText);
    }
}

/**
 * @brief Handles the LLM plan response in AgentPhase::Planner_Plan.
 * @param finishReason LLM finish reason.
 */
void AgentService::handlePlannerPlanFinished(const QString& finishReason) {
    Q_UNUSED(finishReason)

    m_planSteps = parsePlanJson(m_stepText);
    if (m_planSteps.isEmpty()) {
        // Fallback: treat the LLM text as a single-step plan
        PlanStep single;
        single.stepId = 1;
        single.description = m_stepText.left(120);
        single.toolName.clear();
        m_planSteps.append(single);
    }

    // Emit plan to UI
    QStringList descriptions;
    for (const PlanStep& s : m_planSteps) {
        descriptions.append(s.description);
    }
    emit planReady(descriptions);
    emit agentStepCompleted(0, QStringLiteral("Plan generated"), true);

    // Start executing steps
    m_planStepIdx = 0;
    executePlanStep();
}

/**
 * @brief Executes the current plan step (tool call or no-op).
 */
void AgentService::executePlanStep() {
    if (m_cancel.load()) {
        finishExecution(m_stepText);
        return;
    }

    if (m_planStepIdx >= m_planSteps.size()) {
        // All steps done — synthesize
        synthesizePlanResults();
        return;
    }

    const PlanStep& step = m_planSteps[m_planStepIdx];
    emit agentStepStarted(m_planStepIdx + 1, step.description);

    if (step.toolName.isEmpty() || step.toolName == QStringLiteral("null")) {
        // No tool needed
        m_planResults.append({step, QJsonValue(QStringLiteral("No tool needed"))});
        emit agentStepCompleted(m_planStepIdx + 1, step.description, true);
        ++m_planStepIdx;
        executePlanStep();
        return;
    }

    // Execute the tool off-thread
    m_phase = AgentPhase::Planner_Tools;
    ToolService* toolSvc = &m_tools;
    const PlanStep capturedStep = step;
    const int capturedIdx = m_planStepIdx;

    auto* watcher = new QFutureWatcher<QJsonValue>(this);
    connect(watcher,
            &QFutureWatcher<QJsonValue>::finished,
            this,
            [this, watcher, capturedStep, capturedIdx]() {
                watcher->deleteLater();
                if (!m_isRunning)
                    return;

                const QJsonValue result = watcher->result();
                m_planResults.append({capturedStep, result});
                emit agentStepCompleted(capturedIdx + 1, capturedStep.description, true);

                ++m_planStepIdx;
                executePlanStep();
            });
    setResidencyAwareToolFuture(watcher,
                                toolSvc,
                                capturedStep.toolName,
                                capturedStep.arguments,
                                m_baseRequest.conversationId,
                                m_config.callerAgentId,
                                m_config.callerAgentAlias);
}

/**
 * @brief Sends the synthesis request after all plan steps are done.
 */
void AgentService::synthesizePlanResults() {
    const int synthesisStep = m_planSteps.size() + 1;
    emit agentStepStarted(synthesisStep, QStringLiteral("Synthesizing results..."));
    m_phase = AgentPhase::Planner_Synthesize;

    LlmRequest synthReq = m_baseRequest;
    const QString summary = QStringLiteral("Here are the results of executing the plan:\n") +
                            formatPlanResults() +
                            QStringLiteral("\nPlease synthesize a final answer.");
    LlmMessage synthMsg;
    synthMsg.role = QStringLiteral("user");
    synthMsg.content = summary;
    synthReq.messages.append(synthMsg);

    routeToLLM(synthReq);
}

/**
 * @brief Handles the synthesis response in AgentPhase::Planner_Synthesize.
 */
void AgentService::handlePlannerSynthesizeFinished(const QString& finishReason) {
    Q_UNUSED(finishReason)
    emit agentStepCompleted(m_planSteps.size() + 1, QStringLiteral("Synthesis complete"), true);
    finishExecution(m_stepText);
}

/**
 * @brief Handles the classification response in AgentPhase::Router_Classify.
 */
void AgentService::handleRouterClassifyFinished(const QString& finishReason) {
    Q_UNUSED(finishReason)
    m_routerIntent = m_stepText.trimmed().toLower();
    emit agentStepCompleted(0, QStringLiteral("Intent: ") + m_routerIntent, true);

    // Enters AgentPhase::Router_Execute: build a specialized request and
    // run ReAct over it.
    LlmRequest specialized = m_baseRequest;

    if (m_routerIntent == QStringLiteral("code")) {
        specialized.systemPrompt =
            QStringLiteral("You are an expert programmer. Help with programming tasks, "
                           "debugging, and code generation.");
        specialized.availableTools = filterTools({QStringLiteral("run_shell"),
                                                  QStringLiteral("read_file"),
                                                  QStringLiteral("write_file")});
    } else if (m_routerIntent == QStringLiteral("search")) {
        specialized.systemPrompt =
            QStringLiteral("You are a research assistant. Help find and summarize information.");
        specialized.availableTools = filterTools({QStringLiteral("search_web")});
    } else if (m_routerIntent == QStringLiteral("math")) {
        specialized.systemPrompt =
            QStringLiteral("You are a mathematician. Help with calculations and analysis.");
        specialized.availableTools = filterTools({QStringLiteral("run_shell")});
    } else {
        // general / creative / unknown — use original request
    }

    emit agentStepStarted(1, QStringLiteral("Request type: ") + m_routerIntent);
    m_phase = AgentPhase::Router_Execute;
    m_baseRequest = specialized;
    m_messages = specialized.messages;
    ++m_currentIteration;
    emit iterationChanged();
    routeToLLM(buildIterationRequest());
}

/**
 * @brief Handles the decomposition response in
 *        AgentPhase::MultiAgent_Decompose.
 */
void AgentService::handleMultiAgentDecomposeFinished(const QString& finishReason) {
    Q_UNUSED(finishReason)
    m_subTasks = parseSubTasks(m_stepText);

    if (m_subTasks.isEmpty()) {
        // Fallback: treat as single sub-task
        SubTask single;
        single.taskId = QStringLiteral("1");
        single.description = QStringLiteral("Main task");
        single.prompt =
            m_baseRequest.messages.isEmpty() ? QString{} : m_baseRequest.messages.last().content;
        m_subTasks.append(single);
    }

    emit agentStepCompleted(
        0, QStringLiteral("Split into %1 sub-task(s)").arg(m_subTasks.size()), true);

    m_subTaskIdx = 0;
    m_phase = AgentPhase::MultiAgent_SubTasks;
    runNextSubAgent();
}

/**
 * @brief Runs the next sub-agent in the MultiAgent sequence.
 */
void AgentService::runNextSubAgent() {
    if (m_cancel.load()) {
        finishExecution(m_stepText);
        return;
    }

    if (m_subTaskIdx >= m_subTasks.size()) {
        // All sub-agents done — synthesize
        const int synthesisStep = m_subTasks.size() + 1;
        emit agentStepStarted(synthesisStep, QStringLiteral("Synthesizing results..."));
        m_phase = AgentPhase::MultiAgent_Synthesize;

        LlmRequest synthReq = m_baseRequest;
        const QString summary = QStringLiteral("Sub-agent results:\n") + formatSubTaskResults() +
                                QStringLiteral("\nSynthesize these into a coherent final answer.");
        LlmMessage synthMsg;
        synthMsg.role = QStringLiteral("user");
        synthMsg.content = summary;
        synthReq.messages.append(synthMsg);

        routeToLLM(synthReq);
        return;
    }

    const SubTask& task = m_subTasks[m_subTaskIdx];
    emit agentStepStarted(m_subTaskIdx + 1, task.description);

    // Build a request for this sub-task
    LlmRequest subReq = m_baseRequest;
    LlmMessage subMsg;
    subMsg.role = QStringLiteral("user");
    subMsg.content = task.prompt;
    subReq.messages = {subMsg};

    resetStepAccumulator();
    routeToLLM(subReq);
}

/**
 * @brief Handles the synthesis response in
 *        AgentPhase::MultiAgent_Synthesize.
 */
void AgentService::handleMultiAgentSynthesizeFinished(const QString& finishReason) {
    Q_UNUSED(finishReason)
    emit agentStepCompleted(m_subTasks.size() + 1, QStringLiteral("Synthesis complete"), true);
    finishExecution(m_stepText);
}

// ---------------------------------------------------------------------------
// Tool execution helpers
// ---------------------------------------------------------------------------

/**
 * @brief Starts sequential tool execution for the current iteration's tool calls.
 * @param phaseAfterTools Phase to transition to after all tools complete.
 */
void AgentService::startToolExecution(AgentPhase phaseAfterTools) {
    m_pendingTools = m_stepToolCalls;
    m_pendingToolIdx = 0;
    m_phase = AgentPhase::React_Tools;
    executeNextTool(phaseAfterTools);
}

/**
 * @brief Executes the next queued tool, respecting requireConfirmation.
 * @param phaseAfterTools Phase to resume after all tools are done.
 */
void AgentService::executeNextTool(AgentPhase phaseAfterTools) {
    if (m_cancel.load()) {
        finishExecution(m_stepText);
        return;
    }

    if (m_pendingToolIdx >= m_pendingTools.size()) {
        onAllToolsExecuted(phaseAfterTools);
        return;
    }

    const ToolCall& call = m_pendingTools[m_pendingToolIdx];
    emit agentStepStarted(m_currentIteration, QStringLiteral("Calling: ") + call.toolName);
    emit toolCallRequested(call);

    if (m_config.requireConfirmation) {
        // Pause and wait for user approval
        m_pendingApproval = call;
        return;  // Execution resumes in approveToolCall() or denyToolCall()
    }

    // Execute immediately off-thread
    ToolService* toolSvc = &m_tools;
    const ToolCall capturedCall = call;
    const int capturedIdx = m_pendingToolIdx;

    auto* watcher = new QFutureWatcher<QJsonValue>(this);
    connect(watcher,
            &QFutureWatcher<QJsonValue>::finished,
            this,
            [this, watcher, capturedCall, capturedIdx, phaseAfterTools]() {
                watcher->deleteLater();
                if (!m_isRunning)
                    return;

                const QJsonValue result = watcher->result();

                ToolCall completed = capturedCall;
                completed.result = result;
                completed.status = QStringLiteral("success");
                emit toolCallCompleted(completed);
                emit agentStepCompleted(m_currentIteration, capturedCall.toolName, true);

                appendToolResultMessage(capturedCall.id, result);

                m_pendingToolIdx = capturedIdx + 1;
                executeNextTool(phaseAfterTools);
            });
    setResidencyAwareToolFuture(watcher,
                                toolSvc,
                                capturedCall.toolName,
                                capturedCall.arguments,
                                m_baseRequest.conversationId,
                                m_config.callerAgentId,
                                m_config.callerAgentAlias);
}

/**
 * @brief Called when all tools in the current iteration have been executed.
 * @param phaseAfterTools The phase to resume.
 */
void AgentService::onAllToolsExecuted(AgentPhase phaseAfterTools) {
    if (m_cancel.load()) {
        finishExecution(m_stepText);
        return;
    }

    // Resume LLM loop with updated messages
    m_phase = phaseAfterTools;

    if (phaseAfterTools == AgentPhase::React_LLM || phaseAfterTools == AgentPhase::Router_Execute ||
        phaseAfterTools == AgentPhase::Memory_Execute) {
        if (m_currentIteration >= m_config.maxIterations) {
            failExecution(QStringLiteral("Max iterations (%1) reached without final answer")
                              .arg(m_config.maxIterations));
            return;
        }

        ++m_currentIteration;
        emit iterationChanged();
        emit agentStepStarted(m_currentIteration, QStringLiteral("Reasoning..."));
        routeToLLM(buildIterationRequest());
    }
}

// ---------------------------------------------------------------------------
// Lifecycle helpers
// ---------------------------------------------------------------------------

/**
 * @brief Connects AgentService slots to ModelRouter signals.
 */
void AgentService::connectRouter() {
    m_chunkConn = connect(
        &m_router, &ModelRouter::chunkReceived, this, [this](quint64 id, const LlmChunk& chunk) {
            if (id != m_activeRequestId)
                return;
            onChunkReceived(chunk);
        });
    m_finishConn = connect(&m_router,
                           &ModelRouter::requestFinished,
                           this,
                           [this](quint64 id, const QString& finishReason, int totalTokens) {
                               if (id != m_activeRequestId)
                                   return;
                               onStepFinished(finishReason, totalTokens);
                           });
    m_errorConn = connect(&m_router,
                          &ModelRouter::requestError,
                          this,
                          [this](quint64 id, const QString& errorMessage) {
                              if (id != m_activeRequestId)
                                  return;
                              onStepError(errorMessage);
                          });
}

/**
 * @brief Disconnects AgentService slots from ModelRouter signals.
 */
void AgentService::disconnectRouter() {
    disconnect(m_chunkConn);
    disconnect(m_finishConn);
    disconnect(m_errorConn);
}

/**
 * @brief Resets the per-step text/tool accumulator before each new LLM call.
 */
void AgentService::resetStepAccumulator() {
    m_stepText.clear();
    m_stepToolCalls.clear();
    m_stepTokens = 0;
}

/**
 * @brief Routes the given request to ModelRouter, after resetting the accumulator.
 * @param request LLM request to dispatch.
 */
void AgentService::routeToLLM(const LlmRequest& request) {
    resetStepAccumulator();
    // Stamp a process-unique id for THIS step and remember it so the
    // shared-ModelRouter signal handlers ignore other concurrent agents'
    // (and other conversations') chunks/terminals. Steps within one agent
    // run are sequential, so one active id at a time is sufficient.
    LlmRequest req = request;
    req.requestId = nextStepRequestId();
    m_activeRequestId = req.requestId;
    m_router.route(req);
}

/**
 * @brief Completes the agent execution successfully.
 * @param result Final result text.
 * @sideeffects Disconnects router, resets state, emits finished().
 */
void AgentService::finishExecution(const QString& result) {
    disconnectRouter();
    m_activeRequestId = 0;
    m_isRunning = false;
    m_phase = AgentPhase::Idle;
    emit isRunningChanged();
    emit finished(result);
    qCInfo(verzetaUi) << "AgentService: execution finished, iteration" << m_currentIteration;
}

/**
 * @brief Fails the agent execution with an error.
 * @param error Human-readable error message.
 */
void AgentService::failExecution(const QString& error) {
    disconnectRouter();
    m_activeRequestId = 0;
    m_isRunning = false;
    m_phase = AgentPhase::Idle;
    emit isRunningChanged();
    emit errorOccurred(error);
    qCWarning(verzetaUi) << "AgentService: execution failed:" << error;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

/**
 * @brief Extracts a ToolCall from the raw LlmChunk::toolCallJson.
 * @param json Raw JSON object from LlmChunk (keys: id, name, arguments).
 * @return Populated ToolCall.
 */
ToolCall AgentService::toolCallFromJson(const QJsonObject& json) {
    ToolCall tc;
    tc.id = json.value(QStringLiteral("id"))
                .toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
    tc.toolName = json.value(QStringLiteral("name")).toString();
    tc.arguments = json.value(QStringLiteral("arguments")).toObject();
    tc.status = QStringLiteral("pending");
    return tc;
}

/**
 * @brief Parses a JSON array of plan steps from LLM output text.
 * @param planText LLM text containing a JSON array.
 * @return Parsed list of PlanStep. Empty on parse failure.
 */
QList<AgentService::PlanStep> AgentService::parsePlanJson(const QString& planText) const {
    QList<PlanStep> steps;

    // Extract JSON array from the text (LLM may wrap it in markdown)
    QString jsonStr = planText;
    const int start = jsonStr.indexOf(QLatin1Char('['));
    const int end = jsonStr.lastIndexOf(QLatin1Char(']'));
    if (start >= 0 && end > start) {
        jsonStr = jsonStr.mid(start, end - start + 1);
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        qCWarning(verzetaUi) << "AgentService: failed to parse plan JSON:" << err.errorString();
        return steps;
    }

    const QJsonArray arr = doc.array();
    for (const QJsonValue& v : arr) {
        const QJsonObject obj = v.toObject();
        PlanStep step;
        step.stepId = obj.value(QStringLiteral("step_id")).toInt();
        step.description = obj.value(QStringLiteral("description")).toString();
        step.toolName = obj.value(QStringLiteral("tool_name")).toString();
        step.arguments = obj.value(QStringLiteral("arguments")).toObject();
        steps.append(step);
    }
    return steps;
}

/**
 * @brief Parses a JSON array of sub-tasks from LLM decomposition output.
 * @param decomposeText LLM text containing a JSON array.
 * @return Parsed list of SubTask. Empty on parse failure.
 */
QList<AgentService::SubTask> AgentService::parseSubTasks(const QString& decomposeText) const {
    QList<SubTask> tasks;

    QString jsonStr = decomposeText;
    const int start = jsonStr.indexOf(QLatin1Char('['));
    const int end = jsonStr.lastIndexOf(QLatin1Char(']'));
    if (start >= 0 && end > start) {
        jsonStr = jsonStr.mid(start, end - start + 1);
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        qCWarning(verzetaUi) << "AgentService: failed to parse sub-tasks JSON:"
                             << err.errorString();
        return tasks;
    }

    const QJsonArray arr = doc.array();
    for (const QJsonValue& v : arr) {
        const QJsonObject obj = v.toObject();
        SubTask task;
        task.taskId = obj.value(QStringLiteral("task_id")).toString();
        task.description = obj.value(QStringLiteral("description")).toString();
        task.prompt = obj.value(QStringLiteral("prompt")).toString();
        tasks.append(task);
    }
    return tasks;
}

/**
 * @brief Formats plan step results as a numbered summary for synthesis.
 * @return Multi-line text summary.
 */
QString AgentService::formatPlanResults() const {
    QString out;
    for (int i = 0; i < m_planResults.size(); ++i) {
        const auto& [step, result] = m_planResults[i];
        out += QStringLiteral("Step %1: %2\nResult: %3\n\n")
                   .arg(i + 1)
                   .arg(step.description)
                   .arg(QJsonDocument(result.toObject().isEmpty()
                                          ? QJsonObject{{QStringLiteral("value"), result}}
                                          : result.toObject())
                            .toJson(QJsonDocument::Compact));
    }
    return out;
}

/**
 * @brief Formats sub-task results as a numbered summary for synthesis.
 * @return Multi-line text summary.
 */
QString AgentService::formatSubTaskResults() const {
    QString out;
    for (int i = 0; i < m_subTaskResults.size(); ++i) {
        const auto& [desc, result] = m_subTaskResults[i];
        out += QStringLiteral("Sub-task %1: %2\nResult: %3\n\n").arg(i + 1).arg(desc).arg(result);
    }
    return out;
}

/**
 * @brief Builds an LlmRequest for the next iteration using m_messages.
 * @return Request based on m_baseRequest but with m_messages.
 */
LlmRequest AgentService::buildIterationRequest() const {
    LlmRequest req = m_baseRequest;
    req.messages = m_messages;

    // Apply enabled tools filter if specified
    if (!m_config.enabledTools.isEmpty() && !req.availableTools.isEmpty()) {
        req.availableTools = filterTools(m_config.enabledTools);
    }
    return req;
}

/**
 * @brief Appends an assistant message to m_messages.
 * @param text         Accumulated LLM text.
 * @param toolCallsJson Optional tool calls JSON array.
 */
void AgentService::appendAssistantMessage(const QString& text, const QJsonArray& toolCallsJson) {
    LlmMessage msg;
    msg.role = QStringLiteral("assistant");
    msg.content = text;
    msg.toolCallsJson = toolCallsJson;
    m_messages.append(msg);
}

/**
 * @brief Appends a tool result message to m_messages.
 * @param toolCallId Tool call UUID.
 * @param result     Tool execution result JSON.
 */
void AgentService::appendToolResultMessage(const QString& toolCallId, const QJsonValue& result) {
    LlmMessage msg;
    msg.role = QStringLiteral("tool");
    msg.toolCallId = toolCallId;
    msg.content =
        QJsonDocument(result.toObject().isEmpty() ? QJsonObject{{QStringLiteral("result"), result}}
                                                  : result.toObject())
            .toJson(QJsonDocument::Compact);
    m_messages.append(msg);
}

/**
 * @brief Filters the available tools to only include those in the enabled list.
 * @param enabledNames List of tool names to keep.
 * @return Filtered list of ToolSchema from m_tools.
 */
QList<ToolSchema> AgentService::filterTools(const QStringList& enabledNames) const {
    if (enabledNames.isEmpty()) {
        return m_tools.availableTools();
    }
    QList<ToolSchema> filtered;
    for (const ToolSchema& schema : m_tools.availableTools()) {
        if (enabledNames.contains(schema.name)) {
            filtered.append(schema);
        }
    }
    return filtered;
}

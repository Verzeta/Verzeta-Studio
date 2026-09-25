// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent-service.h
 * @brief Orchestrates agent reasoning patterns (ReAct,
 *        Planner-Executor, Router, Multi-Agent, Memory-Augmented)
 *        for complex multi-step LLM workflows. Each pattern is
 *        selectable per conversation and executes asynchronously on
 *        the main thread using a signal-driven state machine.
 * @layer Service
 * @dependencies ModelRouter (Service), ToolService (Service),
 *               RagService (Service), Qt6::Core, Qt6::Concurrent
 */


#pragma once

#include "../api/llm-interface.h"
#include "../models/tool-call.h"

#include <atomic>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// Forward declarations
class ModelRouter;
class RagService;
struct RagChunk;
class ToolService;

// ---------------------------------------------------------------------------
// AgentPattern — available reasoning patterns
// ---------------------------------------------------------------------------

/**
 * @brief Available agent reasoning patterns.
 */
enum class AgentPattern {
    Direct,           ///< Single LLM call, no agentic loop
    ReAct,            ///< Iterative Reason→Act→Observe loop
    PlannerExecutor,  ///< Two-phase: generate plan, then execute steps
    Router,           ///< Intent classification → specialized sub-agent dispatch
    MultiAgent,       ///< Sequential specialized agents with result synthesis
    MemoryAugmented   ///< Uses long-term memory (RAG) for context enrichment
};

// ---------------------------------------------------------------------------
// AgentConfig — configuration for a single agent execution
// ---------------------------------------------------------------------------

/**
 * @brief Configuration for an agent execution.
 */
struct AgentConfig {
    AgentPattern pattern = AgentPattern::Direct;  ///< Execution strategy for the run.
    int maxIterations = 10;                       ///< Max reasoning/tool cycles
    bool requireConfirmation = false;             ///< Ask user before each tool call
    QStringList enabledTools;                     ///< Subset of tools (empty = all)
    QString routerModel;                          ///< For Router: model for classification
    int maxParallelAgents = 3;                    ///< For MultiAgent: sub-agent limit
    bool enableMemory = false;                    ///< For MemoryAugmented: use long-term memory
    /// Responder identity for THIS agent run, forwarded as the caller
    /// agent id/alias on every tool invocation so conv-scoped + cascade-
    /// consuming tools (audit attribution, coordinator gates, per-client
    /// routing) attach to the correct agent. Empty for a 1:1 chat with no
    /// agent member; the responder's id/alias for a group-chat turn.
    QString callerAgentId;
    QString callerAgentAlias;  ///< Alias of the same responder; see callerAgentId.
    /// Originating conversation id, used by the MemoryAugmented pattern to
    /// scope its RAG retrieval to {global} ∪ {conversation:} ∪ {project:}.
    /// Empty falls back to global-only.
    QString conversationId;
};

// ---------------------------------------------------------------------------
// AgentService — pattern orchestrator
// ---------------------------------------------------------------------------

/**
 * @brief Orchestrates agent reasoning patterns for complex multi-step workflows.
 *
 * ## Usage
 *
 * 1. Construct with references to ModelRouter, ToolService, and RagService.
 * 2. Call execute(request, config) to start an agent run.
 * 3. Connect signals to observe progress and receive the final result.
 * 4. Call cancel() to abort an in-progress execution.
 *
 * ## Threading
 *
 * AgentService runs on the main thread. execute() is non-blocking; all work is
 * driven by ModelRouter signals via Qt's signal/slot mechanism.
 * Tool invocations are dispatched to a QtConcurrent thread (same as ChatController).
 *
 * ## Pattern Implementations
 *
 * - **ReAct**: Iterative Reason→Act→Observe loop. The LLM decides on
 *   each iteration whether to call a tool or produce a final answer.
 * - **PlannerExecutor**: First a JSON step plan is generated; each
 *   step's tool call is then executed; finally a synthesis call
 *   produces the answer from the accumulated step results.
 * - **Router**: An intent classifier runs against a fast model, then
 *   the request is re-dispatched into a specialized ReAct config
 *   matching the classified intent.
 * - **MultiAgent**: The task is decomposed into sub-tasks, each
 *   sub-task is run sequentially via a temporary AgentService
 *   instance (presented as parallel sub-agents via the
 *   MultiAgentCoordinator interface), and the results are
 *   synthesized into the final answer.
 * - **MemoryAugmented**: Relevant RAG chunks are retrieved, the
 *   system prompt is augmented with them, then ReAct runs.
 *
 * ## State Machine
 *
 * The internal AgentPhase enum tracks where execution is within a pattern, so
 * onStepFinished() can dispatch correctly when ModelRouter signals arrive.
 */
class AgentService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isRunning READ isRunning NOTIFY isRunningChanged)
    Q_PROPERTY(int currentIteration READ currentIteration NOTIFY iterationChanged)

  public:
    /**
     * @brief Internal per-step result accumulated from ModelRouter signals.
     */
    struct StepResult {
        QString text;               ///< Accumulated text from chunkReceived
        QList<ToolCall> toolCalls;  ///< Extracted tool calls from LLM response
        QString finishReason;       ///< Final finish reason
        int tokens = 0;             ///< Token count (if provided)
    };

    /**
     * @brief Constructs AgentService.
     * @param router  ModelRouter for LLM dispatch.
     * @param tools   ToolService for tool invocation.
     * @param rag     RagService for memory retrieval (MemoryAugmented pattern).
     * @param parent  Optional Qt parent.
     * @sideeffects Does NOT connect to ModelRouter signals here; connections are
     *              made per-execute() call and disconnected on completion/cancel.
     */
    explicit AgentService(ModelRouter& router,
                          ToolService& tools,
                          RagService& rag,
                          QObject* parent = nullptr);

    /**
     * @brief Destroys AgentService. Cancels any in-progress execution.
     */
    ~AgentService() override;

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    /**
     * @brief Starts an agent execution asynchronously.
     * @param request The base LLM request (messages, config, systemPrompt).
     * @param config  Agent pattern and behaviour configuration.
     * @sideeffects Sets m_isRunning=true, connects to ModelRouter signals, starts
     *              the pattern-specific execution flow. No-op if already running.
     */
    void execute(const LlmRequest& request, const AgentConfig& config);

    /**
     * @brief Cancels the current agent execution.
     * @sideeffects Sets cancel flag, calls ModelRouter::cancelCurrent(), emits
     *              finished() with partial result. Safe to call if not running.
     */
    void cancel();

    /**
     * @brief Approves the pending tool call (when requireConfirmation=true).
     * @param callId UUID of the tool call to approve.
     * @sideeffects Continues execution by invoking the approved tool.
     */
    Q_INVOKABLE void approveToolCall(const QString& callId);

    /**
     * @brief Denies the pending tool call.
     * @param callId UUID of the tool call to deny.
     * @sideeffects Skips tool execution, treats tool result as "{denied: true}",
     *              continues the agent loop.
     */
    Q_INVOKABLE void denyToolCall(const QString& callId);

    /**
     * @brief Whether an agent execution is currently in progress.
     * @returns true between execute() and the terminal finished /
     *          errorOccurred signal; false otherwise. When this instance
     *          is the relay facade (see registerRelaySource()), returns
     *          true if THIS instance OR any registered per-run source is
     *          running, so the QML AgentProgressPanel and the remote
     *          bridge still reflect agent activity once the chat path
     *          executes on per-conversation instances.
     */
    bool isRunning() const;

    /**
     * @brief The current iteration count for the in-flight run.
     * @returns Iteration index (0-based); 0 when no run is active. As the
     *          relay facade, returns this instance's own iteration when it
     *          is running, otherwise the iteration of the first registered
     *          source that is currently running (0 if none).
     */
    int currentIteration() const;

    /**
     * @brief The ModelRouter this instance dispatches through.
     * @returns Reference to the shared ModelRouter passed at construction.
     */
    ModelRouter& modelRouter() const { return m_router; }

    /**
     * @brief The ToolService this instance invokes tools through.
     * @returns Reference to the shared ToolService passed at construction.
     */
    ToolService& toolService() const { return m_tools; }

    /**
     * @brief The RagService this instance retrieves memories from.
     * @returns Reference to the shared RagService passed at construction.
     */
    RagService& ragService() const { return m_rag; }

    /**
     * @brief Registers a per-conversation AgentService as a relay source
     *        of this instance (used when this instance is the app-level
     *        facade).
     *
     * The chat path executes agent patterns on a per-conversation
     * AgentService so two agent-pattern turns on different providers can
     * run concurrently. The app-level singleton is
     * retained ONLY as the QML / remote-bridge facade: it executes
     * nothing itself, but every per-run source forwards its UI-facing
     * signals (step started/completed, tool-call requested/completed,
     * isRunning / iteration changes) through this facade so existing
     * observers see a single stable object. Confirmation routing
     * (approveToolCall / denyToolCall) on this facade is broadcast to all
     * registered sources; each source self-filters by call id, so only
     * the source holding the matching pending approval acts.
     *
     * @param source The per-run AgentService to relay from. Must not be
     *               this instance. A null source, or a source already
     *               registered, is ignored. The source is automatically
     *               unregistered when it is destroyed (tracked via
     *               QObject::destroyed), so the caller need not unregister
     *               explicitly.
     */
    void registerRelaySource(AgentService* source);

    /**
     * @brief Unregisters a previously registered relay source.
     * @param source The per-run AgentService to stop relaying. A null
     *               source, or one that was never registered, is ignored.
     *               Disconnects all relay connections established for it.
     */
    void unregisterRelaySource(AgentService* source);

  signals:
    /** @brief Emitted for each text token during any agent step. */
    void chunkReceived(const LlmChunk& chunk);

    /** @brief Emitted when a new reasoning step begins. */
    void agentStepStarted(int iteration, const QString& stepDescription);

    /**
     * @brief Emitted when a reasoning step completes.
     * @param iteration       Iteration index of the step that finished.
     * @param stepDescription Human-readable step description.
     * @param success         true if the step succeeded.
     */
    void agentStepCompleted(int iteration, const QString& stepDescription, bool success);

    /**
     * @brief Emitted when the LLM requests a tool call (for
     *        confirmation UI).
     * @param call The proposed tool call.
     */
    void toolCallRequested(const ToolCall& call);

    /**
     * @brief Emitted after a tool call finishes executing.
     * @param call The completed tool call (with result populated).
     */
    void toolCallCompleted(const ToolCall& call);

    /**
     * @brief Emitted when Planner-Executor generates a plan.
     * @param steps List of plan step descriptions in execution order.
     */
    void planReady(const QStringList& steps);

    /**
     * @brief Emitted when agent execution finishes (all patterns).
     * @param result Final response text from the agent run.
     */
    void finished(const QString& result);

    /**
     * @brief Emitted on unrecoverable error or max iterations
     *        exceeded.
     * @param error Human-readable error description.
     */
    void errorOccurred(const QString& error);

    /** @brief Emitted whenever isRunning() changes value. */
    void isRunningChanged();

    /** @brief Emitted whenever currentIteration() changes value. */
    void iterationChanged();

  private slots:
    /**
     * @brief Accumulates text/tool-call chunks from the active LLM request.
     * @param chunk Partial text or tool-call chunk from ModelRouter.
     */
    void onChunkReceived(const LlmChunk& chunk);

    /**
     * @brief Handles step completion and drives the state machine forward.
     * @param finishReason "stop" | "tool_calls" | "length" | "user_interrupted"
     * @param totalTokens  Total tokens for this step.
     */
    void onStepFinished(const QString& finishReason, int totalTokens);

    /**
     * @brief Handles LLM errors during agent execution.
     * @param errorMessage Human-readable error description.
     */
    void onStepError(const QString& errorMessage);

  private:
    // -----------------------------------------------------------------------
    // Internal state machine phase enum
    // -----------------------------------------------------------------------

    /**
     * @brief Tracks which phase of the current agent pattern is executing.
     */
    enum class AgentPhase {
        Idle,
        // ReAct
        React_LLM,    ///< Waiting for LLM reasoning response
        React_Tools,  ///< Executing tool calls from last LLM response
        // PlannerExecutor
        Planner_Plan,        ///< Waiting for plan JSON from LLM
        Planner_Tools,       ///< Executing a tool in the plan
        Planner_Synthesize,  ///< Waiting for synthesis LLM response
        // Router
        Router_Classify,  ///< Waiting for intent classification
        Router_Execute,   ///< Running ReAct after routing
        // MultiAgent
        MultiAgent_Decompose,   ///< Waiting for task decomposition
        MultiAgent_SubTasks,    ///< Running sub-agents sequentially
        MultiAgent_Synthesize,  ///< Waiting for synthesis LLM response
        // MemoryAugmented — recall is unified pre-turn (MemoryRetriever); this
        // pattern only executes as ReAct over the already-augmented request.
        Memory_Execute,  ///< Running ReAct with the pre-injected context
    };

    // -----------------------------------------------------------------------
    // Plan step for Planner-Executor pattern
    // -----------------------------------------------------------------------

    /** @brief Single step in a Planner-Executor plan. */
    struct PlanStep {
        int stepId;
        QString description;
        QString toolName;
        QJsonObject arguments;
    };

    // -----------------------------------------------------------------------
    // Sub-task for Multi-Agent pattern
    // -----------------------------------------------------------------------

    /** @brief Single sub-task in the MultiAgent decomposition. */
    struct SubTask {
        QString taskId;
        QString description;
        QString prompt;
    };

    // -----------------------------------------------------------------------
    // Service references
    // -----------------------------------------------------------------------

    ModelRouter& m_router;
    ToolService& m_tools;
    RagService& m_rag;

    // -----------------------------------------------------------------------
    // Execution state
    // -----------------------------------------------------------------------

    std::atomic<bool> m_cancel{false};
    bool m_isRunning = false;
    int m_currentIteration = 0;
    /// Process-unique id of the ModelRouter request for the CURRENT LLM
    /// step. Stamped onto every route() in routeToLLM() and used to filter
    /// ModelRouter's shared chunk/finished/error signals so that, when two
    /// per-conversation AgentService instances run concurrently over the
    /// one shared ModelRouter, each instance only consumes the signals for
    /// its own in-flight step.
    quint64 m_activeRequestId = 0;
    AgentPhase m_phase = AgentPhase::Idle;
    AgentConfig m_config;
    LlmRequest m_baseRequest;      ///< Original request from execute()
    QList<LlmMessage> m_messages;  ///< Accumulated conversation history

    // Per-step accumulation (reset at start of each LLM call)
    QString m_stepText;
    QList<ToolCall> m_stepToolCalls;
    int m_stepTokens = 0;

    // Tool execution queue
    QList<ToolCall> m_pendingTools;  ///< Tools waiting to be executed this iteration
    int m_pendingToolIdx = 0;
    ToolCall m_pendingApproval;  ///< Tool awaiting user confirmation

    // PlannerExecutor state
    QList<PlanStep> m_planSteps;
    QList<QPair<PlanStep, QJsonValue>> m_planResults;
    int m_planStepIdx = 0;

    // MultiAgent state
    QList<SubTask> m_subTasks;
    QList<QPair<QString, QString>> m_subTaskResults;  ///< {description, result}
    int m_subTaskIdx = 0;
    QString m_currentSubAgentText;

    // Router state
    QString m_routerIntent;

    // Qt signal connections (stored for disconnection on finish)
    QMetaObject::Connection m_chunkConn;
    QMetaObject::Connection m_finishConn;
    QMetaObject::Connection m_errorConn;

    // -----------------------------------------------------------------------
    // Relay facade state (only populated on the app-level singleton; empty
    // on a per-conversation instance). Maps each registered per-run source
    // to the relay connections established for it so they can be torn down
    // on unregister / source destruction.
    // -----------------------------------------------------------------------
    QHash<AgentService*, QList<QMetaObject::Connection>> m_relaySources;

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Returns a process-unique request id for the next LLM step.
     * @returns A monotonically increasing id, never 0 (0 is the "no
     *          in-flight step" sentinel). Shared across every AgentService
     *          instance so two concurrent agent runs never collide.
     */
    static quint64 nextStepRequestId();

    /** @brief Connects AgentService slots to ModelRouter signals. */
    void connectRouter();

    /** @brief Disconnects AgentService slots from ModelRouter signals. */
    void disconnectRouter();

    /** @brief Resets per-step accumulator before each new LLM call. */
    void resetStepAccumulator();

    /** @brief Routes a request to the LLM; also resets the step accumulator. */
    void routeToLLM(const LlmRequest& request);

    /** @brief Called when all patterns finish: disconnects, resets, emits finished(). */
    void finishExecution(const QString& result);

    /** @brief Called on unrecoverable error. */
    void failExecution(const QString& error);

    // -----------------------------------------------------------------------
    // Pattern entry points (called from execute())
    // -----------------------------------------------------------------------

    void executeReAct(const LlmRequest& req, const AgentConfig& cfg);
    void executePlannerExecutor(const LlmRequest& req, const AgentConfig& cfg);
    void executeRouter(const LlmRequest& req, const AgentConfig& cfg);
    void executeMultiAgent(const LlmRequest& req, const AgentConfig& cfg);

    /**
     * @brief Runs the MemoryAugmented pattern: execute as ReAct over the request,
     *        whose memory context was already injected pre-turn by the unified
     *        MemoryRetriever coordinator (forced on for this pattern). No separate
     *        retrieval here: exactly one recall per turn.
     * @param req The base request (already memory-augmented).
     * @param cfg The agent config (unused; scope/gating handled by the coordinator).
     */
    void executeMemoryAugmented(const LlmRequest& req, const AgentConfig& cfg);

    // -----------------------------------------------------------------------
    // Pattern continuation handlers (called from onStepFinished())
    // -----------------------------------------------------------------------

    void handleReActLLMFinished(const QString& finishReason);
    void handlePlannerPlanFinished(const QString& finishReason);
    void handlePlannerSynthesizeFinished(const QString& finishReason);
    void handleRouterClassifyFinished(const QString& finishReason);
    void handleMultiAgentDecomposeFinished(const QString& finishReason);
    void handleMultiAgentSynthesizeFinished(const QString& finishReason);

    // -----------------------------------------------------------------------
    // Tool execution helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Starts sequential tool execution for the current iteration's tool calls.
     * @param afterTools Phase to transition to after all tools are done.
     */
    void startToolExecution(AgentPhase phaseAfterTools);

    /**
     * @brief Executes the next tool in m_pendingTools, respecting confirmation.
     * @param phaseAfterTools Phase to restore after all tools complete.
     */
    void executeNextTool(AgentPhase phaseAfterTools);

    /**
     * @brief Called when all tools in m_pendingTools have been executed.
     * @param phaseAfterTools The phase to transition to.
     */
    void onAllToolsExecuted(AgentPhase phaseAfterTools);

    // -----------------------------------------------------------------------
    // JSON helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Extracts a ToolCall from the raw toolCallJson in an LlmChunk.
     * @param json Raw JSON object from LlmChunk::toolCallJson.
     * @return Populated ToolCall (id may be empty if not provided).
     */
    static ToolCall toolCallFromJson(const QJsonObject& json);

    /**
     * @brief Parses a JSON array of plan steps from LLM output.
     * @param planText LLM text containing a JSON array of step objects.
     * @return Parsed list of PlanStep. Empty if parsing fails.
     */
    QList<PlanStep> parsePlanJson(const QString& planText) const;

    /**
     * @brief Parses a JSON array of sub-tasks from LLM output.
     * @param decomposeText LLM text containing a JSON array of sub-task objects.
     * @return Parsed list of SubTask. Empty if parsing fails.
     */
    QList<SubTask> parseSubTasks(const QString& decomposeText) const;

    /**
     * @brief Formats plan step results as a readable summary for synthesis.
     * @return Multi-line summary string.
     */
    QString formatPlanResults() const;

    /**
     * @brief Formats sub-agent results as a readable summary for synthesis.
     * @return Multi-line summary string.
     */
    QString formatSubTaskResults() const;

    /**
     * @brief Builds a LlmRequest for a new iteration with updated messages.
     * @return Request with same config as m_baseRequest but using m_messages.
     */
    LlmRequest buildIterationRequest() const;

    /**
     * @brief Appends an assistant message (with optional tool calls) to m_messages.
     * @param text         Accumulated LLM text for this step.
     * @param toolCallsJson Optional tool calls JSON array.
     */
    void appendAssistantMessage(const QString& text, const QJsonArray& toolCallsJson = {});

    /**
     * @brief Appends a tool result message to m_messages.
     * @param toolCallId  Tool call UUID.
     * @param result      Tool execution result.
     */
    void appendToolResultMessage(const QString& toolCallId, const QJsonValue& result);

    /**
     * @brief Returns available tools filtered to only those in enabledNames.
     * @param enabledNames Allowed tool names (empty = all tools).
     * @return Filtered list of ToolSchema from m_tools.
     */
    QList<ToolSchema> filterTools(const QStringList& enabledNames) const;

    /**
     * @brief Executes the current PlanStep at m_planStepIdx.
     * @sideeffects Spawns off-thread tool execution or advances the step index.
     */
    void executePlanStep();

    /**
     * @brief Sends synthesis request after all plan steps complete.
     */
    void synthesizePlanResults();

    /**
     * @brief Runs the sub-agent at m_subTaskIdx in the MultiAgent pattern.
     */
    void runNextSubAgent();
};

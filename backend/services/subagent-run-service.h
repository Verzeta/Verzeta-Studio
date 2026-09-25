// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file subagent-run-service.h
 * @brief Agent-spawned sub-agents: run lifecycle state
 *        machine, FIFO queue + concurrency gate, per-run owned
 *        provider dispatch with a bounded tool loop, run-scoped
 *        transcript persistence, and result delivery signals.
 * @layer Service
 * @dependencies DbManager, ToolService, ModelRouter, SettingsService,
 *               Ragp classifier provider factory (owned providers),
 *               Qt6::Core, Qt6::Sql.
 *
 * Architecture:
 *   #1  Spawn is ASYNC: spawn() returns {runId, status} immediately;
 *       completion arrives via runFinished.
 *   #3  Transcript lives in `subagent_messages` (run-scoped), so the
 *       parent conversation's context stays clean.
 *   #5  Dispatch on a run-owned ILLMProvider built by the classifier
 *       provider factory (foreground slot collides with user turns;
 *       background slot is single-in-flight + heartbeat-owned).
 *   #6  Concurrency cap 2; FIFO queue beyond (queued runs are
 *       visible via their status).
 *   #7  Tool whitelist passed by the parent; DEFAULT read-only set
 *       (search_web, read_file, list_files, read_conversation).
 *       write tools require explicit opt-in by the parent.
 *   #8  Depth 1: spawn_subagent is NEVER in a sub-agent's
 *       whitelist, so recursion is structurally impossible.
 *   #9  Bounds: kMaxToolRounds tool rounds + kRunTimeoutMs wall
 *       clock; breach → failed("budget").
 *   #10 Provider/model: parent conversation's pair by default;
 *       explicit overrides honoured when constructible.
 *   #12 cancel(runId) → status cancelled; in-flight request aborted.
 *
 * Threading: strictly main-thread orchestration; provider I/O is
 * async signal-driven; exactly-once finalize + guaranteed no-context
 * QTimer timeout + QPointer guards (the proven summarizer/RAGP
 * lifetime design; rules 6/9/10/12).
 */

#pragma once

#include "../models/subagent-run.h"
#include "ragp/classifier-provider-factory.h"

#include <map>
#include <memory>
#include <optional>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QQueue>
#include <QString>

class DbManager;
class ModelRouter;
class SettingsService;
class ToolService;
struct LlmMessage;

/**
 * @brief Agent-spawned sub-agent runner: lifecycle state machine,
 *        FIFO queue + concurrency gate, per-run owned provider
 *        dispatch with a bounded whitelist tool loop, run-scoped
 *        transcript persistence, and result-delivery signals. One
 *        instance app-wide (AppController-owned); main-thread
 *        orchestration with async provider I/O.
 */
class SubagentRunService : public QObject {
    Q_OBJECT

  public:
    /** Concurrency cap (decision #6): at most this many runs in the
     *  `running` state host-wide; further spawns queue FIFO. */
    static constexpr int kMaxConcurrentRuns = 2;

    /** Tool-round bound per run (decision #9), with the same kill-switch
     *  philosophy as ToolDispatcher::kMaxToolIterations. */
    static constexpr int kMaxToolRounds = 10;

    /** Wall-clock bound per run (decision #9). */
    static constexpr int kRunTimeoutMs = 5 * 60 * 1000;

    /** Per-request provider timeout inside a run (one model turn). */
    static constexpr int kTurnTimeoutMs = 120000;

    /**
     * @param db       Engine layer (non-owning).
     * @param toolSvc  Tool execution for the sub-agent's rounds
     *                 (non-owning).
     * @param router   Active provider/model fallback resolution
     *                 (non-owning).
     * @param settings Provider construction config (non-owning).
     * @param parent   Qt parent (AppController).
     */
    SubagentRunService(DbManager& db,
                       ToolService& toolSvc,
                       ModelRouter& router,
                       SettingsService& settings,
                       QObject* parent = nullptr);
    ~SubagentRunService() override;

    /**
     * @brief Decision #1: asynchronous spawn. Persists the run row
     *        (queued), then starts it immediately when a concurrency
     *        slot is free, else leaves it queued FIFO.
     * @param conversationId Parent conversation UUID.
     * @param parentMsgId    Spawning assistant message id.
     * @param requesterAlias Parent agent alias.
     * @param task           Delegated task text (required).
     * @param toolsWhitelist Tool names the sub-agent may call; empty →
     *                       the read-only default set (decision #7).
     *                       `spawn_subagent` is stripped if present
     *                       (decision #8).
     * @param providerId     Optional provider override (decision #10).
     * @param modelName      Optional model override.
     * @returns The new run id, or empty string when the task is empty
     *          or persistence failed.
     */
    QString spawn(const QString& conversationId,
                  const QString& parentMsgId,
                  const QString& requesterAlias,
                  const QString& task,
                  const QStringList& toolsWhitelist = {},
                  const QString& providerId = {},
                  const QString& modelName = {});

    /**
     * @brief Loads a run row.
     * @param runId Run UUID.
     * @returns The run, or nullopt when unknown.
     */
    std::optional<SubagentRun> run(const QString& runId) const;

    /**
     * @brief All runs for a conversation, newest first.
     * @param conversationId Conversation UUID.
     * @returns Run rows (possibly empty).
     */
    QList<SubagentRun> runsForConversation(const QString& conversationId) const;

    /**
     * @brief Run transcript, seq order: {role, content, tool_name}.
     * @param runId Run UUID.
     * @returns JSON array for the UI run viewer.
     */
    QJsonArray transcript(const QString& runId) const;

    /**
     * @brief Decision #12: cancels a queued or running run. Running
     *        runs get their in-flight provider request aborted; the
     *        run finalizes as `cancelled` and the parent is notified
     *        via runFinished.
     * @param runId Run UUID.
     * @returns true when the run existed and was cancellable.
     */
    bool cancel(const QString& runId);

    /**
     * @brief Number of runs currently in the `running` state.
     * @returns Active-run count (0..kMaxConcurrentRuns).
     */
    int activeCount() const { return m_activeCount; }

    /**
     * @brief TEST-ONLY seam (same pattern as ConversationSummarizer):
     *        pre-seeds the provider bundle used by the NEXT started
     *        run so the full async loop is testable with a stub.
     * @param providerId Provider id the stub impersonates.
     * @param provider   Owned stub instance.
     */
    void setProviderForTest(const QString& providerId, std::unique_ptr<ILLMProvider> provider) {
        m_prov.http.reset();
        m_prov.provider = std::move(provider);
        m_prov.providerId = providerId;
    }

  signals:
    /**
     * @brief Run state changed (queued→running, or terminal). UI
     *        status rows subscribe.
     * @param runId  Run UUID.
     * @param status New status string.
     */
    void runStatusChanged(const QString& runId, const QString& status);

    /**
     * @brief Run reached a terminal state. ChatController subscribes
     *        to deliver the result to the parent agent as a
     *        continuation turn (decision #2).
     * @param runId          Run UUID.
     * @param conversationId Parent conversation.
     * @param requesterAlias Parent agent alias.
     * @param status         "done" | "failed" | "cancelled".
     * @param resultText     Final report (empty unless done).
     */
    void runFinished(const QString& runId,
                     const QString& conversationId,
                     const QString& requesterAlias,
                     const QString& status,
                     const QString& resultText);

  private:
    /** Starts the next queued run if a concurrency slot is free. */
    void pump();

    /** Transitions + persists status; emits runStatusChanged. */
    void setStatus(const QString& runId, const QString& status, const QString& failReason = {});

    /** Drives one model turn for the run (async). */
    void dispatchTurn(const QString& runId);

    /** Appends one transcript row. */
    void appendTranscript(const QString& runId,
                          const QString& role,
                          const QString& content,
                          const QString& toolName = {});

    /** Finalizes the run (exactly-once per run id). */
    void finalizeRun(const QString& runId,
                     const QString& status,
                     const QString& resultText,
                     const QString& failReason);

    /** Builds the sub-agent's system prompt. */
    QString buildSystemPrompt(const SubagentRun& run) const;

    /** Read-only default whitelist (decision #7). */
    static QStringList defaultWhitelist();

    /** @brief Per-run in-memory dispatch state. */
    struct RunState {
        QList<LlmMessage> messages;  ///< Sub-agent's own context
        int toolRounds = 0;
        bool inFlight = false;
        qint64 deadlineMs = 0;  ///< Wall-clock budget (epoch ms)
    };

    DbManager& m_db;
    ToolService& m_toolSvc;
    ModelRouter& m_router;
    SettingsService& m_settings;

    QQueue<QString> m_queue;            ///< FIFO of queued run ids
    QHash<QString, RunState> m_states;  ///< Active run states
    int m_activeCount = 0;

    /** Provider bundle for the CURRENT dispatching run. With cap 2 a
     *  single bundle would collide; bundles are per-run in m_runProv. */
    Ragp::ClassifierProvider m_prov;  ///< test-seam / template bundle
    /** std::map (not QHash): ClassifierProvider is move-only and
     *  QHash requires copyable values. */
    std::map<QString, Ragp::ClassifierProvider> m_runProv;
};

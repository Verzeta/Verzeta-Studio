// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-subagent-service.h
 * @brief Tier-1 orchestrator for proactive autonomous agent activity
 *        ("heartbeat subagents"). Owns the scheduling tick, the FIFO
 *        queue, the per-config rate-limit bookkeeping, and the async
 *        dispatch via ModelRouter's BACKGROUND slot.
 *
 *        The service NEVER touches the foreground ModelRouter slot,
 *        and ChatController NEVER subscribes to background signals;
 *        the dual-slot routing guarantees no chunk-cross-contamination
 *        between user-driven turns and proactive background activity.
 *
 * @layer Service (top-level, owned by AppController)
 * @dependencies HeartbeatConfigService, AgentRegistry, ConversationService,
 *               MessageService, ModelRouter, ToolService, SkillService,
 *               Chat::RequestBuilder, optional Chat::CascadeController /
 *               Chat::StreamingManager / ChatController, Qt6::Core.
 */


#pragma once

#include "../api/llm-interface.h"
#include "../models/heartbeat-config.h"
#include "../models/heartbeat-report.h"
#include "heartbeat-subagent-queue.h"

#include <QTimer>

#include <functional>
#include <memory>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QVariantMap>

class HeartbeatConfigService;
class AgentRegistry;
struct Agent;
class ChatController;
class ConversationService;
class MessageService;
class ModelRouter;
class SkillService;
class ToolService;

namespace Chat {
class CascadeController;
class RequestBuilder;
class StreamingManager;
}  // namespace Chat

/**
 * @brief Tier-1 (and Tier-2 / Tier-3 entry point) orchestrator for
 *        the heartbeat subagent feature.
 */
class HeartbeatSubagentService : public QObject {
    Q_OBJECT
    /** @brief Whether all heartbeat scheduling is paused process-wide. */
    Q_PROPERTY(bool globallyPaused READ globallyPaused WRITE setGloballyPaused NOTIFY
                   globallyPausedChanged)
    /** @brief Number of currently-enabled heartbeat configs. */
    Q_PROPERTY(int totalEnabledConfigs READ totalEnabledConfigs NOTIFY configsChanged)
    /**
     * @brief Diagnostic ring buffer of recent heartbeat-service log
     *        lines (most recent last). Capped at kLogRingCap entries;
     *        oldest entries are dropped on overflow. The Settings →
     *        Heartbeat (Diagnostics) panel binds to this to render an
     *        in-app log viewer without scraping stderr.
     */
    Q_PROPERTY(QStringList recentLogLines READ recentLogLines NOTIFY recentLogLinesChanged)

  public:
    /** @brief Returns the cascade to route a post through, or nullptr. */
    using CascadeResolver = std::function<Chat::CascadeController*()>;
    /**
     * @brief Construct the heartbeat subagent service.
     *
     *        The cascade resolver and chat controller are optional.
     *        Without a cascade, a surfaced report is still saved but is
     *        not routed to other members. Without a chat controller,
     *        the check that defers a post while the conversation is
     *        streaming is skipped (acceptable in tests).
     *
     *        The Tier-2 review call uses no tools; it sees only the
     *        agent's system prompt and recent conversation context.
     *
     * @param configSvc      Owner of heartbeat_configs CRUD.
     * @param agents         Agent template registry.
     * @param convs          Conversation service.
     * @param msgs           Message persistence service.
     * @param router         Model router (background slot dispatch).
     * @param toolSvc        Tool registry; sub-agents receive the
     *                       same tool surface as foreground agents.
     * @param skillSvc       Skill registry; sub-agents resolve
     *                       preferred-skill lists via folder /
     *                       conversation scope.
     * @param requestBuilder Heartbeat-dedicated RequestBuilder
     *                       instance, separate from the foreground
     *                       builder so their state stays isolated.
     * @param cascadeResolver Returns the cascade of the run that should
     *                       route a surfaced report, resolved at post
     *                       time because the in-flight run varies. May
     *                       be empty or return nullptr.
     * @param chatCtrl       Optional chat controller; answers
     *                       hasInflightStreamFor(convId) for the
     *                       deferral check and supplies the
     *                       streamFinalized signal.
     * @param parent         Qt parent (AppController).
     */

    HeartbeatSubagentService(HeartbeatConfigService& configSvc,
                             AgentRegistry& agents,
                             ConversationService& convs,
                             MessageService& msgs,
                             ModelRouter& router,
                             ToolService& toolSvc,
                             SkillService& skillSvc,
                             Chat::RequestBuilder& requestBuilder,
                             CascadeResolver cascadeResolver,
                             ChatController* chatCtrl,
                             QObject* parent = nullptr);
    ~HeartbeatSubagentService() override;

    /**
     * @brief Loads enabled configs + arms the scheduling tick.
     *        Idempotent, so safe to call from AppController::initialize.
     *
     *        Missed-fire amnesty applies here: configs whose schedule
     *        would have fired during downtime do NOT replay their
     *        missed fires. The next fire is computed as max(now,
     *        lastFireAt + interval), so the schedule resumes at the
     *        correct future tick rather than dumping N runs at once.
     */
    void initialize();

    /**
     * @brief Stops all timers + cancels any in-flight run via
     *        ModelRouter::cancelBackground. Idempotent.
     */
    void shutdown();

    /**
     * @brief Reader for the globallyPaused Q_PROPERTY.
     * @returns Current pause state.
     */
    bool globallyPaused() const { return m_globallyPaused; }

    /**
     * @brief Setter for the globallyPaused Q_PROPERTY. Emits
     *        globallyPausedChanged when the value changes.
     * @param paused New pause state (true = halt all scheduling).
     */
    void setGloballyPaused(bool paused);

    /**
     * @brief Reader for the totalEnabledConfigs Q_PROPERTY.
     * @returns Current count of enabled heartbeat configs.
     */
    int totalEnabledConfigs() const;

    /**
     * @brief Reader for the recentLogLines Q_PROPERTY.
     * @returns Snapshot of the diagnostic ring buffer (most recent
     *          last). Capped at kLogRingCap entries.
     */
    QStringList recentLogLines() const { return m_logRing; }

    // -----------------------------------------------------------------
    // QML-exposed API. The HeartbeatSubagent QML singleton routes here.
    // -----------------------------------------------------------------

    /**
     * @brief Manually triggers a fire for the given config, bypassing
     *        the schedule but NOT the global pause. Used by
     *        AgentEditor's "Run now" affordance + by tests.
     * @param configId Heartbeat config UUID to fire.
     * @returns runId on success; empty string on failure (config
     *          missing, pause active, queue overflow, etc.).
     */
    Q_INVOKABLE QString runNow(const QString& configId);

    /**
     * @brief Cancels an in-flight run by runId. If the run is
     *        currently dispatching: calls
     *        ModelRouter::cancelBackground() + finalises the
     *        heartbeat_reports row with outcome="cancelled". If
     *        queued (not yet started): drops the queue entry and
     *        persists outcome="cancelled". If the runId is unknown:
     *        no-op.
     * @param runId Run UUID to cancel.
     */
    Q_INVOKABLE void cancelRun(const QString& runId);

    /**
     * @brief Recompute the next-fire-time for a config after the user
     *        edited its schedule (or enabled / disabled it). Pure
     *        bookkeeping with no LLM calls.
     * @param configId Heartbeat config UUID whose schedule to
     *                 rebuild.
     */
    Q_INVOKABLE void rebuildSchedule(const QString& configId);

    /**
     * @brief Diagnostics: current state of a config's scheduler
     *        entry.
     * @param configId Heartbeat config UUID to inspect.
     * @returns QVariantMap with keys: enabled, last_fire_at,
     *          last_fire_outcome, in-flight, queued count.
     */
    Q_INVOKABLE QVariantMap configStatus(const QString& configId) const;

    // -----------------------------------------------------------------
    // Settings → Heartbeat (Diagnostics) surface.
    // The Settings panel binds to these Q_INVOKABLEs to render the
    // "Recent runs" list, "Schedule preview", and the log ring buffer
    // without any DB / scheduler details leaking into QML.
    // -----------------------------------------------------------------

    /**
     * @brief Returns the most-recent N heartbeat_reports rows across
     *        ALL configs, ordered started_at DESC.
     * @param limit Cap on returned rows. Clamped to [1, 200].
     * @returns QVariantList of row dicts: id, configId, agentName,
     *          alias, startedAtMs, durationMs, outcome,
     *          surfaceStatus, title.
     */
    Q_INVOKABLE QVariantList recentRunsList(int limit = 50) const;

    /**
     * @brief Returns the next N scheduled fires across all enabled
     *        configs, ordered by next-fire-time ASC. Configs whose
     *        schedule is empty (manual-fire-only) are excluded.
     * @param limit Cap on returned entries. Clamped to [1, 50].
     * @returns QVariantList of entry dicts: configId, alias,
     *          schedule, nextFireMs.
     */
    Q_INVOKABLE QVariantList nextFiresPreview(int limit = 10) const;

    // -----------------------------------------------------------------
    // Manual override. Skipped reports stay visible in the activity
    // overlay with two row-level actions:
    //   "Post anyway" → manualPost(reportId, editedSummary)
    //   "Dismiss"     → manualDismiss(reportId)
    //
    // manualPost reuses the SAME doInsertAndCascade path as auto-post,
    // only the metadata.produced_by tag differs (heartbeat_surface_manual
    // vs heartbeat_surface_auto). Cascade fires normally if the post
    // contains @-mentions.
    //
    // manualDismiss is a DB-only state transition — no message is
    // inserted. The audit trail is preserved via surface_status =
    // dismissed_by_user.
    // -----------------------------------------------------------------

    /**
     * @brief Manually surface a previously-skipped or pending report
     *        as a chat post.
     * @param reportId       Report UUID. Must exist; idempotent on
     *                       already-posted rows (returns true without
     *                       re-posting).
     * @param editedSummary  User-edited post body (the overlay's
     *                       confirm-and-edit popover pre-fills with
     *                       report.summary; the user may edit before
     *                       confirming).
     * @returns true if the post was inserted (or was already
     *          posted); false on persistence failure or unknown
     *          reportId.
     */
    Q_INVOKABLE bool manualPost(const QString& reportId, const QString& editedSummary);

    /**
     * @brief Mark a report as dismissed by the user. No message is
     *        inserted; surface_status transitions to dismissed_by_user.
     * @param reportId Report UUID to dismiss.
     * @returns true on success; false on unknown reportId or DB
     *          failure.
     */
    Q_INVOKABLE bool manualDismiss(const QString& reportId);

    // -----------------------------------------------------------------
    // Test / production hooks
    // -----------------------------------------------------------------

    /**
     * @brief Test hook: override the "now" clock supplier so tests
     *        can drive deterministic schedules without sleeping.
     *        Default is QDateTime::currentDateTime in production.
     * @param fn Clock supplier callable.
     */
    void setClockFn(std::function<QDateTime()> fn);

    /**
     * @brief Test hook: override the master tick interval (ms).
     *        Default is 60000 (1 minute) in production. Tests
     *        typically use a small value to drive schedule
     *        progression quickly.
     * @param ms New tick interval in milliseconds.
     */
    void setTickIntervalMs(int ms);

    /**
     * @brief Test hook: override the per-run wall-clock timeout
     *        (ms). Default is 300000 (5 minutes).
     * @param ms New per-run timeout in milliseconds.
     */
    void setRunTimeoutMs(int ms);

    /**
     * @brief Diagnostic accessor for the queue depth.
     * @returns Number of pending entries in the dispatch FIFO.
     */
    int queueDepth() const { return m_queue.size(); }

    /**
     * @brief The runId currently dispatched on the bg slot.
     * @returns Run UUID, or empty string if the bg slot is idle.
     */
    QString inflightRunId() const { return m_inflightRunId; }

    /**
     * @brief Test-only seam: manually seed a pending-post entry on
     *        the stream-collision deferral queue. Used by the
     *        stream-collision drain test to drive `onStreamFinalized`
     *        without standing up a real ChatController +
     *        StreamingManager pair. Production code paths populate
     *        `m_pendingPosts` via doInsertAndCascade's
     *        hasInflightStreamFor check.
     * @param convId   Target conversation UUID.
     * @param reportId Report UUID the pending post belongs to.
     * @param postBody Post body text to persist on drain.
     * @param mode     `"auto"` or `"manual"`; drives the
     *                 metadata.produced_by tag on the persisted row.
     */
    void enqueuePendingPostForTest(const QString& convId,
                                   const QString& reportId,
                                   const QString& postBody,
                                   const QString& mode);

    /**
     * @brief Test-only diagnostic: number of pending posts queued
     *        for the given conv.
     * @param convId Target conversation UUID.
     * @returns Pending-post count for the bucket; 0 when the bucket
     *          is empty or absent.
     */
    int pendingPostCountForTest(const QString& convId) const;

    /**
     * @brief Test-only entry into the drain path. Forwards to the
     *        private `onStreamFinalized` slot so tests can drive the
     *        drain without standing up a real StreamingManager.
     *        Production code reaches this path through the
     *        streamFinalized signal connection set up in the ctor.
     * @param msgId        UUID of the just-finalised foreground
     *                     stream's message row.
     * @param finishReason Provider finish reason string.
     * @param ok           true iff the finalize DB write succeeded.
     */
    void invokeStreamFinalizedForTest(const QString& msgId, const QString& finishReason, bool ok);

  signals:
    /**
     * @brief Emitted when a heartbeat run begins dispatching on the
     *        background slot.
     * @param runId    Run UUID.
     * @param configId Heartbeat config UUID that produced the run.
     */
    void runStarted(QString runId, QString configId);

    /**
     * @brief Emitted when a heartbeat run completes successfully.
     * @param runId   Run UUID.
     * @param outcome Outcome string (e.g. `"ok"`, `"empty_reply"`).
     */
    void runCompleted(QString runId, QString outcome);

    /**
     * @brief Emitted when a heartbeat run fails (provider error,
     *        timeout, cancellation, etc.).
     * @param runId Run UUID.
     * @param error Human-readable error message.
     */
    void runFailed(QString runId, QString error);

    /**
     * @brief Emitted when a config's next-fire schedule changes (via
     *        rebuildSchedule, enable/disable, or completion of a fire
     *        that updates lastFireAt).
     * @param configId Heartbeat config UUID whose schedule changed.
     */
    void scheduleChanged(QString configId);

    /** @brief Notifier for the globallyPaused Q_PROPERTY. */
    void globallyPausedChanged();

    /** @brief Notifier for the totalEnabledConfigs Q_PROPERTY (and a
     *         general signal that the config catalog mutated). */
    void configsChanged();

    /** @brief Notifier for the recentLogLines Q_PROPERTY. */
    void recentLogLinesChanged();

    /**
     * @brief Emitted after Tier-2 review reaches a decision (or is
     *        gated off / rate-limited). The overlay + tests subscribe
     *        to this to know when a report's surface_status has been
     *        finalised.
     * @param reportId      Report UUID.
     * @param surfaceStatus One of HeartbeatSurfaceStatus:: constants.
     * @param messageId     Persisted message id for posted_*
     *                      outcomes; empty for skipped_* /
     *                      dismissed_by_user.
     */
    void surfaceDecision(QString reportId, QString surfaceStatus, QString messageId);

  private slots:
    /** @brief Slot: master schedule tick. Checks all enabled configs
     *         for due fires. */
    void onScheduleTick();

    /** @brief Slot: per-run wall-clock timeout. Cancels the in-flight
     *         bg dispatch and marks the report row as timed_out. */
    void onRunTimeout();

    /**
     * @brief Slot: background-slot chunk received. Accumulates into
     *        m_inflightAccumulator when the requestId matches the
     *        in-flight bg dispatch.
     * @param requestId Chunk's request id.
     * @param chunk     Streaming chunk payload.
     */
    void onBgChunk(quint64 requestId, const LlmChunk& chunk);

    /**
     * @brief Slot: background-slot request finished. Dispatches to
     *        the Tier-1 or Tier-2 finishing path based on the current
     *        InflightPhase.
     * @param requestId    Finished request id.
     * @param finishReason Provider finish reason string.
     * @param totalTokens  Aggregated token count.
     */
    void onBgFinished(quint64 requestId, const QString& finishReason, int totalTokens);

    /**
     * @brief Slot: background-slot request error. Marks the run as
     *        failed and clears in-flight state.
     * @param requestId    Failed request id.
     * @param errorMessage Human-readable error description.
     */
    void onBgError(quint64 requestId, const QString& errorMessage);

    /**
     * @brief Drain the per-conversation pendingPosts FIFO when an
     *        in-flight foreground stream finishes. Resolves msgId →
     *        convId via MessageService::getMessage; on hit, drains
     *        m_pendingPosts[convId] AND sweeps any other bucket
     *        whose target conv is no longer streaming (orphan-bucket
     *        sweep).
     * @param msgId        UUID of the just-finalised foreground
     *                     stream's message row.
     * @param finishReason Provider finish reason string.
     * @param ok           true iff the finalize DB write succeeded.
     */
    void onStreamFinalized(const QString& msgId, const QString& finishReason, bool ok);

  private:
    // -----------------------------------------------------------------
    // In-flight phase. The bg slot is shared between Tier-1 (subagent
    // run) and Tier-2 (parent review). Both can NEVER be in flight
    // simultaneously — Tier-2 is dispatched only after Tier-1 finishes
    // and clears in-flight state. The phase tag on the signal-handler
    // dispatch keeps each path's bookkeeping isolated.
    // -----------------------------------------------------------------
    enum class InflightPhase {
        None,   ///< bg slot is idle.
        Tier1,  ///< Tier-1 subagent run dispatched on bg slot.
        Tier2   ///< Tier-2 surface-review dispatched on bg slot.
    };

    // -----------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------
    void drainQueue();
    void dispatchEntry(const HeartbeatQueueEntry& entry);
    void finishCurrentRun(const QString& outcome, const QString& errorMessage = {});
    void persistReportRow(const HeartbeatReport& report);
    void updateLastFire(const QString& configId, const QString& outcome);
    bool isRateLimited(const HeartbeatConfig& cfg) const;
    int runsInLast24h(const QString& configId) const;
    QDateTime nowDt() const;

    QString
    buildSubagentRequest(const HeartbeatConfig& cfg, const QString& runId, LlmRequest& outReq);

    /**
     * @brief Resolve the provider+model a heartbeat request for
     *        `agent` should use. Mirrors RequestBuilder's per-request
     *        substitution on the background slot: the active bg
     *        provider/model is the base; the agent's modelProvider /
     *        modelName win when non-empty. An agent modelProvider
     *        that has no registered BACKGROUND provider is ignored
     *        (the active bg default stands) with a qCWarning.
     * @param agent       The heartbeat config's agent.
     * @param outProvider Resolved provider id (may be empty if
     *                    neither the agent nor the active bg slot
     *                    supplies one).
     * @param outModel    Resolved model name (same caveat).
     * @sideeffects None beyond a qCWarning on the unregistered-
     *              provider fallback path.
     */
    void
    resolveProviderModelForAgent(const Agent& agent, QString& outProvider, QString& outModel) const;

    void disconnectFromRouter();

    // -----------------------------------------------------------------
    // Tier-2 review + Tier-3 cascade + stream-collision deferral.
    // -----------------------------------------------------------------

    /**
     * @brief Entry point for the Tier-2 review path. Called from
     *        finishCurrentRun on Tier-1 success. Decides whether the
     *        report is eligible for review (gate ON, target conv
     *        set, per-conv daily cap not exceeded). When eligible,
     *        builds the surface-review request and dispatches on the
     *        bg slot. When ineligible, marks the report's
     *        parent_review_status / surface_status to record the
     *        gate outcome and emits surfaceDecision so the UI
     *        updates.
     * @param reportId Tier-1 report UUID (also the runId).
     * @param configId The config row that produced the report.
     */
    void reviewReport(const QString& reportId, const QString& configId);

    /**
     * @brief Tier-2 bg-slot completion handler. The agent's response
     *        is inspected for the [SKIP] sentinel: present →
     *        skipped_by_agent, absent → POST via doInsertAndCascade
     *        with mode "auto".
     */
    void onReviewFinished(quint64 requestId, const QString& finishReason, int totalTokens);
    void onReviewError(quint64 requestId, const QString& errorMessage);

    /**
     * @brief Tier-3 cascade entry. Persists the post-bound assistant
     *        message via MessageService::addMessage, annotates
     *        surface_status + surfaced_message_id on the report row,
     *        then invokes CascadeController::routeOrFinalize with a
     *        CascadeRouteInputs struct populated identically to
     *        ChatController's regular-turn path. Called from
     *        onReviewFinished (mode=auto) and manualPost
     *        (mode=manual).
     *
     *        The cascade handle is OPTIONAL on the ctor. When
     *        absent (e.g. tests), the message is still persisted and
     *        the report row is updated, but no routeOrFinalize call
     *        fires.
     *
     *        Stream-collision deferral check: when the target conv
     *        has an in-flight foreground stream (per
     *        ChatController::hasInflightStreamFor), the post is
     *        queued onto m_pendingPosts[convId] and the actual
     *        addMessage + routeOrFinalize fire later from
     *        drainPendingPostsFor() which is triggered by
     *        StreamingManager::streamFinalized.
     * @returns The persisted msgId on success, empty string on
     *          deferral OR on persistence failure.
     */
    QString doInsertAndCascade(const QString& reportId,
                               const QString& postBody,
                               const QString& convId,
                               const QString& mode);

    /**
     * @brief Drain m_pendingPosts[convId] when its in-flight
     *        foreground stream finalises. Each pending entry is run
     *        through the post-and-cascade path with the original
     *        mode preserved.
     */
    void drainPendingPostsFor(const QString& convId);

    /**
     * @brief Per-conv daily cap check. Counts assistant messages with
     *        metadata.produced_by ∈ {heartbeat_surface_auto,
     *        heartbeat_surface_manual} in the last 24h vs the conv's
     *        autoSurfaceMaxPerDay.
     */
    bool isAutoSurfaceRateLimited(const QString& convId) const;

    /**
     * @brief Update the surface_status / parent_review_status /
     *        linked msgId of a report row in a single UPDATE.
     */
    void updateReportSurface(const QString& reportId,
                             const QString& parentReviewStatus,
                             const QString& surfaceStatus,
                             const QString& surfacedMsgId = {});

    // -----------------------------------------------------------------
    // Owned state
    // -----------------------------------------------------------------

    HeartbeatConfigService& m_configSvc;
    AgentRegistry& m_agents;
    ConversationService& m_convs;
    MessageService& m_msgs;
    ModelRouter& m_router;
    /// Non-owning reference to the system tool registry. Tier-1
    /// sub-agents receive the SAME tool surface as regular agent
    /// turns (built-in + custom + MCP). No allowlist, no gate.
    ToolService& m_toolSvc;
    /// Non-owning ref. Tier-1 sub-agents resolve their preferred-
    /// skill list via SkillService::resolveForFolder (folder scope)
    /// or resolveForConversation (1to1 / group scope) and append the
    /// AVAILABLE SKILLS block to the heartbeat addendum.
    SkillService& m_skillSvc;
    Chat::RequestBuilder& m_requestBuilder;

    /// Resolver for the Tier-3 cascade host run, resolved
    /// per post (replaces the startup-pinned QPointer<CascadeController>).
    /// May be empty; doInsertAndCascade null-checks the resolved pointer.
    CascadeResolver m_cascadeResolver;
    /// Optional chat controller. QPointer so we don't UAF if a parent
    /// (AppController) tears it down before this service is shut down.
    /// Also the stable source of the aggregated streamFinalized signal
    /// (replaces the startup-pinned QPointer<StreamingManager>).
    QPointer<ChatController> m_chatCtrl;

    HeartbeatSubagentQueue m_queue;
    QTimer m_scheduleTimer;
    QTimer m_runTimeoutTimer;

    bool m_globallyPaused = false;

    /// Diagnostic ring buffer. Cap is sized to keep the in-app log
    /// viewer responsive without unbounded growth.
    static constexpr int kLogRingCap = 200;
    QStringList m_logRing;

    /// Append a log line to the ring buffer + emit change. Internal
    /// helper called from key dispatch / review / cascade points to
    /// give the diagnostics panel a meaningful timeline.
    void appendLogLine(const QString& line);

    /// Current bg-slot phase. Tier-1 + Tier-2 share the slot but
    /// never occupy it simultaneously.
    InflightPhase m_inflightPhase = InflightPhase::None;

    /// runId currently dispatched on the bg slot (empty when idle).
    QString m_inflightRunId;
    /// configId associated with m_inflightRunId.
    QString m_inflightConfigId;
    /// LlmRequest::requestId stamped on the in-flight bg dispatch, used
    /// to filter chunkReceived/requestFinished/requestError signals.
    quint64 m_inflightLlmRequestId = 0;
    /// Accumulator for the bg slot's chunkReceived deltas.
    QString m_inflightAccumulator;
    /// Started-at timestamp of the in-flight run.
    QDateTime m_inflightStartedAt;

    // Tier-2 review state (only valid while m_inflightPhase == Tier2).
    QString m_reviewReportId;
    QString m_reviewConvId;
    QString m_reviewConfigId;

    // -----------------------------------------------------------------
    // Pending-posts FIFO. Keyed by target convId. Each entry is a
    // {reportId, postBody, mode} bundle waiting for the
    // conversation's in-flight foreground stream to finalise.
    // -----------------------------------------------------------------
    /** @brief A single deferred post awaiting stream-collision drain. */
    struct PendingPost {
        QString reportId;
        QString postBody;
        QString mode;  ///< "auto" | "manual"
    };
    QHash<QString, QQueue<PendingPost>> m_pendingPosts;
    QMetaObject::Connection m_streamFinalizedConn;

    /// Master schedule tick interval (60s default). Override in tests.
    int m_tickIntervalMs = 60000;
    /// Per-run wall-clock timeout (300s default).
    int m_runTimeoutMs = 300000;

    /// Test-injectable clock supplier.
    std::function<QDateTime()> m_clockFn;

    // Connection handles for the bg-slot signal forwarding lifetime.
    QMetaObject::Connection m_bgChunkConn;
    QMetaObject::Connection m_bgFinishedConn;
    QMetaObject::Connection m_bgErrorConn;
};

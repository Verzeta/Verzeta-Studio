// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-subagent-service.cpp
 * @brief Tier-1 heartbeat dispatch orchestrator.
 *
 *        Drives the scheduling tick, the FIFO queue, the per-config
 *        rate-limit bookkeeping, and the async LlmRequest dispatch
 *        via ModelRouter's BACKGROUND slot. Persists `heartbeat_reports`
 *        rows directly via inline SQL helpers; the reports list model
 *        reads from the same table.
 *
 *        Every fire updates `last_fire_at` on the config row AND
 *        inserts a `heartbeat_reports` row regardless of outcome, so
 *        the scheduler anchor is reliable across crash-restart and
 *        the activity overlay sees every fire attempt for diagnostics.
 * @layer Service
 * @dependencies HeartbeatConfigService, ModelRouter, AgentRegistry,
 *               ConversationService, MessageService, DbManager,
 *               Qt6::Core, Qt6::Sql.
 */


#include "heartbeat-subagent-service.h"

#include "../api/llm-interface.h"
#include "../models/agent.h"
#include "../models/conversation.h"
#include "../models/db-manager.h"
#include "../models/message.h"
#include "../utils/heartbeat-report-parser.h"
#include "../utils/heartbeat-schedule.h"
#include "../utils/logger.h"
#include "agent-registry.h"
#include "chat-controller.h"
#include "chat/cascade-controller.h"
#include "chat/request-builder.h"
#include "chat/streaming-manager.h"
#include "conversation-service.h"
#include "heartbeat-config-service.h"
#include "message-service.h"
#include "model-router.h"
#include "skill-service.h"
#include "tool-service.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

// ---------------------------------------------------------------------------
// Internal anonymous helpers
// ---------------------------------------------------------------------------
namespace {

/**
 * @brief Build the system-prompt addendum that wraps the agent's
 *        systemPrompt for a Tier-1 background-activity run. The text
 *        below is intentionally activity-agnostic.
 */
QString buildSubagentAddendum(const Agent& agent, const HeartbeatConfig& cfg) {
    const QString aliasOrName = cfg.alias.isEmpty() ? agent.name : cfg.alias;
    return QStringLiteral("\n\n[BACKGROUND ACTIVITY MODE]\n"
                          "You are running as %1's background activity routine. The kind of "
                          "routine — research, monitoring, drafting, planning, checking, "
                          "anything else — is fully defined by your standing instruction below. "
                          "The goal IS the routine.\n\n"
                          "Your output will NOT be sent to the team directly. It is stored "
                          "privately in the heartbeat overlay; %1 will read it on the "
                          "surface-review step that follows this run and decide whether to "
                          "share with the team.\n\n"
                          "Standing instruction: %2\n\n"
                          "Do the work. Output a single structured report in this exact format "
                          "(the parser depends on these labels verbatim):\n\n"
                          "  TITLE: <one-line summary, max 100 chars>\n"
                          "  RESULTS:\n"
                          "  <full result body — what you did, what you found / produced / "
                          "observed / drafted / checked. Tool outputs, snippets, etc.>\n"
                          "  SUMMARY:\n"
                          "  <≤ 3 sentences in your normal team voice. This is what the "
                          "surface-review step will use as the basis for a chat post if it "
                          "decides to post. If nothing meaningful happened, say so.>\n\n"
                          "Use tools as needed. The TITLE / RESULTS / SUMMARY labels are "
                          "load-bearing.\n")
        .arg(aliasOrName, cfg.goal);
}

/**
 * @brief Resolves a single canonical conversation id for diagnostic
 *        purposes (LlmRequest::conversationId stamping). For 1:1 /
 *        group scopes the scope_id IS the conversation id. For folder
 *        scope the closest single conversation is the
 *        designated auto-surface target, when set. When unset
 *        (overlay-only configs) we return the folder id itself so
 *        downstream logging has SOMETHING; the request is still
 *        dispatchable because the message-history fetch handles
 *        folder scope explicitly via cross-context recall rather
 *        than getRecentMessages of this id.
 */
QString resolveContextConvId(const HeartbeatConfig& cfg) {
    switch (cfg.scopeType) {
        case HeartbeatScopeType::Conversation1to1:
        case HeartbeatScopeType::ConversationGroup:
            return cfg.scopeId;
        case HeartbeatScopeType::Folder:
            return cfg.autoSurfaceTargetConversationId.isEmpty()
                       ? cfg.scopeId
                       : cfg.autoSurfaceTargetConversationId;
    }
    return QString();
}

/**
 * @brief Generate a fresh UUID v4 string (no braces).
 */
QString newUuidString() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

HeartbeatSubagentService::HeartbeatSubagentService(HeartbeatConfigService& configSvc,
                                                   AgentRegistry& agents,
                                                   ConversationService& convs,
                                                   MessageService& msgs,
                                                   ModelRouter& router,
                                                   ToolService& toolSvc,
                                                   SkillService& skillSvc,
                                                   Chat::RequestBuilder& requestBuilder,
                                                   CascadeResolver cascadeResolver,
                                                   ChatController* chatCtrl,
                                                   QObject* parent)
    : QObject(parent)
    , m_configSvc(configSvc)
    , m_agents(agents)
    , m_convs(convs)
    , m_msgs(msgs)
    , m_router(router)
    , m_toolSvc(toolSvc)
    , m_skillSvc(skillSvc)
    , m_requestBuilder(requestBuilder)
    , m_cascadeResolver(std::move(cascadeResolver))
    , m_chatCtrl(chatCtrl)
    , m_clockFn([] { return QDateTime::currentDateTime(); }) {
    m_scheduleTimer.setSingleShot(false);
    m_scheduleTimer.setInterval(m_tickIntervalMs);
    connect(&m_scheduleTimer, &QTimer::timeout, this, &HeartbeatSubagentService::onScheduleTick);

    m_runTimeoutTimer.setSingleShot(true);
    connect(&m_runTimeoutTimer, &QTimer::timeout, this, &HeartbeatSubagentService::onRunTimeout);

    if (m_chatCtrl) {
        m_streamFinalizedConn = connect(m_chatCtrl.data(),
                                        &ChatController::streamFinalized,
                                        this,
                                        &HeartbeatSubagentService::onStreamFinalized,
                                        Qt::UniqueConnection);
    }
}

HeartbeatSubagentService::~HeartbeatSubagentService() {
    shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void HeartbeatSubagentService::initialize() {
    if (m_scheduleTimer.isActive())
        return;
    const int n = totalEnabledConfigs();
    qCInfo(verzetaUi) << "HeartbeatSubagentService: initialize, enabled configs =" << n;
    // Operator-facing log line. The missed-fire amnesty rationale
    // is documented in source comments + the plan doc; the in-app
    // diagnostic just reports the user-visible effect (no backfill).
    appendLogLine(QStringLiteral("Service started. %1 heartbeat(s) enabled. "
                                 "Schedules resume at the next due time. Runs missed "
                                 "while the app was offline are not replayed.")
                      .arg(n));
    m_scheduleTimer.start();
}

void HeartbeatSubagentService::shutdown() {
    if (m_scheduleTimer.isActive()) {
        m_scheduleTimer.stop();
    }
    m_runTimeoutTimer.stop();
    if (!m_inflightRunId.isEmpty()) {
        // Best-effort cancel — don't block destruction.
        m_router.cancelBackground();
        finishCurrentRun(HeartbeatReportOutcome::kCancelled);
    }
    disconnectFromRouter();
}

void HeartbeatSubagentService::setGloballyPaused(bool paused) {
    if (m_globallyPaused == paused)
        return;
    m_globallyPaused = paused;
    emit globallyPausedChanged();
    if (paused && !m_inflightRunId.isEmpty()) {
        // Cancel the in-flight run when the user flips the kill switch.
        cancelRun(m_inflightRunId);
    }
}

int HeartbeatSubagentService::totalEnabledConfigs() const {
    return m_configSvc.enabledConfigs().size();
}

// ---------------------------------------------------------------------------
// QML-exposed API
// ---------------------------------------------------------------------------

QString HeartbeatSubagentService::runNow(const QString& configId) {
    if (m_globallyPaused) {
        qCInfo(verzetaUi) << "HeartbeatSubagentService::runNow refused —"
                             "globally paused";
        return {};
    }
    const HeartbeatConfig cfg = m_configSvc.configById(configId);
    if (!cfg.isValid()) {
        qCWarning(verzetaUi) << "HeartbeatSubagentService::runNow — unknown config" << configId;
        return {};
    }
    HeartbeatQueueEntry entry;
    entry.configId = cfg.id;
    entry.runId = newUuidString();
    entry.scheduledAt = nowDt();

    const auto result = m_queue.enqueue(entry);
    switch (result) {
        case HeartbeatSubagentQueue::EnqueueResult::Accepted:
            drainQueue();
            return entry.runId;
        case HeartbeatSubagentQueue::EnqueueResult::DroppedThrottled:
            // For runNow we still want a deterministic record.
            updateLastFire(cfg.id, HeartbeatFireOutcome::kQueueOverflow);
            return {};
        case HeartbeatSubagentQueue::EnqueueResult::DroppedOverflow:
            updateLastFire(cfg.id, HeartbeatFireOutcome::kQueueOverflow);
            return {};
    }
    return {};
}

void HeartbeatSubagentService::cancelRun(const QString& runId) {
    if (runId.isEmpty())
        return;

    if (m_inflightRunId == runId) {
        // In-flight cancellation. Order is important: cancel the bg
        // dispatch first, then mark the row + drain.
        m_router.cancelBackground();
        finishCurrentRun(HeartbeatReportOutcome::kCancelled);
        return;
    }

    // Walk the queue to find a queued entry with this runId. We can't
    // ask the queue to lookup by runId — it's keyed on configId for
    // throttle. Pop entries to a side buffer until we find / exhaust.
    QList<HeartbeatQueueEntry> kept;
    bool found = false;
    while (!m_queue.isEmpty()) {
        auto e = m_queue.popNext();
        if (!e)
            break;
        if (e->runId == runId) {
            // Drop this one + persist a cancelled record so the user
            // sees the run never started.
            HeartbeatReport r;
            r.id = e->runId;
            r.configId = e->configId;
            r.startedAt = nowDt();
            r.completedAt = nowDt();
            r.outcome = HeartbeatReportOutcome::kCancelled;
            persistReportRow(r);
            updateLastFire(e->configId, HeartbeatFireOutcome::kCancelled);
            found = true;
            continue;
        }
        kept.append(*e);
    }
    // Re-enqueue the kept entries in original order.
    for (const auto& e : kept) {
        // Use direct re-add to avoid double-throttle counting on
        // the same logical attempt — the queue's internal state
        // already tracked these. We re-append to m_fifo's tail.
        // The public API doesn't expose this; the simplest path is
        // to call enqueue() and accept that re-enqueue may briefly
        // hit throttle thresholds. For a small kept buffer this is
        // fine.
        m_queue.enqueue(e);
    }

    if (!found) {
        qCDebug(verzetaUi) << "cancelRun — runId not found:" << runId;
    }
}

void HeartbeatSubagentService::rebuildSchedule(const QString& configId) {
    // For H1, rebuilding a schedule is purely informational — the
    // schedule tick re-reads enabled configs every tick from the
    // service, so the change is picked up automatically. Emit the
    // signal so any UI watching this config refreshes.
    emit scheduleChanged(configId);
}

QVariantMap HeartbeatSubagentService::configStatus(const QString& configId) const {
    QVariantMap out;
    const HeartbeatConfig cfg = m_configSvc.configById(configId);
    if (!cfg.isValid())
        return out;
    out.insert(QStringLiteral("enabled"), cfg.enabled);
    out.insert(QStringLiteral("schedule"), cfg.schedule);
    out.insert(QStringLiteral("lastFireAt"), cfg.lastFireAt);
    out.insert(QStringLiteral("lastFireOutcome"), cfg.lastFireOutcome);
    out.insert(QStringLiteral("inflight"), m_inflightConfigId == configId);
    out.insert(QStringLiteral("queuedCount"), m_queue.countQueuedForConfig(configId));
    return out;
}


QVariantList HeartbeatSubagentService::recentRunsList(int limit) const {
    if (limit < 1)
        limit = 1;
    if (limit > 200)
        limit = 200;
    QVariantList out;
    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("SELECT id, config_id, started_at, completed_at, outcome,"
                             "       title, surface_status "
                             "FROM heartbeat_reports "
                             "ORDER BY started_at DESC LIMIT ?"));
    q.addBindValue(limit);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "recentRunsList query failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        QVariantMap m;
        const QString reportId = q.value(0).toString();
        const QString configId = q.value(1).toString();
        const HeartbeatConfig cfg = m_configSvc.configById(configId);
        const Agent agent = cfg.agentId.isEmpty() ? Agent{} : m_agents.getAgent(cfg.agentId);

        const QDateTime startedAt = QDateTime::fromString(q.value(2).toString(), Qt::ISODateWithMs);
        const QDateTime completedAt =
            QDateTime::fromString(q.value(3).toString(), Qt::ISODateWithMs);
        qint64 durationMs = -1;
        if (startedAt.isValid() && completedAt.isValid()) {
            durationMs = completedAt.toMSecsSinceEpoch() - startedAt.toMSecsSinceEpoch();
        }

        m.insert(QStringLiteral("id"), reportId);
        m.insert(QStringLiteral("configId"), configId);
        m.insert(QStringLiteral("agentName"), agent.name);
        m.insert(QStringLiteral("alias"), cfg.alias);
        m.insert(QStringLiteral("startedAtMs"),
                 startedAt.isValid() ? QVariant(startedAt.toMSecsSinceEpoch())
                                     : QVariant(qint64(-1)));
        m.insert(QStringLiteral("durationMs"), durationMs);
        m.insert(QStringLiteral("outcome"), q.value(4).toString());
        m.insert(QStringLiteral("title"), q.value(5).toString());
        m.insert(QStringLiteral("surfaceStatus"), q.value(6).toString());
        out.append(m);
    }
    return out;
}

QVariantList HeartbeatSubagentService::nextFiresPreview(int limit) const {
    if (limit < 1)
        limit = 1;
    if (limit > 50)
        limit = 50;

    const QDateTime now = nowDt();
    const auto enabled = m_configSvc.enabledConfigs();

    // Compute (configId, alias, schedule, nextFireDt) for every
    // enabled config that has a non-empty schedule. Skip empties
    // (manual-fire-only — they never auto-fire).
    /**
     * @brief One row in the "next-fire candidates" projection. Pairs
     *        a configId / alias / schedule with its computed next
     *        fire instant for diagnostic listing.
     */
    struct Entry {
        QString configId;
        QString alias;
        QString schedule;
        QDateTime nextFire;
    };
    QList<Entry> entries;
    entries.reserve(enabled.size());

    for (const HeartbeatConfig& cfg : enabled) {
        const HeartbeatSchedule s = parseHeartbeatSchedule(cfg.schedule);
        if (!s.valid || s.kind == HeartbeatSchedule::Empty)
            continue;

        QDateTime lastDt;
        if (!cfg.lastFireAt.isEmpty()) {
            lastDt = QDateTime::fromString(cfg.lastFireAt, Qt::ISODateWithMs);
        }
        const QDateTime next = nextFireTime(s, now, lastDt);
        if (!next.isValid())
            continue;

        Entry e;
        e.configId = cfg.id;
        e.alias = cfg.alias;
        e.schedule = cfg.schedule;
        e.nextFire = next;
        entries.append(e);
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.nextFire < b.nextFire;
    });

    QVariantList out;
    const int n = std::min<int>(limit, entries.size());
    for (int i = 0; i < n; ++i) {
        QVariantMap m;
        m.insert(QStringLiteral("configId"), entries[i].configId);
        m.insert(QStringLiteral("alias"), entries[i].alias);
        m.insert(QStringLiteral("schedule"), entries[i].schedule);
        m.insert(QStringLiteral("nextFireMs"), entries[i].nextFire.toMSecsSinceEpoch());
        out.append(m);
    }
    return out;
}

void HeartbeatSubagentService::appendLogLine(const QString& line) {
    if (line.isEmpty())
        return;
    const QString stamped =
        QStringLiteral("[%1] %2").arg(nowDt().toString(QStringLiteral("HH:mm:ss")), line);
    m_logRing.append(stamped);
    if (m_logRing.size() > kLogRingCap) {
        // Drop the oldest entries; keep the ring bounded.
        m_logRing.erase(m_logRing.begin(), m_logRing.begin() + (m_logRing.size() - kLogRingCap));
    }
    emit recentLogLinesChanged();
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

void HeartbeatSubagentService::setClockFn(std::function<QDateTime()> fn) {
    m_clockFn = fn ? std::move(fn) : [] { return QDateTime::currentDateTime(); };
}

void HeartbeatSubagentService::setTickIntervalMs(int ms) {
    if (ms < 1)
        ms = 1;
    m_tickIntervalMs = ms;
    m_scheduleTimer.setInterval(ms);
}

void HeartbeatSubagentService::setRunTimeoutMs(int ms) {
    if (ms < 1)
        ms = 1;
    m_runTimeoutMs = ms;
}

// ---------------------------------------------------------------------------
// Schedule tick
// ---------------------------------------------------------------------------

void HeartbeatSubagentService::onScheduleTick() {
    if (m_globallyPaused)
        return;

    const QDateTime now = nowDt();
    const auto enabled = m_configSvc.enabledConfigs();

    for (const HeartbeatConfig& cfg : enabled) {
        const HeartbeatSchedule sched = parseHeartbeatSchedule(cfg.schedule);
        if (!sched.valid || sched.kind == HeartbeatSchedule::Empty) {
            continue;
        }

        // Compute next fire time relative to lastFireAt anchor (or
        // empty = first-ever fire).
        QDateTime lastDt;
        if (!cfg.lastFireAt.isEmpty()) {
            lastDt = QDateTime::fromString(cfg.lastFireAt, Qt::ISODateWithMs);
        }
        const QDateTime nextDt = nextFireTime(sched, now, lastDt);
        if (!nextDt.isValid())
            continue;
        if (nextDt > now)
            continue;  // not yet due

        // Due — try to enqueue.
        HeartbeatQueueEntry entry;
        entry.configId = cfg.id;
        entry.runId = newUuidString();
        entry.scheduledAt = now;

        const auto result = m_queue.enqueue(entry);
        if (result == HeartbeatSubagentQueue::EnqueueResult::DroppedThrottled ||
            result == HeartbeatSubagentQueue::EnqueueResult::DroppedOverflow) {
            // Record the drop so the anchor moves forward + the row
            // shows up in the activity overlay.
            HeartbeatReport r;
            r.id = entry.runId;
            r.configId = entry.configId;
            r.startedAt = now;
            r.completedAt = now;
            r.outcome = result == HeartbeatSubagentQueue::EnqueueResult::DroppedThrottled
                            ? HeartbeatReportOutcome::kQueueOverflow
                            : HeartbeatReportOutcome::kQueueOverflow;
            persistReportRow(r);
            updateLastFire(cfg.id, HeartbeatFireOutcome::kQueueOverflow);
        }
    }

    drainQueue();
}

// ---------------------------------------------------------------------------
// Drain + dispatch
// ---------------------------------------------------------------------------

void HeartbeatSubagentService::drainQueue() {
    if (m_globallyPaused)
        return;
    if (!m_inflightRunId.isEmpty())
        return;  // single in-flight rule
    auto entryOpt = m_queue.popNext();
    if (!entryOpt)
        return;
    dispatchEntry(*entryOpt);
}

void HeartbeatSubagentService::dispatchEntry(const HeartbeatQueueEntry& entry) {
    const HeartbeatConfig cfg = m_configSvc.configById(entry.configId);
    if (!cfg.isValid()) {
        // Config was deleted between enqueue and pop. Record + drain.
        HeartbeatReport r;
        r.id = entry.runId;
        r.configId = entry.configId;
        r.startedAt = nowDt();
        r.completedAt = nowDt();
        r.outcome = HeartbeatReportOutcome::kError;
        r.error = QStringLiteral("config no longer exists");
        persistReportRow(r);
        drainQueue();
        return;
    }

    // Per-config rate-limit check (last 24h vs maxRunsPerDay).
    if (isRateLimited(cfg)) {
        HeartbeatReport r;
        r.id = entry.runId;
        r.configId = entry.configId;
        r.startedAt = nowDt();
        r.completedAt = nowDt();
        r.outcome = HeartbeatReportOutcome::kRateLimited;
        persistReportRow(r);
        updateLastFire(cfg.id, HeartbeatFireOutcome::kRateLimited);
        drainQueue();
        return;
    }

    // Build the LlmRequest.
    LlmRequest req;
    const QString errMsg = buildSubagentRequest(cfg, entry.runId, req);
    if (!errMsg.isEmpty()) {
        HeartbeatReport r;
        r.id = entry.runId;
        r.configId = entry.configId;
        r.startedAt = nowDt();
        r.completedAt = nowDt();
        r.outcome = HeartbeatReportOutcome::kError;
        r.error = errMsg;
        persistReportRow(r);
        updateLastFire(cfg.id, HeartbeatFireOutcome::kError);
        drainQueue();
        return;
    }

    // Mark in-flight + persist a "pending" report row up front.
    m_inflightPhase = InflightPhase::Tier1;
    m_inflightRunId = entry.runId;
    m_inflightConfigId = entry.configId;
    m_inflightLlmRequestId = req.requestId;
    m_inflightAccumulator.clear();
    m_inflightStartedAt = nowDt();
    m_queue.markInflight(entry.configId);

    {
        HeartbeatReport r;
        r.id = entry.runId;
        r.configId = entry.configId;
        r.startedAt = m_inflightStartedAt;
        r.outcome = HeartbeatReportOutcome::kPending;
        persistReportRow(r);
    }

    m_bgChunkConn = connect(&m_router,
                            &ModelRouter::backgroundChunkReceived,
                            this,
                            &HeartbeatSubagentService::onBgChunk,
                            Qt::UniqueConnection);
    m_bgFinishedConn = connect(&m_router,
                               &ModelRouter::backgroundRequestFinished,
                               this,
                               &HeartbeatSubagentService::onBgFinished,
                               Qt::UniqueConnection);
    m_bgErrorConn = connect(&m_router,
                            &ModelRouter::backgroundRequestError,
                            this,
                            &HeartbeatSubagentService::onBgError,
                            Qt::UniqueConnection);

    m_runTimeoutTimer.start(m_runTimeoutMs);

    qCDebug(verzetaUi) << "HeartbeatSubagentService dispatching"
                       << "runId=" << entry.runId << "configId=" << entry.configId
                       << "agentId=" << cfg.agentId;
    appendLogLine(QStringLiteral("Run started: alias=%1 schedule=%2")
                      .arg(cfg.alias.isEmpty() ? cfg.agentId : cfg.alias,
                           cfg.schedule.isEmpty() ? QStringLiteral("manual") : cfg.schedule));
    emit runStarted(entry.runId, entry.configId);

    m_router.routeBackground(req);
}

// ---------------------------------------------------------------------------
// Background-slot signal handlers
// ---------------------------------------------------------------------------

void HeartbeatSubagentService::onBgChunk(quint64 requestId, const LlmChunk& chunk) {
    if (requestId != m_inflightLlmRequestId)
        return;
    if (m_inflightPhase == InflightPhase::None)
        return;
    m_inflightAccumulator.append(chunk.delta);
}

void HeartbeatSubagentService::onBgFinished(quint64 requestId,
                                            const QString& finishReason,
                                            int totalTokens) {
    if (requestId != m_inflightLlmRequestId)
        return;

    // Phase-aware dispatch: Tier-1 finalises the heartbeat_reports row,
    // Tier-2 parses [SKIP] vs post body and routes to doInsertAndCascade.
    if (m_inflightPhase == InflightPhase::Tier2) {
        onReviewFinished(requestId, finishReason, totalTokens);
        return;
    }
    if (m_inflightPhase != InflightPhase::Tier1)
        return;
    if (m_inflightRunId.isEmpty())
        return;

    Q_UNUSED(finishReason);
    Q_UNUSED(totalTokens);

    // Parse the structured TITLE / RESULTS / SUMMARY output.
    const ParsedHeartbeatReport parsed = parseHeartbeatReport(m_inflightAccumulator);

    // Update the report row with parsed content + final outcome.
    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("UPDATE heartbeat_reports SET "
                             "  outcome = ?,"
                             "  completed_at = ?,"
                             "  title = ?,"
                             "  body = ?,"
                             "  summary = ?"
                             " WHERE id = ?"));
    q.addBindValue(HeartbeatReportOutcome::kSuccess);
    q.addBindValue(nowDt().toString(Qt::ISODateWithMs));
    q.addBindValue(parsed.title);
    q.addBindValue(parsed.body);
    q.addBindValue(parsed.summary);
    q.addBindValue(m_inflightRunId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "onBgFinished UPDATE heartbeat_reports failed:"
                             << q.lastError().text();
    }

    finishCurrentRun(HeartbeatReportOutcome::kSuccess);
}

void HeartbeatSubagentService::onBgError(quint64 requestId, const QString& errorMessage) {
    if (requestId != m_inflightLlmRequestId)
        return;

    // Phase-aware dispatch — Tier-2 has its own error path.
    if (m_inflightPhase == InflightPhase::Tier2) {
        onReviewError(requestId, errorMessage);
        return;
    }
    if (m_inflightPhase != InflightPhase::Tier1)
        return;
    if (m_inflightRunId.isEmpty())
        return;

    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("UPDATE heartbeat_reports SET "
                             "  outcome = ?, completed_at = ?, error = ? "
                             "WHERE id = ?"));
    q.addBindValue(HeartbeatReportOutcome::kError);
    q.addBindValue(nowDt().toString(Qt::ISODateWithMs));
    q.addBindValue(errorMessage);
    q.addBindValue(m_inflightRunId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "onBgError UPDATE heartbeat_reports failed:"
                             << q.lastError().text();
    }

    finishCurrentRun(HeartbeatReportOutcome::kError, errorMessage);
}

void HeartbeatSubagentService::onRunTimeout() {
    if (m_inflightRunId.isEmpty())
        return;
    qCWarning(verzetaUi) << "HeartbeatSubagentService timeout — runId=" << m_inflightRunId;
    m_router.cancelBackground();

    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("UPDATE heartbeat_reports SET "
                             "  outcome = ?, completed_at = ? "
                             "WHERE id = ?"));
    q.addBindValue(HeartbeatReportOutcome::kTimeout);
    q.addBindValue(nowDt().toString(Qt::ISODateWithMs));
    q.addBindValue(m_inflightRunId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "onRunTimeout UPDATE failed:" << q.lastError().text();
    }

    finishCurrentRun(HeartbeatReportOutcome::kTimeout);
}

void HeartbeatSubagentService::finishCurrentRun(const QString& outcome,
                                                const QString& errorMessage) {
    Q_UNUSED(errorMessage);
    if (m_inflightRunId.isEmpty())
        return;

    const QString runId = m_inflightRunId;
    const QString configId = m_inflightConfigId;

    m_runTimeoutTimer.stop();
    disconnectFromRouter();
    m_queue.markNotInflight(configId);

    // Update last_fire_at on the config row. Map report outcomes to
    // the (slightly different) HeartbeatFireOutcome:: enumeration.
    QString fireOutcome = outcome;
    if (outcome == HeartbeatReportOutcome::kSuccess)
        fireOutcome = HeartbeatFireOutcome::kSuccess;
    else if (outcome == HeartbeatReportOutcome::kError)
        fireOutcome = HeartbeatFireOutcome::kError;
    else if (outcome == HeartbeatReportOutcome::kTimeout)
        fireOutcome = HeartbeatFireOutcome::kTimeout;
    else if (outcome == HeartbeatReportOutcome::kCancelled)
        fireOutcome = HeartbeatFireOutcome::kCancelled;
    updateLastFire(configId, fireOutcome);

    // Clear in-flight state BEFORE emitting signals so subscribers see
    // the service in idle state (they may immediately call drainQueue
    // from within their slot).
    m_inflightPhase = InflightPhase::None;
    m_inflightRunId.clear();
    m_inflightConfigId.clear();
    m_inflightLlmRequestId = 0;
    m_inflightAccumulator.clear();
    m_inflightStartedAt = QDateTime();

    if (outcome == HeartbeatReportOutcome::kSuccess) {
        emit runCompleted(runId, outcome);
        appendLogLine(QStringLiteral("Run succeeded: runId=%1").arg(runId));
    } else if (outcome == HeartbeatReportOutcome::kError ||
               outcome == HeartbeatReportOutcome::kTimeout) {
        emit runFailed(runId, outcome);
        appendLogLine(QStringLiteral("Run ended (%1): runId=%2").arg(outcome, runId));
    } else {
        emit runCompleted(runId, outcome);
        appendLogLine(QStringLiteral("Run ended (%1): runId=%2").arg(outcome, runId));
    }

    if (outcome == HeartbeatReportOutcome::kSuccess) {
        reviewReport(runId, configId);
        if (m_inflightPhase == InflightPhase::Tier2) {
            // Tier-2 took the bg slot; drain happens when it finishes.
            return;
        }
    }

    // Drain the next entry (if any).
    drainQueue();
}

void HeartbeatSubagentService::disconnectFromRouter() {
    QObject::disconnect(m_bgChunkConn);
    QObject::disconnect(m_bgFinishedConn);
    QObject::disconnect(m_bgErrorConn);
    m_bgChunkConn = {};
    m_bgFinishedConn = {};
    m_bgErrorConn = {};
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void HeartbeatSubagentService::resolveProviderModelForAgent(const Agent& agent,
                                                            QString& outProvider,
                                                            QString& outModel) const {
    // Base: the active bg default (mirrored from the foreground active
    // selection by ModelRouter::setActiveProvider).
    outProvider = m_router.activeProviderId();
    outModel = m_router.activeModelName();

    if (!agent.modelProvider.isEmpty()) {
        // Q1 (LOCKED b): the override is honoured only when a BACKGROUND
        // provider is registered for that id — heartbeat dispatch goes
        // through the bg slot. An unregistered override is ignored and
        // the active bg default stands.
        if (m_router.bgProviderForId(agent.modelProvider)) {
            outProvider = agent.modelProvider;
            if (!agent.modelName.isEmpty()) {
                outModel = agent.modelName;
            }
        } else {
            qCWarning(verzetaUi) << "HeartbeatSubagentService: agent" << agent.id
                                 << "names background-unregistered provider" << agent.modelProvider
                                 << "— falling back to active bg provider" << outProvider;
        }
    } else if (!agent.modelName.isEmpty()) {
        // Model-only override: pin a model on the active bg provider.
        outModel = agent.modelName;
    }
}

QString HeartbeatSubagentService::buildSubagentRequest(const HeartbeatConfig& cfg,
                                                       const QString& runId,
                                                       LlmRequest& outReq) {
    Q_UNUSED(runId);

    // Resolve the agent.
    const Agent agent = m_agents.getAgent(cfg.agentId);
    if (!agent.isValid()) {
        return QStringLiteral("agent %1 not found").arg(cfg.agentId);
    }
    QString resolvedProvider;
    QString resolvedModel;
    resolveProviderModelForAgent(agent, resolvedProvider, resolvedModel);
    if (resolvedProvider.isEmpty() || resolvedModel.isEmpty()) {
        return QStringLiteral("no active provider/model configured");
    }

    LlmConfig cfgCopy;
    cfgCopy.providerId = resolvedProvider;
    cfgCopy.modelName = resolvedModel;
    cfgCopy.temperature = -1;  // model defaults
    cfgCopy.maxTokens = 1024;
    cfgCopy.contextWindow = 8192;
    cfgCopy.stream = true;

    outReq = LlmRequest();
    outReq.requestId = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    outReq.conversationId = resolveContextConvId(cfg);
    outReq.config = cfgCopy;
    outReq.systemPrompt = agent.systemPrompt + buildSubagentAddendum(agent, cfg);
    outReq.turnKind = QStringLiteral("heartbeat_subagent");

    {
        SkillService::ResolvedScope skills;
        if (cfg.scopeType == HeartbeatScopeType::Folder) {
            skills = m_skillSvc.resolveForFolder(cfg.scopeId);
        } else {
            skills = m_skillSvc.resolveForConversation(resolveContextConvId(cfg));
        }
        if (!skills.preferredSkillIds.isEmpty()) {
            const int budgetTokens = qMin(1500, qMax(200, cfgCopy.contextWindow / 32));
            auto estTokens = [](const QString& s) {
                return static_cast<int>(s.toUtf8().size() / 4);
            };
            QString header;
            if (skills.exposeOnly) {
                header = QStringLiteral("\n\n=== AVAILABLE SKILLS ===\n"
                                        "These are the only skills available for this scope. "
                                        "Use one when it directly matches your goal.\n");
            } else {
                header = QStringLiteral("\n\n=== AVAILABLE SKILLS ===\n"
                                        "Preferred skills for this scope. Use one when it "
                                        "matches your goal. Call discover_skills to find "
                                        "others.\n");
            }
            int spent = estTokens(header);
            QStringList lines;
            for (const QString& sid : skills.preferredSkillIds) {
                const Skill s = m_skillSvc.skillById(sid);
                if (!s.isValid())
                    continue;
                QString line = QStringLiteral("- %1: %2\n").arg(s.id, s.description);
                if (!s.tags.isEmpty()) {
                    line += QStringLiteral("  tags: %1\n").arg(s.tags.join(", "));
                }
                if (!s.declaredTools.isEmpty()) {
                    line +=
                        QStringLiteral("  expected tools: %1\n").arg(s.declaredTools.join(", "));
                }
                const int cost = estTokens(line);
                if (spent + cost > budgetTokens)
                    continue;
                spent += cost;
                lines.append(line);
            }
            if (!lines.isEmpty()) {
                outReq.systemPrompt += header + lines.join(QString());
            }
        }
    }

    if (cfg.scopeType == HeartbeatScopeType::Folder) {
        // List child conversations of the folder. ConversationService::
        // listConversations returns them ordered by updated_at DESC;
        // we reverse iterate to interleave by created_at ASC after the
        // per-conv pulls so the agent reads context oldest-first.
        const auto children = m_convs.listConversations(cfg.scopeId);

        // Pull last-5 from each child, then sort the union by
        // createdAt ASC, then truncate to the cap of 30.
        QList<Message> pooled;
        constexpr int kPerConv = 5;
        constexpr int kFolderRecallCap = 30;
        for (const Conversation& child : children) {
            if (child.id.isEmpty())
                continue;
            const auto recent = m_msgs.getRecentMessages(child.id, kPerConv);
            for (const Message& m : recent) {
                pooled.append(m);
            }
        }
        std::sort(pooled.begin(), pooled.end(), [](const Message& a, const Message& b) {
            return a.createdAt < b.createdAt;
        });
        if (pooled.size() > kFolderRecallCap) {
            // Drop the oldest overflow; agent sees the most recent
            // kFolderRecallCap chronologically-ordered messages.
            pooled.erase(pooled.begin(), pooled.begin() + (pooled.size() - kFolderRecallCap));
        }
        for (const Message& m : pooled) {
            LlmMessage lm;
            lm.role = m.role;
            lm.content = m.content;
            outReq.messages.append(lm);
        }
    } else {
        const QString convId = resolveContextConvId(cfg);
        if (!convId.isEmpty()) {
            const auto primary = m_msgs.getRecentMessages(convId, 10);

            QList<Message> siblingPool;
            const auto convOpt = m_convs.getConversation(convId);
            if (convOpt && !convOpt->folderId.isEmpty()) {
                constexpr int kPerSibling = 5;
                constexpr int kSiblingRecallCap = 25;
                const auto siblings = m_convs.listConversations(convOpt->folderId);
                for (const Conversation& sib : siblings) {
                    if (sib.id == convId)
                        continue;  // exclude self
                    const auto recent = m_msgs.getRecentMessages(sib.id, kPerSibling);
                    for (const Message& m : recent) {
                        siblingPool.append(m);
                    }
                }
                std::sort(
                    siblingPool.begin(), siblingPool.end(), [](const Message& a, const Message& b) {
                        return a.createdAt < b.createdAt;
                    });
                if (siblingPool.size() > kSiblingRecallCap) {
                    siblingPool.erase(siblingPool.begin(),
                                      siblingPool.begin() +
                                          (siblingPool.size() - kSiblingRecallCap));
                }
            }

            // Order: siblings first (oldest broader context), then
            // the primary 10 last so they are the most-recent +
            // most-relevant context the LLM sees. Recency-weighted
            // attention biases the agent toward acting on the
            // primary conversation while seeing folder activity as
            // background.
            for (const Message& m : siblingPool) {
                // A tool row needs the tool call it answers, which this
                // context does not carry, and the APIs reject it alone.
                if (m.role == QStringLiteral("tool") || m.content.trimmed().isEmpty())
                    continue;
                LlmMessage lm;
                lm.role = m.role;
                lm.content = m.content;
                outReq.messages.append(lm);
            }
            for (const Message& m : primary) {
                // A tool row needs the tool call it answers, which this
                // context does not carry, and the APIs reject it alone.
                if (m.role == QStringLiteral("tool") || m.content.trimmed().isEmpty())
                    continue;
                LlmMessage lm;
                lm.role = m.role;
                lm.content = m.content;
                outReq.messages.append(lm);
            }
        }
    }

    outReq.availableTools = m_toolSvc.availableTools();

    return QString();  // success
}

void HeartbeatSubagentService::persistReportRow(const HeartbeatReport& report) {
    // Coerce null QStrings to empty strings — see the same comment in
    // HeartbeatConfigService::upsertConfig. Every text column in
    // heartbeat_reports is NOT NULL DEFAULT ''.
    auto bindStr = [](const QString& s) -> QString { return s.isNull() ? QStringLiteral("") : s; };

    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO heartbeat_reports ("
                             "  id, config_id, started_at, completed_at, outcome,"
                             "  title, body, summary,"
                             "  parent_review_status, surface_status,"
                             "  surfaced_message_id, error"
                             ") VALUES ("
                             "  ?, ?, ?, ?, ?,"
                             "  ?, ?, ?,"
                             "  ?, ?,"
                             "  ?, ?"
                             ")"));
    q.addBindValue(bindStr(report.id));
    q.addBindValue(bindStr(report.configId));
    q.addBindValue(bindStr(report.startedAt.isValid() ? report.startedAt.toString(Qt::ISODateWithMs)
                                                      : QString()));
    // completed_at is nullable in spirit (default-constructed = no
    // completion yet) — accept null binding here.
    q.addBindValue(report.completedAt.isValid()
                       ? QVariant(report.completedAt.toString(Qt::ISODateWithMs))
                       : QVariant(QString()));
    q.addBindValue(bindStr(report.outcome));
    q.addBindValue(bindStr(report.title));
    q.addBindValue(bindStr(report.body));
    q.addBindValue(bindStr(report.summary));
    q.addBindValue(bindStr(report.parentReviewStatus.isEmpty() ? HeartbeatReviewStatus::kPending
                                                               : report.parentReviewStatus));
    q.addBindValue(bindStr(report.surfaceStatus.isEmpty() ? HeartbeatSurfaceStatus::kPending
                                                          : report.surfaceStatus));
    q.addBindValue(bindStr(report.surfacedMessageId));
    q.addBindValue(bindStr(report.error));
    if (!q.exec()) {
        qCWarning(verzetaUi) << "persistReportRow INSERT failed:" << q.lastError().text();
    }
}

void HeartbeatSubagentService::updateLastFire(const QString& configId, const QString& outcome) {
    m_configSvc.updateLastFire(configId, nowDt().toString(Qt::ISODateWithMs), outcome);
}

bool HeartbeatSubagentService::isRateLimited(const HeartbeatConfig& cfg) const {
    if (cfg.maxRunsPerDay <= 0)
        return false;
    return runsInLast24h(cfg.id) >= cfg.maxRunsPerDay;
}

int HeartbeatSubagentService::runsInLast24h(const QString& configId) const {
    if (configId.isEmpty())
        return 0;
    const QDateTime cutoff = nowDt().addDays(-1);
    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM heartbeat_reports "
                             "WHERE config_id = ? "
                             "  AND started_at > ? "
                             "  AND outcome IN ('success', 'error', 'timeout', 'cancelled')"));
    q.addBindValue(configId);
    q.addBindValue(cutoff.toString(Qt::ISODateWithMs));
    if (!q.exec()) {
        qCWarning(verzetaUi) << "runsInLast24h query failed:" << q.lastError().text();
        return 0;
    }
    if (!q.next())
        return 0;
    return q.value(0).toInt();
}

QDateTime HeartbeatSubagentService::nowDt() const {
    if (m_clockFn)
        return m_clockFn();
    return QDateTime::currentDateTime();
}

void HeartbeatSubagentService::enqueuePendingPostForTest(const QString& convId,
                                                         const QString& reportId,
                                                         const QString& postBody,
                                                         const QString& mode) {
    if (convId.isEmpty() || reportId.isEmpty())
        return;
    PendingPost p;
    p.reportId = reportId;
    p.postBody = postBody;
    p.mode = mode.isEmpty() ? QStringLiteral("auto") : mode;
    m_pendingPosts[convId].enqueue(p);
}

int HeartbeatSubagentService::pendingPostCountForTest(const QString& convId) const {
    auto it = m_pendingPosts.find(convId);
    if (it == m_pendingPosts.end())
        return 0;
    return it.value().size();
}

void HeartbeatSubagentService::invokeStreamFinalizedForTest(const QString& msgId,
                                                            const QString& finishReason,
                                                            bool ok) {
    onStreamFinalized(msgId, finishReason, ok);
}


void HeartbeatSubagentService::reviewReport(const QString& reportId, const QString& configId) {
    if (reportId.isEmpty() || configId.isEmpty())
        return;
    if (m_globallyPaused)
        return;

    const HeartbeatConfig cfg = m_configSvc.configById(configId);
    if (!cfg.isValid())
        return;

    // Resolve the target conversation. For 1:1 / group scopes the
    // scope_id IS the target. For folder scope the
    // auto_surface_target_conversation_id picks one (H4 territory; in
    // H3 we accept that field for forward-compatibility but the
    // schedule-tick already filters folder configs out).
    QString targetConvId;
    switch (cfg.scopeType) {
        case HeartbeatScopeType::Conversation1to1:
        case HeartbeatScopeType::ConversationGroup:
            targetConvId = cfg.scopeId;
            break;
        case HeartbeatScopeType::Folder:
            targetConvId = cfg.autoSurfaceTargetConversationId;
            break;
    }

    if (targetConvId.isEmpty()) {
        updateReportSurface(reportId,
                            HeartbeatReviewStatus::kNotReviewableYet,
                            HeartbeatSurfaceStatus::kSkippedByGate);
        emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kSkippedByGate, QString());
        return;
    }

    // Gate 1 — per-conversation allow_heartbeat_auto_surface.
    const auto convOpt = m_convs.getConversation(targetConvId);
    if (!convOpt || !convOpt->allowHeartbeatAutoSurface()) {
        updateReportSurface(reportId,
                            HeartbeatReviewStatus::kNotReviewableYet,
                            HeartbeatSurfaceStatus::kSkippedByGate);
        emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kSkippedByGate, QString());
        return;
    }

    // Gate 2 — per-conv daily cap on auto-surfaced posts.
    if (isAutoSurfaceRateLimited(targetConvId)) {
        updateReportSurface(reportId,
                            HeartbeatReviewStatus::kNotReviewableYet,
                            HeartbeatSurfaceStatus::kSkippedByRateLimit);
        emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kSkippedByRateLimit, QString());
        return;
    }

    // Both gates pass — build the review request.
    const Agent agent = m_agents.getAgent(cfg.agentId);
    if (!agent.isValid()) {
        updateReportSurface(
            reportId, HeartbeatReviewStatus::kReviewed, HeartbeatSurfaceStatus::kSkippedByAgent);
        emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kSkippedByAgent, QString());
        return;
    }

    // Reload the report row from the DB so we have the freshly-parsed
    // title/body/summary (the row was just updated in onBgFinished).
    QSqlQuery rq(DbManager::instance().db());
    rq.prepare(QStringLiteral("SELECT * FROM heartbeat_reports WHERE id = ?"));
    rq.addBindValue(reportId);
    if (!rq.exec() || !rq.next()) {
        qCWarning(verzetaUi) << "reviewReport — could not reload report" << reportId;
        return;
    }
    const HeartbeatReport report = HeartbeatReport::fromSqlRecord(rq.record());

    Chat::SurfaceReviewInputs inputs;
    inputs.agentId = agent.id;
    inputs.agentName = agent.name;
    inputs.agentSystemPrompt = agent.systemPrompt;
    inputs.goal = cfg.goal;
    inputs.surfaceCriteria = cfg.surfaceCriteria;
    inputs.alias = cfg.alias;
    inputs.reportId = report.id;
    inputs.reportTitle = report.title;
    inputs.reportBody = report.body;
    inputs.reportSummary = report.summary;
    inputs.targetConvId = targetConvId;
    {
        QString reviewProvider;
        QString reviewModel;
        resolveProviderModelForAgent(agent, reviewProvider, reviewModel);
        inputs.providerId = reviewProvider;
        inputs.modelName = reviewModel;
    }

    const auto recent = m_msgs.getRecentMessages(targetConvId, 5);
    for (const auto& m : recent) {
        LlmMessage lm;
        lm.role = m.role;
        lm.content = m.content;
        inputs.recentMessages.append(lm);
    }

    const Chat::BuildResult build = Chat::buildSurfaceReviewRequest(inputs);
    if (!build.success) {
        qCWarning(verzetaUi) << "reviewReport build failed:" << build.errorReason;
        updateReportSurface(
            reportId, HeartbeatReviewStatus::kReviewed, HeartbeatSurfaceStatus::kSkippedByAgent);
        emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kSkippedByAgent, QString());
        return;
    }

    // Take the bg slot for Tier-2.
    m_inflightPhase = InflightPhase::Tier2;
    m_inflightLlmRequestId = build.request.requestId;
    m_inflightAccumulator.clear();
    m_reviewReportId = reportId;
    m_reviewConvId = targetConvId;
    m_reviewConfigId = configId;

    m_bgChunkConn = connect(&m_router,
                            &ModelRouter::backgroundChunkReceived,
                            this,
                            &HeartbeatSubagentService::onBgChunk,
                            Qt::UniqueConnection);
    m_bgFinishedConn = connect(&m_router,
                               &ModelRouter::backgroundRequestFinished,
                               this,
                               &HeartbeatSubagentService::onBgFinished,
                               Qt::UniqueConnection);
    m_bgErrorConn = connect(&m_router,
                            &ModelRouter::backgroundRequestError,
                            this,
                            &HeartbeatSubagentService::onBgError,
                            Qt::UniqueConnection);

    m_runTimeoutTimer.start(m_runTimeoutMs);

    qCDebug(verzetaUi) << "[heartbeat] Tier-2 review dispatching reportId=" << reportId
                       << "convId=" << targetConvId;
    appendLogLine(QStringLiteral("Report review started: alias=%1 target=%2")
                      .arg(cfg.alias.isEmpty() ? agent.name : cfg.alias, targetConvId));
    m_router.routeBackground(build.request);
}

void HeartbeatSubagentService::onReviewFinished(quint64 requestId,
                                                const QString& finishReason,
                                                int totalTokens) {
    Q_UNUSED(finishReason);
    Q_UNUSED(totalTokens);
    if (requestId != m_inflightLlmRequestId)
        return;
    if (m_inflightPhase != InflightPhase::Tier2)
        return;

    const QString reportId = m_reviewReportId;
    const QString convId = m_reviewConvId;
    const QString body = m_inflightAccumulator;

    // Tear down Tier-2 in-flight state — done before any onward
    // dispatch so the slot is free for the next Tier-1 / Tier-2 if
    // needed.
    m_runTimeoutTimer.stop();
    disconnectFromRouter();
    m_inflightPhase = InflightPhase::None;
    m_inflightLlmRequestId = 0;
    m_inflightAccumulator.clear();
    m_reviewReportId.clear();
    m_reviewConvId.clear();
    m_reviewConfigId.clear();

    // Detect [SKIP] sentinel. The plan template instructs the agent
    // to either return exactly [SKIP] (no surface) or 1-3 sentences
    // (surface). We accept the sentinel anywhere in the body but
    // strip it from the post text either way, defending against the
    // "agent included [SKIP] in the post itself" failure mode.
    const bool hasSkip = body.contains(QStringLiteral("[SKIP]"), Qt::CaseInsensitive);
    QString postBody = body;
    postBody.remove(QStringLiteral("[SKIP]"), Qt::CaseInsensitive);
    postBody = postBody.trimmed();

    if (hasSkip || postBody.isEmpty()) {
        updateReportSurface(
            reportId, HeartbeatReviewStatus::kReviewed, HeartbeatSurfaceStatus::kSkippedByAgent);
        emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kSkippedByAgent, QString());
        drainQueue();
        return;
    }

    // Auto-post path — through doInsertAndCascade. The deferral
    // check inside doInsertAndCascade handles that case.
    const QString msgId = doInsertAndCascade(reportId, postBody, convId, QStringLiteral("auto"));
    Q_UNUSED(msgId);

    drainQueue();
}

void HeartbeatSubagentService::onReviewError(quint64 requestId, const QString& errorMessage) {
    if (requestId != m_inflightLlmRequestId)
        return;
    if (m_inflightPhase != InflightPhase::Tier2)
        return;

    const QString reportId = m_reviewReportId;

    m_runTimeoutTimer.stop();
    disconnectFromRouter();
    m_inflightPhase = InflightPhase::None;
    m_inflightLlmRequestId = 0;
    m_inflightAccumulator.clear();
    m_reviewReportId.clear();
    m_reviewConvId.clear();
    m_reviewConfigId.clear();

    qCWarning(verzetaUi) << "[heartbeat] Tier-2 review error reportId=" << reportId
                         << "err=" << errorMessage;

    // Treat a review-error like an agent-skip for surfacing purposes —
    // the report stays visible in the overlay with manual override
    // still available.
    updateReportSurface(
        reportId, HeartbeatReviewStatus::kReviewed, HeartbeatSurfaceStatus::kSkippedByAgent);
    emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kSkippedByAgent, QString());

    drainQueue();
}


QString HeartbeatSubagentService::doInsertAndCascade(const QString& reportId,
                                                     const QString& postBody,
                                                     const QString& convId,
                                                     const QString& mode) {
    if (reportId.isEmpty() || convId.isEmpty() || postBody.isEmpty()) {
        return {};
    }

    if (m_chatCtrl && m_chatCtrl->hasInflightStreamFor(convId)) {
        PendingPost p;
        p.reportId = reportId;
        p.postBody = postBody;
        p.mode = mode;
        m_pendingPosts[convId].enqueue(p);
        qCInfo(verzetaUi) << "[heartbeat] post deferred (in-flight stream)"
                          << "reportId=" << reportId << "convId=" << convId;
        return {};
    }

    // Look up the config row (for agentId + alias on the message).
    const HeartbeatReport rOnDisk = [&]() {
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT * FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(reportId);
        if (q.exec() && q.next()) {
            return HeartbeatReport::fromSqlRecord(q.record());
        }
        return HeartbeatReport{};
    }();
    if (rOnDisk.id.isEmpty()) {
        qCWarning(verzetaUi) << "doInsertAndCascade — report not found" << reportId;
        return {};
    }
    const HeartbeatConfig cfg = m_configSvc.configById(rOnDisk.configId);
    if (!cfg.isValid()) {
        qCWarning(verzetaUi) << "doInsertAndCascade — config not found" << rOnDisk.configId;
        return {};
    }
    const Agent agent = m_agents.getAgent(cfg.agentId);

    // Persist the assistant message.
    Message msg;
    msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.conversationId = convId;
    msg.role = QStringLiteral("assistant");
    msg.content = postBody;
    msg.agentId = cfg.agentId;
    msg.memberAlias = cfg.alias;
    msg.createdAt = QDateTime::currentDateTimeUtc();
    msg.finishReason = QStringLiteral("stop");
    QJsonObject metadata;
    metadata.insert(QStringLiteral("produced_by"),
                    mode == QStringLiteral("manual") ? QStringLiteral("heartbeat_surface_manual")
                                                     : QStringLiteral("heartbeat_surface_auto"));
    metadata.insert(QStringLiteral("heartbeat_config_id"), cfg.id);
    metadata.insert(QStringLiteral("heartbeat_run_id"), reportId);
    metadata.insert(QStringLiteral("alias"), cfg.alias);
    msg.metadata = metadata;

    const QString persistedId = m_msgs.addMessage(msg);
    if (persistedId.isEmpty()) {
        qCWarning(verzetaUi) << "doInsertAndCascade — addMessage failed for conv" << convId;
        // Treat as agent-skip — the report stays visible.
        updateReportSurface(
            reportId, HeartbeatReviewStatus::kReviewed, HeartbeatSurfaceStatus::kSkippedByAgent);
        emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kSkippedByAgent, QString());
        return {};
    }

    // Update the report row's surface state + linked msgId.
    const QString surfaceStatus = mode == QStringLiteral("manual")
                                      ? HeartbeatSurfaceStatus::kPostedManual
                                      : HeartbeatSurfaceStatus::kPostedAuto;
    updateReportSurface(reportId, HeartbeatReviewStatus::kReviewed, surfaceStatus, persistedId);
    emit surfaceDecision(reportId, surfaceStatus, persistedId);

    Chat::CascadeController* cascade = m_cascadeResolver ? m_cascadeResolver() : nullptr;
    if (cascade) {
        const QString resolvedAlias = cfg.alias.isEmpty() ? agent.name : cfg.alias;
        cascade->setCurrentResponder(resolvedAlias, cfg.agentId);

        Chat::CascadeRouteInputs inputs;
        inputs.convId = convId;
        inputs.content = postBody;
        inputs.requestId = 0;  // HB has no router request id once persisted
        inputs.responderMsgId = persistedId;
        inputs.declaredTaskStatus = QString();
        inputs.finishReason = QStringLiteral("stop");
        inputs.totalTokens = 0;
        inputs.elapsedMs = 0;
        cascade->routeOrFinalize(inputs);
    }

    return persistedId;
}


void HeartbeatSubagentService::onStreamFinalized(const QString& msgId,
                                                 const QString& finishReason,
                                                 bool ok) {
    Q_UNUSED(finishReason);
    Q_UNUSED(ok);
    if (msgId.isEmpty())
        return;

    const Message m = m_msgs.getMessage(msgId);
    if (m.id.isEmpty()) {
        // Stream finalised on a deleted message — nothing actionable
        // for us; orphan-bucket sweep below still runs.
    } else if (m_pendingPosts.contains(m.conversationId)) {
        drainPendingPostsFor(m.conversationId);
    }

    // Orphan-bucket sweep: any bucket whose target conv is no longer
    // mid-streaming (per ChatController::hasInflightStreamFor) gets
    // drained. Defends against streams that finalised by a path that
    // didn't fire streamFinalized cleanly (force-close edge cases,
    // network errors that bypass the normal teardown).
    if (!m_chatCtrl)
        return;
    const QList<QString> convIds = m_pendingPosts.keys();
    for (const QString& convId : convIds) {
        if (!m_chatCtrl->hasInflightStreamFor(convId)) {
            drainPendingPostsFor(convId);
        }
    }
}

void HeartbeatSubagentService::drainPendingPostsFor(const QString& convId) {
    if (convId.isEmpty())
        return;
    auto it = m_pendingPosts.find(convId);
    if (it == m_pendingPosts.end())
        return;

    while (!it.value().isEmpty()) {
        const PendingPost p = it.value().dequeue();
        // Re-enter doInsertAndCascade. The hasInflightStreamFor check
        // at the top SHOULD now pass through — but if a new stream
        // started between the trigger and the dequeue, this entry
        // gets re-queued cleanly.
        doInsertAndCascade(p.reportId, p.postBody, convId, p.mode);
    }
    if (it.value().isEmpty()) {
        m_pendingPosts.erase(it);
    }
}


bool HeartbeatSubagentService::isAutoSurfaceRateLimited(const QString& convId) const {
    if (convId.isEmpty())
        return false;
    const auto convOpt = m_convs.getConversation(convId);
    if (!convOpt)
        return false;
    const int cap = convOpt->heartbeatAutoSurfaceMaxPerDay();
    if (cap <= 0)
        return true;  // defensive — UI clamps to >= 1

    const QDateTime cutoff = nowDt().addDays(-1);
    QSqlQuery q(DbManager::instance().db());
    // Count auto-posted heartbeat_reports rows in this conv's last 24h.
    // The link is the surfaced_message_id pointing at a message in the
    // target conv; we use the report's surface_status to filter for
    // posted_auto only (manual posts don't count toward the auto cap
    // — the user explicitly approved each one). started_at is the
    // available time anchor on the report row.
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM heartbeat_reports "
                             "WHERE surface_status = ? "
                             "  AND started_at > ? "
                             "  AND surfaced_message_id IN ("
                             "      SELECT id FROM messages WHERE conversation_id = ?"
                             "  )"));
    q.addBindValue(HeartbeatSurfaceStatus::kPostedAuto);
    q.addBindValue(cutoff.toString(Qt::ISODateWithMs));
    q.addBindValue(convId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "isAutoSurfaceRateLimited query failed:" << q.lastError().text();
        return false;
    }
    if (!q.next())
        return false;
    return q.value(0).toInt() >= cap;
}

void HeartbeatSubagentService::updateReportSurface(const QString& reportId,
                                                   const QString& parentReviewStatus,
                                                   const QString& surfaceStatus,
                                                   const QString& surfacedMsgId) {
    if (reportId.isEmpty())
        return;
    // Coerce default-constructed QStrings to empty literals — every
    // column on heartbeat_reports is NOT NULL DEFAULT ''. Qt binds a
    // null QString as SQL NULL which collides with the NOT NULL
    // constraint even though the column has a default. Same lesson
    // as upsertConfig + persistReportRow elsewhere in this service.
    auto bindStr = [](const QString& s) -> QString { return s.isNull() ? QStringLiteral("") : s; };
    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("UPDATE heartbeat_reports SET "
                             "  parent_review_status = ?, "
                             "  surface_status = ?, "
                             "  surfaced_message_id = ? "
                             "WHERE id = ?"));
    q.addBindValue(bindStr(parentReviewStatus));
    q.addBindValue(bindStr(surfaceStatus));
    q.addBindValue(bindStr(surfacedMsgId));
    q.addBindValue(reportId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "updateReportSurface failed:" << q.lastError().text();
    }
}


bool HeartbeatSubagentService::manualPost(const QString& reportId, const QString& editedSummary) {
    if (reportId.isEmpty() || editedSummary.trimmed().isEmpty())
        return false;

    // Reload the report to pick up its current surface_status.
    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("SELECT * FROM heartbeat_reports WHERE id = ?"));
    q.addBindValue(reportId);
    if (!q.exec() || !q.next()) {
        qCWarning(verzetaUi) << "manualPost — report not found" << reportId;
        return false;
    }
    const HeartbeatReport report = HeartbeatReport::fromSqlRecord(q.record());

    // Idempotent on already-posted rows.
    if (report.surfaceStatus == HeartbeatSurfaceStatus::kPostedAuto ||
        report.surfaceStatus == HeartbeatSurfaceStatus::kPostedManual) {
        qCInfo(verzetaUi) << "manualPost — report already posted, no-op" << reportId;
        return true;
    }

    const HeartbeatConfig cfg = m_configSvc.configById(report.configId);
    if (!cfg.isValid())
        return false;

    QString convId;
    switch (cfg.scopeType) {
        case HeartbeatScopeType::Conversation1to1:
        case HeartbeatScopeType::ConversationGroup:
            convId = cfg.scopeId;
            break;
        case HeartbeatScopeType::Folder:
            convId = cfg.autoSurfaceTargetConversationId;
            break;
    }
    if (convId.isEmpty()) {
        qCWarning(verzetaUi) << "manualPost — no target conversation for config" << cfg.id;
        return false;
    }

    const QString posted =
        doInsertAndCascade(reportId, editedSummary.trimmed(), convId, QStringLiteral("manual"));
    return !posted.isEmpty()
           // OR deferred (still success from the user's perspective)
           || (m_chatCtrl && m_chatCtrl->hasInflightStreamFor(convId));
}

bool HeartbeatSubagentService::manualDismiss(const QString& reportId) {
    if (reportId.isEmpty())
        return false;

    // Reload to verify existence + idempotency.
    QSqlQuery q(DbManager::instance().db());
    q.prepare(QStringLiteral("SELECT surface_status FROM heartbeat_reports WHERE id = ?"));
    q.addBindValue(reportId);
    if (!q.exec() || !q.next()) {
        qCWarning(verzetaUi) << "manualDismiss — report not found" << reportId;
        return false;
    }
    const QString currentStatus = q.value(0).toString();
    if (currentStatus == HeartbeatSurfaceStatus::kPostedAuto ||
        currentStatus == HeartbeatSurfaceStatus::kPostedManual) {
        // Refuse to dismiss a posted message — the audit trail must
        // preserve that the user / agent sent it.
        qCInfo(verzetaUi) << "manualDismiss — refused on posted report" << reportId;
        return false;
    }
    if (currentStatus == HeartbeatSurfaceStatus::kDismissedByUser) {
        return true;  // idempotent
    }

    updateReportSurface(
        reportId, HeartbeatReviewStatus::kReviewed, HeartbeatSurfaceStatus::kDismissedByUser);
    emit surfaceDecision(reportId, HeartbeatSurfaceStatus::kDismissedByUser, QString());
    return true;
}

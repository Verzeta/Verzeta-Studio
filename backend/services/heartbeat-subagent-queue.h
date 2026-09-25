// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-subagent-queue.h
 * @brief Single global FIFO for heartbeat subagent runs with
 *        per-config throttle + soft-cap backpressure. Pure
 *        data-structure-and-policy class (NOT a QObject); the
 *        HeartbeatSubagentService uses it as a building block.
 *
 *        Queue rules:
 *          - Single global FIFO across the whole app. The router's
 *            background slot supports one in-flight subagent run at
 *            a time; the queue parks the rest.
 *          - Per-config throttle: at most 1 in-flight + 1 queued per
 *            configId. Subsequent fires while throttled are DROPPED;
 *            the caller records them with
 *            outcome="queue_overflow".
 *          - Backpressure: queue depth soft cap of 100. Beyond cap,
 *            new entries are DROPPED. (No mid-queue eviction: older
 *            entries keep priority because they fired their schedule
 *            earlier.)
 *          - FIFO ordering: ties broken by (configId, runId)
 *            lexicographic so test scenarios are deterministic.
 *
 *        Wall-clock timeouts live at the dispatch layer, NOT in the
 *        queue, because the queue does not own runs.
 *
 * @layer Service (helper structure)
 * @dependencies Qt6::Core
 */

#pragma once

#include <optional>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

/**
 * @brief One entry in the heartbeat subagent FIFO.
 *
 * Carries enough context that the service can reconstruct the run's
 * identity without a follow-up DB hit at pop time. configId points at
 * the heartbeat_configs row that scheduled this fire; runId is a
 * pre-allocated UUID that becomes the heartbeat_reports row id once
 * the dispatch starts.
 */
struct HeartbeatQueueEntry {
    QString configId;       ///< References heartbeat_configs.id.
    QString runId;          ///< Pre-allocated heartbeat_reports.id.
    QDateTime scheduledAt;  ///< When the schedule fired (for diagnostics).
};

/**
 * @brief Single global FIFO + per-config throttle + soft-cap backpressure.
 *
 * NOT a QObject. The HeartbeatSubagentService owns one of these and
 * emits Qt signals on its own QObject when entries are enqueued /
 * popped / dropped, but the queue itself is pure state-and-policy
 * so it can be unit-tested without an event loop or DB.
 */
class HeartbeatSubagentQueue {
  public:
    /**
     * @brief Outcome of an enqueue attempt.
     *
     *   Accepted:          entry added to FIFO tail.
     *   DroppedThrottled:  config already has 1 inflight + 1 queued.
     *   DroppedOverflow:   queue depth >= soft cap.
     */
    enum class EnqueueResult {
        Accepted,
        DroppedThrottled,
        DroppedOverflow,
    };

    HeartbeatSubagentQueue() = default;
    ~HeartbeatSubagentQueue() = default;

    /**
     * @brief Override the soft-cap (default 100). For tests that
     *        want to exercise the overflow path with smaller numbers.
     * @param cap New soft-cap value.
     */
    void setSoftCap(int cap);

    /**
     * @brief The current soft-cap.
     * @returns Maximum queue depth before new entries are dropped.
     */
    int softCap() const { return m_softCap; }

    /**
     * @brief Append an entry to the FIFO subject to throttle +
     *        soft-cap. The caller decides whether to record the
     *        drop in heartbeat_reports (typically yes, so the
     *        last_fire_at anchor moves forward on every attempt
     *        outcome).
     * @param entry Queue entry to append.
     * @returns Accepted on success; DroppedThrottled or
     *          DroppedOverflow on rejection.
     */
    EnqueueResult enqueue(const HeartbeatQueueEntry& entry);

    /**
     * @brief Mark a config as having one in-flight run. Call when
     *        the service has dequeued an entry and dispatched its
     *        LlmRequest. The throttle prevents enqueueing a SECOND
     *        queued entry while this one is running: at most 1
     *        in-flight + 1 queued per config.
     * @param configId Config id whose run is now in flight.
     */
    void markInflight(const QString& configId);

    /**
     * @brief Mark a config as no longer in-flight. Call on success /
     *        error / timeout / cancellation, anywhere the dispatch
     *        ends. Throttle accepts new entries again afterwards.
     * @param configId Config id whose run just ended.
     */
    void markNotInflight(const QString& configId);

    /**
     * @brief Whether the given config currently has a run in flight
     *        per the queue's bookkeeping.
     * @param configId Config id to test.
     * @returns true iff that config has an active dispatch.
     */
    bool isInflight(const QString& configId) const;

    /**
     * @brief Pop the head of the FIFO.
     * @return The next entry to dispatch, or std::nullopt if the
     *         queue is empty.
     *
     * Does NOT mark the popped entry's config as in-flight; the
     * caller calls markInflight() AFTER successfully dispatching.
     * (Decoupling lets a caller pop, then defer dispatch, then
     * markInflight + dispatch atomically.)
     */
    std::optional<HeartbeatQueueEntry> popNext();

    /**
     * @brief Drop every queued (not in-flight) entry whose configId
     *        matches the argument. Used when a config is deleted,
     *        the global pause is enabled, etc.
     * @param configId Config id whose queued entries to drop.
     * @returns Number of entries removed.
     */
    int dropQueuedForConfig(const QString& configId);

    /**
     * @brief Total queued entries.
     * @returns Queue depth. Does not include the in-flight run, if
     *          any.
     */
    int size() const { return static_cast<int>(m_fifo.size()); }

    /**
     * @brief Whether the queue is empty.
     * @returns true iff size() == 0.
     */
    bool isEmpty() const { return m_fifo.isEmpty(); }

    /**
     * @brief Number of QUEUED entries for a given config (does NOT
     *        include the in-flight run).
     * @param configId Config id to count entries for.
     * @returns Queued count for that config.
     */
    int countQueuedForConfig(const QString& configId) const;

  private:
    QList<HeartbeatQueueEntry> m_fifo;
    QSet<QString> m_inflightConfigIds;
    int m_softCap = 100;
};

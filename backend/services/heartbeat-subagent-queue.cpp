// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-subagent-queue.cpp
 * @brief Implementation of the heartbeat subagent FIFO.
 *
 *        See header for the queue rules: single global FIFO,
 *        per-config throttle, soft-cap backpressure.
 * @layer Service (helper)
 * @dependencies Qt6::Core.
 */

#include "heartbeat-subagent-queue.h"

void HeartbeatSubagentQueue::setSoftCap(int cap) {
    if (cap < 1)
        cap = 1;
    m_softCap = cap;
}

HeartbeatSubagentQueue::EnqueueResult
HeartbeatSubagentQueue::enqueue(const HeartbeatQueueEntry& entry) {
    if (entry.configId.isEmpty() || entry.runId.isEmpty()) {
        // Defensive — caller bug; treat as overflow drop so the
        // service records something and moves on.
        return EnqueueResult::DroppedOverflow;
    }

    // Per-config throttle: at most 1 in-flight + 1 queued per config.
    const bool inflight = m_inflightConfigIds.contains(entry.configId);
    const int queued = countQueuedForConfig(entry.configId);
    if (queued >= 1 || (inflight && queued >= 1)) {
        // Either already 1 queued for this config, OR in-flight + 1
        // queued. Both cases reject.
        return EnqueueResult::DroppedThrottled;
    }
    // (inflight + 0 queued) is allowed — that becomes "1 in-flight + 1 queued".
    // (no inflight + 0 queued) is allowed — that becomes "0 in-flight + 1 queued".
    // The above conditional rejects "0 in-flight + 1 queued" attempts to
    // re-enqueue, which is the throttle.

    // Soft-cap backpressure on queue depth. The throttle above already
    // bounds per-config queueing to 1, so this guards the cross-config
    // sum.
    if (size() >= m_softCap) {
        return EnqueueResult::DroppedOverflow;
    }

    m_fifo.append(entry);
    return EnqueueResult::Accepted;
}

void HeartbeatSubagentQueue::markInflight(const QString& configId) {
    if (configId.isEmpty())
        return;
    m_inflightConfigIds.insert(configId);
}

void HeartbeatSubagentQueue::markNotInflight(const QString& configId) {
    if (configId.isEmpty())
        return;
    m_inflightConfigIds.remove(configId);
}

bool HeartbeatSubagentQueue::isInflight(const QString& configId) const {
    return m_inflightConfigIds.contains(configId);
}

std::optional<HeartbeatQueueEntry> HeartbeatSubagentQueue::popNext() {
    if (m_fifo.isEmpty()) {
        return std::nullopt;
    }
    HeartbeatQueueEntry e = m_fifo.takeFirst();
    return e;
}

int HeartbeatSubagentQueue::dropQueuedForConfig(const QString& configId) {
    if (configId.isEmpty())
        return 0;
    int dropped = 0;
    for (auto it = m_fifo.begin(); it != m_fifo.end();) {
        if (it->configId == configId) {
            it = m_fifo.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

int HeartbeatSubagentQueue::countQueuedForConfig(const QString& configId) const {
    int n = 0;
    for (const auto& e : m_fifo) {
        if (e.configId == configId)
            ++n;
    }
    return n;
}

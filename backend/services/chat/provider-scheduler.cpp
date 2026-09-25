// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file provider-scheduler.cpp
 * @brief Implementation of Chat::ProviderScheduler. See the header for
 *        the acquire/release invariant and the concurrency model.
 * @layer Service (Chat subsystem)
 * @dependencies See provider-scheduler.h.
 */

#include "provider-scheduler.h"

#include "../../utils/logger.h"
#include "../../utils/thread-discipline.h"

#include <QTimer>

#include <QStringLiteral>

namespace Chat {

namespace {
// Stable bucket for an empty provider id. A request that reaches the
// scheduler without a stamped providerId is a misconfiguration; folding
// it onto one bucket keeps it SERIALIZED rather than silently bypassing
// the gate (the safe failure mode).
inline QString emptyProviderSentinel() {
    return QStringLiteral("__verzeta_unstamped_provider__");
}
}  // namespace

ProviderScheduler::ProviderScheduler(QObject* parent) : QObject(parent) {
    qCInfo(verzetaUi) << "ProviderScheduler initialized (per-provider "
                         "serialize, parallel across providers, default "
                         "limit 1)";
}

ProviderScheduler::~ProviderScheduler() = default;

QString ProviderScheduler::bucketKey(const QString& providerId) {
    return providerId.isEmpty() ? emptyProviderSentinel() : providerId;
}

bool ProviderScheduler::acquire(const QString& providerId,
                                const QString& waiterId,
                                GrantCallback onGranted) {
    VERZETA_ASSERT_MAIN_THREAD();

    const QString key = bucketKey(providerId);
    ProviderState& st = m_states[key];  // lazily creates an idle state

    // Free slot → grant immediately. The caller dispatches NOW and owns
    // the matching release().
    if (st.inFlight < st.limit) {
        ++st.inFlight;
        recordPeak(st);
        return true;
    }

    // Busy → park as a FIFO waiter. Reject a duplicate enqueue so a run
    // can never occupy two queue positions for the same provider (which
    // would later double-dispatch / leak a grant).
    for (const Waiter& w : st.queue) {
        if (w.waiterId == waiterId) {
            qCWarning(verzetaUi) << "ProviderScheduler::acquire: waiter" << waiterId
                                 << "already queued for provider" << key
                                 << "— ignoring duplicate enqueue";
            return false;
        }
    }

    st.queue.append(Waiter{waiterId, std::move(onGranted)});
    qCDebug(verzetaUi) << "ProviderScheduler: provider" << key << "busy — queued waiter" << waiterId
                       << "(queue depth" << st.queue.size() << ")";
    return false;
}

void ProviderScheduler::release(const QString& providerId) {
    VERZETA_ASSERT_MAIN_THREAD();

    const QString key = bucketKey(providerId);
    auto it = m_states.find(key);
    if (it == m_states.end() || it->inFlight <= 0) {
        // Guarded no-op: a release with no matching in-flight slot can
        // only be a programming error (double release / release without
        // acquire). Never underflow the counter.
        qCWarning(verzetaUi) << "ProviderScheduler::release: no in-flight slot for provider" << key
                             << "— ignoring (double release?)";
        return;
    }

    ProviderState& st = *it;
    --st.inFlight;

    if (st.queue.isEmpty()) {
        return;  // slot freed; nobody waiting.
    }

    // Hand the freed slot to the FIFO head. Take the slot on its behalf
    // NOW (re-increment) so there is no window in which another acquire()
    // could grab the slot ahead of the already-queued waiter, then fire
    // its callback deferred to avoid release→grant recursion and to let
    // a torn-down run's existence guard run on a clean stack.
    Waiter next = st.queue.takeFirst();
    ++st.inFlight;
    recordPeak(st);

    qCDebug(verzetaUi) << "ProviderScheduler: provider" << key << "freed — granting queued waiter"
                       << next.waiterId << "(remaining queue" << st.queue.size() << ")";

    GrantCallback cb = std::move(next.onGranted);
    QTimer::singleShot(0, this, [cb = std::move(cb)]() {
        if (cb)
            cb();
    });
}

bool ProviderScheduler::cancel(const QString& waiterId) {
    VERZETA_ASSERT_MAIN_THREAD();

    bool removed = false;
    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
        ProviderState& st = it.value();
        for (int i = st.queue.size() - 1; i >= 0; --i) {
            if (st.queue.at(i).waiterId == waiterId) {
                st.queue.removeAt(i);
                removed = true;
            }
        }
    }
    if (removed) {
        qCDebug(verzetaUi) << "ProviderScheduler: cancelled queued waiter" << waiterId;
    }
    return removed;
}

void ProviderScheduler::setProviderLimit(const QString& providerId, int limit) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString key = bucketKey(providerId);
    ProviderState& st = m_states[key];
    st.limit = qMax(1, limit);

    // Raising the limit can free capacity for already-queued waiters —
    // drain as many as the new headroom allows, FIFO, deferred.
    while (st.inFlight < st.limit && !st.queue.isEmpty()) {
        Waiter next = st.queue.takeFirst();
        ++st.inFlight;
        recordPeak(st);
        GrantCallback cb = std::move(next.onGranted);
        QTimer::singleShot(0, this, [cb = std::move(cb)]() {
            if (cb)
                cb();
        });
    }
}

int ProviderScheduler::inFlightCount(const QString& providerId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto it = m_states.constFind(bucketKey(providerId));
    return it == m_states.constEnd() ? 0 : it->inFlight;
}

int ProviderScheduler::queuedCount(const QString& providerId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto it = m_states.constFind(bucketKey(providerId));
    return it == m_states.constEnd() ? 0 : it->queue.size();
}

bool ProviderScheduler::anyInFlight() const {
    VERZETA_ASSERT_MAIN_THREAD();
    for (auto it = m_states.constBegin(); it != m_states.constEnd(); ++it) {
        if (it->inFlight > 0)
            return true;
    }
    return false;
}

int ProviderScheduler::totalInFlight() const {
    VERZETA_ASSERT_MAIN_THREAD();
    int total = 0;
    for (auto it = m_states.constBegin(); it != m_states.constEnd(); ++it) {
        total += it->inFlight;
    }
    return total;
}

int ProviderScheduler::distinctProvidersInFlight() const {
    VERZETA_ASSERT_MAIN_THREAD();
    int n = 0;
    for (auto it = m_states.constBegin(); it != m_states.constEnd(); ++it) {
        if (it->inFlight > 0)
            ++n;
    }
    return n;
}

int ProviderScheduler::peakConcurrency() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_peakConcurrency;
}

int ProviderScheduler::peakProviderConcurrency(const QString& providerId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto it = m_states.constFind(bucketKey(providerId));
    return it == m_states.constEnd() ? 0 : it->peak;
}

void ProviderScheduler::recordPeak(ProviderState& st) {
    // Per-provider high-water mark (proves serialization was never
    // violated — must stay <= the provider's limit).
    if (st.inFlight > st.peak) {
        st.peak = st.inFlight;
    }
    // Global cross-provider high-water mark (proves real parallelism
    // occurred — peak >= 2 means two providers were live at once).
    const int total = totalInFlight();
    if (total > m_peakConcurrency) {
        m_peakConcurrency = total;
    }
}

}  // namespace Chat

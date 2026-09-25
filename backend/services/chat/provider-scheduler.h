// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file provider-scheduler.h
 * @brief Per-provider concurrency gate for chat dispatch. Serializes
 *        in-flight LLM requests PER PROVIDER (default 1 at a time) while
 *        allowing requests to DIFFERENT providers to run in parallel.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core only. App-level singleton owned by
 *               AppController and shared by every Chat::ConversationRun
 *               across every ChatController instance (local + wire
 *               sessions).
 *
 * Architecture: this is the single point of cross-conversation,
 * cross-session provider serialization. Before this class existed,
 * dispatch was gated globally (one in-flight LLM request for the entire
 * process, enforced by ConversationRun::m_isGenerating as a cross-
 * conversation gate plus SessionRouter::anySessionGenerating). That made
 * a second chat's send queue behind the first even when it targeted a
 * completely different provider. The scheduler replaces that global gate
 * with a per-provider one: provider A and provider B run concurrently;
 * two turns on provider A serialize (the second auto-dispatches when the
 * first frees its slot).
 *
 * Concurrency model: the desired parallelism is
 * concurrent async I/O on the MAIN THREAD (Qt event loop + per-provider
 * network access managers), NOT worker threads. There are therefore no
 * data races to guard; every method asserts main-thread residency. The
 * scheduler holds only plain integers and FIFO queues; no locks.
 *
 * ACQUIRE / RELEASE INVARIANT (the load-bearing correctness property):
 *   - Each waiter (a ConversationRun, identified by a stable string
 *     waiterId) holds AT MOST ONE provider slot at any instant, because
 *     a turn is exactly one provider request and a run runs one turn at
 *     a time.
 *   - Every successful acquire() (returns true) is paired with EXACTLY
 *     ONE release() on EVERY terminal path of that turn (finished /
 *     error / abort / stop / user-cancel). A lost release would wedge
 *     that provider permanently; a double release would let an extra
 *     waiter dispatch concurrently. Callers therefore track "do I
 *     currently hold a slot for provider P?" and make their release
 *     idempotent (release only when held, then clear the held flag).
 *   - A waiter that is still QUEUED (acquire returned false, onGranted
 *     not yet fired) must call cancel(), never release(), if its turn
 *     is torn down before the grant. cancel() removes the queued waiter
 *     without touching the in-flight count.
 *   - When a grant fires, the onGranted callback is invoked via
 *     QTimer::singleShot(0, ...) (deferred to the next event-loop turn)
 *     so release()→grant cannot recurse into a deep call stack and so a
 *     run torn down between dequeue and invoke is caught by the
 *     callback's own existence guard (it captures a QPointer to the run
 *     / re-checks the run still exists before dispatching).
 *
 * Liveness: release() always either lowers the in-flight count or hands
 * the freed slot to the next FIFO waiter, so no provider can wedge as
 * long as the acquire/release pairing above holds. There is no cross-
 * provider ordering, so no cross-provider deadlock is possible.
 */

#pragma once

#include <functional>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace Chat {

/**
 * @brief Per-provider single-flight scheduler with cross-provider
 *        parallelism.
 *
 * Owned app-level (one instance for the whole process). Every
 * ConversationRun calls acquire() before dispatching a turn and
 * release() on the turn's terminal signal. Default capacity is one
 * in-flight request per provider; setProviderLimit() can raise a
 * specific provider's cap if a future setting wants it (not used yet).
 */
class ProviderScheduler : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Callback invoked when a queued waiter is granted a slot.
     *
     * Fired deferred (QTimer::singleShot(0)) from release(). The waiter
     * dispatches its turn inside this callback. Implementations MUST
     * guard against their own destruction between enqueue and grant
     * (e.g. capture a QPointer and bail if it is null).
     */
    using GrantCallback = std::function<void()>;

    /**
     * @brief Constructs an empty scheduler with no busy providers.
     * @param parent Qt parent (typically AppController).
     */
    explicit ProviderScheduler(QObject* parent = nullptr);
    ~ProviderScheduler() override;

    ProviderScheduler(const ProviderScheduler&) = delete;
    ProviderScheduler& operator=(const ProviderScheduler&) = delete;

    /**
     * @brief Requests an in-flight slot for a provider.
     *
     * If the provider currently has a free slot (in-flight count below
     * its limit), the slot is taken immediately and the method returns
     * true; the caller dispatches NOW and is responsible for the
     * matching release(). Otherwise the {waiterId, onGranted} pair is
     * appended to the provider's FIFO queue and the method returns false;
     * the caller waits, and onGranted fires (deferred) when the slot
     * frees. A given waiterId may be enqueued only once per provider;
     * a duplicate enqueue is ignored (returns false without adding a
     * second entry) so a run can never occupy two queue positions.
     *
     * @param providerId Provider identifier (req.config.providerId).
     *                   An empty providerId is bucketed under a stable
     *                   sentinel so a misconfigured request still
     *                   serializes rather than bypassing the gate.
     * @param waiterId   Stable per-run identity (used by cancel() and
     *                   for duplicate-enqueue rejection).
     * @param onGranted  Invoked (deferred) when the slot is granted to a
     *                   queued waiter. Ignored when the call returns true
     *                   (the caller already holds the slot). Must not be
     *                   empty when the caller intends to wait.
     * @returns true if the slot was granted immediately (dispatch now),
     *          false if the request was queued (wait for onGranted).
     */
    bool acquire(const QString& providerId, const QString& waiterId, GrantCallback onGranted);

    /**
     * @brief Releases a previously-acquired slot for a provider.
     *
     * Decrements the provider's in-flight count, then (if a waiter is
     * queued for that provider) pops the FIFO head and invokes its
     * onGranted callback via QTimer::singleShot(0, ...) AFTER taking the
     * freed slot on its behalf (so the slot is never momentarily double-
     * free between dequeue and the deferred dispatch). Calling release()
     * for a provider with a zero in-flight count is a guarded no-op
     * (logged once). It can only happen on a programming error and must
     * not underflow the counter.
     *
     * @param providerId Provider whose slot is being released. Must
     *                   match the providerId passed to the paired
     *                   acquire().
     */
    void release(const QString& providerId);

    /**
     * @brief Removes a still-queued waiter from every provider queue.
     *
     * For a run whose turn was torn down (conversation deleted, send
     * aborted) AFTER it enqueued but BEFORE its grant fired. This does
     * NOT touch any in-flight count, because a queued waiter holds no slot.
     * Safe to call for a waiterId that is not queued (no-op). A run that
     * already HOLDS a slot must call release(), not cancel().
     *
     * @param waiterId Stable per-run identity to remove from all queues.
     * @returns true if a queued entry was removed; false if none matched.
     */
    bool cancel(const QString& waiterId);

    /**
     * @brief Sets the maximum concurrent in-flight requests for one
     *        provider. Default for every provider is 1 (per the LOCKED
     *        concurrency model). Raising it lets that provider run N
     *        turns at once; lowering it below the current in-flight
     *        count does not abort live requests; it simply stops new
     *        grants until the count drops under the new limit.
     * @param providerId Provider identifier.
     * @param limit      Maximum in-flight requests (clamped to >= 1).
     */
    void setProviderLimit(const QString& providerId, int limit);

    // -----------------------------------------------------------------
    // Introspection (tests + diagnostics).
    // -----------------------------------------------------------------

    /**
     * @brief Current in-flight count for a provider.
     * @param providerId Provider identifier.
     * @returns Number of slots currently held (0 when idle).
     */
    int inFlightCount(const QString& providerId) const;

    /**
     * @brief Number of waiters currently queued for a provider.
     * @param providerId Provider identifier.
     * @returns FIFO queue depth (0 when no one is waiting).
     */
    int queuedCount(const QString& providerId) const;

    /**
     * @brief Whether any provider currently has an in-flight slot.
     * @returns true if at least one provider is busy; false when fully
     *          idle.
     */
    bool anyInFlight() const;

    /**
     * @brief Total in-flight slots across ALL providers right now.
     *
     * The sum of every provider's current in-flight count. Used by the
     * concurrent stress harness to sample instantaneous cross-provider
     * concurrency: a value of 2+ proves two providers were live at the
     * same instant (true parallelism), while per-provider serialization
     * keeps each individual bucket at its limit (1 by default).
     *
     * @returns Sum of all providers' in-flight counts (0 when fully idle).
     */
    int totalInFlight() const;

    /**
     * @brief Number of DISTINCT providers that currently hold at least one
     *        in-flight slot.
     *
     * A return value >= 2 is the direct observable proof of cross-provider
     * parallelism: two or more different providers are dispatching at the
     * same instant. Distinct from totalInFlight() (which would also rise
     * if a single provider's limit were raised above 1).
     *
     * @returns Count of providers with inFlight > 0 (0 when fully idle).
     */
    int distinctProvidersInFlight() const;

    /**
     * @brief Highest totalInFlight() value ever observed since
     *        construction (the peak cross-provider concurrency).
     *
     * Updated on every slot grant (immediate acquire and deferred queue
     * grant). Lets a stress harness assert that real concurrency occurred
     * (peak >= 2) rather than every request having serialized by accident
     * of timing. Pure observability; never affects scheduling.
     *
     * @returns Maximum simultaneous in-flight slots seen across all
     *          providers (0 if nothing was ever dispatched).
     */
    int peakConcurrency() const;

    /**
     * @brief Highest in-flight count ever observed for ONE specific
     *        provider since construction.
     *
     * With the default per-provider limit of 1 this must never exceed 1;
     * a stress harness asserts exactly that to prove per-provider
     * serialization was never violated. Pure observability.
     *
     * @param providerId Provider identifier.
     * @returns Maximum in-flight count seen for that provider (0 if it
     *          was never dispatched).
     */
    int peakProviderConcurrency(const QString& providerId) const;

  private:
    /**
     * @brief One waiter parked on a provider's FIFO queue.
     */
    struct Waiter {
        QString waiterId;         ///< Stable per-run identity.
        GrantCallback onGranted;  ///< Deferred dispatch callback.
    };

    /**
     * @brief Per-provider scheduling state.
     */
    struct ProviderState {
        int inFlight = 0;       ///< Slots currently held.
        int limit = 1;          ///< Max concurrent (default 1).
        int peak = 0;           ///< Max inFlight ever seen (obs.).
        QVector<Waiter> queue;  ///< FIFO of parked waiters.
    };

    /**
     * @brief Records a fresh in-flight high-water mark after a slot was
     *        just taken. Updates the per-provider peak and the global
     *        peakConcurrency. Pure observability; called from both the
     *        immediate-acquire and deferred-grant paths so every slot
     *        grant is sampled. No scheduling side effects.
     * @param st The provider state whose slot was just incremented.
     */
    void recordPeak(ProviderState& st);

    /**
     * @brief Maps an externally-supplied provider id onto the bucket key
     *        used internally. An empty id is folded onto a stable
     *        sentinel so misconfigured requests still serialize.
     * @param providerId Raw provider id from the request config.
     * @returns Non-empty bucket key.
     */
    static QString bucketKey(const QString& providerId);

    /// Per-provider state keyed by bucket id. Created lazily on first
    /// acquire/limit touch for a provider.
    QHash<QString, ProviderState> m_states;

    /// Highest totalInFlight() ever observed (cross-provider peak).
    /// Observability only; never gates scheduling.
    int m_peakConcurrency = 0;
};

}  // namespace Chat

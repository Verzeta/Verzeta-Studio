// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file contention-ledger.h
 * @brief Process-wide diagnostic ledger for every outbound inference
 *        call (chat turns, RAGP classification, intent-confirm
 *        one-shots, summarizer generations, embeddings). Each call is
 *        recorded with queue-wait / time-to-first-token / total
 *        durations and a snapshot of which other call classes were in
 *        flight when it started, then emitted as ONE structured
 *        "LEDGER" log line under the `verzeta.ledger` category. The
 *        lines quantify backend contention (which call classes overlap
 *        a live turn on the same provider) so scheduling changes are
 *        justified by measurements instead of intuition.
 * @layer Utility
 * @dependencies Qt6::Core only (QMutex, QHash, QElapsedTimer,
 *               QLoggingCategory). No Qt event-loop dependency; safe
 *               from any thread, including inference worker threads.
 */

#pragma once

#include <QtGlobal>

#include <QString>

/**
 * @brief Static, thread-safe recorder of outbound inference calls.
 *
 * Usage per call site:
 *   const quint64 id = ContentionLedger::begin(
 *       ContentionLedger::CallClass::Ragp, providerKey, modelName);
 *   ...                                    // queued (optional phase)
 *   ContentionLedger::markDispatched(id);  // request actually sent
 *   ContentionLedger::markFirstToken(id);  // first streamed byte (optional)
 *   ContentionLedger::end(id, ok, note);   // terminal; logs the line
 *
 * Contracts:
 *  - begin() captures the classes of every other in-flight entry at
 *    that instant (the `concurrent=` field) as the overlap evidence.
 *  - end() is idempotent: the first call logs and erases the entry;
 *    later calls with the same id are no-ops. Call sites with several
 *    terminal paths may therefore end() from every one of them.
 *  - markDispatched()/markFirstToken() on an unknown/ended id are
 *    no-ops. begin() without markDispatched() reports queueMs = -1
 *    (never dispatched, e.g. aborted while parked in a queue).
 *  - All methods are safe from any thread; internal state is guarded
 *    by one mutex and the shared clock is a monotonic QElapsedTimer.
 */
class ContentionLedger {
  public:
    /**
     * @brief The workload class of one outbound inference call.
     */
    enum class CallClass {
        Turn,     ///< Interactive chat turn (incl. cascade member turns)
        Ragp,     ///< RAGP tier-3 @-mention classification
        Confirm,  ///< Deferred-action intent-confirm one-shot
        Summary,  ///< Conversation summarizer / compaction generation
        Embed,    ///< Embedding computation (index or query)
        Warm      ///< Provider capability warm (contextWindowFor)
    };

    /**
     * @brief Opens a ledger entry and snapshots the concurrent classes.
     * @param cls         Workload class of this call.
     * @param providerKey Backend identity as known by the caller
     *                    (provider id like "ollama", or host:port for
     *                    raw-HTTP callers, or "local-gguf").
     * @param model       Model name/id used for the call (may be empty).
     * @returns Non-zero entry id to pass to the other methods.
     */
    static quint64 begin(CallClass cls, const QString& providerKey, const QString& model);

    /**
     * @brief Marks the instant the request was actually sent (ends the
     *        queue-wait phase). No-op on unknown/ended ids.
     * @param id Entry id from begin().
     */
    static void markDispatched(quint64 id);

    /**
     * @brief Marks the first streamed token/byte of the response.
     *        Only the first call per entry is recorded. No-op on
     *        unknown/ended ids.
     * @param id Entry id from begin().
     */
    static void markFirstToken(quint64 id);

    /**
     * @brief Terminates the entry and emits its LEDGER log line.
     *        Idempotent: only the first call logs. No-op on unknown ids.
     * @param id   Entry id from begin().
     * @param ok   Whether the call succeeded (false = error/timeout/abort).
     * @param note Optional short outcome tag (e.g. "timeout", "aborted").
     */
    static void end(quint64 id, bool ok, const QString& note = QString());

    /**
     * @brief Number of currently open (begun, not yet ended) entries.
     * @returns Open-entry count; primarily a test seam.
     */
    static int openCount();
};

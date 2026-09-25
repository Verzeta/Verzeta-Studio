// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file iragp-backend.h
 * @brief Interface contract for RAGP classification backends.
 *        Implementations include a remote-provider backend, a
 *        local-llama.cpp backend, and a stub.
 * @layer Service
 * @dependencies Qt6::Core
 */

#pragma once

#include "ragp-types.h"

#include <QFuture>
#include <QPromise>
#include <QString>

namespace Ragp {

/**
 * @brief Pure virtual interface for a RAGP classification backend.
 *
 * Implementations perform the Tier 3 LLM-backed classification after
 * Tier 1 (rules) and Tier 2 (cache) have declined to classify. The
 * service never reaches a backend when Tiers 1 or 2 produce a conclusive
 * result, so backend throughput doesn't bound hot-path latency.
 *
 * Implementations MUST:
 *   - Be strictly non-blocking: classifyAsync returns immediately.
 *   - Deliver the result via QFuture::finished() (caller uses
 *     QFutureWatcher). NO nested event loops anywhere.
 *   - Never throw. Deliver a Classification with Intent::UNKNOWN on
 *     failure and log the cause internally.
 *   - Remain callable while isAvailable() == false; return an
 *     already-ready future with UNKNOWN.
 *
 * Threading: classifyAsync MUST be called on the main thread. The
 * future's continuation runs on the thread chosen by the backend,
 * typically the main thread (so ChatController's watcher callback
 * can mutate main-thread-only state like m_agentCascadeQueue).
 *
 * Lifetime: backends are owned by RagpService. Destruction is on the
 * main thread. Any pending futures must be cleaned up by the backend
 * in its destructor (cancel pending network replies, etc.).
 */
class IRagpBackend {
  public:
    virtual ~IRagpBackend() = default;

    /**
     * @brief Classify an agent response asynchronously.
     * @param req Request envelope with content, author, and roster.
     * @return A QFuture that completes with a Classification. On any
     *         error (network timeout, malformed response, unavailable
     *         backend), the future completes with a Classification
     *         whose targets are empty / UNKNOWN and confidence is 0.0.
     *         The future NEVER throws and NEVER cancels; it always
     *         completes, possibly with an UNKNOWN result.
     */
    virtual QFuture<Classification> classifyAsync(const Request& req) = 0;

    /**
     * @brief Whether the backend is currently ready to serve
     *        requests. Informational only; classifyAsync remains callable
     *        when false (returns an immediately-ready UNKNOWN
     *        future).
     * @returns true when the backend is operational.
     */
    virtual bool isAvailable() const = 0;

    /**
     * @brief Human-readable name for telemetry and logging.
     *        e.g. "remote:ollama", "local:llama.cpp", "stub".
     * @returns Short canonical backend identifier.
     */
    virtual QString backendName() const = 0;

    /**
     * @brief One-shot, non-cascading text completion on the SAME provider
     *        this backend is configured with (internal GGUF or remote).
     *
     * Generic escape hatch so callers that need a single yes/no-style LLM
     * answer can reuse RAGP's already-configured provider WITHOUT adding a
     * separate provider/setting and WITHOUT going through @-mention
     * classification. Used by Chat::ActionIntentConfirmer to confirm a
     * file-creation intent before firing a continuation nudge.
     *
     * Contract: strictly non-blocking; resolves via QFuture::finished();
     * NEVER throws. Returns an EMPTY string on any failure / unavailability
     * (the caller treats empty as "could not determine"). Default
     * implementation returns an already-ready empty string, so a backend
     * that cannot serve a generic completion (Stub, or a not-yet-wired
     * local backend) degrades safely.
     *
     * @param prompt The full user-role prompt to send.
     * @returns QFuture<QString> resolving to the raw completion text, or
     *          empty on failure.
     */
    virtual QFuture<QString> oneShotCompleteAsync(const QString& prompt) {
        Q_UNUSED(prompt);
        QPromise<QString> p;
        p.start();
        p.addResult(QString());
        p.finish();
        return p.future();
    }
};

}  // namespace Ragp

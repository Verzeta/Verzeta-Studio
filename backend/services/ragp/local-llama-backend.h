// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file local-llama-backend.h
 * @brief The "internal" RAGP backend, a PURE BRIDGE to the
 *        verzeta-inference sidecar (the app's single llama.cpp
 *        integration). Keeps the exact IRagpBackend surface, prompt
 *        construction (RemoteBackend::buildPrompt) and result parsing
 *        (RemoteBackend::parseProviderContent) the engine has always
 *        had; only the token generation happens out of process. No
 *        llama code, no llama linkage, no model RAM in the app,
 *        which is also what makes this backend exist in EVERY build
 *        (it was compiled out of shipped artifacts before).
 * @layer Service (RAGP subsystem)
 * @dependencies IRagpBackend, RemoteBackend statics (prompt/parse),
 *               Verzeta::Infer::InferenceSidecarHost, Qt6::Core
 *               (own worker thread for the blocking bridge calls).
 */

#pragma once

#include "iragp-backend.h"

#include <atomic>
#include <memory>
#include <QObject>
#include <QPointer>
#include <QString>

class QThread;

namespace Verzeta::Infer {
class InferenceSidecarHost;
}

namespace Ragp {

namespace detail {
class BridgeWorker;
}

/**
 * @brief Local (sidecar-backed) RAGP classification backend.
 *
 * Threading: classifyAsync/oneShotCompleteAsync are called on the main
 * thread and must not block; each dispatches onto this backend's own
 * worker thread, which is ALLOWED to block inside the sidecar host's
 * request API (that is the bridge calling convention). The worker
 * thread is joined in the destructor, which is why the AppController-
 * owned host (declared before every consumer) can never be called
 * after its own destruction.
 *
 * Model lifecycle: construction schedules an eager WARM of the GGUF on
 * the worker thread (the sidecar loads it once and keeps it resident),
 * emitting loadSucceeded/loadFailed exactly like the old in-process
 * engine, so the CascadeController auto-fallback to RemoteBackend keeps
 * working unchanged.
 */
class LocalLlamaBackend : public QObject, public IRagpBackend {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the bridge and schedules the eager model warm.
     * @param modelPath Absolute path to the instruct GGUF file.
     * @param host      Non-owning sidecar host (AppController-owned;
     *                  outlives this backend by declaration order).
     *                  nullptr ⇒ loadFailed is emitted (deferred) and
     *                  every call degrades to UNKNOWN/empty.
     * @param parent    Optional QObject parent.
     */
    explicit LocalLlamaBackend(QString modelPath,
                               Verzeta::Infer::InferenceSidecarHost* host,
                               QObject* parent = nullptr);
    ~LocalLlamaBackend() override;

    // IRagpBackend ------------------------------------------------------

    /**
     * @brief IRagpBackend: classify on the worker thread via the
     *        sidecar (json_stop generation; prompt/parse identical to
     *        the remote path's statics).
     * @param req Classification request.
     * @returns Future resolved with the Classification (UNKNOWN-filled
     *          on unavailability, timeout, or parse failure).
     */
    QFuture<Classification> classifyAsync(const Request& req) override;

    /**
     * @brief IRagpBackend: one-shot generic completion on the local
     *        model (same worker thread). Resolves to raw text; empty
     *        on failure/timeout (callers treat empty as
     *        could-not-determine).
     * @param prompt Full user-role prompt.
     * @returns QFuture<QString> with the completion text or empty.
     */
    QFuture<QString> oneShotCompleteAsync(const QString& prompt) override;

    /**
     * @brief IRagpBackend readiness: true when the eager warm succeeded
     *        and the sidecar host is still available.
     * @returns True once the model warmed successfully.
     */
    bool isAvailable() const override;

    /**
     * @brief IRagpBackend: short identifier for diagnostics.
     * @returns "local-llama" (name kept because consumers, logs and tests
     *          key on it).
     */
    QString backendName() const override;

    // Testing hooks -----------------------------------------------------

    /**
     * @brief Overrides the per-classify wait budget. The default
     *        (30 s) is generous because the FIRST classify may still
     *        be behind the model warm on a cold sidecar; steady-state
     *        classify on a warmed small instruct model is 1–2 s class.
     * @param ms New per-classify timeout in milliseconds.
     */
    void setTimeoutMs(int ms) { m_classifyTimeoutMs = ms; }

    /**
     * @brief Absolute model path this backend was constructed with.
     * @returns Absolute path to the GGUF file.
     */
    QString modelPath() const { return m_modelPath; }

  signals:
    /**
     * @brief Emitted on the main thread after the eager warm loaded
     *        the model in the sidecar. Consumers unchanged.
     */
    void loadSucceeded();

    /**
     * @brief Emitted on the main thread when the warm fails (host
     *        missing, sidecar unavailable, bad model file, …).
     *        CascadeController swaps to RemoteBackend in response,
     *        with behavior identical to the old in-process engine.
     * @param reason Human-readable reason for logging and UI.
     */
    void loadFailed(QString reason);

  private:
    QString m_modelPath;
    QPointer<Verzeta::Infer::InferenceSidecarHost> m_host;

    // Own worker thread hosting the blocking bridge calls. The worker
    // object is owned by the thread (deleteLater on finished); the
    // QPointer lets main-thread dispatch null-check after shutdown.
    std::unique_ptr<QThread> m_thread;
    QPointer<detail::BridgeWorker> m_worker;

    std::atomic<bool> m_shuttingDown{false};
    std::atomic<bool> m_available{false};

    int m_classifyTimeoutMs = 30000;
};

}  // namespace Ragp

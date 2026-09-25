// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file inference-sidecar-host.h
 * @brief App-side owner of the ONE `verzeta-inference` sidecar process
 *        (the single llama.cpp integration). Exposes a THREAD-SAFE
 *        BLOCKING request API consumed by the app's existing inference
 *        worker threads (embedding workers, the RAGP local worker).
 *        Those threads exist to block, so the bridge classes keep
 *        their surfaces and simply ride through this host. The main
 *        thread must never call the blocking API.
 *
 *        Process model: all QProcess/pipe work lives on a private IO
 *        thread (correct Qt affinity: the process object is created
 *        and used only there). Callers serialize through one mutex,
 *        submit via queued invocation, and wait on a semaphore with a
 *        timeout. The IO thread never blocks on the main thread and
 *        the main thread never services caller waits, so the
 *        shutdown-deadlock class fixed in the wire host bridge cannot
 *        occur here by construction.
 *
 *        Lifecycle: the process is spawned lazily on the first request
 *        and terminated in the destructor (stdin EOF first, which is the
 *        sidecar's polite shutdown, then bounded terminate/kill).
 *        Repeated spawn failures latch the host unavailable so a
 *        missing/broken binary degrades once, quietly, per the
 *        degrade-gracefully contract (callers fall back exactly as when the engine is
 *        absent today).
 * @layer Service
 * @dependencies Qt6::Core (QProcess, QThread), Verzeta::Infer codec.
 */

#pragma once

#include <atomic>
#include <QObject>
#include <QString>
#include <QVector>

class QThread;

namespace Verzeta::Infer {

class SidecarIoWorker;

/**
 * @brief Owner + blocking facade of the verzeta-inference process.
 *
 * Threading contract:
 *  - construct/destruct on the MAIN thread (AppController-owned;
 *    declare it BEFORE the consumers whose worker threads call it, so
 *    reverse destruction stops every caller before this host dies);
 *  - embedBlocking()/completeBlocking() from WORKER threads only;
 *  - isAvailable()/setBinaryPathOverride() from any thread.
 */
class InferenceSidecarHost : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Creates the host and its (idle) IO thread. No process is
     *        spawned until the first blocking request.
     * @param parent Qt parent (AppController).
     */
    explicit InferenceSidecarHost(QObject* parent = nullptr);

    /**
     * @brief Stops the IO thread FIRST, then shuts the child process
     *        down (EOF → bounded terminate/kill). This is the ordering
     *        contract that keeps exit abort-free.
     */
    ~InferenceSidecarHost() override;

    /**
     * @brief Overrides the sidecar binary location (tests; default is
     *        `verzeta-inference` beside the application binary).
     *        Also resets the unavailable latch.
     * @param path Absolute path to the sidecar executable.
     */
    void setBinaryPathOverride(const QString& path);

    /**
     * @brief Cheap availability probe: the binary exists and the host
     *        has not latched unavailable after repeated spawn failures.
     * @returns True when a request is worth attempting.
     */
    bool isAvailable() const;

    /**
     * @brief Blocking embed of one text on the sidecar's embed slot,
     *        loading/replacing the slot's model first when @p ggufPath
     *        differs from the currently loaded one.
     *        WORKER THREADS ONLY.
     * @param text       Text to embed (non-empty).
     * @param ggufPath   Embedding GGUF the caller is configured for.
     * @param outModelId Receives the producing model id (file
     *                   basename) so stored rows keep today's
     *                   model_used semantics.
     * @param outError   Failure reason (set only on empty return).
     * @param timeoutMs  End-to-end wait budget (covers a lazy model
     *                   load on first use).
     * @returns The embedding vector, or EMPTY on any failure (caller
     *          degrades exactly as today).
     */
    QVector<float> embedBlocking(const QString& text,
                                 const QString& ggufPath,
                                 QString* outModelId,
                                 QString* outError,
                                 int timeoutMs = 120000);

    /**
     * @brief Blocking model warm: ensures @p slot has @p ggufPath
     *        loaded (spawning the sidecar first if needed) without
     *        running any inference. Lets bridge classes pay the load
     *        cost eagerly at configure time instead of on the first
     *        classify. WORKER THREADS ONLY.
     * @param slot      "embed" or "ragp".
     * @param ggufPath  Model to load.
     * @param outError  Failure reason (set only on false).
     * @param timeoutMs Wait budget (multi-GB loads take seconds).
     * @returns True when the slot is loaded and ready.
     */
    bool warmModelBlocking(const QString& slot,
                           const QString& ggufPath,
                           QString* outError,
                           int timeoutMs = 120000);

    /**
     * @brief Blocking one-shot completion on the sidecar's ragp slot,
     *        loading/replacing the slot's model first when @p ggufPath
     *        differs. WORKER THREADS ONLY.
     * @param prompt    Full prompt BODY (the caller owns prompt
     *                  building; the sidecar owns chat-template
     *                  wrapping; see @p wrapChatTemplate).
     * @param ggufPath  Instruct GGUF the caller is configured for.
     * @param maxTokens Generation ceiling.
     * @param jsonStop  Stop at the first brace-balanced `}` (classify).
     * @param outError  Failure reason (set only on null return).
     * @param timeoutMs End-to-end wait budget.
     * @param wrapChatTemplate Ask the sidecar to wrap @p prompt as one
     *                  user message in the loaded model's own chat
     *                  template (ChatML fallback). Instruct models
     *                  need this for bare prompt bodies. The remote
     *                  RAGP path gets the same treatment implicitly
     *                  because its server templates chat messages.
     * @returns The generated text (possibly empty on an immediate
     *          end-of-generation), or a NULL QString on failure;
     *          callers distinguish via @p outError being set.
     */
    QString completeBlocking(const QString& prompt,
                             const QString& ggufPath,
                             int maxTokens,
                             bool jsonStop,
                             QString* outError,
                             int timeoutMs = 120000,
                             bool wrapChatTemplate = false);

    /**
     * @brief Starts an ASYNC streaming chat completion on the sidecar's
     *        chat slot (loading/replacing the model first when the
     *        GGUF or context window differs). MAIN-THREAD friendly:
     *        it returns immediately; deltas arrive via chatStreamChunk()
     *        and exactly one chatStreamFinished() terminates the
     *        stream. Streams share the sidecar's single request lane
     *        with the blocking calls (the process is sequential).
     * @param prompt      Full prompt (caller-side formatting).
     * @param ggufPath    Chat GGUF to serve.
     * @param nCtx        Context window for the chat slot.
     * @param maxTokens   Generation ceiling.
     * @param temperature Sampling temperature (<= 0 ⇒ greedy).
     * @returns A non-zero stream id, or 0 when refused (shutting down).
     */
    quint64 startChatStream(const QString& prompt,
                            const QString& ggufPath,
                            int nCtx,
                            int maxTokens,
                            double temperature);

    /**
     * @brief Requests cancellation of a live stream. The sidecar polls
     *        between tokens, so cancellation lands within one token;
     *        the stream still terminates through chatStreamFinished()
     *        (finish "cancelled"). Safe on unknown/finished ids.
     * @param streamId Id returned by startChatStream().
     */
    void cancelChatStream(quint64 streamId);

    /**
     * @brief Last acceleration reported for a slot's model load, meaning what
     *        ACTUALLY runs, derived by the sidecar from the ggml device
     *        table ("gpu (<device>)" or "cpu"). Empty until the slot's
     *        first successful load this session. MAIN THREAD ONLY.
     * @param slot "embed" | "ragp" | "chat".
     * @returns The acceleration label, or empty when not yet loaded.
     */
    QString slotAcceleration(const QString& slot) const;

  signals:
    /**
     * @brief A slot's model load reported its acceleration (fires on
     *        every successful load, implicit or explicit).
     * @param slot  "embed" | "ragp" | "chat".
     * @param accel "gpu (<device>)" or "cpu".
     */
    void slotAccelerationChanged(const QString& slot, const QString& accel);

    /**
     * @brief One generated text piece of a live stream.
     * @param streamId Stream id from startChatStream().
     * @param delta    UTF-8 text piece.
     */
    void chatStreamChunk(quint64 streamId, const QString& delta);

    /**
     * @brief Terminal outcome of a stream (exactly once per id).
     * @param streamId     Stream id from startChatStream().
     * @param ok           False on transport/slot errors.
     * @param finishReason "stop" | "length" | "cancelled" (ok only).
     * @param tokens       Generated piece count (ok only).
     * @param error        Failure reason (only when @p ok is false).
     */
    void chatStreamFinished(
        quint64 streamId, bool ok, const QString& finishReason, int tokens, const QString& error);

  private:
    QThread* m_ioThread = nullptr;        ///< IO thread (host-owned)
    SidecarIoWorker* m_worker = nullptr;  ///< Lives on m_ioThread
    std::atomic<bool> m_shuttingDown{false};
    quint64 m_nextStreamId = 0;  ///< main-thread only
    // Last-reported acceleration per slot (main-thread cache fed by
    // the worker's queued slotAccelerationReported signal).
    QString m_ragpAccel;
    QString m_embedAccel;
    QString m_chatAccel;
};

}  // namespace Verzeta::Infer

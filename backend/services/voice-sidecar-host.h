// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-sidecar-host.h
 * @brief App-side owner of the ONE external `verzeta-voice` daemon
 *        process: spawn, greet handshake, event relay, and shutdown.
 *        Pure transport and lifecycle; every policy decision (version
 *        skew, failure latching, call semantics) lives in
 *        VoiceCallService so this class stays a dumb, testable pipe.
 *
 *        Process model mirrors the inference sidecar host: all
 *        QProcess/pipe work lives on a private IO thread (the process
 *        object is created and used only there); results reach the
 *        main thread as queued signals. The IO thread never blocks on
 *        the main thread, so destruction cannot deadlock: the child is
 *        stopped ON the IO thread, then the thread is joined.
 * @layer Service
 * @dependencies Qt6::Core (QProcess, QThread), Verzeta::Voice protocol
 *               (which reuses the Verzeta::Infer frame codec).
 */

#pragma once

#include <atomic>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QThread;

namespace Verzeta::Voice {

class VoiceIoWorker;

/**
 * @brief Owner of the verzeta-voice daemon process and its IPC stream.
 *
 * Threading contract:
 *  - construct/destruct on the MAIN thread;
 *  - start()/stop()/sendOp() from the main thread (each is a queued
 *    hand-off to the IO thread and returns immediately);
 *  - running() from any thread (atomic snapshot, may lag one queued
 *    delivery behind the IO thread's reality);
 *  - all signals are emitted on the main thread.
 *
 * Lifecycle: start() spawns the binary and sends the greet frame; the
 * daemon's `greeted` reply is surfaced verbatim through greeted() (the
 * service judges the version). A daemon that fails to spawn, exits
 * before greeting, or stays silent past the greet deadline surfaces as
 * startFailed(). After a successful greet, an unrequested exit surfaces
 * as stopped(reason); a requested stop() surfaces as stopped("").
 */
class VoiceSidecarHost : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Creates the host and its (idle) IO thread. No process is
     *        spawned until start().
     * @param parent Qt parent.
     */
    explicit VoiceSidecarHost(QObject* parent = nullptr);

    /**
     * @brief Stops the child process ON the IO thread (EOF, then
     *        bounded terminate/kill), then joins the IO thread.
     */
    ~VoiceSidecarHost() override;

    /**
     * @brief Spawns the daemon at @p binaryPath and starts the greet
     *        handshake. Returns immediately; the outcome arrives as
     *        exactly one of greeted() or startFailed(). A no-op when
     *        the daemon is already running.
     * @param binaryPath Absolute path of the verzeta-voice binary.
     */
    void start(const QString& binaryPath);

    /**
     * @brief Requests a graceful stop (shutdown op, then stdin EOF,
     *        then a bounded terminate/kill escalation). Returns
     *        immediately; stopped("") follows. A no-op when nothing is
     *        running.
     */
    void stop();

    /**
     * @brief Whether the daemon is running and has greeted.
     * @returns Atomic snapshot; may lag one queued delivery.
     */
    bool running() const { return m_running.load(std::memory_order_acquire); }

    /**
     * @brief Sends one op frame to the daemon (fire-and-forget). Ops
     *        sent while the daemon is not running-and-greeted are
     *        dropped with a warning; senders that care about delivery
     *        must gate on running() or the stopped() signal.
     * @param op      Protocol op name (voice-protocol.h constants).
     * @param payload Op payload; the "op" field is added by the host.
     */
    void sendOp(const QString& op, const QJsonObject& payload = {});

    /**
     * @brief Overrides the greet deadline (default 10000 ms). Test and
     *        diagnostics hook; call before start().
     * @param ms Milliseconds the daemon has to reply `greeted`.
     */
    void setGreetTimeoutMs(int ms);

  signals:
    /**
     * @brief The daemon replied to the greet. The payload is the raw
     *        `greeted` frame header (proto_version, pack_version,
     *        voices, stt_models); version policy is the caller's.
     * @param payload The daemon's greeted frame header.
     */
    void greeted(const QJsonObject& payload);

    /**
     * @brief The start attempt failed: spawn error, exit before
     *        greeting, or greet deadline expiry. The process is
     *        already stopped when this fires.
     * @param reason Human-readable failure reason.
     */
    void startFailed(const QString& reason);

    /**
     * @brief One daemon event frame (anything but `greeted`).
     * @param op      Event name (voice-protocol.h constants).
     * @param payload The full frame header.
     */
    void eventReceived(const QString& op, const QJsonObject& payload);

    /**
     * @brief The daemon stopped after a successful greet.
     * @param reason Empty for a requested stop(); otherwise why the
     *        daemon went away (crash, protocol error).
     */
    void stopped(const QString& reason);

  private:
    QThread* m_ioThread = nullptr;      ///< IO thread (host-owned)
    VoiceIoWorker* m_worker = nullptr;  ///< Lives on m_ioThread
    std::atomic<bool> m_running{false};
};

}  // namespace Verzeta::Voice

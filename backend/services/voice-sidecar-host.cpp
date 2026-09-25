// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-sidecar-host.cpp
 * @brief IO-thread worker owning the verzeta-voice QProcess: spawn,
 *        greet deadline, frame decode/relay, and the EOF-first bounded
 *        shutdown escalation.
 * @layer Service
 * @dependencies Qt6::Core (QProcess, QThread, QTimer), voice-protocol.h.
 */


#include "voice-sidecar-host.h"

#include "../utils/thread-discipline.h"
#include "../voice/voice-protocol.h"

#include <QThread>
#include <QTimer>

#include <QLoggingCategory>
#include <QProcess>

Q_LOGGING_CATEGORY(verzetaVoiceHost, "verzeta.voice.host", QtInfoMsg)

namespace Verzeta::Voice {

namespace {
/** Default deadline for the daemon's greeted reply. */
constexpr int kDefaultGreetTimeoutMs = 10000;
/** Grace after the shutdown op / EOF before terminate(). */
constexpr int kStopGraceMs = 2000;
/** Grace after terminate() before kill(). */
constexpr int kKillGraceMs = 1000;
}  // namespace

/**
 * @brief IO-thread resident: owns the QProcess (correct Qt affinity),
 *        the frame reader, and the greet deadline. Every member is
 *        touched on the IO thread only.
 */
class VoiceIoWorker : public QObject {
    Q_OBJECT

  public:
    int m_greetTimeoutMs = kDefaultGreetTimeoutMs;  ///< How long to wait for the greeted reply.

    /**
     * @brief Spawns the daemon and begins the greet handshake.
     * @param binaryPath Absolute path of the verzeta-voice binary.
     */
    void start(const QString& binaryPath) {
        if (m_proc && m_proc->state() != QProcess::NotRunning) {
            return;  // Already running; start() is idempotent.
        }
        if (!m_proc) {
            m_proc = new QProcess(this);  // IO-thread affinity
            connect(m_proc, &QProcess::readyReadStandardOutput, this, &VoiceIoWorker::onStdout);
            connect(m_proc, &QProcess::readyReadStandardError, this, &VoiceIoWorker::onStderr);
            connect(m_proc, &QProcess::finished, this, &VoiceIoWorker::onFinished);
        }
        if (!m_greetTimer) {
            m_greetTimer = new QTimer(this);
            m_greetTimer->setSingleShot(true);
            connect(m_greetTimer, &QTimer::timeout, this, &VoiceIoWorker::onGreetDeadline);
        }
        m_reader.clear();
        m_stopRequested = false;
        m_greetedOk = false;

        m_proc->start(binaryPath, QStringList{});
        if (!m_proc->waitForStarted(5000)) {
            emit workerStartFailed(
                QStringLiteral("Verzeta-Voice failed to start: %1").arg(m_proc->errorString()));
            return;
        }
        qCInfo(verzetaVoiceHost) << "spawned" << binaryPath << "pid" << m_proc->processId();

        QJsonObject greet;
        greet.insert(QLatin1String("op"), QLatin1String(kOpGreet));
        greet.insert(QLatin1String("proto_version"), kVoiceProtocolVersion);
        QString err;
        if (!sendFrame(greet, &err)) {
            stopProcess();
            emit workerStartFailed(
                QStringLiteral("Could not send the start-up greeting to Verzeta-Voice: %1")
                    .arg(err));
            return;
        }
        m_awaitingGreet = true;
        // Bounded deadline (single-shot, cancelled on greeted) — the
        // escalation-deadline idiom, not a poll.
        m_greetTimer->start(m_greetTimeoutMs);
    }

    /** @brief Graceful stop: shutdown op, EOF, bounded escalation.
     *         onFinished() emits the workerStopped(""). */
    void stop() {
        if (!m_proc || m_proc->state() == QProcess::NotRunning) {
            return;
        }
        m_stopRequested = true;
        QJsonObject bye;
        bye.insert(QLatin1String("op"), QLatin1String(kOpShutdown));
        QString ignored;
        sendFrame(bye, &ignored);     // Best effort; EOF is the contract.
        m_proc->closeWriteChannel();  // stdin EOF = the clean signal
        // Bounded escalation if the daemon ignores the EOF.
        QTimer::singleShot(kStopGraceMs, this, [this]() {
            if (m_proc && m_proc->state() != QProcess::NotRunning) {
                m_proc->terminate();
                QTimer::singleShot(kKillGraceMs, this, [this]() {
                    if (m_proc && m_proc->state() != QProcess::NotRunning) {
                        m_proc->kill();
                    }
                });
            }
        });
    }

    /** @brief Destructor-path stop: fully synchronous on the IO thread
     *         (EOF → bounded waits → terminate → kill). No signals. */
    void shutdownNow() {
        m_stopRequested = true;
        if (m_greetTimer)
            m_greetTimer->stop();
        if (!m_proc || m_proc->state() == QProcess::NotRunning)
            return;
        m_proc->closeWriteChannel();
        if (!m_proc->waitForFinished(kStopGraceMs)) {
            m_proc->terminate();
            if (!m_proc->waitForFinished(kKillGraceMs)) {
                m_proc->kill();
                m_proc->waitForFinished(500);
            }
        }
    }

    /**
     * @brief Sends one op frame; drops (with a warning) when the
     *        daemon is not running-and-greeted.
     * @param op      Protocol op name (voice-protocol.h constants).
     * @param payload Op payload; the "op" field is added here.
     */
    void sendOp(const QString& op, const QJsonObject& payload) {
        if (!m_proc || m_proc->state() == QProcess::NotRunning || !m_greetedOk) {
            qCWarning(verzetaVoiceHost) << "dropped op (daemon not ready):" << op;
            return;
        }
        QJsonObject header = payload;
        header.insert(QLatin1String("op"), op);
        QString err;
        if (!sendFrame(header, &err)) {
            qCWarning(verzetaVoiceHost) << "send failed for" << op << err;
        }
    }

  signals:
    /**
     * @brief Daemon greeted.
     * @param payload The raw greeted frame header.
     */
    void workerGreeted(const QJsonObject& payload);
    /**
     * @brief Spawn/greet failure; the process is already stopped.
     * @param reason Human-readable failure reason.
     */
    void workerStartFailed(const QString& reason);
    /**
     * @brief One non-greeted daemon frame.
     * @param op      Event name from the frame header.
     * @param payload The full frame header.
     */
    void workerEvent(const QString& op, const QJsonObject& payload);
    /**
     * @brief Post-greet exit.
     * @param reason Empty on a requested stop; otherwise the cause.
     */
    void workerStopped(const QString& reason);

  private slots:
    /** @brief Feeds fresh stdout bytes to the frame reader. */
    void onStdout() {
        const QByteArray chunk = m_proc->readAllStandardOutput();
        const bool ok = m_reader.feed(chunk, [this](const VoiceFrame& f) { handleFrame(f); });
        if (!ok) {
            qCWarning(verzetaVoiceHost) << "protocol error: oversized frame";
            m_stopRequested = false;  // Surface as an unexpected stop.
            stopProcess();
        }
    }

    /** @brief Echoes the daemon's stderr under the debug category. */
    void onStderr() {
        // The daemon's own log lines; visible under the debug category.
        const QByteArray text = m_proc->readAllStandardError();
        qCDebug(verzetaVoiceHost) << "daemon:" << text.trimmed();
    }

    /**
     * @brief Routes a process exit to the single correct outcome
     *        signal (requested stop, pre-greet failure, or crash).
     * @param exitCode The child's exit code.
     * @param status   Normal exit or crash, per QProcess.
     */
    void onFinished(int exitCode, QProcess::ExitStatus status) {
        if (m_greetTimer)
            m_greetTimer->stop();
        const bool wasAwaitingGreet = m_awaitingGreet;
        m_awaitingGreet = false;
        if (m_stopRequested) {
            emit workerStopped(QString());
            return;
        }
        const QString detail = (status == QProcess::CrashExit)
                                   ? QStringLiteral("crashed")
                                   : QStringLiteral("exit code %1").arg(exitCode);
        if (wasAwaitingGreet || !m_greetedOk) {
            emit workerStartFailed(
                QStringLiteral("Verzeta-Voice exited before sending its greeting (%1)")
                    .arg(detail));
        } else {
            m_greetedOk = false;
            emit workerStopped(
                QStringLiteral("Verzeta-Voice stopped unexpectedly (%1)").arg(detail));
        }
    }

    /** @brief Greet deadline expiry: stop the child, fail the start. */
    void onGreetDeadline() {
        if (!m_awaitingGreet)
            return;
        m_awaitingGreet = false;
        m_stopRequested = true;  // Suppress the onFinished stop signal…
        stopProcess();
        m_stopRequested = false;
        emit workerStartFailed(
            QStringLiteral("Verzeta-Voice did not send its greeting within %1 ms")
                .arg(m_greetTimeoutMs));  // …this is the single outcome.
    }

  private:
    void handleFrame(const VoiceFrame& frame) {
        const QString op = frame.header.value(QLatin1String("op")).toString();
        if (op == QLatin1String(kEvGreeted) && m_awaitingGreet) {
            m_awaitingGreet = false;
            m_greetedOk = true;
            if (m_greetTimer)
                m_greetTimer->stop();
            emit workerGreeted(frame.header);
            return;
        }
        emit workerEvent(op, frame.header);
    }

    bool sendFrame(const QJsonObject& header, QString* err) {
        const QByteArray bytes = encodeInferFrame(header);
        if (bytes.isEmpty()) {
            if (err)
                *err = QStringLiteral("frame exceeds protocol cap");
            return false;
        }
        if (m_proc->write(bytes) != bytes.size()) {
            if (err)
                *err = QStringLiteral("pipe write failed");
            return false;
        }
        return true;
    }

    /** @brief Synchronous bounded stop (greet-failure and error paths). */
    void stopProcess() {
        if (!m_proc || m_proc->state() == QProcess::NotRunning)
            return;
        m_proc->closeWriteChannel();
        if (!m_proc->waitForFinished(kStopGraceMs)) {
            m_proc->terminate();
            if (!m_proc->waitForFinished(kKillGraceMs)) {
                m_proc->kill();
                m_proc->waitForFinished(500);
            }
        }
    }

    QProcess* m_proc = nullptr;
    QTimer* m_greetTimer = nullptr;
    VoiceFrameReader m_reader;
    bool m_awaitingGreet = false;
    bool m_greetedOk = false;
    bool m_stopRequested = false;
};

VoiceSidecarHost::VoiceSidecarHost(QObject* parent) : QObject(parent) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("verzeta-voice-host"));
    m_worker = new VoiceIoWorker();
    m_worker->moveToThread(m_ioThread);
    connect(m_ioThread, &QThread::finished, m_worker, &QObject::deleteLater);

    // Queued relays (worker on IO thread → host on main). The running
    // flag flips on the same deliveries the public signals ride, so a
    // consumer never observes a signal that contradicts running().
    connect(m_worker, &VoiceIoWorker::workerGreeted, this, [this](const QJsonObject& payload) {
        m_running.store(true, std::memory_order_release);
        emit greeted(payload);
    });
    connect(m_worker, &VoiceIoWorker::workerStartFailed, this, [this](const QString& reason) {
        m_running.store(false, std::memory_order_release);
        emit startFailed(reason);
    });
    connect(m_worker, &VoiceIoWorker::workerStopped, this, [this](const QString& reason) {
        m_running.store(false, std::memory_order_release);
        emit stopped(reason);
    });
    connect(m_worker, &VoiceIoWorker::workerEvent, this, &VoiceSidecarHost::eventReceived);
    m_ioThread->start();
}

VoiceSidecarHost::~VoiceSidecarHost() {
    VERZETA_ASSERT_MAIN_THREAD();
    // Stop the child ON the IO thread, then join the thread. The IO
    // thread never blocks on main, so the blocking invoke + wait()
    // cannot deadlock.
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker]() { w->shutdownNow(); }, Qt::BlockingQueuedConnection);
    m_ioThread->quit();
    m_ioThread->wait();
}

void VoiceSidecarHost::start(const QString& binaryPath) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, binaryPath]() { w->start(binaryPath); });
}

void VoiceSidecarHost::stop() {
    QMetaObject::invokeMethod(m_worker, [w = m_worker]() { w->stop(); });
}

void VoiceSidecarHost::sendOp(const QString& op, const QJsonObject& payload) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, op, payload]() { w->sendOp(op, payload); });
}

void VoiceSidecarHost::setGreetTimeoutMs(int ms) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, ms]() { w->m_greetTimeoutMs = ms; });
}

}  // namespace Verzeta::Voice

#include "voice-sidecar-host.moc"

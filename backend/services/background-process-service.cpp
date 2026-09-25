// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file background-process-service.cpp
 * @brief Implementation of BackgroundProcessService. See the header for the
 *        threading contract; in short: the service and every QProcess live on
 *        the main thread, start() is marshalled there from the run_shell worker
 *        and then polls a mutex-guarded buffer (on the worker) for the grace
 *        window so the main loop is never blocked.
 * @layer Service
 * @dependencies ProcessSandbox, Verzeta::scanForDangerousPatterns, Qt6::Core
 *               (QProcess / QFile / QDir).
 */

#include "background-process-service.h"

#include "../utils/dangerous-pattern-scanner.h"
#include "../utils/logger.h"
#include "../utils/process-sandbox.h"

#include <QThread>
#include <QTimer>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>

namespace {
/** @brief Grace period, in ms, given to a SIGTERM'd process to exit before a
 *         SIGKILL escalation during normal operation. */
constexpr int kTerminateGraceMs = 3000;

/** @brief The login shell used to run the command (mirrors ProcessSandbox). */
QString shellPath() {
#ifdef Q_OS_WIN
    return QStringLiteral("cmd.exe");
#else
    static const QString kShell = QFileInfo::exists(QStringLiteral("/bin/bash"))
                                      ? QStringLiteral("/bin/bash")
                                      : QStringLiteral("/bin/sh");
    return kShell;
#endif
}
}  // namespace

BackgroundProcessService::BackgroundProcessService(ProcessSandbox& sandbox, QObject* parent)
    : QObject(parent), m_sandbox(sandbox) {}

BackgroundProcessService::~BackgroundProcessService() {
    // Deterministic teardown (the shutdown lesson): the dtor runs on the main
    // thread AFTER the event loop has stopped, so we cannot rely on queued
    // slots or QTimer here. Stop each child synchronously with the OS-level
    // QProcess wait (which does not need the Qt event loop): SIGTERM, wait,
    // then SIGKILL any survivor. No child is left running.
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        QProcess* proc = it->proc;
        if (!proc)
            continue;
        if (proc->state() != QProcess::NotRunning) {
            proc->terminate();
            if (!proc->waitForFinished(2000)) {
                proc->kill();
                proc->waitForFinished(1000);
            }
        }
    }
    m_entries.clear();
}

BackgroundProcessService::Entry* BackgroundProcessService::findLocked(const QString& id) {
    auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : &it.value();
}

void BackgroundProcessService::pruneExitedLocked() {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (!it->running) {
            if (it->proc)
                it->proc->deleteLater();
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

void BackgroundProcessService::appendOutputLocked(Entry& e, const QString& chunk) {
    if (chunk.isEmpty())
        return;
    e.buffer += chunk;
    if (e.buffer.size() > kMaxBufferBytes)
        e.buffer = e.buffer.right(kMaxBufferBytes);  // drop-oldest, bounded
    e.lastOutputMsEpoch = QDateTime::currentMSecsSinceEpoch();

    // Mirror to the log file, capped so a chatty process cannot fill the disk.
    if (e.logBytesWritten >= kMaxLogBytes || e.logPath.isEmpty())
        return;
    QFile f(e.logPath);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    const QByteArray bytes = chunk.toUtf8();
    if (e.logBytesWritten + bytes.size() <= kMaxLogBytes) {
        f.write(bytes);
        e.logBytesWritten += bytes.size();
    } else {
        const qint64 room = kMaxLogBytes - e.logBytesWritten;
        if (room > 0)
            f.write(bytes.left(room));
        f.write("\n[output truncated: log file cap reached; process still "
                "running]\n");
        e.logBytesWritten = kMaxLogBytes;
    }
    f.close();
}

int BackgroundProcessService::runningCount() const {
    QMutexLocker lk(&m_mutex);
    int n = 0;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        if (it->running)
            ++n;
    return n;
}

QVariantList BackgroundProcessService::runningProcesses() const {
    QMutexLocker lk(&m_mutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVariantList out;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (!it->running)
            continue;
        QVariantMap m;
        m[QStringLiteral("id")] = it->id;
        m[QStringLiteral("pid")] = it->pid;
        m[QStringLiteral("command")] = it->command;
        m[QStringLiteral("conversationId")] = it->conversationId;
        m[QStringLiteral("runtimeMs")] = now - it->startedMsEpoch;
        out.append(m);
    }
    return out;
}

BackgroundProcessService::StartOutcome BackgroundProcessService::start(
    const QString& command, const QString& workingDir, const QString& conversationId, int graceMs) {
    // Create + start on the service (main) thread; never block the main loop.
    StartOutcome outcome;
    if (QThread::currentThread() == this->thread()) {
        outcome = startOnMain(command, workingDir, conversationId);
    } else {
        QMetaObject::invokeMethod(
            this,
            [&]() { outcome = startOnMain(command, workingDir, conversationId); },
            Qt::BlockingQueuedConnection);
    }
    if (!outcome.ok)
        return outcome;

    // Grace window (runs on the CALLING thread — a worker for run_shell, so the
    // main loop keeps filling the buffer via QProcess signals). Return early
    // once the process exits, or once it has produced output and then gone
    // quiet (a dev server that printed its banner and is now serving).
    //
    // When start() is (unusually) called on the service's own thread — e.g. a
    // unit test — the QProcess signals can only fire if we pump that thread's
    // event loop; a worker caller instead just sleeps while the main loop pumps.
    const bool onServiceThread = (QThread::currentThread() == this->thread());
    QElapsedTimer t;
    t.start();
    QString snapshot;
    while (t.elapsed() < graceMs) {
        if (onServiceThread)
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        else
            QThread::msleep(50);
        QMutexLocker lk(&m_mutex);
        Entry* e = findLocked(outcome.id);
        if (!e) {  // pruned → already exited
            break;
        }
        snapshot = e->buffer;
        outcome.pid = e->pid;
        const bool running = e->running;
        const qint64 idleMs = QDateTime::currentMSecsSinceEpoch() - e->lastOutputMsEpoch;
        if (!running)
            break;
        if (!snapshot.isEmpty() && idleMs > 400)
            break;
    }
    outcome.initialOutput = snapshot;
    return outcome;
}

BackgroundProcessService::StartOutcome BackgroundProcessService::startOnMain(
    const QString& command, const QString& workingDir, const QString& conversationId) {
    StartOutcome outcome;

#ifdef Q_OS_ANDROID
    Q_UNUSED(workingDir);
    Q_UNUSED(conversationId);
    outcome.error = QStringLiteral("Background shell execution is not available on Android.");
    return outcome;
#else
    {
        QMutexLocker lk(&m_mutex);
        pruneExitedLocked();
        int running = 0;
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
            if (it->running)
                ++running;
        if (running >= kMaxConcurrent) {
            outcome.error = QStringLiteral("%1 background processes are already running "
                                           "(the maximum). Stop one with `kill <pid>` "
                                           "before starting another.")
                                .arg(kMaxConcurrent);
            return outcome;
        }
    }

    // Same gates as a foreground run_shell: the live user-owned allow-list
    // (first token) AND the destructive-pattern scanner (full command).
    if (!m_sandbox.isCommandAllowed(command)) {
        outcome.error = QStringLiteral("Program '%1' is not in the allow-list.")
                            .arg(command.section(QLatin1Char(' '), 0, 0));
        return outcome;
    }
    const Verzeta::ScanResult scan = Verzeta::scanForDangerousPatterns(command);
    if (!scan.allowed) {
        outcome.error = scan.reason;
        return outcome;
    }

    const QString id = QStringLiteral("bg%1").arg(++m_idCounter);

    // Log location: <workingDir>/.verzeta/proc/<id>.log so the agent can read
    // it with the file tools; fall back to a temp dir when no project dir.
    QString baseDir = workingDir.isEmpty() ? QDir::tempPath() + QStringLiteral("/verzeta-proc")
                                           : workingDir + QStringLiteral("/.verzeta/proc");
    QDir().mkpath(baseDir);
    const QString logPath = baseDir + QLatin1Char('/') + id + QStringLiteral(".log");

    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    if (!workingDir.isEmpty())
        proc->setWorkingDirectory(workingDir);

    connect(proc, &QProcess::started, this, [this, id]() { onProcessStarted(id); });
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, id]() { onReadyRead(id); });
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, id](int code, QProcess::ExitStatus) { onProcessFinished(id, code); });

#ifdef Q_OS_WIN
    // Verbatim command line: cmd.exe (shellPath() here) mis-parses
    // QProcess-quoted arguments for any command with embedded quotes.
    // setNativeArguments is Qt's documented cmd.exe path.
    proc->setNativeArguments(QStringLiteral("/c ") + command);
    proc->start(shellPath(), QStringList{});
#else
    proc->start(shellPath(), {QStringLiteral("-c"), command});
#endif

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker lk(&m_mutex);
        Entry e;
        e.id = id;
        e.proc = proc;
        e.command = command;
        e.conversationId = conversationId;
        e.logPath = logPath;
        e.startedMsEpoch = now;
        e.lastOutputMsEpoch = now;
        e.running = true;
        // Seed the log with a header line so `tail` shows what is running.
        QFile f(logPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            const QByteArray hdr =
                (QStringLiteral("# background process %1: %2\n").arg(id, command)).toUtf8();
            f.write(hdr);
            e.logBytesWritten = hdr.size();
            f.close();
        }
        m_entries.insert(id, std::move(e));
    }

    outcome.ok = true;
    outcome.id = id;
    outcome.logPath = logPath;
    qCInfo(verzetaTools) << "BackgroundProcessService: started" << id << "cmd:" << command;
    return outcome;
#endif
}

void BackgroundProcessService::onProcessStarted(const QString& id) {
    QString command;
    {
        QMutexLocker lk(&m_mutex);
        Entry* e = findLocked(id);
        if (!e || !e->proc)
            return;
        e->pid = e->proc->processId();
        command = e->command;
    }
    emit processStarted(id, command);
    emit runningChanged();
}

void BackgroundProcessService::onReadyRead(const QString& id) {
    QMutexLocker lk(&m_mutex);
    Entry* e = findLocked(id);
    if (!e || !e->proc)
        return;
    const QString chunk = QString::fromLocal8Bit(e->proc->readAllStandardOutput());
    appendOutputLocked(*e, chunk);
}

void BackgroundProcessService::onProcessFinished(const QString& id, int exitCode) {
    {
        QMutexLocker lk(&m_mutex);
        Entry* e = findLocked(id);
        if (!e)
            return;
        // Drain any final output before marking exited.
        if (e->proc) {
            const QString tail = QString::fromLocal8Bit(e->proc->readAllStandardOutput());
            appendOutputLocked(*e, tail);
        }
        e->running = false;
        e->exitCode = exitCode;
    }
    qCInfo(verzetaTools) << "BackgroundProcessService: exited" << id << "code:" << exitCode;
    emit processExited(id, exitCode);
    emit runningChanged();
}

bool BackgroundProcessService::stop(const QString& id) {
    // QProcess is main-thread affine; marshal if called from elsewhere.
    if (QThread::currentThread() != this->thread()) {
        bool ok = false;
        QMetaObject::invokeMethod(
            this, [this, id, &ok]() { ok = stop(id); }, Qt::BlockingQueuedConnection);
        return ok;
    }

    QProcess* proc = nullptr;
    {
        QMutexLocker lk(&m_mutex);
        Entry* e = findLocked(id);
        if (!e || !e->running || !e->proc)
            return false;
        proc = e->proc;
    }
    if (proc->state() == QProcess::NotRunning)
        return false;

    proc->terminate();  // SIGTERM — graceful stop
    // Escalate to SIGKILL if it ignores SIGTERM. QPointer guards against the
    // process being deleted (via pruneExited) before the deadline fires. This
    // is a bounded escalation deadline, not a polling loop.
    QPointer<QProcess> guard(proc);
    QTimer::singleShot(kTerminateGraceMs, this, [guard]() {
        if (guard && guard->state() != QProcess::NotRunning)
            guard->kill();
    });
    return true;
}

void BackgroundProcessService::reapConversation(const QString& conversationId) {
    QStringList ids;
    {
        QMutexLocker lk(&m_mutex);
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
            if (it->running && it->conversationId == conversationId)
                ids.append(it->id);
    }
    for (const QString& id : ids)
        stop(id);
}

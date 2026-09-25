// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file background-process-service.h
 * @brief Owned registry of long-running (background) shell processes started
 *        via run_shell(background:true). Unlike ProcessSandbox, which builds a
 *        per-call QProcess bounded by a wall-clock timeout, a background
 *        process persists across tool calls until it exits, is stopped, or its
 *        conversation closes.
 *
 *        Each process streams its merged stdout/stderr to a bounded in-memory
 *        buffer AND to a workspace log file, so an agent observes it with the
 *        tools it already has (read_file / tail on the log, ps, kill \<pid\>).
 *        The service enforces a concurrency cap and reaps every process
 *        deterministically on conversation-close and on app shutdown. It does
 *        NOT impose a wall-clock lifetime kill (a dev server should live as long
 *        as the session).
 *
 * @layer Service
 * @dependencies ProcessSandbox (allow-list check, reused with the same live
 *               user-owned list), Verzeta::scanForDangerousPatterns, Qt6::Core
 *               (QProcess). Owned by AppController; main-thread affinity.
 *
 * \@threading The service and every QProcess it owns live on the MAIN thread.
 *            start() is called from the run_shell WORKER thread; it marshals
 *            process creation to the main thread (non-blocking) and then waits,
 *            on the worker, up to a short grace window for the first output;
 *            main is never blocked. All other public methods are main-thread or
 *            marshal internally. The in-memory buffer is mutex-guarded because
 *            the worker reads it during the grace window while the main thread
 *            fills it from QProcess signals.
 */

#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVariantList>

class ProcessSandbox;
class QProcess;

/**
 * @brief Registry + lifecycle manager for background shell processes.
 */
class BackgroundProcessService : public QObject {
    Q_OBJECT

  public:
    /** @brief Maximum number of concurrently-RUNNING background processes.
     *         A new start beyond this fails with an actionable error rather than
     *         evicting an existing one. Protects host RAM/CPU/ports. */
    static constexpr int kMaxConcurrent = 4;

    /** @brief In-memory per-process buffer cap (drop-oldest). Bounds memory. */
    static constexpr int kMaxBufferBytes = 64 * 1024;

    /** @brief Per-process log-file byte cap. Bounds disk for a chatty server. */
    static constexpr qint64 kMaxLogBytes = 1 * 1024 * 1024;

    /** @brief Result of a start() request. */
    struct StartOutcome {
        bool ok = false;        ///< true if the process was created + started
        QString id;             ///< Stable service id ("bg1", "bg2", …)
        qint64 pid = 0;         ///< OS process id (for `kill <pid>`); 0 if unknown
        QString logPath;        ///< Absolute path to the output log file
        QString initialOutput;  ///< Grace-window captured output (may be empty)
        QString error;          ///< Populated when ok == false
    };

    /**
     * @brief Constructs the service.
     * @param sandbox Reference to the app ProcessSandbox, reused so background
     *                commands pass the SAME live user-owned allow-list. Must
     *                outlive this service.
     * @param parent  Optional Qt parent (AppController).
     */
    explicit BackgroundProcessService(ProcessSandbox& sandbox, QObject* parent = nullptr);
    ~BackgroundProcessService() override;

    /**
     * @brief Starts a background process. Callable from the run_shell worker.
     * @param command        Full shell command (allow-list + scanner checked).
     * @param workingDir     Directory to run in (the conversation project dir);
     *                       the log is written under workingDir/.verzeta/proc/.
     * @param conversationId Owning conversation (for reap-on-close); may be empty.
     * @param graceMs        How long to wait for the first output before
     *                       returning (returns earlier once the process exits or
     *                       goes quiet after producing output).
     * @returns A StartOutcome. On failure ok == false and error explains why
     *          (Android, allow-list, dangerous pattern, or the concurrency cap).
     */
    StartOutcome start(const QString& command,
                       const QString& workingDir,
                       const QString& conversationId,
                       int graceMs = 3000);

    /**
     * @brief Stops a background process (SIGTERM, then SIGKILL if needed).
     * @param id Service id returned by start().
     * @returns true if a running process with that id was found and signalled.
     */
    Q_INVOKABLE bool stop(const QString& id);

    /**
     * @brief Stops every background process owned by a conversation.
     * @param conversationId The closing conversation.
     */
    void reapConversation(const QString& conversationId);

    /**
     * @brief Number of currently-running background processes.
     * @returns The count of tracked processes that are still running.
     */
    int runningCount() const;

    /**
     * @brief Running processes for the activity indicator.
     * @returns A list of maps {id, pid, command, conversationId, runtimeMs}.
     */
    Q_INVOKABLE QVariantList runningProcesses() const;

  signals:
    /**
     * @brief Emitted when a background process starts.
     * @param id The service id of the process.
     * @param command The command line it is running.
     */
    void processStarted(const QString& id, const QString& command);

    /**
     * @brief Emitted when a background process exits (or is stopped).
     * @param id The service id of the process.
     * @param exitCode The process exit code.
     */
    void processExited(const QString& id, int exitCode);

    /** @brief Emitted whenever the running set changes (for the UI to refresh). */
    void runningChanged();

  private:
    /** @brief One tracked background process. */
    struct Entry {
        QString id;
        qint64 pid = 0;
        QProcess* proc = nullptr;  ///< Owned (child of the service, main thread)
        QString command;
        QString conversationId;
        QString logPath;
        qint64 logBytesWritten = 0;
        qint64 startedMsEpoch = 0;
        QString buffer;  ///< Bounded; guarded by m_mutex
        qint64 lastOutputMsEpoch = 0;
        bool running = true;
        int exitCode = -1;
    };

    // -- main-thread implementation of start() (marshalled to from a worker) --
    StartOutcome
    startOnMain(const QString& command, const QString& workingDir, const QString& conversationId);

    void onProcessStarted(const QString& id);
    void onReadyRead(const QString& id);
    void onProcessFinished(const QString& id, int exitCode);
    void appendOutputLocked(Entry& e, const QString& chunk);  // caller holds m_mutex
    void pruneExitedLocked();                                 // caller holds m_mutex
    Entry* findLocked(const QString& id);                     // caller holds m_mutex

    ProcessSandbox& m_sandbox;
    mutable QMutex m_mutex;  ///< Guards m_entries + buffers
    QHash<QString, Entry> m_entries;
    int m_idCounter = 0;
};

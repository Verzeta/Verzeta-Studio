// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file process-sandbox.h
 * @brief Safe wrapper around QProcess for executing shell commands.
 *        Enforces an allow-list of permitted programs and timeout limits.
 *        Exposes Q_INVOKABLE executeCommand() for use as "TerminalController" in QML.
 * @layer Utility
 * @dependencies Qt6::Core (QProcess)
 */


#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

/**
 * @brief Shell command sandbox with program allow-list and timeout enforcement.
 *
 * ## Security model
 * - The allow-list is checked against the first whitespace-delimited token of the command.
 * - Commands not in the allow-list emit commandBlocked() and are NOT executed.
 * - Commands are run as: /bin/sh -c "command" to support shell features.
 * - The allow-list can be extended via setAllowList(); callers are responsible for
 *   vetting any additions.
 *
 * ## Threading
 * - execute() blocks the calling thread and must NOT be called from the UI thread.
 *   Use ToolExecutionWorker to run it off-thread.
 * - executeAsync() / executeCommand() are non-blocking and safe to call from the
 *   UI thread. Output arrives via the outputLine() signal.
 */
class ProcessSandbox : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Result of a synchronous command execution.
     */
    struct CommandResult {
        QString stdoutOutput;   ///< Standard output content
        QString stderrOutput;   ///< Standard error content
        int exitCode = -1;      ///< Process exit code; -1 if not run / killed
        bool timedOut = false;  ///< true if the process was killed due to timeout
    };

    /**
     * @brief Constructs a ProcessSandbox with this platform's default
     *        allow-list (see defaultAllowList()). AppController then
     *        applies the user's saved allow-list from SettingsService.
     * @param parent Optional Qt parent.
     */
    explicit ProcessSandbox(QObject* parent = nullptr);
    ~ProcessSandbox() override;

    // -----------------------------------------------------------------------
    // Synchronous execution (call from worker thread only)
    // -----------------------------------------------------------------------

    /**
     * @brief Executes a shell command synchronously and returns all output.
     * @param command   Shell command string. The first token is allow-list checked.
     * @param timeoutMs Maximum allowed runtime in milliseconds (default 10 s).
     * @param workingDir Optional working directory for the child process. When
     *                  non-empty the child runs there (e.g. the conversation's
     *                  project dir) so relative paths resolve consistently with
     *                  the file tools. Empty = inherit the host process CWD.
     * @return CommandResult with stdout, stderr, exit code, and timedOut flag.
     * @sideeffects Spawns a child process. Blocks the calling thread.
     *              May read/write the filesystem depending on the command.
     * @complexity O(command_runtime).
     *
     * IMPORTANT: Do not call from the UI thread. Use ToolExecutionWorker instead.
     */
    CommandResult
    execute(const QString& command, int timeoutMs = 10000, const QString& workingDir = {});

    // -----------------------------------------------------------------------
    // Asynchronous execution (safe from UI thread)
    // -----------------------------------------------------------------------

    /**
     * @brief Starts an asynchronous command, emitting outputLine() for each line.
     * @param command   Shell command string. The first token is allow-list checked.
     * @param timeoutMs Maximum runtime in milliseconds (default 10 s).
     * @sideeffects Spawns a child process. Emits outputLine() per stdout/stderr line,
     *              then commandFinished() when the process exits or times out.
     *              Kills any in-progress async process before starting the new one.
     */
    void executeAsync(const QString& command, int timeoutMs = 10000);

    /**
     * @brief QML-friendly wrapper around executeAsync().
     *        Registered as "TerminalController.executeCommand()" in QML.
     * @param command Shell command string.
     */
    Q_INVOKABLE void executeCommand(const QString& command);

    // -----------------------------------------------------------------------
    // Allow-list management
    // -----------------------------------------------------------------------

    /**
     * @brief Checks whether the command's program is in the allow-list.
     * @param command Full command string (first whitespace-delimited token used).
     * @return true if allowed; false if blocked.
     */
    bool isCommandAllowed(const QString& command) const;

    /**
     * @brief Replaces the current allow-list with a new one.
     * @param allowedPrograms List of permitted program names (e.g., {"ls", "grep"}).
     */
    void setAllowList(const QStringList& allowedPrograms);

    /**
     * @brief Returns the current allow-list.
     * @return List of permitted program names.
     */
    QStringList allowList() const;

    /**
     * @brief The compiled-in default program allow-list for this platform.
     * @returns The factory allow-list a fresh sandbox starts with: read-only
     *          inspection utilities + the developer toolchain (interpreters,
     *          package managers, VCS, build tools) + network fetch (curl/wget)
     *          + process control (kill/pkill). This is the single source of
     *          truth: the constructor seeds m_allowList from it, and the
     *          settings surface's "Restore to Default" resets to it.
     */
    static QStringList defaultAllowList();

    /**
     * @brief Refusal reason for command forms that run code which cannot be
     *        checked before it runs (eval of dynamic text, a dynamic
     *        `sh -c` script, text piped into a shell). Applied after the
     *        allow-list and the destructive-pattern scan; everything else
     *        is unaffected.
     * @param command Full command string.
     * @returns The reason to refuse, or an empty string. Always empty on
     *          Windows, where commands are not POSIX shell syntax.
     */
    static QString uncheckableCodeReason(const QString& command);

    /**
     * @brief Turns the optional write restriction for agent commands on or
     *        off (Linux Landlock; see Verzeta::WriteGuard). Off by default.
     *
     * When on, run_shell and background processes may write only inside
     * their working folder, the temp folders, /dev and @p extraRoots;
     * reading and running programs are unaffected. Package caches are
     * pointed at a temp folder so pip and npm keep working.
     *
     * @param enabled    Whether to restrict writes.
     * @param extraRoots Additional folders the user allows writes to.
     */
    void setWriteRestriction(bool enabled, const QStringList& extraRoots);

    /**
     * @brief Whether agent commands are currently write-restricted.
     * @returns true when the restriction is enabled AND the system
     *          supports it.
     */
    bool writeRestrictionActive() const;

    /**
     * @brief Folders an agent command may write to under the restriction.
     * @param workingDir The command's working folder (may be empty).
     * @returns workingDir, the temp folders, /dev and the extra folders.
     */
    QStringList writableRoots(const QString& workingDir) const;

    /**
     * @brief Applies the write restriction (when active) to a process that
     *        is about to run an agent command. No-op when inactive.
     * @param proc       The process, before start().
     * @param workingDir The command's working folder (may be empty).
     */
    void prepareAgentProcess(QProcess& proc, const QString& workingDir) const;

    // -----------------------------------------------------------------------
    // Control
    // -----------------------------------------------------------------------

    /**
     * @brief Kills the currently running async process (if any).
     * @sideeffects Terminates the process. commandFinished() will still be emitted.
     */
    void kill();

  signals:
    /**
     * @brief Emitted for each line of stdout or stderr from an async command.
     * @param line Output line (without trailing newline).
     */
    void outputLine(const QString& line);

    /**
     * @brief Emitted when an async command completes (normally or killed/timed out).
     * @param result Final CommandResult.
     */
    void commandFinished(const CommandResult& result);

    /**
     * @brief Emitted when a command is blocked by the allow-list.
     * @param command The blocked command string.
     * @param reason  Human-readable reason.
     */
    void commandBlocked(const QString& command, const QString& reason);

  private slots:
    /**
     * @brief Drains pending bytes from the active QProcess's stdout
     *        channel and appends them to the in-flight buffer.
     *
     * Wired to QProcess::readyReadStandardOutput.  Read in chunks
     * rather than blocked-on-finish so streamed output (e.g.
     * progress lines) reaches consumers as the child process emits it.
     */
    void onReadyReadStdout();

    /**
     * @brief Drains pending bytes from the active QProcess's stderr
     *        channel and appends them to the in-flight buffer.
     *
     * Wired to QProcess::readyReadStandardError.  Mirrors
     * onReadyReadStdout() for the stderr channel.
     */
    void onReadyReadStderr();

    /**
     * @brief Finalises an async QProcess run when the child exits.
     *
     * Wired to QProcess::finished.  Packages the accumulated stdout +
     * stderr buffers + exit code into a CommandResult and emits
     * commandFinished().  Cleans up the QProcess instance for the
     * next run.
     *
     * @param exitCode    Process exit code (zero on normal termination).
     * @param exitStatus  Whether the process exited normally or crashed.
     */
    void onAsyncFinished(int exitCode, QProcess::ExitStatus exitStatus);

  private:
    // Effective program allow-list checked against the first token of every
    // command. Seeded from defaultAllowList() in the constructor (the single
    // source of truth) and overridable at runtime via setAllowList(), which is
    // wired to SettingsService::shellAllowList — the user owns this policy.
    // The destructive-pattern scanner still gates the full command on every
    // execute(), so an allow-listed program cannot smuggle rm -rf / or curl|sh.
    QStringList m_allowList;

    bool m_writeRestricted = false;    ///< User setting; off by default
    QStringList m_extraWritableRoots;  ///< User-granted writable folders

    QProcess* m_process = nullptr;  ///< Active async process (owned)
    QString m_asyncStdout;          ///< Accumulated stdout for async run
    QString m_asyncStderr;          ///< Accumulated stderr for async run
    CommandResult m_asyncResult;    ///< Result being built for async run

    /**
     * @brief Extracts the program name (first whitespace-delimited token).
     * @param command Full command string.
     * @return Program name, or empty if command is empty.
     */
    static QString extractProgram(const QString& command);
};

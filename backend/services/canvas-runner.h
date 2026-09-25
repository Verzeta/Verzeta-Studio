// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file canvas-runner.h
 * @brief Canvas execution + IDE handoff service.
 *
 *        Two affordances live on this service:
 *
 *          1. `runActiveCanvas(convId)` executes the active canvas
 *             content via Python or Bash through a tightly-scoped
 *             QProcess. Streams stdout to the bound `CanvasConsoleModel`
 *             one line at a time; buffers stderr silently until the
 *             run finishes, then emits at most one concise error line
 *             on a non-zero exit. Per the user's standing directive,
 *             the console stays a minimal surface with no long traces.
 *          2. `sendToIde(convId)`: for non-executable code languages
 *             (cpp / js / ts / java / etc.) the action bar swaps Run
 *             out for "Send to IDE", which calls
 *             `QDesktopServices::openUrl` on the canvas's existing
 *             disk-mirror file. The OS routes to whichever app the
 *             user has set as default for that extension (VSCode,
 *             vim, IntelliJ, …).
 *
 *        Sandbox safety uses REAL OS-level isolation via bubblewrap:
 *
 *          The runner invokes the interpreter through `bwrap`
 *          (bubblewrap, the user-namespace sandbox shipped on most
 *          modern Linux distros: `apt install bubblewrap` on Debian
 *          / Ubuntu, `dnf install bubblewrap` on Fedora, `pacman -S
 *          bubblewrap` on Arch). Without bwrap installed the runner
 *          REFUSES to execute and surfaces a one-line install hint in
 *          the console, because partial protection is not real protection.
 *
 *          The bwrap recipe is INTENTIONALLY NARROW about what it
 *          binds into the sandbox:
 *
 *            --unshare-{user,pid,uts,ipc,cgroup-try} : process /
 *                mount-namespace isolation. We deliberately do NOT
 *                `--unshare-net` because Ubuntu 24.04+ ships with
 *                AppArmor restrictions
 *                (`kernel.apparmor_restrict_unprivileged_userns=1`)
 *                that block bwrap's loopback setup inside a new
 *                network namespace. Sharing the host net namespace
 *                gives the script network access, but the rest of
 *                the recipe ensures there is no useful exfiltration
 *                channel and `pip install` cannot persist.
 *            --die-with-parent : child dies if our QProcess dies.
 *            --new-session     : fresh session, outer-tty signals
 *                can't reach the script.
 *            --ro-bind /usr /usr (plus /lib, /lib64 symlinks): system
 *                binaries + libraries available read-only so the
 *                interpreter can load its stdlib.
 *            --ro-bind-try /etc/{alternatives,ld.so.*,python3} : the
 *                specific config files interpreters consult. /etc
 *                itself is NOT bound so user-installed daemon
 *                configs aren't visible.
 *            (NOT bound: /home, /root, /opt, /var, /srv, /mnt,
 *             /media, /run, /boot.) The script literally cannot see
 *            ~/.ssh/id_rsa or ~/.bashrc; they don't exist inside
 *            the namespace.
 *            --tmpfs /tmp      : writable scratch only inside the
 *                sandbox; vanishes when bwrap exits.
 *            --proc /proc      : real /proc for the namespace.
 *            --dev /dev        : minimal device tree.
 *            --setenv HOME /tmp / PATH /usr/bin:/bin / LANG C plus
 *            --unsetenv for USER / LOGNAME / DISPLAY / SSH_AUTH_SOCK
 *            / XDG_RUNTIME_DIR / DBUS_SESSION_BUS_ADDRESS so no host
 *            session info leaks in.
 *
 *          Inside that container the interpreter runs with its own
 *          isolation flags as a defence-in-depth layer:
 *            Python: `-I -B -E -- <tempfile>` (isolated, no .pyc, no
 *                     PYTHON* env).
 *            Bash:   `bash --noprofile --norc <tempfile>`.
 *
 *          Combined effect: `rm -rf ~/anything` fails because /home
 *          doesn't exist in the namespace; `cat ~/.ssh/id_rsa` fails for the
 *          same reason; `sudo` cannot escalate (user namespace + no
 *          SUID binaries reachable); `pip install` may download but
 *          cannot persist (read-only /usr, tmpfs /tmp, ephemeral HOME);
 *          a fork bomb can't escape its own pid namespace. The 10s
 *          wall-clock timeout is a final cap.
 *
 *          Tempfiles live under QStandardPaths::TempLocation on the
 *          host, are bind-mounted read-only into the sandbox at a
 *          fixed `/work/script.<ext>` path, and are unlinked from the
 *          host on every finished / failed-to-start / cancel
 *          transition.
 *          No shell wrapping: QProcess runs `bwrap` directly with
 *          explicit argv so canvas content never reaches a shell
 *          parser anywhere.
 *
 *        Output capping:
 *          - Each stdout line is appended live to the console model,
 *            which itself caps at 50 rows total (FIFO trim of oldest).
 *          - Each individual line is trimmed to 500 chars with a
 *            "…" trailer to prevent giant single-line dumps.
 *          - stderr is buffered until finish; on exit != 0 a single
 *            "Error: <last non-empty stderr line>" row appears. On
 *            exit == 0 stderr is silent unless the script printed to
 *            stderr without crashing; in that case it is still silent.
 *
 *        Threading: strictly main-thread. QProcess events fire on the
 *        main thread; we never block.
 *
 * @layer Service
 * @dependencies CanvasService (for active canvas content + filename),
 *               FileService (for the disk-mirror absolute path used by
 *               sendToIde), Qt6::Core (QProcess + QTimer + QSaveFile +
 *               QDesktopServices via the .cpp).
 */
#pragma once

#include <QTimer>

#include <functional>
#include <memory>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

class CanvasService;
class FileService;
class CanvasConsoleModel;

/**
 * @brief Canvas execution + IDE-handoff orchestrator. Runs canvas
 *        content under a sandboxed interpreter (bwrap on Linux,
 *        sandbox-exec on macOS, direct subprocess on Windows; refuses
 *        on Android) and emits structured status / output events to
 *        the bound CanvasConsoleModel. Also routes non-executable
 *        canvas languages to the OS's default IDE for the matching
 *        file extension.
 */
class CanvasRunner : public QObject {
    Q_OBJECT
    /**
     * @brief True from the moment a run is dispatched until the
     *        QProcess emits finished / errorOccurred. Drives the
     *        Run ↔ Stop toggle in the action bar.
     */
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)

    /**
     * @brief True when this platform has a real OS sandbox the runner
     *        can use to isolate canvas scripts:
     *          - Linux : true when bwrap is on PATH
     *          - macOS : true when sandbox-exec is on PATH
     *                    (deprecated by Apple but still functional)
     *          - Windows: false (no built-in equivalent; runs direct
     *                    subprocess + dangerous-pattern scanner only)
     *          - Android: false (no fork/exec available at all)
     *        QML reads this to decide whether to display the
     *        "running without a sandbox -- user responsibility"
     *        warning banner on the Canvas page. Static across the
     *        process lifetime; the NOTIFY signal exists for QML
     *        binding correctness but is never emitted in practice.
     */
    Q_PROPERTY(bool sandboxAvailable READ sandboxAvailable NOTIFY sandboxAvailabilityChanged)

  public:
    /**
     * @brief Construct the runner. Initialises the timeout timer; the
     *        canvas / file / console dependencies are attached via
     *        their setters during AppController initialize.
     * @param parent Qt parent.
     */
    explicit CanvasRunner(QObject* parent = nullptr);
    ~CanvasRunner() override;

    /**
     * @brief Attach the canvas service. Required before
     *        runActiveCanvas / sendToIde.
     * @param svc Non-owning pointer; AppController owns the service.
     */
    void setCanvasService(CanvasService* svc);

    /**
     * @brief Attach the file service for disk-mirror path resolution
     *        (used by `sendToIde`).
     * @param svc Non-owning pointer; AppController owns the service.
     */
    void setFileService(FileService* svc);

    /**
     * @brief Attach the C++ console model. The runner emits structured
     *        status / output events to it; the QML console binds to
     *        the model directly.
     * @param model Non-owning pointer; AppController owns the model.
     */
    void setConsoleModel(CanvasConsoleModel* model);

    /**
     * @brief Test seam: override the URL-open implementation so unit
     *        tests can assert sendToIde's dispatch without launching
     *        a real GUI app. Default is `QDesktopServices::openUrl`.
     * @param handler Callable invoked with the URL to open; should
     *                return true on success.
     */
    void setOpenUrlHandler(std::function<bool(const QString&)> handler);

    /**
     * @brief Whether a canvas run is currently in flight.
     * @returns true between dispatch and the QProcess terminal
     *          signal; false otherwise.
     */
    Q_INVOKABLE bool isRunning() const;

    /**
     * @brief Whether this platform has a real OS-level sandbox
     *        available for Canvas Run. Result is platform-compile-time
     *        fixed except on Linux (where bwrap can be installed /
     *        removed) and macOS (sandbox-exec is normally always
     *        present but we probe anyway). See the sandboxAvailable
     *        Q_PROPERTY documentation for the full per-platform table.
     * @returns true when a sandbox binary is available on PATH.
     */
    bool sandboxAvailable() const;

    /**
     * @brief Run the active canvas under the right interpreter. Fails
     *        (returns false + emits errorOccurred) when:
     *          - no canvas is active,
     *          - the canvas language is not in the run-supported set
     *            (python / shell / bash),
     *          - a run is already in flight,
     *          - the interpreter binary is missing on the system.
     *        On success, returns true synchronously and starts the
     *        QProcess; per-chunk signals fire on the main thread as
     *        output arrives.
     * @param conversationId Conversation whose active canvas is run.
     * @returns true when the run started; false on any failure listed
     *          above, each of which also emits errorOccurred.
     */
    Q_INVOKABLE bool runActiveCanvas(const QString& conversationId);

    /**
     * @brief Cancel the in-flight run (terminate → kill if needed).
     *        Always safe to call, no-op when nothing is running.
     */
    Q_INVOKABLE void cancel();

    /**
     * @brief Feed one line to the running script's stdin (for `input()` /
     *        `read`). Appends a newline, echoes the text to the console as an
     *        "input" row, and restarts the inactivity timer.
     * @param text The line to send (without a trailing newline).
     * @sideeffects No-op with a console note when no script is running.
     */
    Q_INVOKABLE void sendInput(const QString& text);

    /**
     * @brief Close the script's stdin (end-of-input), so a script blocked in
     *        `sys.stdin.read()` sees EOF. Idempotent; no-op when not running.
     */
    Q_INVOKABLE void sendEof();

    /**
     * @brief Open the active canvas's disk-mirror file in the OS's
     *        default app for that extension. `QDesktopServices::openUrl`
     *        delegates to VSCode / vim / IntelliJ / etc. depending on
     *        the user's MIME associations.
     * @param conversationId Conversation UUID whose active canvas to
     *                       open in the IDE.
     * @returns true on success; false (and emits errorOccurred) when
     *          the canvas is missing, when the disk-mirror file does
     *          not exist, or when the URL handler rejects.
     */
    Q_INVOKABLE bool sendToIde(const QString& conversationId);

    /**
     * @brief Run-supported language tags. Used by the action bar to
     *        decide between Run/Stop and Send to IDE.
     * @returns List of language tags whose interpreters this runner
     *          can execute (python / shell / bash).
     */
    Q_INVOKABLE QStringList supportedRunLanguages() const;

    /**
     * @brief IDE-handoff-supported language tags (the set with known
     *        editor extensions).
     * @returns List of language tags whose canvases route to the
     *          OS's default IDE rather than the in-app runner.
     */
    Q_INVOKABLE QStringList supportedIdeLanguages() const;

    /**
     * @brief Public for tests and sandbox status indicators. Cached
     *        per process.
     * @returns Empty string when the sandbox is usable on this host;
     *          a user-facing reason string otherwise.
     */
    static QString probeSandboxBlocker();

    /**
     * @brief The Python interpreter flags shared by EVERY launch path (bwrap,
     *        macOS sandbox-exec, Windows direct, no-sandbox fallback). Single
     *        source of truth; these were previously duplicated four times.
     *        Public so tests can pin the unbuffered flag.
     * @returns `{"-u", "-I", "-B", "-E"}`. `-u` keeps stdout/stderr unbuffered
     *          so an `input()` prompt reaches the console before the script
     *          blocks reading stdin (stdout is a pipe, not a tty). NOTE: `-E`
     *          makes Python ignore every `PYTHON*` env var, so
     *          `PYTHONUNBUFFERED` cannot be used and `-u` is the only way.
     *          `-I` isolated mode, `-B` no .pyc.
     */
    static QStringList pythonFlags();

  signals:
    /** @brief Emitted whenever isRunning() changes value. */
    void runningChanged();

    /**
     * @brief Bound to the sandboxAvailable Q_PROPERTY purely for QML
     *        binding correctness. Never emitted in practice, since the
     *        property is set at compile time per platform and doesn't
     *        change at runtime. Future: could fire if bwrap gets
     *        installed / uninstalled mid-session, but that's a polish
     *        feature for later.
     */
    void sandboxAvailabilityChanged();

    /**
     * @brief Emitted when a run / send-to-IDE attempt fails. The
     *        message is the same user-facing string surfaced in the
     *        console; subscribers may also display it as a toast.
     * @param message Human-readable failure description.
     */
    void errorOccurred(const QString& message);

  private slots:
    /** @brief Drain the QProcess stdout buffer into the console. */
    void onProcessStdout();
    /** @brief Buffer the QProcess stderr stream until process exit. */
    void onProcessStderr();
    /**
     * @brief Handle QProcess finished: emit summary + cleanup.
     * @param exitCode Process exit status code.
     * @param status   Normal vs CrashExit indicator.
     */
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

    /**
     * @brief Handle QProcess errorOccurred: emit failure + cleanup.
     * @param err QProcess error code (FailedToStart, Crashed, etc.).
     */
    void onProcessErrorOccurred(QProcess::ProcessError err);
    /** @brief Inactivity timeout fired (no output/input): terminate then kill. */
    void onTimeout();
    /** @brief Absolute run-duration ceiling fired: terminate then kill. */
    void onAbsoluteTimeout();
    /**
     * @brief Partial-flush debounce fired: emit a lingering newline-less
     *        remainder (e.g. an `input()` prompt) so it appears in the console.
     */
    void onPartialFlush();

  private:
    CanvasService* m_canvasSvc = nullptr;
    FileService* m_fileSvc = nullptr;
    QPointer<CanvasConsoleModel> m_console;
    std::function<bool(const QString&)> m_openUrl;

    std::unique_ptr<QProcess> m_process;
    /** Inactivity timer: restarted on every stdout/stderr chunk and on every
     *  sendInput, so a script sitting at a prompt is not killed while the user
     *  types. Fires only after real silence. */
    QTimer m_timeoutTimer;
    /** Absolute run ceiling: started once, never reset, so a chatty runaway
     *  (which keeps resetting the inactivity timer) still dies. */
    QTimer m_absoluteTimer;
    /** Debounce that flushes a lingering newline-less stdout remainder (an
     *  `input()` prompt) to the console. */
    QTimer m_partialFlushTimer;
    QElapsedTimer m_elapsed;
    bool m_running = false;
    bool m_userCancelled = false;
    bool m_timeoutFired = false;
    /** True when the kill came from the absolute ceiling rather than
     *  inactivity. Selects the console message. */
    bool m_timeoutWasAbsolute = false;
    QString m_tempFile;
    QString m_displayName;

    /** Per-stream chunk buffers, split into lines on '\n' before
     *  appending to the console model so each model row corresponds
     *  to one display line. */
    QString m_stdoutBuffer;
    /** stderr is kept hidden during the run; the "last non-empty
     *  line" is emitted on non-zero exit. */
    QString m_stderrBuffer;

    /** Hard cap on a single-line render to avoid pathological input.
     *  Lines longer than this trim to the prefix + "…" trailer. */
    static constexpr int kMaxLineChars = 500;

    /** Inactivity window. A run is killed only after this long with NO script
     *  output and NO user input, so an interactive script may sit at a prompt
     *  while the user reads and types. (Replaced a flat 10s wall-clock, which
     *  killed every interactive script mid-answer.) */
    static constexpr int kIdleTimeoutMs = 60000;

    /** Absolute ceiling on total run duration, never reset. Bounds a runaway
     *  that keeps printing (and thus keeps resetting the inactivity timer). */
    static constexpr int kMaxRunMs = 300000;  // 5 minutes

    /** Debounce before a newline-less stdout remainder is flushed as its own
     *  console row. Short enough that an input() prompt appears promptly; long
     *  enough that a line arriving in several chunks still coalesces. */
    static constexpr int kPartialFlushMs = 120;

    /** Hard ceiling on a process's total stdout chunk volume. Beyond
     *  this we stop appending lines (cap kept by the console model
     *  itself; this counter just prevents us from doing any work
     *  past the limit). */
    static constexpr int kMaxStdoutBytes = 64 * 1024;
    int m_stdoutBytesEmitted = 0;
    static constexpr int kMaxStderrBytes = 16 * 1024;
    int m_stderrBytesBuffered = 0;

    /** Locate the named interpreter binary via QStandardPaths.
     *  Returns empty when the interpreter is not on PATH; runActiveCanvas
     *  then refuses with errorOccurred. */
    static QString resolveInterpreter(const QString& name);

    /** Write the canvas content to a temporary file with the given
     *  extension. Returns the absolute path or empty on failure. */
    QString writeTempFile(const QString& content, const QString& suffix);

    /** Drop the per-run tempfile if any. Idempotent. */
    void cleanupTempFile();

    /** Stop the inactivity, absolute-ceiling and partial-flush timers.
     *  Called on every terminal transition (finished / failed / cancelled). */
    void stopTimers();

    /** Common run dispatcher: wires the QProcess, sets the
     *  trimmed environment, starts with explicit argv (no shell). */
    bool startRun(const QString& interpreter, const QStringList& args, const QString& displayName);

    /** Drain any partial line(s) accumulated in m_stdoutBuffer to
     *  the console model. Called on each readyReadStandardOutput
     *  AND once on finished to flush a final partial line. */
    void flushStdout(bool flushPartial);

    /** Emit the run summary row (success vs error + elapsed ms). */
    void emitFinishSummary(int exitCode, qint64 elapsedMs);
};

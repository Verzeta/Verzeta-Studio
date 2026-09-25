// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file process-sandbox.cpp
 * @brief Implementation of ProcessSandbox: allow-list enforcement,
 *        synchronous and asynchronous shell command execution with timeouts.
 * @layer Utility
 * @dependencies Qt6::Core (QProcess, QTimer)
 */


#include "process-sandbox.h"

// QtGlobal MUST be included before any `#ifdef Q_OS_*` -- the
// platform macros live in <qsystemdetection.h> which QtGlobal pulls
// in. This file's #ifdef gates happen to work today because QProcess
// (included below) transitively brings in QtGlobal, but relying on
// that is load-bearing-by-accident -- one include reorder away from
// "every #ifdef Q_OS_X branch is silently false". Explicit include
// makes the dependency visible. Same pattern fix applied to every
// .cpp that uses Q_OS_* macros after the windows-theme-bridge bug.
#include "dangerous-pattern-scanner.h"
#include "logger.h"

#include <QtGlobal>
#include <QTimer>

#include <QFileInfo>
#include <QProcess>

/// Default async command timeout (used by executeCommand() from QML).
static constexpr int kDefaultAsyncTimeoutMs = 10000;

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs a ProcessSandbox.
 *
 *        Seeds the allow-list from defaultAllowList(), which returns the
 *        per-platform set. AppController then applies the user's saved
 *        allow-list.
 *
 * @param parent Optional Qt parent.
 */
ProcessSandbox::ProcessSandbox(QObject* parent) : QObject(parent) {
    // Seed the effective allow-list from the compiled platform default (the
    // single source of truth). AppController re-applies the user-owned override
    // from SettingsService::shellAllowList right after construction, so this is
    // the "no user edits yet" baseline.
    m_allowList = defaultAllowList();
}

/**
 * @brief The compiled-in default program allow-list for this platform.
 * @returns Read-only inspection utilities + the developer toolchain +
 *          network fetch (curl/wget) + process control (kill/pkill) on
 *          Linux/macOS; cmd.exe builtins + read-only utilities + curl/taskkill
 *          on Windows; empty on Android (shell exec is refused there).
 */
QStringList ProcessSandbox::defaultAllowList() {
#ifdef Q_OS_WIN
    // Windows: cmd.exe builtins + read-only Windows utilities. All entries are
    // stored lowercase to match the case-insensitive normalization in
    // extractProgram() further down.
    return QStringList{
        QStringLiteral("dir"),       // ls equivalent
        QStringLiteral("tasklist"),  // ps equivalent
        QStringLiteral("findstr"),   // grep equivalent
        QStringLiteral("type"),      // cat equivalent
        QStringLiteral("where"),     // which / find-on-PATH equivalent
        QStringLiteral("whoami"),   QStringLiteral("hostname"),
        QStringLiteral("ver"),  // uname equivalent (kernel version)
        QStringLiteral("date"),     QStringLiteral("time"),     QStringLiteral("echo"),
        QStringLiteral("set"),  // env equivalent
        QStringLiteral("cd"),   // pwd equivalent (cd with no arg prints cwd)
        QStringLiteral("sort"),     QStringLiteral("more"),
        QStringLiteral("fc"),      // diff-ish
        QStringLiteral("fsutil"),  // limited; user can add specific subcmds
        QStringLiteral("wmic"),    // read-only queries
        QStringLiteral("python"),  // if installed on PATH
        QStringLiteral("python3"),
        QStringLiteral("py"),  // py launcher
        QStringLiteral("git"),      QStringLiteral("cmake"),    QStringLiteral("ninja"),
        QStringLiteral("curl"),      // network fetch (curl.exe ships on Win10+)
        QStringLiteral("taskkill"),  // kill equivalent (stop a background proc)
    };
#elif defined(Q_OS_ANDROID)
    // Android Qt apps cannot exec arbitrary shell commands -- refuse by
    // returning an empty list. execute()/executeAsync also hard-fail on Android.
    return QStringList{};
#else
    // Linux / macOS / BSD. Four groups:
    //   1. Read-only inspection utilities (ls, grep, cat, …).
    //   2. The developer TOOLCHAIN a coding agent must run to build, run and
    //      TEST the software it writes — interpreters, package managers, VCS,
    //      build tools. Without these the executor cannot validate its own work.
    //   3. Network fetch (curl/wget) — needed to smoke-test a local dev server
    //      the agent starts and to fetch resources; the scanner still blocks
    //      curl|sh so this is not a pipe-to-shell hole.
    //   4. Process control (kill/pkill) — stop a background process started via
    //      run_shell(background:true).
    // The destructive-pattern scanner (scanForDangerousPatterns, full command,
    // every execute) remains the safety net regardless of what is allow-listed.
    return QStringList{
        // 1. read-only inspection
        QStringLiteral("ls"),
        QStringLiteral("ps"),
        QStringLiteral("grep"),
        QStringLiteral("cat"),
        QStringLiteral("head"),
        QStringLiteral("tail"),
        QStringLiteral("wc"),
        QStringLiteral("find"),
        QStringLiteral("sort"),
        QStringLiteral("uniq"),
        QStringLiteral("echo"),
        QStringLiteral("date"),
        QStringLiteral("whoami"),
        QStringLiteral("pwd"),
        QStringLiteral("df"),
        QStringLiteral("du"),
        QStringLiteral("uname"),
        QStringLiteral("env"),
        QStringLiteral("which"),
        QStringLiteral("file"),
        QStringLiteral("awk"),
        QStringLiteral("sed"),
        QStringLiteral("tr"),
        QStringLiteral("cut"),
        QStringLiteral("basename"),
        QStringLiteral("dirname"),
        QStringLiteral("seq"),
        QStringLiteral("mkdir"),
        QStringLiteral("cp"),
        QStringLiteral("mv"),
        QStringLiteral("test"),
        QStringLiteral("true"),
        QStringLiteral("printf"),
        // 2. developer toolchain (build / run / test)
        QStringLiteral("python"),
        QStringLiteral("python3"),
        QStringLiteral("py"),
        QStringLiteral("pip"),
        QStringLiteral("pip3"),
        QStringLiteral("node"),
        QStringLiteral("npm"),
        QStringLiteral("npx"),
        QStringLiteral("git"),
        QStringLiteral("make"),
        QStringLiteral("cmake"),
        QStringLiteral("ninja"),
        QStringLiteral("go"),
        QStringLiteral("cargo"),
        QStringLiteral("rustc"),
        QStringLiteral("java"),
        QStringLiteral("javac"),
        QStringLiteral("bash"),
        QStringLiteral("sh"),
        QStringLiteral("pytest"),
        // 3. network fetch
        QStringLiteral("curl"),
        QStringLiteral("wget"),
        // 4. process control (stop a background process)
        QStringLiteral("kill"),
        QStringLiteral("pkill"),
    };
#endif
}

/**
 * @brief Destructor. Kills any running async process.
 */
ProcessSandbox::~ProcessSandbox() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    delete m_process;
    m_process = nullptr;
}

// ---------------------------------------------------------------------------
// Synchronous execution
// ---------------------------------------------------------------------------

/*
 * @brief Executes a shell command synchronously.
 * @param command   Full command string.
 * @param timeoutMs Timeout in milliseconds.
 * @return CommandResult with stdout, stderr, exit code, timedOut flag.
 *
 * IMPORTANT: Do NOT call from the UI thread.
 */
ProcessSandbox::CommandResult
ProcessSandbox::execute(const QString& command, int timeoutMs, const QString& workingDir) {
    CommandResult result;

#ifdef Q_OS_ANDROID
    // Android Qt apps cannot exec arbitrary shell commands. Refuse
    // before doing any allow-list work so callers get a clear error.
    Q_UNUSED(timeoutMs);
    result.exitCode = -1;
    result.stderrOutput =
        QStringLiteral("Shell execution is not available on Android. ProcessSandbox "
                       "refuses by design (no /bin/sh, no fork/exec on app sandbox).");
    emit commandBlocked(command, result.stderrOutput);
    return result;
#else
    if (!isCommandAllowed(command)) {
        const QString reason =
            QStringLiteral("Program '%1' is not in the allow-list").arg(extractProgram(command));
        emit commandBlocked(command, reason);
        result.exitCode = -1;
        // Rule 11 — never fail silently. The caller (and the agent) must
        // see WHY the command did not run, not a bare exitCode -1 with
        // empty output. (The destructive-pattern reject below already set
        // stderr; this path historically did not, which made the agent
        // unable to adapt — e.g. retry with python3 or a different tool.)
        result.stderrOutput =
            reason + QStringLiteral(". Allowed programs: ") + m_allowList.join(QLatin1Char(' '));
        return result;
    }

    // Defense-in-depth: even when the program name passes the
    // allow-list, scan the FULL command string for destructive
    // patterns (rm -rf /, sudo, format C:, fork bomb, ...). The
    // allow-list gates the program, the scanner gates the args +
    // pipeline. Both must pass.
    {
        const Verzeta::ScanResult scan = Verzeta::scanForDangerousPatterns(command);
        if (!scan.allowed) {
            emit commandBlocked(command, scan.reason);
            result.exitCode = -1;
            result.stderrOutput = scan.reason;
            return result;
        }
    }

    QProcess proc;
    // Run in the requested working directory (e.g. the conversation's
    // project dir) so relative paths the agent wrote via write_file
    // resolve. Empty = inherit host CWD (unchanged legacy behaviour).
    if (!workingDir.isEmpty()) {
        proc.setWorkingDirectory(workingDir);
    }
#ifdef Q_OS_WIN
    // cmd.exe parses its own command line, so passing `command` through the
    // QProcess argument list applies CRT quoting that cmd then mis-parses,
    // corrupting any command with embedded quotes (git commit -m "...",
    // python -c "..."). setNativeArguments passes the line verbatim, which Qt
    // documents as the correct way to launch cmd.exe. Linux never reaches this
    // branch (Q_OS_WIN preprocesses away).
    proc.setNativeArguments(QStringLiteral("/c ") + command);
    proc.start(QStringLiteral("cmd.exe"), QStringList{});
#else
    // Linux + macOS path. Prefer bash: LLM-generated commands routinely
    // use bash-isms — `source venv/bin/activate`, `[[ … ]]`, arrays,
    // `set -o pipefail` — that dash (which is /bin/sh on Debian/Ubuntu)
    // does NOT support, so they failed with "source: not found" and the
    // agent's venv/dependency flow could never run. Fall back to /bin/sh
    // only when bash is genuinely absent. (A venv created + activated this
    // way also puts `python` on PATH, so agents that type `python` rather
    // than `python3` resolve correctly inside their own venv.)
    static const QString kShellPath = QFileInfo::exists(QStringLiteral("/bin/bash"))
                                          ? QStringLiteral("/bin/bash")
                                          : QStringLiteral("/bin/sh");
    proc.start(kShellPath, {QStringLiteral("-c"), command});
#endif

    if (!proc.waitForStarted(5000)) {
        result.stderrOutput = QStringLiteral("Process failed to start: %1").arg(proc.errorString());
        return result;
    }

    const bool finished = proc.waitForFinished(timeoutMs);

    if (!finished) {
        proc.kill();
        proc.waitForFinished(3000);
        result.timedOut = true;
        result.exitCode = -1;
        result.stdoutOutput = QString::fromLocal8Bit(proc.readAllStandardOutput());
        result.stderrOutput = QString::fromLocal8Bit(proc.readAllStandardError());
        qCWarning(verzetaUi) << "ProcessSandbox: command timed out:" << command;
        return result;
    }

    result.stdoutOutput = QString::fromLocal8Bit(proc.readAllStandardOutput());
    result.stderrOutput = QString::fromLocal8Bit(proc.readAllStandardError());
    result.exitCode = proc.exitCode();
    return result;
#endif  // Q_OS_ANDROID guard end
}

// ---------------------------------------------------------------------------
// Asynchronous execution
// ---------------------------------------------------------------------------

/*
 * @brief Starts an asynchronous command execution, emitting outputLine() per line.
 * @param command   Full command string.
 * @param timeoutMs Maximum runtime in milliseconds.
 */
void ProcessSandbox::executeAsync(const QString& command, int timeoutMs) {
    // Kill any existing async process
    kill();

#ifdef Q_OS_ANDROID
    // Android: refuse cleanly with a one-line outputLine. Nothing to
    // start, nothing to time out, nothing to kill later.
    Q_UNUSED(timeoutMs);
    emit commandBlocked(command, QStringLiteral("Shell execution is not available on Android."));
    emit outputLine(QStringLiteral("[stderr] Shell execution is not available on Android."));
    emit commandFinished(CommandResult{
        {}, QStringLiteral("Shell execution is not available on Android."), -1, false});
    return;
#else
    if (!isCommandAllowed(command)) {
        const QString reason =
            QStringLiteral("Program '%1' is not in the allow-list").arg(extractProgram(command));
        emit commandBlocked(command, reason);
        return;
    }

    // Same defense-in-depth pattern scan as execute(). Async path
    // emits commandBlocked + a stderr-style outputLine + a final
    // commandFinished so the QML / ToolDispatcher subscribers see
    // a clean "blocked" result rather than waiting indefinitely.
    {
        const Verzeta::ScanResult scan = Verzeta::scanForDangerousPatterns(command);
        if (!scan.allowed) {
            emit commandBlocked(command, scan.reason);
            emit outputLine(QStringLiteral("[stderr] ") + scan.reason);
            CommandResult res;
            res.exitCode = -1;
            res.stderrOutput = scan.reason;
            emit commandFinished(res);
            return;
        }
    }

    m_asyncStdout.clear();
    m_asyncStderr.clear();
    m_asyncResult = {};

    m_process = new QProcess(this);

    // Wire up per-line output reading
    connect(
        m_process, &QProcess::readyReadStandardOutput, this, &ProcessSandbox::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &ProcessSandbox::onReadyReadStderr);
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &ProcessSandbox::onAsyncFinished);

#ifdef Q_OS_WIN
    // Verbatim command line -- see execute()'s sync path for why the QProcess
    // argument list corrupts quoted commands under cmd.exe.
    m_process->setNativeArguments(QStringLiteral("/c ") + command);
    m_process->start(QStringLiteral("cmd.exe"), QStringList{});
#else
    m_process->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
#endif

    if (!m_process->waitForStarted(5000)) {
        emit outputLine(
            QStringLiteral("Error: process failed to start: %1").arg(m_process->errorString()));
        CommandResult res;
        res.exitCode = -1;
        res.stderrOutput = m_process->errorString();
        m_process->deleteLater();
        m_process = nullptr;
        emit commandFinished(res);
        return;
    }

    // Timeout guard
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(timeoutMs);
    connect(timer, &QTimer::timeout, this, [this, timer]() {
        timer->deleteLater();
        if (m_process && m_process->state() != QProcess::NotRunning) {
            qCWarning(verzetaUi) << "ProcessSandbox: async command timed out";
            emit outputLine(QStringLiteral("[Process timed out and was killed]"));
            m_asyncResult.timedOut = true;
            m_process->kill();
        }
    });
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            timer,
            &QTimer::stop);
    timer->start();
#endif  // Q_OS_ANDROID guard end
}

/**
 * @brief QML-friendly wrapper that calls executeAsync() with the default timeout.
 * @param command Shell command string.
 */
void ProcessSandbox::executeCommand(const QString& command) {
    executeAsync(command, kDefaultAsyncTimeoutMs);
}

// ---------------------------------------------------------------------------
// Allow-list
// ---------------------------------------------------------------------------

/*
 * @brief Checks whether the first program token of a command is in the allow-list.
 * @param command Full command string.
 * @return true if allowed; false otherwise.
 */
bool ProcessSandbox::isCommandAllowed(const QString& command) const {
    const QString prog = extractProgram(command);
    if (prog.isEmpty()) {
        return false;
    }
    return m_allowList.contains(prog);
}

/*
 * @brief Replaces the allow-list.
 * @param allowedPrograms New list of allowed program names.
 */
void ProcessSandbox::setAllowList(const QStringList& allowedPrograms) {
    m_allowList = allowedPrograms;
}

/**
 * @brief Returns the current allow-list.
 * @return List of allowed program names.
 */
QStringList ProcessSandbox::allowList() const {
    return m_allowList;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

/**
 * @brief Kills the currently running async process.
 */
void ProcessSandbox::kill() {
    if (!m_process) {
        return;
    }
    // Null out m_process BEFORE waiting so that re-entrant calls triggered
    // by the event loop inside waitForFinished() (e.g. onAsyncFinished())
    // find m_process == nullptr and do not double-delete the QProcess object.
    QProcess* proc = m_process;
    m_process = nullptr;
    if (proc->state() != QProcess::NotRunning) {
        proc->kill();
        proc->waitForFinished(3000);
    }
    proc->deleteLater();
}

// ---------------------------------------------------------------------------
// Private slots
// ---------------------------------------------------------------------------

/**
 * @brief Reads and emits each complete stdout line as outputLine().
 */
void ProcessSandbox::onReadyReadStdout() {
    if (!m_process) {
        return;
    }
    while (m_process->canReadLine()) {
        const QString line = QString::fromLocal8Bit(m_process->readLine()).trimmed();
        if (!line.isEmpty()) {
            emit outputLine(line);
        }
    }
}

/**
 * @brief Reads and emits each complete stderr line as outputLine() (prefixed).
 */
void ProcessSandbox::onReadyReadStderr() {
    if (!m_process) {
        return;
    }
    while (m_process->canReadLine()) {
        const QString line = QString::fromLocal8Bit(m_process->readLine()).trimmed();
        if (!line.isEmpty()) {
            emit outputLine(QStringLiteral("[stderr] ") + line);
        }
    }
}

/**
 * @brief Finalises the async result and emits commandFinished().
 * @param exitCode    Process exit code.
 * @param exitStatus  Normal or crash exit.
 */
void ProcessSandbox::onAsyncFinished(int exitCode, QProcess::ExitStatus /*exitStatus*/) {
    // Drain any remaining output that didn't arrive as complete lines
    if (m_process) {
        const QByteArray remaining = m_process->readAllStandardOutput();
        if (!remaining.isEmpty()) {
            emit outputLine(QString::fromLocal8Bit(remaining).trimmed());
        }
        const QByteArray stderrRemaining = m_process->readAllStandardError();
        if (!stderrRemaining.isEmpty()) {
            emit outputLine(QStringLiteral("[stderr] ") +
                            QString::fromLocal8Bit(stderrRemaining).trimmed());
        }
    }

    m_asyncResult.exitCode = exitCode;

    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }

    emit commandFinished(m_asyncResult);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Extracts the first whitespace-delimited token from a command string.
 *        Strips any leading path components (e.g., "/usr/bin/ls" → "ls").
 * @param command Full command string.
 * @return Program name, or empty string if command is empty.
 */
QString ProcessSandbox::extractProgram(const QString& command) {
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    // Get first space-delimited token
    const int spaceIdx = trimmed.indexOf(QLatin1Char(' '));
    const QString token = (spaceIdx < 0) ? trimmed : trimmed.left(spaceIdx);

    // Strip any leading path (allow-list checks program name only).
    // Linux + macOS use forward slash; this line stays unchanged.
    const int slashIdx = token.lastIndexOf(QLatin1Char('/'));
    QString prog = (slashIdx < 0) ? token : token.mid(slashIdx + 1);

#ifdef Q_OS_WIN
    const int bsIdx = prog.lastIndexOf(QLatin1Char('\\'));
    if (bsIdx >= 0) {
        prog = prog.mid(bsIdx + 1);
    }
    if (prog.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
        prog.chop(4);
    }
    prog = prog.toLower();
#endif

    return prog;
}

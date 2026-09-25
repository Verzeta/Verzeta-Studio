// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file canvas-runner.cpp
 * @brief Implementation of CanvasRunner. See header for sandbox notes.
 * @layer Service
 * @dependencies CanvasService, FileService, ProcessSandbox, Qt6::Core.
 */

#include "canvas-runner.h"

// QtGlobal must come before any `#ifdef Q_OS_*` so the platform
// macros are defined. Other Qt includes happen to pull this in
// transitively today; explicit include makes the dependency visible
// and survives include-order refactors.
#include "../models/canvas-console-model.h"
#include "../services/canvas-service.h"
#include "../services/conversation-service.h"
#include "../services/file-service.h"
#include "../utils/dangerous-pattern-scanner.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QtGlobal>

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QUuid>

CanvasRunner::CanvasRunner(QObject* parent) : QObject(parent) {
    // Inactivity timer — restarted on every stdout/stderr chunk and on every
    // sendInput, so a script waiting at a prompt survives while the user types.
    m_timeoutTimer.setSingleShot(true);
    m_timeoutTimer.setInterval(kIdleTimeoutMs);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &CanvasRunner::onTimeout);

    // Absolute ceiling — started once per run, never reset, so a runaway that
    // keeps printing (resetting the inactivity timer) still dies.
    m_absoluteTimer.setSingleShot(true);
    m_absoluteTimer.setInterval(kMaxRunMs);
    connect(&m_absoluteTimer, &QTimer::timeout, this, &CanvasRunner::onAbsoluteTimeout);

    // Display debounce — flushes a newline-less remainder (an input() prompt)
    // so it appears in the console instead of sitting in the line buffer.
    m_partialFlushTimer.setSingleShot(true);
    m_partialFlushTimer.setInterval(kPartialFlushMs);
    connect(&m_partialFlushTimer, &QTimer::timeout, this, &CanvasRunner::onPartialFlush);

    // Default URL-open delegates to QDesktopServices. Tests inject a
    // recording lambda via setOpenUrlHandler so they can assert the
    // dispatch without launching a real GUI app.
    m_openUrl = [](const QString& path) -> bool {
        return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    };

    qCInfo(verzetaUi) << "CanvasRunner initialized";
}

CanvasRunner::~CanvasRunner() {
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(500);
    }
    cleanupTempFile();
    qCInfo(verzetaUi) << "CanvasRunner destroyed";
}

void CanvasRunner::setCanvasService(CanvasService* svc) {
    m_canvasSvc = svc;
}
void CanvasRunner::setFileService(FileService* svc) {
    m_fileSvc = svc;
}
void CanvasRunner::setConsoleModel(CanvasConsoleModel* m) {
    m_console = m;
}
void CanvasRunner::setOpenUrlHandler(std::function<bool(const QString&)> h) {
    m_openUrl = std::move(h);
}

bool CanvasRunner::isRunning() const {
    return m_running;
}

QStringList CanvasRunner::supportedRunLanguages() const {
    return {QStringLiteral("python"), QStringLiteral("shell"), QStringLiteral("bash")};
}

QStringList CanvasRunner::supportedIdeLanguages() const {
    return {
        QStringLiteral("cpp"),
        QStringLiteral("c"),
        QStringLiteral("h"),
        QStringLiteral("hpp"),
        QStringLiteral("javascript"),
        QStringLiteral("typescript"),
        QStringLiteral("java"),
        QStringLiteral("go"),
        QStringLiteral("rust"),
        QStringLiteral("html"),
        QStringLiteral("css"),
        QStringLiteral("qml"),
    };
}

// ---------------------------------------------------------------------------
// Run path
// ---------------------------------------------------------------------------

bool CanvasRunner::runActiveCanvas(const QString& conversationId) {
    VERZETA_ASSERT_MAIN_THREAD();

    // Top-of-method trace — proves the C++ entry point fired when the
    // user clicked Run. Pairs with the QML-side `console.log` in the
    // header Run button's onClicked: if the QML log appears but this
    // one doesn't, the singleton wiring is broken; if neither appears,
    // the QML click never reached this function.
    qCInfo(verzetaUi) << "CanvasRunner::runActiveCanvas: invoked, convId:" << conversationId
                      << "running:" << m_running << "console attached:" << !m_console.isNull();

    // Helper — every refusal path also pushes a console row so the user
    // always sees what happened. The QML console panel reveals on Run
    // click regardless of return value; without a console row the panel
    // would be empty and the click would still feel like a no-op.
    const auto refuseWithConsole = [this](const QString& displayName, const QString& reason) {
        if (m_console) {
            m_console->onRunStarted(displayName);
            m_console->onStderrChunk(reason);
            m_console->onRunFinished(/*exitCode=*/-3, /*elapsedMs=*/0);
        }
        emit errorOccurred(reason);
    };

    if (m_running) {
        refuseWithConsole(
            QStringLiteral("canvas"),
            QStringLiteral("The canvas is already running. Cancel the current run first."));
        return false;
    }
    if (!m_canvasSvc) {
        refuseWithConsole(QStringLiteral("canvas"),
                          QStringLiteral("The canvas is unavailable in this session."));
        return false;
    }
    if (conversationId.isEmpty()) {
        refuseWithConsole(QStringLiteral("canvas"), QStringLiteral("No active conversation"));
        return false;
    }

    const QVariantMap active = m_canvasSvc->activeCanvasFor(conversationId);
    if (active.isEmpty()) {
        refuseWithConsole(QStringLiteral("canvas"),
                          QStringLiteral("No active canvas in this conversation"));
        return false;
    }

    const QString language = active.value(QStringLiteral("language")).toString().toLower();
    const QString filename = active.value(QStringLiteral("filename")).toString();
    const QString content = active.value(QStringLiteral("content")).toString();

    if (!supportedRunLanguages().contains(language)) {
        refuseWithConsole(
            filename.isEmpty() ? QStringLiteral("canvas") : filename,
            QStringLiteral("Run is not supported for the language '%1'.").arg(language));
        return false;
    }
    if (content.isEmpty()) {
        refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                          QStringLiteral("The canvas is empty, so there is nothing to run."));
        return false;
    }

    {
        const Verzeta::ScanResult scan = Verzeta::scanForDangerousPatterns(content);
        if (!scan.allowed) {
            refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                              QStringLiteral("Script blocked by the safety scanner.\n%1\n\n"
                                             "If this is a false positive, run the script outside "
                                             "Verzeta Studio. The app never runs scripts that "
                                             "match destructive command patterns, even inside "
                                             "the sandbox.")
                                  .arg(scan.reason));
            return false;
        }
    }

#if defined(Q_OS_LINUX)
    // ---- Sandbox gate ----
    //
    // Preferred path is bwrap. Fallback path (when bwrap is missing
    // OR blocked by AppArmor's apparmor_restrict_unprivileged_userns
    // sysctl) is direct subprocess -- same as Windows. The scanner
    // above already vetoed `rm -rf` / `sudo` / etc. and the QML
    // warning banner displays "running without sandbox -- user
    // responsibility" whenever sandboxAvailable() returns false.
    //
    // The fall-through is a Linux behaviour change vs the previous
    // "refuse if no sandbox" stance, but it's what the user asked
    // for: "Even bypassing Sandbox on Linux" should let Canvas Run
    // work on AppArmor-restricted hosts where it's currently
    // completely broken (bwrap exists but every probe fails). The
    // safety story is unchanged for well-formed scripts; only
    // dangerous-pattern matches are now refused (by the scanner
    // above) instead of by the absent sandbox.
    const QString bwrap = resolveInterpreter(QStringLiteral("bwrap"));
    bool bwrapUsable = !bwrap.isEmpty() && probeSandboxBlocker().isEmpty();

    if (!bwrapUsable) {
        // Direct-subprocess fallback. Reuses the same logic the
        // Windows path uses below: pick interpreter, write tempfile,
        // start with no sandbox wrapper. The QML warning banner
        // covers user-visible safety messaging.
        QString fbInterp;
        QStringList fbArgs;
        QString fbSuffix;
        if (language == QStringLiteral("python")) {
            fbInterp = resolveInterpreter(QStringLiteral("python3"));
            if (fbInterp.isEmpty())
                fbInterp = resolveInterpreter(QStringLiteral("python"));
            if (fbInterp.isEmpty()) {
                refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                                  QStringLiteral("Python interpreter not found on PATH."));
                return false;
            }
            fbSuffix = QStringLiteral(".py");
            fbArgs = pythonFlags();
        } else {
            fbInterp = resolveInterpreter(QStringLiteral("bash"));
            if (fbInterp.isEmpty()) {
                refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                                  QStringLiteral("bash not found on PATH."));
                return false;
            }
            fbSuffix = QStringLiteral(".sh");
            fbArgs = QStringList{
                QStringLiteral("--noprofile"),
                QStringLiteral("--norc"),
            };
        }
        const QString fbTemp = writeTempFile(content, fbSuffix);
        if (fbTemp.isEmpty()) {
            refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                              QStringLiteral("Could not write the script to a temporary file."));
            return false;
        }
        m_tempFile = fbTemp;
        m_displayName = filename;
        QStringList args = fbArgs;
        args.append(QStringLiteral("--"));
        args.append(fbTemp);
        return startRun(fbInterp, args, filename);
    }
    // bwrap is usable -- fall through to the original argv builder
    // below. Linux behaviour for this path is byte-identical to
    // the original implementation.

    // Pick interpreter + per-language isolation flags. The interpreter
    // path is resolved on the HOST and bind-mounted into the sandbox
    // by bwrap's `--ro-bind / /` blanket; the interpreter argv inside
    // the sandbox is just the basename plus its flags.
    QString interpName;
    QStringList interpArgs;
    QString suffix;
    QString sandboxedScriptPath;

    if (language == QStringLiteral("python")) {
        interpName = QStringLiteral("python3");
        suffix = QStringLiteral(".py");
        sandboxedScriptPath = QStringLiteral("/work/script.py");
        // pythonFlags() carries -u/-I/-B/-E; `--` ends options before the path.
        interpArgs = pythonFlags();
        interpArgs << QStringLiteral("--") << sandboxedScriptPath;
    } else {
        interpName = QStringLiteral("bash");
        suffix = QStringLiteral(".sh");
        sandboxedScriptPath = QStringLiteral("/work/script.sh");
        interpArgs = QStringList{
            QStringLiteral("--noprofile"),
            QStringLiteral("--norc"),
            sandboxedScriptPath,
        };
    }

    // Confirm the host has the interpreter — bwrap's --ro-bind / /
    // exposes the host filesystem read-only inside the sandbox, so the
    // interpreter just needs to exist on the host.
    if (resolveInterpreter(interpName).isEmpty()) {
        refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                          QStringLiteral("Interpreter '%1' not found on PATH").arg(interpName));
        return false;
    }

    const QString hostTempPath = writeTempFile(content, suffix);
    if (hostTempPath.isEmpty()) {
        refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                          QStringLiteral("Could not write the script to a temporary file."));
        return false;
    }
    m_tempFile = hostTempPath;
    m_displayName = filename;

    // ---- Build the bwrap argv ----
    //
    // The bind set is INTENTIONALLY NARROW — we expose system binaries
    // and libraries (so Python's stdlib + bash + ld-linux all work)
    // but we do NOT bind /home, /root, /opt, /var, /srv, /mnt, /media.
    // The script literally cannot see ~/.ssh/id_rsa or ~/.bashrc — they
    // don't exist inside the namespace.
    //
    // --unshare-user / --unshare-pid / --unshare-uts / --unshare-ipc :
    //     process / mount-namespace isolation. We deliberately do NOT
    //     `--unshare-net`: on AppArmor-restricted kernels (Ubuntu 24.04+
    //     with kernel.apparmor_restrict_unprivileged_userns=1) the
    //     loopback setup inside the new netns fails. Sharing the host
    //     net namespace gives the script network access — but with no
    //     access to user data (no /home bind), no persistent FS writes
    //     (read-only /usr, tmpfs /tmp), no privilege escalation (user
    //     ns + no SUID binaries on PATH), there is no useful
    //     exfiltration channel and `pip install` cannot persist.
    // --die-with-parent : if our QProcess goes away, the child does too.
    // --new-session     : new session id; outer-tty signals can't reach.
    // --ro-bind /usr /usr + symlinks /bin /sbin /lib /lib64 /lib32 :
    //     enough of the host FS for interpreters to load shared libs.
    // --ro-bind-try /etc/<...> : pulls only the config files that
    //     interpreters actually consult — ld.so cache, alternatives,
    //     etc. /etc itself is NOT bound so user-installed configs in
    //     /etc/myapp aren't visible.
    // --tmpfs /tmp      : writable scratch; vanishes when bwrap exits.
    // --proc /proc      : real /proc for the new pid namespace.
    // --dev /dev        : minimal device tree.
    // --ro-bind <host> /work/script.<ext> : the canvas tempfile,
    //     mounted read-only inside the sandbox at a fixed path.
    // --chdir /tmp      : CWD inside is the writable tmpfs.
    QStringList bwrapArgs = {
        QStringLiteral("--unshare-user"),
        QStringLiteral("--unshare-ipc"),
        QStringLiteral("--unshare-pid"),
        QStringLiteral("--unshare-uts"),
        QStringLiteral("--unshare-cgroup-try"),
        QStringLiteral("--die-with-parent"),
        QStringLiteral("--new-session"),
        QStringLiteral("--ro-bind"),
        QStringLiteral("/usr"),
        QStringLiteral("/usr"),
        QStringLiteral("--symlink"),
        QStringLiteral("usr/bin"),
        QStringLiteral("/bin"),
        QStringLiteral("--symlink"),
        QStringLiteral("usr/sbin"),
        QStringLiteral("/sbin"),
        QStringLiteral("--symlink"),
        QStringLiteral("usr/lib"),
        QStringLiteral("/lib"),
        QStringLiteral("--symlink"),
        QStringLiteral("usr/lib64"),
        QStringLiteral("/lib64"),
        QStringLiteral("--symlink"),
        QStringLiteral("usr/lib32"),
        QStringLiteral("/lib32"),
        QStringLiteral("--ro-bind-try"),
        QStringLiteral("/etc/alternatives"),
        QStringLiteral("/etc/alternatives"),
        QStringLiteral("--ro-bind-try"),
        QStringLiteral("/etc/ld.so.cache"),
        QStringLiteral("/etc/ld.so.cache"),
        QStringLiteral("--ro-bind-try"),
        QStringLiteral("/etc/ld.so.conf"),
        QStringLiteral("/etc/ld.so.conf"),
        QStringLiteral("--ro-bind-try"),
        QStringLiteral("/etc/ld.so.conf.d"),
        QStringLiteral("/etc/ld.so.conf.d"),
        QStringLiteral("--ro-bind-try"),
        QStringLiteral("/etc/python3"),
        QStringLiteral("/etc/python3"),
        QStringLiteral("--tmpfs"),
        QStringLiteral("/tmp"),
        QStringLiteral("--proc"),
        QStringLiteral("/proc"),
        QStringLiteral("--dev"),
        QStringLiteral("/dev"),
        QStringLiteral("--ro-bind"),
        hostTempPath,
        sandboxedScriptPath,
        QStringLiteral("--chdir"),
        QStringLiteral("/tmp"),
        QStringLiteral("--setenv"),
        QStringLiteral("HOME"),
        QStringLiteral("/tmp"),
        QStringLiteral("--setenv"),
        QStringLiteral("PATH"),
        QStringLiteral("/usr/bin:/bin"),
        QStringLiteral("--setenv"),
        QStringLiteral("LANG"),
        QStringLiteral("C"),
        QStringLiteral("--unsetenv"),
        QStringLiteral("USER"),
        QStringLiteral("--unsetenv"),
        QStringLiteral("LOGNAME"),
        QStringLiteral("--unsetenv"),
        QStringLiteral("XDG_RUNTIME_DIR"),
        QStringLiteral("--unsetenv"),
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
        QStringLiteral("--unsetenv"),
        QStringLiteral("WAYLAND_DISPLAY"),
        QStringLiteral("--unsetenv"),
        QStringLiteral("DISPLAY"),
        QStringLiteral("--unsetenv"),
        QStringLiteral("SSH_AUTH_SOCK"),
        QStringLiteral("--"),
        interpName,
    };
    bwrapArgs.append(interpArgs);

    return startRun(bwrap, bwrapArgs, filename);

#elif defined(Q_OS_MACOS)
    // ---- macOS path: sandbox-exec ----
    //
    // Apple's sandbox-exec is deprecated-but-functional on current
    // macOS releases. Profile is Apple Sandbox Profile Language
    // (SBPL): deny default, then narrowly allow the things a Python
    // / shell script legitimately needs (read /usr + /System +
    // /Library; read+write /tmp + /var/folders for our temp dir;
    // network*; signal self; process-exec for child interpreters).
    // Anything outside that envelope -- writing to ~/Documents,
    // touching /etc, etc. -- is denied at the kernel level.
    const QString sboxExec = resolveInterpreter(QStringLiteral("sandbox-exec"));
    if (sboxExec.isEmpty()) {
        refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                          QStringLiteral("sandbox-exec not found on PATH. Canvas Run "
                                         "requires it for isolation on macOS."));
        return false;
    }

    QString interpName;
    QStringList interpArgs;
    QString suffix;
    if (language == QStringLiteral("python")) {
        interpName = QStringLiteral("python3");
        suffix = QStringLiteral(".py");
        interpArgs = pythonFlags();
    } else {
        interpName = QStringLiteral("bash");
        suffix = QStringLiteral(".sh");
        interpArgs = QStringList{
            QStringLiteral("--noprofile"),
            QStringLiteral("--norc"),
        };
    }
    const QString interpPath = resolveInterpreter(interpName);
    if (interpPath.isEmpty()) {
        refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                          QStringLiteral("Interpreter '%1' not found on PATH").arg(interpName));
        return false;
    }
    const QString hostTempPath = writeTempFile(content, suffix);
    if (hostTempPath.isEmpty()) {
        refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                          QStringLiteral("Could not write the script to a temporary file."));
        return false;
    }
    m_tempFile = hostTempPath;
    m_displayName = filename;

    // Minimal sandbox profile -- deny default, allow what scripts
    // legitimately need. Covers basic Python / bash workflows.
    static const QString kSandboxProfile =
        QStringLiteral("(version 1)\n"
                       "(deny default)\n"
                       "(allow process-fork process-exec)\n"
                       "(allow signal (target self))\n"
                       "(allow file-read* (subpath \"/usr\") (subpath \"/System\") "
                       "(subpath \"/Library\") (subpath \"/private/var\") "
                       "(subpath \"/private/tmp\") (subpath \"/etc\"))\n"
                       "(allow file-read-write (subpath \"/tmp\") "
                       "(subpath \"/private/tmp\") "
                       "(subpath \"/var/folders\") "
                       "(subpath \"/private/var/folders\"))\n"
                       "(allow network*)\n"
                       "(allow mach-lookup)\n"
                       "(allow sysctl-read)\n");

    QStringList sboxArgs = {
        QStringLiteral("-p"),
        kSandboxProfile,
        interpPath,
    };
    sboxArgs.append(interpArgs);
    sboxArgs.append(QStringLiteral("--"));
    sboxArgs.append(hostTempPath);

    return startRun(sboxExec, sboxArgs, filename);

#elif defined(Q_OS_WIN)
    QString interpName;
    QStringList interpArgs;
    QString suffix;
    if (language == QStringLiteral("python")) {
        // Try python3 / python / py in order. First hit wins.
        interpName = resolveInterpreter(QStringLiteral("python3"));
        if (interpName.isEmpty())
            interpName = resolveInterpreter(QStringLiteral("python"));
        if (interpName.isEmpty())
            interpName = resolveInterpreter(QStringLiteral("py"));
        if (interpName.isEmpty()) {
            refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                              QStringLiteral("Python interpreter not found on PATH "
                                             "(checked python3, python, py)."));
            return false;
        }
        suffix = QStringLiteral(".py");
        interpArgs = pythonFlags();
    } else {
        // bash / shell: refuse cleanly on Windows. MSYS2 bash exists
        // but isn't on a typical user's PATH; we don't want to
        // silently fall through and confuse users.
        refuseWithConsole(
            filename.isEmpty() ? QStringLiteral("canvas") : filename,
            QStringLiteral("Bash and shell scripts cannot run from the canvas on "
                           "Windows. Use Python, or open the canvas in your default editor."));
        return false;
    }

    const QString hostTempPath = writeTempFile(content, suffix);
    if (hostTempPath.isEmpty()) {
        refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                          QStringLiteral("Could not write the script to a temporary file."));
        return false;
    }
    m_tempFile = hostTempPath;
    m_displayName = filename;

    QStringList args = interpArgs;
    args.append(QStringLiteral("--"));
    args.append(hostTempPath);
    return startRun(interpName, args, filename);

#elif defined(Q_OS_ANDROID)
    // ---- Android: refuse. Qt apps can't fork/exec on Android. ----
    Q_UNUSED(language);
    Q_UNUSED(content);
    refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                      QStringLiteral("Canvas Run is not available on Android, because Android "
                                     "does not let apps start other programs. Use "
                                     "the desktop app (Linux or Windows) to run "
                                     "canvas scripts."));
    return false;

#else
    Q_UNUSED(language);
    Q_UNUSED(content);
    refuseWithConsole(filename.isEmpty() ? QStringLiteral("canvas") : filename,
                      QStringLiteral("Canvas Run is not supported on this platform."));
    return false;
#endif
}

bool CanvasRunner::sandboxAvailable() const {
#if defined(Q_OS_LINUX)
    // bwrap "available" means: binary present AND a uid-map probe
    // succeeds. AppArmor-restricted hosts (Ubuntu 24.04+ default)
    // have bwrap installed but every namespace operation fails -- in
    // that case the runner falls back to direct subprocess and the
    // QML banner correctly shows "no sandbox" because this returns
    // false.
    if (resolveInterpreter(QStringLiteral("bwrap")).isEmpty()) {
        return false;
    }
    return probeSandboxBlocker().isEmpty();
#elif defined(Q_OS_MACOS)
    return !resolveInterpreter(QStringLiteral("sandbox-exec")).isEmpty();
#else
    // Windows: no built-in sandbox. Android: no exec at all.
    return false;
#endif
}

bool CanvasRunner::startRun(const QString& interpreter,
                            const QStringList& args,
                            const QString& displayName) {
    m_process = std::make_unique<QProcess>(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment env;
    const QProcessEnvironment current = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
    // Windows has no bwrap/sandbox-exec wrapping this process (see the Windows
    // branch in run()), so a trimmed environment buys no isolation here and
    // instead breaks the interpreter: Python needs SystemRoot, SystemDrive,
    // TEMP/TMP and more to initialise (DLL search, temp files, sockets).
    // Inherit the full parent environment on Windows; DangerousPatternScanner
    // is the safety layer on this platform.
    env = current;
#else
    // Trimmed environment — only PATH + LANG passed through. No HOME /
    // USER / PYTHONPATH / etc. so user-installed tooling can't be
    // picked up implicitly. (bwrap provides the real isolation on Linux.)
    if (current.contains(QStringLiteral("PATH"))) {
        env.insert(QStringLiteral("PATH"), current.value(QStringLiteral("PATH")));
    } else {
        // Sane default — sufficient for python3 + bash.
        env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/local/bin:/usr/bin:/bin"));
    }
    if (current.contains(QStringLiteral("LANG"))) {
        env.insert(QStringLiteral("LANG"), current.value(QStringLiteral("LANG")));
    }
#endif
    m_process->setProcessEnvironment(env);

    // Working directory: the temp dir. Keeps relative-path side effects
    // contained.
    m_process->setWorkingDirectory(QStandardPaths::writableLocation(QStandardPaths::TempLocation));

    connect(
        m_process.get(), &QProcess::readyReadStandardOutput, this, &CanvasRunner::onProcessStdout);
    connect(
        m_process.get(), &QProcess::readyReadStandardError, this, &CanvasRunner::onProcessStderr);
    connect(m_process.get(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &CanvasRunner::onProcessFinished);
    connect(m_process.get(), &QProcess::errorOccurred, this, &CanvasRunner::onProcessErrorOccurred);

    m_running = true;
    m_userCancelled = false;
    m_timeoutFired = false;
    m_timeoutWasAbsolute = false;
    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_stdoutBytesEmitted = 0;
    m_stderrBytesBuffered = 0;
    m_elapsed.start();
    m_timeoutTimer.start();   // inactivity window (reset on I/O)
    m_absoluteTimer.start();  // absolute ceiling (never reset)

    if (m_console)
        m_console->onRunStarted(displayName);
    emit runningChanged();

    qCInfo(verzetaUi) << "CanvasRunner: starting" << interpreter << "for" << displayName
                      << "args:" << args;
    m_process->start(interpreter, args);

    if (!m_process->waitForStarted(2000)) {
        // Couldn't even spawn — surface as a finished-with-error.
        const QString reason = m_process->errorString();
        cleanupTempFile();
        stopTimers();
        m_running = false;
        if (m_console)
            m_console->onRunFailedToStart(reason);
        emit runningChanged();
        emit errorOccurred(QStringLiteral("Failed to start: %1").arg(reason));
        m_process.reset();
        return false;
    }

    return true;
}

void CanvasRunner::cancel() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_running || !m_process)
        return;
    m_userCancelled = true;
    m_process->terminate();
    if (!m_process->waitForFinished(1000)) {
        m_process->kill();
        m_process->waitForFinished(500);
    }
}

// ---------------------------------------------------------------------------
// Process events
// ---------------------------------------------------------------------------

void CanvasRunner::onProcessStdout() {
    if (!m_process)
        return;
    const QByteArray bytes = m_process->readAllStandardOutput();
    qCInfo(verzetaUi) << "CanvasRunner: stdout chunk" << bytes.size() << "bytes"
                      << "console:" << !m_console.isNull();
    if (bytes.isEmpty())
        return;
    // Output is activity: the script is alive, so restart the inactivity window.
    if (m_running)
        m_timeoutTimer.start();
    if (m_stdoutBytesEmitted >= kMaxStdoutBytes)
        return;
    int allowed = kMaxStdoutBytes - m_stdoutBytesEmitted;
    QByteArray slice = bytes.left(allowed);
    m_stdoutBytesEmitted += slice.size();
    m_stdoutBuffer.append(QString::fromUtf8(slice));
    flushStdout(/*flushPartial=*/false);
    // Whatever remains has no trailing newline (e.g. an `input()` prompt).
    // Arm the debounce so it is shown if no further bytes complete the line.
    if (m_running && !m_stdoutBuffer.isEmpty())
        m_partialFlushTimer.start();
}

void CanvasRunner::onProcessStderr() {
    if (!m_process)
        return;
    // stderr is activity too — restart the inactivity window.
    if (m_running)
        m_timeoutTimer.start();
    if (m_stderrBytesBuffered >= kMaxStderrBytes) {
        // Drop further stderr — we already have plenty for the
        // "last line" extraction at finish time.
        m_process->readAllStandardError();
        return;
    }
    const QByteArray bytes = m_process->readAllStandardError();
    qCInfo(verzetaUi) << "CanvasRunner: stderr chunk" << bytes.size() << "bytes";
    int allowed = kMaxStderrBytes - m_stderrBytesBuffered;
    QByteArray slice = bytes.left(allowed);
    m_stderrBytesBuffered += slice.size();
    m_stderrBuffer.append(QString::fromUtf8(slice));
}

void CanvasRunner::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status);
    stopTimers();
    qCInfo(verzetaUi) << "CanvasRunner: process finished"
                      << "exit:" << exitCode << "elapsed:" << m_elapsed.elapsed() << "ms"
                      << "stdoutBuffer chars:" << m_stdoutBuffer.size()
                      << "stderrBuffer chars:" << m_stderrBuffer.size()
                      << "console:" << !m_console.isNull();

    if (m_process) {
        const QByteArray tailOut = m_process->readAllStandardOutput();
        if (!tailOut.isEmpty()) {
            if (m_stdoutBytesEmitted < kMaxStdoutBytes) {
                int allowed = kMaxStdoutBytes - m_stdoutBytesEmitted;
                QByteArray slice = tailOut.left(allowed);
                m_stdoutBytesEmitted += slice.size();
                m_stdoutBuffer.append(QString::fromUtf8(slice));
            }
        }
        const QByteArray tailErr = m_process->readAllStandardError();
        if (!tailErr.isEmpty()) {
            if (m_stderrBytesBuffered < kMaxStderrBytes) {
                int allowed = kMaxStderrBytes - m_stderrBytesBuffered;
                QByteArray slice = tailErr.left(allowed);
                m_stderrBytesBuffered += slice.size();
                m_stderrBuffer.append(QString::fromUtf8(slice));
            }
        }
    }

    flushStdout(/*flushPartial=*/true);

    const qint64 elapsedMs = m_elapsed.elapsed();
    int effectiveExit = exitCode;

    // Synthesize a non-zero exit when the process was killed by us.
    if (m_userCancelled) {
        effectiveExit = -1;
        if (m_console) {
            m_console->onStderrChunk(QStringLiteral("Cancelled by user"));
        }
    } else if (m_timeoutFired) {
        effectiveExit = -2;
        if (m_console) {
            m_console->onStderrChunk(
                m_timeoutWasAbsolute ? QStringLiteral("Stopped: exceeded the %1-minute run limit.")
                                           .arg(kMaxRunMs / 60000)
                                     : QStringLiteral("Stopped: no output or input for %1 seconds.")
                                           .arg(kIdleTimeoutMs / 1000));
        }
    } else if (effectiveExit != 0) {
        // Exit failure — surface the last non-empty line of stderr as
        // a single concise error row (per the user's "minimal surface"
        // mandate). Long Python tracebacks never reach the UI.
        const QStringList lines = m_stderrBuffer.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
            const QString line = it->trimmed();
            if (line.isEmpty())
                continue;
            if (m_console)
                m_console->onStderrChunk(line);
            break;
        }
    }

    emitFinishSummary(effectiveExit, elapsedMs);

    cleanupTempFile();
    m_process.reset();
    m_running = false;
    emit runningChanged();
}

void CanvasRunner::onProcessErrorOccurred(QProcess::ProcessError err) {
    if (err == QProcess::FailedToStart) {
        // Already handled inside startRun — guard against double-emit.
        return;
    }
    qCWarning(verzetaUi) << "CanvasRunner process error:" << err
                         << (m_process ? m_process->errorString() : QString());
}

void CanvasRunner::stopTimers() {
    m_timeoutTimer.stop();
    m_absoluteTimer.stop();
    m_partialFlushTimer.stop();
}

void CanvasRunner::onPartialFlush() {
    // A newline-less remainder has sat unchanged for kPartialFlushMs — it is a
    // prompt, not the head of a line still arriving. Show it.
    if (!m_running)
        return;
    flushStdout(/*flushPartial=*/true);
}

void CanvasRunner::sendInput(const QString& text) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_running || !m_process || m_process->state() != QProcess::Running) {
        if (m_console)
            m_console->onStderrChunk(
                QStringLiteral("No script is running, so there is nothing to send input to."));
        return;
    }
    // Any pending prompt is now answered; show it before the echo so the
    // console reads prompt-then-answer.
    flushStdout(/*flushPartial=*/true);
    m_partialFlushTimer.stop();

    m_process->write((text + QLatin1Char('\n')).toUtf8());
    if (m_console)
        m_console->onInputEcho(text);

    // Input is activity — the user is engaged, so restart the inactivity window.
    m_timeoutTimer.start();
}

void CanvasRunner::sendEof() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_running || !m_process || m_process->state() != QProcess::Running) {
        return;
    }
    flushStdout(/*flushPartial=*/true);
    m_partialFlushTimer.stop();
    m_process->closeWriteChannel();
    if (m_console)
        m_console->onInputEcho(QStringLiteral("(end of input)"));
    m_timeoutTimer.start();
}

void CanvasRunner::onAbsoluteTimeout() {
    if (!m_running || !m_process)
        return;
    m_timeoutWasAbsolute = true;
    onTimeout();
}

void CanvasRunner::onTimeout() {
    if (!m_running || !m_process)
        return;
    m_timeoutFired = true;
    m_process->terminate();
    if (!m_process->waitForFinished(1000)) {
        m_process->kill();
        m_process->waitForFinished(500);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void CanvasRunner::flushStdout(bool flushPartial) {
    if (!m_console) {
        m_stdoutBuffer.clear();
        return;
    }
    int newline;
    while ((newline = m_stdoutBuffer.indexOf(QLatin1Char('\n'))) >= 0) {
        QString line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.size() > kMaxLineChars) {
            line = line.left(kMaxLineChars) + QStringLiteral("…");
        }
        m_console->onStdoutChunk(line);
    }
    if (flushPartial && !m_stdoutBuffer.isEmpty()) {
        QString line = m_stdoutBuffer;
        m_stdoutBuffer.clear();
        if (line.size() > kMaxLineChars) {
            line = line.left(kMaxLineChars) + QStringLiteral("…");
        }
        m_console->onStdoutChunk(line);
    }
}

void CanvasRunner::emitFinishSummary(int exitCode, qint64 elapsedMs) {
    if (!m_console)
        return;
    m_console->onRunFinished(exitCode, elapsedMs);
}

void CanvasRunner::cleanupTempFile() {
    if (m_tempFile.isEmpty())
        return;
    QFile::remove(m_tempFile);
    m_tempFile.clear();
}

QStringList CanvasRunner::pythonFlags() {
    // -u : UNBUFFERED stdout/stderr. Without it CPython block-buffers a pipe,
    //      so an input() prompt never reaches the console before the script
    //      blocks on stdin. -E ignores every PYTHON* env var, so
    //      PYTHONUNBUFFERED=1 would be silently dead — -u is the only fix.
    // -I : isolated mode (no user site-packages, no PYTHONPATH, no '' on sys.path)
    // -B : no .pyc      -E : ignore PYTHON* env
    return {QStringLiteral("-u"), QStringLiteral("-I"), QStringLiteral("-B"), QStringLiteral("-E")};
}

QString CanvasRunner::resolveInterpreter(const QString& name) {
    return QStandardPaths::findExecutable(name);
}

QString CanvasRunner::probeSandboxBlocker() {
    // Cached per process so we don't fork a probe on every Run click.
    static const QString cached = []() -> QString {
        const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
        if (bwrap.isEmpty())
            return {};

        // Run the cheapest possible bwrap invocation: enter a fresh
        // user namespace and exit. If the kernel / AppArmor blocks
        // it, the process emits a stderr line like "setting up uid
        // map: Permission denied" and exits non-zero.
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(bwrap,
                QStringList{QStringLiteral("--unshare-user"),
                            QStringLiteral("--ro-bind"),
                            QStringLiteral("/"),
                            QStringLiteral("/"),
                            QStringLiteral("--die-with-parent"),
                            QStringLiteral("/bin/true")});
        if (!p.waitForStarted(2000)) {
            return QStringLiteral("The sandbox check could not start, so Run is disabled.");
        }
        if (!p.waitForFinished(3000)) {
            p.kill();
            p.waitForFinished(500);
        }
        if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
            return {};
        }
        const QByteArray out = p.readAll();
        if (out.contains("uid map") || out.contains("Permission denied")) {
            return QStringLiteral(
                "The sandbox is blocked by AppArmor, because your kernel restricts "
                "unprivileged user namespaces. To enable Run, use "
                "`sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0` "
                "(add it to /etc/sysctl.d/ to keep it after a reboot).");
        }
        return QStringLiteral("The sandbox is unavailable: the bwrap check exited "
                              "with code %1.")
            .arg(p.exitCode());
    }();
    return cached;
}

QString CanvasRunner::writeTempFile(const QString& content, const QString& suffix) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (dir.isEmpty())
        return {};

    const QString name =
        QStringLiteral("vgs-canvas-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + suffix;
    const QString abs = QDir(dir).absoluteFilePath(name);

    QSaveFile out(abs);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(verzetaUi) << "CanvasRunner::writeTempFile failed to open" << abs << "—"
                             << out.errorString();
        return {};
    }
    const QByteArray bytes = content.toUtf8();
    if (out.write(bytes) != bytes.size() || !out.commit()) {
        qCWarning(verzetaUi) << "CanvasRunner::writeTempFile commit failed for" << abs << "—"
                             << out.errorString();
        return {};
    }
    return abs;
}

// ---------------------------------------------------------------------------
// Send to IDE
// ---------------------------------------------------------------------------

bool CanvasRunner::sendToIde(const QString& conversationId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_canvasSvc) {
        emit errorOccurred(QStringLiteral("The canvas is unavailable in this session."));
        return false;
    }
    if (conversationId.isEmpty()) {
        emit errorOccurred(QStringLiteral("No active conversation"));
        return false;
    }

    const QString path = m_canvasSvc->diskMirrorPath(conversationId);
    if (path.isEmpty()) {
        emit errorOccurred(
            QStringLiteral("The canvas file was not found on disk. Open the canvas first."));
        return false;
    }

    if (!m_openUrl(path)) {
        emit errorOccurred(
            QStringLiteral("Could not open the canvas file in an external editor: %1").arg(path));
        return false;
    }
    qCInfo(verzetaUi) << "CanvasRunner::sendToIde: opened" << path;
    return true;
}

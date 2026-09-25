// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file run-shell-tool.cpp
 * @brief Implementation of Tools::RunShellTool.
 * @layer Service (Tool subsystem)
 * @dependencies ProcessSandbox (backend/utils/process-sandbox.h),
 *               api/tool-calling-schema.h.
 */

#include "run-shell-tool.h"

#include "../../services/background-process-service.h"
#include "../../services/file-service.h"
#include "../../utils/process-sandbox.h"

#include <QJsonObject>

namespace Tools {

// 15s was too short for real development work — `pip install`, `python -m
// venv`, `npm install`, a `make`/`cmake` build or a test suite routinely take
// tens of seconds, and the agent only saw a kill with no useful output. 120s
// lets meaningful build/install/test commands finish while still bounding a
// genuinely hung process (the local sandbox kills + reports timedOut on expiry).
static constexpr int kShellTimeoutMs = 120000;  ///< Foreground command time limit.
/// Output cap for client-routed commands (mirrors the file read cap).
static constexpr qint64 kRemoteMaxOutputBytes = 1024 * 1024;

RunShellTool::RunShellTool(ProcessSandbox& sandbox,
                           FileService& fileService,
                           BackgroundProcessService* bgService)
    : m_sandbox(sandbox), m_fileService(fileService), m_bgService(bgService) {}

QString RunShellTool::name() const {
    return QStringLiteral("run_shell");
}

QString RunShellTool::description() const {
    return QStringLiteral("Runs a shell command (via bash) in the current project's working "
                          "directory and returns {stdout, stderr, exitCode, timedOut}. Relative "
                          "paths resolve against that project directory, so a file you saved with "
                          "write_file can be run directly by name. Use it to actually run, build "
                          "and TEST your work, not just describe it. Common developer tools are "
                          "available — including python3 and pip (Python is 'python3', not "
                          "'python'), node/npm/npx, git, make/cmake/ninja, and the usual file "
                          "and text utilities; for Python packages prefer a virtualenv "
                          "(python3 -m venv .venv && . .venv/bin/activate && pip install ...). "
                          "Destructive or unlisted commands are refused with the reason in "
                          "stderr. Each command has up to a 2-minute timeout. To run something "
                          "that must keep running (a dev server, a watch build), set "
                          "background:true — it returns immediately with a pid and a log file "
                          "path plus the first output; watch it with read_file or 'tail' on that "
                          "log, list processes with 'ps', and stop it with 'kill <pid>'.");
}

QList<ToolParameterSchema> RunShellTool::parameters() const {
    ToolParameterSchema cmd;
    cmd.name = QStringLiteral("command");
    cmd.type = QStringLiteral("string");
    cmd.description = QStringLiteral("Shell command to execute (e.g. 'ls -la /tmp')");
    cmd.required = true;

    ToolParameterSchema bg;
    bg.name = QStringLiteral("background");
    bg.type = QStringLiteral("boolean");
    bg.description =
        QStringLiteral("Optional. When true, run the command as a persistent background "
                       "process (a dev server, a watch build) that keeps running after this "
                       "call returns. Returns {background, id, pid, log, stdout}. Watch it "
                       "with read_file/tail on 'log', list with ps, stop with 'kill <pid>'. "
                       "Default false.");
    bg.required = false;
    return {cmd, bg};
}

bool RunShellTool::runsOnMainThread() const {
    // Pure external-process invocation; no ChatController / SQLite
    // interaction, safe for QtConcurrent dispatch.
    return false;
}

QJsonValue RunShellTool::invoke(const QJsonObject& args) {
    const QString command = args[QStringLiteral("command")].toString();
    if (command.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("command is required")}};
    }

    const bool background = args[QStringLiteral("background")].toBool();

    const QString callerFolderId = args[QStringLiteral("__caller_folder_id")].toString();
    const QString callerConvId = args[QStringLiteral("__caller_conv_id")].toString();

    // Background processes run on the HOST registry only. Refuse ONLY when the
    // workspace is an actual mount (connected editor), which owns its own
    // execution + lifecycle — background-over-wire is out of scope. A plain
    // desktop project folder has a folder id but NO mount, so it runs the
    // background process locally (the common case).
    if (background && m_fileService.isFolderMounted(callerFolderId)) {
        return QJsonObject{{QStringLiteral("error"),
                            QStringLiteral("background:true runs on the host only; this "
                                           "conversation's workspace is a connected editor. "
                                           "Run it without background.")}};
    }

    // Foreground mount routing only. A background request skips this and goes
    // straight to the local host path below (mounts were handled above).
    if (!background && !callerFolderId.isEmpty()) {
        QString err;
        const QJsonObject remote = m_fileService.executeRemoteCommand(
            callerFolderId, callerConvId, command, /*timeoutMs=*/0, kRemoteMaxOutputBytes, &err);
        if (err.isEmpty()) {
            // Ran on the client. Normalise to this tool's response
            // contract + surface the extras the agent benefits from.
            QJsonObject result;
            result[QStringLiteral("stdout")] = remote.value(QStringLiteral("stdout"));
            result[QStringLiteral("stderr")] = remote.value(QStringLiteral("stderr"));
            result[QStringLiteral("exitCode")] = remote.value(QStringLiteral("exit_code"));
            result[QStringLiteral("timedOut")] = remote.value(QStringLiteral("timed_out"));
            result[QStringLiteral("sandboxed")] = remote.value(QStringLiteral("sandboxed"));
            result[QStringLiteral("truncated")] = remote.value(QStringLiteral("truncated"));
            return result;
        }
        // DELIBERATE refusals are honoured — never silently re-run them
        // on the host, which would bypass the user's / filter's intent.
        if (err == QStringLiteral("user_rejected")) {
            return QJsonObject{{QStringLiteral("error"),
                                QStringLiteral("The user declined to run this command "
                                               "on the connected editor.")}};
        }
        if (err == QStringLiteral("blocked_by_smart_filter")) {
            return QJsonObject{{QStringLiteral("error"),
                                QStringLiteral("This command was blocked by the safety "
                                               "filter on the connected editor.")}};
        }
        // no_mount / exec_disabled / exec_unsupported / *_offline / ...
        // → fall through to the host sandbox path so the command still
        // runs somewhere (the editor surfaces a consent prompt when it
        // returned exec_disabled).
    }

    // LOCAL HOST fallback ONLY — reached only when there is no WireProtocol
    // mount (the mount/client path returned above via executeRemoteCommand,
    // which runs on the VS Code / Android client with the client's own cwd
    // and policy). Run in the conversation's project dir so relative paths
    // the agent wrote via write_file resolve — mirrors read_file's
    // activeProjectDir() resolution. This NEVER touches the mount path.
    const QString workingDir = m_fileService.activeProjectDir();

    // Background path: hand off to the persistent registry and return a handle
    // (pid + log + the grace-window output) instead of blocking to completion.
    if (background) {
        if (!m_bgService) {
            return QJsonObject{{QStringLiteral("error"),
                                QStringLiteral("Background execution is not available in "
                                               "this build.")}};
        }
        const BackgroundProcessService::StartOutcome out =
            m_bgService->start(command, workingDir, callerConvId);
        if (!out.ok) {
            return QJsonObject{{QStringLiteral("error"), out.error}};
        }
        return QJsonObject{{QStringLiteral("background"), true},
                           {QStringLiteral("id"), out.id},
                           {QStringLiteral("pid"), out.pid},
                           {QStringLiteral("log"), out.logPath},
                           {QStringLiteral("stdout"), out.initialOutput},
                           {QStringLiteral("hint"),
                            QStringLiteral("Running in the background. Watch it with "
                                           "read_file or 'tail' on the log, list with 'ps', "
                                           "stop with 'kill <pid>'.")}};
    }

    const ProcessSandbox::CommandResult res =
        m_sandbox.execute(command, kShellTimeoutMs, workingDir);

    QJsonObject result;
    result[QStringLiteral("stdout")] = res.stdoutOutput;
    result[QStringLiteral("stderr")] = res.stderrOutput;
    result[QStringLiteral("exitCode")] = res.exitCode;
    result[QStringLiteral("timedOut")] = res.timedOut;
    return result;
}

}  // namespace Tools

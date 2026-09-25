// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file run-shell-tool.h
 * @brief `run_shell` tool: executes an allow-listed shell command
 *        through ProcessSandbox and returns a structured result.
 * @layer Service (Tool subsystem)
 * @dependencies ProcessSandbox (non-owning reference),
 *               backend/tools/itool.h.
 *
 * The tool delegates its entire execution path to ProcessSandbox,
 * which enforces the program allow-list (see ProcessSandbox::
 * isCommandAllowed) and the 15-second wall-clock timeout.
 *
 * Lifetime: held by the handler lambda that ToolService stores for
 * the "run_shell" entry. That lambda captures a std::shared_ptr<ITool>
 * whose underlying object is this RunShellTool instance. The
 * ProcessSandbox reference must outlive the tool; AppController owns
 * both and destroys ToolService (which destroys the handler, which
 * destroys the tool) before destroying ProcessSandbox.
 *
 * Threading: runsOnMainThread() returns false. ProcessSandbox::execute
 * constructs a local QProcess per call, so concurrent invocation from
 * different worker threads is safe.
 */
#pragma once

#include "../itool.h"

class ProcessSandbox;
class FileService;
class BackgroundProcessService;

namespace Tools {

/**
 * @brief ITool implementation for the `run_shell` built-in tool.
 */
class RunShellTool : public ITool {
  public:
    /**
     * @brief Construct the tool.
     * @param sandbox      Non-owning reference for host-side execution.
     *                     Must outlive this tool.
     * @param fileService  Non-owning reference used to route a command
     *                     to a paired client when the conversation's
     *                     workspace is a mount.
     *                     Must outlive this tool.
     * @param bgService    Optional non-owning pointer to the background
     *                     process registry. When present, a call with
     *                     background:true starts a persistent host process
     *                     instead of a bounded foreground one. May be null
     *                     (tests / no-background builds), in which case a
     *                     background request returns an actionable error.
     */
    RunShellTool(ProcessSandbox& sandbox,
                 FileService& fileService,
                 BackgroundProcessService* bgService = nullptr);

    /**
     * @brief Canonical tool name.
     * @returns "run_shell".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the allow-listed shell execution.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `command` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns false, because ProcessSandbox builds a per-call QProcess and is
     *          safe to invoke concurrently from different worker threads.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Execute the command via ProcessSandbox.
     * @param args JSON object with a required "command" string.
     * @return On success, a JSON object containing:
     *           - "stdout":   captured standard output
     *           - "stderr":   captured standard error
     *           - "exitCode": the process exit code (-1 on timeout)
     *           - "timedOut": bool, true iff the 15-second wall
     *                         clock was reached
     *         On missing/empty command, {"error": "command is required"}.
     * @complexity Bounded by the external process; the tool itself
     *             performs constant work.
     * @sideeffects Spawns a /bin/sh child process with a 15-second
     *              timeout. No persistence, no signals on the tool.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    ProcessSandbox& m_sandbox;
    FileService& m_fileService;
    BackgroundProcessService* m_bgService = nullptr;
};

}  // namespace Tools

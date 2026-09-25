// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-execution-worker.h
 * @brief Background worker that executes tool calls off the UI thread.
 *        Routes each tool call to the appropriate handler (shell, file, etc.).
 * @layer Worker
 * @dependencies ProcessSandbox (Utility), FileService (Service), Qt6::Core
 */


#pragma once

#include "../models/tool-call.h"

#include <QJsonValue>
#include <QObject>
#include <QString>

class ProcessSandbox;
class FileService;

/**
 * @brief Worker object that executes a single ToolCall on a background thread.
 *
 * Intended usage with QThread:
 * @code
 *   QThread* thread = new QThread;
 *   ToolExecutionWorker* worker = new ToolExecutionWorker;
 *   worker->moveToThread(thread);
 *   connect(thread, &QThread::finished, worker, &QObject::deleteLater);
 *   thread->start();
 *
 *   // Queue work via queued connection:
 *   QMetaObject::invokeMethod(worker, "execute",
 *       Qt::QueuedConnection,
 *       Q_ARG(ToolCall, call),
 *       Q_ARG(ProcessSandbox*, sandbox));
 * @endcode
 *
 * ## Supported tool names
 * | Name           | Description                                    |
 * |----------------|------------------------------------------------|
 * | run_shell      | Execute a shell command via ProcessSandbox     |
 * | read_file      | Read a file via FileService                    |
 * | write_file     | Write content to a file via FileService        |
 * | list_files     | List directory contents (ls via ProcessSandbox)|
 * | search_web     | Returns an error: not handled by this worker   |
 */
class ToolExecutionWorker : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the worker. Typically moved to a background thread.
     * @param parent Optional Qt parent.
     */
    explicit ToolExecutionWorker(QObject* parent = nullptr);

  public slots:
    /**
     * @brief Executes the given tool call using the supplied sandbox and file service.
     * @param call    The ToolCall struct describing the tool name and arguments.
     * @param sandbox Pointer to a ProcessSandbox for shell-based tools.
     *                Must remain valid for the duration of this call.
     * @param files   Pointer to a FileService for file-based tools.
     *                Must remain valid for the duration of this call.
     * @sideeffects  May spawn child processes (run_shell, list_files) or read/write
     *               files (read_file, write_file). Emits resultReady() or errorOccurred().
     */
    void execute(const ToolCall& call, ProcessSandbox* sandbox, FileService* files);

  signals:
    /**
     * @brief Emitted when the tool call completes successfully.
     * @param callId  The ToolCall::id identifying which call finished.
     * @param result  JSON value with the tool result.
     */
    void resultReady(const QString& callId, const QJsonValue& result);

    /**
     * @brief Emitted when the tool call fails.
     * @param callId The ToolCall::id of the failed call.
     * @param error  Human-readable error message.
     */
    void errorOccurred(const QString& callId, const QString& error);

    /**
     * @brief Emitted for each line of real-time output (e.g., shell stdout).
     * @param callId The ToolCall::id producing the output.
     * @param line   Output line.
     */
    void outputLine(const QString& callId, const QString& line);

  private:
    /**
     * @brief Executes a shell command via ProcessSandbox.
     * @param call    ToolCall with arguments: {"command": "<shell command string>"}.
     * @param sandbox Non-null ProcessSandbox.
     * @sideeffects Emits resultReady() on success, errorOccurred() on failure.
     */
    void executeRunShell(const ToolCall& call, ProcessSandbox* sandbox);

    /**
     * @brief Reads a file's content via FileService.
     * @param call  ToolCall with arguments: {"path": "<absolute file path>"}.
     * @param files Non-null FileService.
     * @sideeffects Emits resultReady() on success, errorOccurred() on failure.
     */
    void executeReadFile(const ToolCall& call, FileService* files);

    /**
     * @brief Writes content to a file via FileService::saveGeneratedFile().
     * @param call  ToolCall with arguments:
     *              {"filename": "\<name\>", "content": "<text>", "dir": "<optional dir>"}.
     * @param files Non-null FileService.
     * @sideeffects Emits resultReady() on success, errorOccurred() on failure.
     */
    void executeWriteFile(const ToolCall& call, FileService* files);

    /**
     * @brief Lists directory contents via ProcessSandbox (uses "ls -la").
     * @param call    ToolCall with arguments: {"path": "<directory path>"}.
     * @param sandbox Non-null ProcessSandbox.
     * @sideeffects Emits resultReady() on success, errorOccurred() on failure.
     */
    void executeListFiles(const ToolCall& call, ProcessSandbox* sandbox);
};

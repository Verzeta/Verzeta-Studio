// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-execution-worker.cpp
 * @brief Implementation of ToolExecutionWorker, covering tool dispatch and execution.
 * @layer Worker
 * @dependencies ProcessSandbox (Utility), FileService (Service)
 */


#include "tool-execution-worker.h"

#include "../services/file-service.h"
#include "../utils/logger.h"
#include "../utils/process-sandbox.h"

#include <QJsonArray>
#include <QJsonObject>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/**
 * @brief Constructs the ToolExecutionWorker.
 * @param parent Optional Qt parent.
 */
ToolExecutionWorker::ToolExecutionWorker(QObject* parent) : QObject(parent) {}

// ---------------------------------------------------------------------------
// Public slot: execute
// ---------------------------------------------------------------------------

/**
 * @brief Dispatches the given tool call to the appropriate handler.
 * @param call    ToolCall to execute.
 * @param sandbox ProcessSandbox for shell-based tools. Must be non-null.
 * @param files   FileService for file-based tools. Must be non-null.
 */
void ToolExecutionWorker::execute(const ToolCall& call,
                                  ProcessSandbox* sandbox,
                                  FileService* files) {
    if (call.toolName == QStringLiteral("run_shell")) {
        executeRunShell(call, sandbox);
    } else if (call.toolName == QStringLiteral("read_file")) {
        executeReadFile(call, files);
    } else if (call.toolName == QStringLiteral("write_file")) {
        executeWriteFile(call, files);
    } else if (call.toolName == QStringLiteral("list_files")) {
        executeListFiles(call, sandbox);
    } else if (call.toolName == QStringLiteral("search_web")) {
        emit errorOccurred(call.id, QStringLiteral("search_web not yet implemented"));
    } else {
        qCWarning(verzetaTools) << "ToolExecutionWorker: unknown tool" << call.toolName;
        emit errorOccurred(call.id, QStringLiteral("Unknown tool: %1").arg(call.toolName));
    }
}

// ---------------------------------------------------------------------------
// Private: run_shell
// ---------------------------------------------------------------------------

/**
 * @brief Runs a shell command via ProcessSandbox::execute() (synchronous).
 * @param call    ToolCall with {"command": "<shell command>"} in arguments.
 * @param sandbox Non-null ProcessSandbox.
 *
 * On success: emits resultReady() with a JSON object:
 *   {"stdout": "...", "stderr": "...", "exit_code": N, "timed_out": false}
 * On block/failure: emits errorOccurred().
 */
void ToolExecutionWorker::executeRunShell(const ToolCall& call, ProcessSandbox* sandbox) {
    const QString command = call.arguments[QStringLiteral("command")].toString().trimmed();

    if (command.isEmpty()) {
        emit errorOccurred(call.id, QStringLiteral("run_shell: 'command' argument is empty"));
        return;
    }

    qCDebug(verzetaTools) << "ToolExecutionWorker: run_shell:" << command;

    // Connect to get per-line output events while the sync command runs
    QMetaObject::Connection outputConn = QObject::connect(
        sandbox, &ProcessSandbox::outputLine, this, [this, id = call.id](const QString& line) {
            emit outputLine(id, line);
        });

    const ProcessSandbox::CommandResult res = sandbox->execute(command);
    QObject::disconnect(outputConn);

    if (res.timedOut) {
        emit errorOccurred(call.id, QStringLiteral("run_shell: command timed out after 10 s"));
        return;
    }

    QJsonObject result;
    result[QStringLiteral("stdout")] = res.stdoutOutput;
    result[QStringLiteral("stderr")] = res.stderrOutput;
    result[QStringLiteral("exit_code")] = res.exitCode;
    result[QStringLiteral("timed_out")] = false;

    emit resultReady(call.id, result);
}

// ---------------------------------------------------------------------------
// Private: read_file
// ---------------------------------------------------------------------------

/**
 * @brief Reads a file's text content via FileService.
 * @param call  ToolCall with {"path": "<file path>"} in arguments.
 * @param files Non-null FileService.
 *
 * On success: emits resultReady() with {"content": "<text>", "mime_type": "<type>"}.
 * On failure: emits errorOccurred().
 */
void ToolExecutionWorker::executeReadFile(const ToolCall& call, FileService* files) {
    const QString path = call.arguments[QStringLiteral("path")].toString().trimmed();

    if (path.isEmpty()) {
        emit errorOccurred(call.id, QStringLiteral("read_file: 'path' argument is empty"));
        return;
    }

    qCDebug(verzetaTools) << "ToolExecutionWorker: read_file:" << path;

    // One-time error handler
    QMetaObject::Connection errConn = QObject::connect(
        files, &FileService::error, this, [this, id = call.id](const QString& msg) {
            emit errorOccurred(id, msg);
        });

    const QByteArray bytes = files->readFileContent(path);
    QObject::disconnect(errConn);

    if (bytes.isEmpty()) {
        // Error was already emitted via the connection above
        return;
    }

    const QString mime = files->mimeType(path);

    QJsonObject result;
    result[QStringLiteral("content")] = QString::fromUtf8(bytes);
    result[QStringLiteral("mime_type")] = mime;
    result[QStringLiteral("size")] = static_cast<qint64>(bytes.size());

    emit resultReady(call.id, result);
}

// ---------------------------------------------------------------------------
// Private: write_file
// ---------------------------------------------------------------------------

/**
 * @brief Saves text content to a file via FileService::saveGeneratedFile().
 * @param call  ToolCall with arguments:
 *              {"filename": "\<name\>", "content": "<text>"}
 *              Optional: {"dir": "<destination directory>"}
 * @param files Non-null FileService.
 *
 * On success: emits resultReady() with {"path": "<saved path>"}.
 * On failure: emits errorOccurred().
 */
void ToolExecutionWorker::executeWriteFile(const ToolCall& call, FileService* files) {
    const QString filename = call.arguments[QStringLiteral("filename")].toString().trimmed();
    const QString content = call.arguments[QStringLiteral("content")].toString();
    const QString destDir = call.arguments[QStringLiteral("dir")].toString().trimmed();

    if (filename.isEmpty()) {
        emit errorOccurred(call.id, QStringLiteral("write_file: 'filename' argument is empty"));
        return;
    }

    qCInfo(verzetaTools) << "ToolExecutionWorker: write_file:" << filename;

    QString savedPath;
    QMetaObject::Connection savedConn =
        QObject::connect(files, &FileService::fileSaved, this, [&savedPath](const QString& path) {
            savedPath = path;
        });
    QMetaObject::Connection errConn = QObject::connect(
        files, &FileService::error, this, [this, id = call.id](const QString& msg) {
            emit errorOccurred(id, msg);
        });

    const QString result = files->saveGeneratedFile(filename, content, destDir);
    QObject::disconnect(savedConn);
    QObject::disconnect(errConn);

    if (result.isEmpty()) {
        return;  // errorOccurred already emitted via connection
    }

    QJsonObject res;
    res[QStringLiteral("path")] = result;
    emit resultReady(call.id, res);
}

// ---------------------------------------------------------------------------
// Private: list_files
// ---------------------------------------------------------------------------

/**
 * @brief Lists directory contents by running "ls -la <path>" via ProcessSandbox.
 * @param call    ToolCall with {"path": "<directory path>"} in arguments.
 * @param sandbox Non-null ProcessSandbox.
 *
 * On success: emits resultReady() with {"listing": "<ls output>"}.
 * On failure: emits errorOccurred().
 */
void ToolExecutionWorker::executeListFiles(const ToolCall& call, ProcessSandbox* sandbox) {
    const QString path = call.arguments[QStringLiteral("path")].toString().trimmed();

    if (path.isEmpty()) {
        emit errorOccurred(call.id, QStringLiteral("list_files: 'path' argument is empty"));
        return;
    }

    // "ls" is in the default allow-list
    const QString command = QStringLiteral("ls -la \"%1\"").arg(path);

    qCInfo(verzetaTools) << "ToolExecutionWorker: list_files:" << path;

    const ProcessSandbox::CommandResult res = sandbox->execute(command);

    if (res.timedOut) {
        emit errorOccurred(call.id, QStringLiteral("list_files: ls timed out"));
        return;
    }

    QJsonObject result;
    result[QStringLiteral("listing")] = res.stdoutOutput;
    result[QStringLiteral("stderr")] = res.stderrOutput;
    result[QStringLiteral("exit_code")] = res.exitCode;
    emit resultReady(call.id, result);
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file logger.h
 * @brief Structured logging wrapper around Qt's QLoggingCategory system.
 *        Declares per-subsystem logging categories and provides initialization.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#pragma once

#include <QLoggingCategory>
#include <QString>

// ---------------------------------------------------------------------------
// Logging categories — one per major subsystem
// ---------------------------------------------------------------------------

/// Database and data access layer events
Q_DECLARE_LOGGING_CATEGORY(verzetaDb)

/// LLM provider, model router, and streaming events
Q_DECLARE_LOGGING_CATEGORY(verzetaLlm)

/// RAG pipeline: embedding, chunking, retrieval events
Q_DECLARE_LOGGING_CATEGORY(verzetaRag)

/// Long-term memory (per-agent AIM / team ACN) events
Q_DECLARE_LOGGING_CATEGORY(verzetaMemory)

/// Tool calling and agent execution events
Q_DECLARE_LOGGING_CATEGORY(verzetaTools)

/// Frontend / QML bridge events
Q_DECLARE_LOGGING_CATEGORY(verzetaUi)

// ---------------------------------------------------------------------------
// Logger initialization
// ---------------------------------------------------------------------------

/**
 * @brief Application-level logging manager.
 *
 * Installs a custom message handler that writes structured log lines to both
 * stderr and a rolling log file under the application data directory.
 *
 * Log format: [LEVEL] [category] [file:line] message
 *
 * Usage:
 *   Logger::initialize(QStandardPaths::writableLocation(...) + "/logs");
 *   qCInfo(verzetaDb) << "Database opened";
 */
class Logger {
  public:
    /**
     * @brief Installs the custom message handler and opens the log file.
     * @param logDir Absolute directory path where log files are written.
     *               Created if it does not exist.
     * @sideeffects Installs Qt message handler via qInstallMessageHandler.
     *              Creates log directory and opens verzeta-studio.log.
     */
    static void initialize(const QString& logDir);

    /**
     * @brief Sets the minimum log level for console output.
     *        File logging always captures all levels.
     * @param level Minimum message type (QtDebugMsg, QtInfoMsg, etc.).
     */
    static void setLogLevel(QtMsgType level);

    /**
     * @brief Test seam: overrides the log-file rotation size cap.
     * @param bytes Maximum size in bytes the current log file may reach
     *              before it rotates to verzeta-studio.log.1. For tests only.
     */
    static void setMaxLogBytesForTest(qint64 bytes);

  private:
    Logger() = default;

    /**
     * @brief Qt message handler callback that routes to file and stderr.
     * @param type Message severity (Debug/Info/Warning/Critical/Fatal).
     * @param context Source location context (file, line, category).
     * @param msg Formatted log message text.
     * @sideeffects Writes to log file and stderr.
     */
    static void
    messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
};

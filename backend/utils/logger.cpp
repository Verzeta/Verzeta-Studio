// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file logger.cpp
 * @brief Implementation of the structured logging system.
 *        Defines logging category instances and the custom Qt message handler.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#include "logger.h"

#include <QTextStream>

#include <iostream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

// ---------------------------------------------------------------------------
// Logging category definitions
// ---------------------------------------------------------------------------

// Default level QtInfoMsg across all categories: per-turn / per-operation
// troubleshooting detail is emitted at Debug (off by default); genuine
// milestones stay at Info. Enable everything with
// QT_LOGGING_RULES="verzeta.*=true" (or one category, e.g.
// verzeta.db.debug=true). The 2-arg macro would default Debug ON, so the
// explicit QtInfoMsg is required for a quiet baseline.
Q_LOGGING_CATEGORY(verzetaDb, "verzeta.db", QtInfoMsg)
Q_LOGGING_CATEGORY(verzetaLlm, "verzeta.llm", QtInfoMsg)
Q_LOGGING_CATEGORY(verzetaRag, "verzeta.rag", QtInfoMsg)
Q_LOGGING_CATEGORY(verzetaMemory, "verzeta.memory", QtInfoMsg)
Q_LOGGING_CATEGORY(verzetaTools, "verzeta.tools", QtInfoMsg)
Q_LOGGING_CATEGORY(verzetaUi, "verzeta.ui", QtInfoMsg)

// ---------------------------------------------------------------------------
// Internal state (file-static, protected by mutex)
// ---------------------------------------------------------------------------

namespace {
QMutex g_logMutex;
QFile g_logFile;
QtMsgType g_minLevel = QtDebugMsg;

// Rotating file sink: cap the current log at g_maxLogBytes and keep a
// single rotated backup (verzeta-studio.log.1), so on-disk logs stay
// bounded to ~2x the cap regardless of how long the app runs or how many
// sessions append. g_logBytes tracks the current file's size to avoid a
// stat() syscall on every written line.
qint64 g_maxLogBytes = 5 * 1024 * 1024;  // 5 MiB (overridable via test seam)
qint64 g_logBytes = 0;

/**
 * @brief Maps QtMsgType to a short level string.
 * @param type Qt message type.
 * @return Human-readable level label.
 */
const char* levelLabel(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:
            return "DEBUG";
        case QtInfoMsg:
            return "INFO ";
        case QtWarningMsg:
            return "WARN ";
        case QtCriticalMsg:
            return "ERROR";
        case QtFatalMsg:
            return "FATAL";
        default:
            return "?????";
    }
}

/**
 * @brief Rotates the log file: closes it, drops the old .1 backup, renames
 *        the current file to .1, and reopens a fresh file.
 * @note Caller MUST hold g_logMutex. Reopens only if the file was open on
 *       entry, so it is safe to call before the initial open (rename-only).
 */
void rotateLocked() {
    const QString path = g_logFile.fileName();
    if (path.isEmpty()) {
        return;
    }
    const bool wasOpen = g_logFile.isOpen();
    if (wasOpen) {
        g_logFile.close();
    }
    const QString backup = path + QStringLiteral(".1");
    QFile::remove(backup);        // drop the older backup
    QFile::rename(path, backup);  // current -> .1
    g_logBytes = 0;
    if (wasOpen && g_logFile.open(QIODevice::Append | QIODevice::Text)) {
        g_logFile.write("=== log rotated ===\n");
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// Logger implementation
// ---------------------------------------------------------------------------

/*
 * @brief Installs the custom message handler and opens the log file.
 * @param logDir Directory path for log files (created if absent).
 * @sideeffects Installs Qt message handler, opens log file.
 */
void Logger::initialize(const QString& logDir) {
    QDir dir(logDir);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    // Open the log file and write the session header inside a scoped lock.
    // The lock MUST be released before qInstallMessageHandler / qCInfo —
    // messageHandler also acquires g_logMutex, and QMutex is non-recursive.
    // Calling qCInfo while still holding the lock causes a same-thread deadlock.
    {
        QMutexLocker lock(&g_logMutex);
        const QString logPath = logDir + QStringLiteral("/verzeta-studio.log");
        g_logFile.setFileName(logPath);
        // Rotate up front if a prior run already grew the file past the cap,
        // so appended sessions cannot accumulate unbounded on disk.
        if (QFileInfo(logPath).size() > g_maxLogBytes) {
            rotateLocked();
        }
        // Append to the current log file; each run starts with a session separator
        if (g_logFile.open(QIODevice::Append | QIODevice::Text)) {
            g_logBytes = g_logFile.size();
            QTextStream stream(&g_logFile);
            stream << QStringLiteral("\n=== Verzeta Studio started at %1 ===\n")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
        }
    }  // lock released here — safe to call messageHandler below

    qInstallMessageHandler(Logger::messageHandler);
    qCInfo(verzetaUi) << "Logger initialized, writing to" << g_logFile.fileName();
}

/*
 * @brief Sets the minimum log level for console output.
 * @param level Minimum severity level.
 */
void Logger::setLogLevel(QtMsgType level) {
    QMutexLocker lock(&g_logMutex);
    g_minLevel = level;
}

/*
 * @brief Test seam: overrides the rotation size cap.
 * @param bytes New maximum current-file size in bytes before rotation.
 */
void Logger::setMaxLogBytesForTest(qint64 bytes) {
    QMutexLocker lock(&g_logMutex);
    g_maxLogBytes = bytes;
}

/**
 * @brief Qt message handler: writes structured log lines to file and stderr.
 * @param type Message severity.
 * @param context Source file/line/category context.
 * @param msg Log message text.
 * @sideeffects Writes to g_logFile and stderr. Calls abort() for QtFatalMsg.
 */
void Logger::messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    const QString category =
        context.category ? QString::fromLatin1(context.category) : QStringLiteral("app");
    const QString line = QStringLiteral("[%1] [%2] [%3] %4\n")
                             .arg(timestamp, QString::fromLatin1(levelLabel(type)), category, msg);

    QMutexLocker lock(&g_logMutex);

    // Write to file always, rotating when the current file crosses the cap.
    if (g_logFile.isOpen()) {
        const QByteArray bytes = line.toUtf8();
        g_logFile.write(bytes);
        g_logFile.flush();
        g_logBytes += bytes.size();
        if (g_logBytes > g_maxLogBytes) {
            rotateLocked();
        }
    }

    // Write to stderr for levels at or above the minimum
    if (type >= g_minLevel) {
        std::cerr << line.toStdString();
    }

    if (type == QtFatalMsg) {
        g_logFile.close();
        std::abort();
    }
}

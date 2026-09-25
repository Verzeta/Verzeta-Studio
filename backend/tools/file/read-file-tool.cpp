// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-file-tool.cpp
 * @brief Implementation of Tools::ReadFileTool.
 * @layer Service (Tool subsystem)
 * @dependencies FileService, Qt6::Core (QByteArray).
 */

#include "read-file-tool.h"

#include "../../services/file-service.h"
#include "mount-error-hint.h"

#include <QByteArray>
#include <QFileInfo>
#include <QJsonObject>

namespace Tools {

static constexpr int kDefaultMaxLines = 200;  ///< Lines read when the call gives no max_lines.
static constexpr int kBytesPerLineCap = 200;  ///< Byte budget per requested line.

ReadFileTool::ReadFileTool(FileService& fileService) : m_fileService(fileService) {}

QString ReadFileTool::name() const {
    return QStringLiteral("read_file");
}

QString ReadFileTool::description() const {
    return QStringLiteral("Reads the content of a file and returns it as a string.");
}

QList<ToolParameterSchema> ReadFileTool::parameters() const {
    ToolParameterSchema path;
    path.name = QStringLiteral("path");
    path.type = QStringLiteral("string");
    path.description = QStringLiteral("Absolute file path to read");
    path.required = true;

    ToolParameterSchema maxLines;
    maxLines.name = QStringLiteral("max_lines");
    maxLines.type = QStringLiteral("integer");
    maxLines.description = QStringLiteral("Maximum number of lines to read (default 200)");
    maxLines.required = false;

    return {path, maxLines};
}

bool ReadFileTool::runsOnMainThread() const {
    return false;
}

QJsonValue ReadFileTool::invoke(const QJsonObject& args) {
    const QString path = args[QStringLiteral("path")].toString();
    const int maxLines = args[QStringLiteral("max_lines")].toInt(kDefaultMaxLines);
    const QString callerFolderId = args[QStringLiteral("__caller_folder_id")].toString();
    const QString callerClientId = args[QStringLiteral("__caller_client_id")].toString();

    if (path.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("path is required")}};
    }

    const qint64 maxBytes = static_cast<qint64>(maxLines) * kBytesPerLineCap;

    if (callerFolderId.isEmpty()) {
        // Back-compat path — symmetric resolution with WriteFileTool:
        // relative paths anchor to the active project directory when
        // one is set. Without this, an agent that just wrote `foo.md`
        // via write_file (resolved to `<projectDir>/foo.md`) cannot
        // read it back with the same name because read_file used to
        // drop straight into QFile(path) which resolves against the
        // process CWD.
        QString resolved = path;
        if (QFileInfo(path).isRelative()) {
            const QString projectDir = m_fileService.activeProjectDir();
            if (!projectDir.isEmpty()) {
                resolved = projectDir + QLatin1Char('/') + path;
            }
        }
        const QByteArray raw = m_fileService.readFileContent(resolved, maxBytes);
        if (raw.isEmpty()) {
            return QJsonObject{
                {QStringLiteral("error"), QStringLiteral("Could not read file: %1").arg(path)}};
        }
        QJsonObject result;
        result[QStringLiteral("path")] = path;
        result[QStringLiteral("content")] = QString::fromUtf8(raw);
        result[QStringLiteral("size")] = static_cast<int>(raw.size());
        return result;
    }

    // Mount-aware path: hand the raw path to the router along with the
    // caller-folder hint. The router decides per-path whether to read
    // locally (no mount, absolute path) or dispatch through the mount
    // client (registered mount + path in manifest + guards pass).
    const QByteArray raw =
        m_fileService.readFileContent(path, maxBytes, callerFolderId, callerClientId);
    if (raw.isEmpty()) {
        const QString kind = m_fileService.lastErrorKind();
        QJsonObject errObj{
            {QStringLiteral("error"), QStringLiteral("Could not read file: %1").arg(path)}};
        if (!kind.isEmpty()) {
            errObj[QStringLiteral("error_kind")] = kind;
            errObj[QStringLiteral("hint")] = mountErrorHint(kind);
        }
        return errObj;
    }
    QJsonObject result;
    result[QStringLiteral("path")] = path;
    result[QStringLiteral("content")] = QString::fromUtf8(raw);
    result[QStringLiteral("size")] = static_cast<int>(raw.size());
    return result;
}

}  // namespace Tools

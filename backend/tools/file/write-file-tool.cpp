// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file write-file-tool.cpp
 * @brief Implementation of Tools::WriteFileTool.
 * @layer Service (Tool subsystem)
 * @dependencies FileService.
 */

#include "write-file-tool.h"

#include "../../services/file-service.h"
#include "../../utils/escaped-text-repair.h"
#include "../../utils/logger.h"
#include "mount-error-hint.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

namespace Tools {

WriteFileTool::WriteFileTool(FileService& fileService) : m_fileService(fileService) {}

QString WriteFileTool::name() const {
    return QStringLiteral("write_file");
}

QString WriteFileTool::description() const {
    return QStringLiteral("Writes text content to a file, creating directories as needed.");
}

QList<ToolParameterSchema> WriteFileTool::parameters() const {
    ToolParameterSchema fname;
    fname.name = QStringLiteral("filename");
    fname.type = QStringLiteral("string");
    fname.description =
        QStringLiteral("File name relative to the project folder. A relative subpath is kept, "
                       "so \"drafts/spec.md\" creates the \"drafts\" subfolder. Files can "
                       "only be written inside the project folder; a path outside it is "
                       "refused and nothing is written.");
    fname.required = true;

    ToolParameterSchema content;
    content.name = QStringLiteral("content");
    content.type = QStringLiteral("string");
    content.description = QStringLiteral("Text content to write");
    content.required = true;

    return {fname, content};
}

bool WriteFileTool::runsOnMainThread() const {
    return false;
}

QJsonValue WriteFileTool::invoke(const QJsonObject& args) {
    const QString filename = args[QStringLiteral("filename")].toString();
    QString content = args[QStringLiteral("content")].toString();
    // Some models intermittently double-escape newlines in tool-call
    // JSON ("\\n" instead of "\n"), which would land the whole document
    // as one line of literal escape sequences on disk. Repair at the
    // consuming boundary; the persisted tool_calls args stay verbatim.
    if (Verzeta::looksDoubleEscaped(content)) {
        content = Verzeta::repairDoubleEscapedText(content);
        qCWarning(verzetaTools).noquote()
            << "write_file: repaired double-escaped newlines in content for" << filename;
    }
    const QString callerFolderId = args[QStringLiteral("__caller_folder_id")].toString();
    const QString callerClientId = args[QStringLiteral("__caller_client_id")].toString();

    if (filename.isEmpty() || content.isEmpty()) {
        const QString msg =
            filename.isEmpty()
                ? QStringLiteral("filename and content are required")
                : QStringLiteral("content is required: you gave filename '%1' but no 'content'."
                                 " Send the FULL file body in the 'content' field — not a "
                                 "summary, reference, or placeholder.")
                      .arg(filename);
        return QJsonObject{{QStringLiteral("error"), msg}};
    }

    // write_file writes only inside the project folder (a registered editor
    // mount handles its own absolute paths). An absolute path inside the
    // project is written exactly there; one outside it is refused BEFORE
    // anything is written, so the agent asks the user instead of saving
    // somewhere they did not ask for.
    QString target = filename;
    const bool mountRoute =
        !callerFolderId.isEmpty() && m_fileService.isFolderMounted(callerFolderId);
    // Namespaced names from list_files ("/local/...", "/mount/<client>/...")
    // look absolute but are routed by FileService; leave them untouched.
    const QString bare = filename.startsWith(QLatin1Char('/')) ? filename.mid(1) : filename;
    const bool namespaced =
        bare.startsWith(QLatin1String("local/")) || bare.startsWith(QLatin1String("mount/"));
    const QString projectDir = m_fileService.activeProjectDir();
    if (!mountRoute && !namespaced && !projectDir.isEmpty() && QFileInfo(filename).isAbsolute()) {
        const QString wanted = QDir::cleanPath(filename);
        const QString root = QDir::cleanPath(QFileInfo(projectDir).absoluteFilePath());
        if (wanted.startsWith(root + QLatin1Char('/'))) {
            target = QDir(root).relativeFilePath(wanted);
        } else {
            return QJsonObject{
                {QStringLiteral("error"),
                 QStringLiteral("Nothing was written: %1 is outside the project folder, and "
                                "write_file can only write inside it. Ask the user whether to "
                                "save it in the project folder instead (for example as '%2').")
                     .arg(filename, QFileInfo(filename).fileName())},
                {QStringLiteral("error_kind"), QStringLiteral("outside_project")}};
        }
    }

    const QString savedPath =
        callerFolderId.isEmpty()
            // Back-compat: 3-arg form preserves the today's-local-write
            // path verbatim. Existing call sites and tests are unaffected.
            ? m_fileService.saveGeneratedFile(target, content, m_fileService.activeProjectDir())
            : m_fileService.saveGeneratedFile(target,
                                              content,
                                              m_fileService.activeProjectDir(),
                                              callerFolderId,
                                              QString(),
                                              callerClientId);
    if (savedPath.isEmpty()) {
        const QString kind = m_fileService.lastErrorKind();
        QJsonObject errObj{
            {QStringLiteral("error"), QStringLiteral("Failed to write file: %1").arg(filename)}};
        if (!kind.isEmpty()) {
            errObj[QStringLiteral("error_kind")] = kind;
            errObj[QStringLiteral("hint")] = mountErrorHint(kind);
        }
        return errObj;
    }

    QJsonObject result;
    result[QStringLiteral("path")] = target;
    result[QStringLiteral("written")] = content.length();
    return result;
}

}  // namespace Tools

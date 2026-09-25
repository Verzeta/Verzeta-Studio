// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file list-files-tool.cpp
 * @brief Implementation of Tools::ListFilesTool.
 * @layer Service (Tool subsystem)
 * @dependencies Qt6::Core (QDir, QDirIterator).
 */

#include "list-files-tool.h"

#include "../../services/file-service.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

namespace Tools {

ListFilesTool::ListFilesTool(FileService& fileService) : m_fileService(fileService) {}

QString ListFilesTool::name() const {
    return QStringLiteral("list_files");
}

QString ListFilesTool::description() const {
    return QStringLiteral("Lists files and subdirectories. To see the PROJECT's files, OMIT "
                          "'path' (or pass \".\" or \"/\") — that lists the project root and is "
                          "what you want by default, so you never need to ask the user where "
                          "project files are. A relative path lists a subfolder of the project. "
                          "A SPECIFIC absolute filesystem path (e.g. /etc/xdg or "
                          "/home/user/Downloads) browses that exact location. Large listings are "
                          "capped.");
}

QList<ToolParameterSchema> ListFilesTool::parameters() const {
    ToolParameterSchema path;
    path.name = QStringLiteral("path");
    path.type = QStringLiteral("string");
    path.description =
        QStringLiteral("Directory to list. Omit (or pass \".\" or \"/\") for the PROJECT "
                       "root — the default. A relative path lists a subfolder. A specific "
                       "absolute filesystem path (e.g. /etc/xdg) browses that exact "
                       "location.");
    path.required = false;

    ToolParameterSchema recursive;
    recursive.name = QStringLiteral("recursive");
    recursive.type = QStringLiteral("boolean");
    recursive.description = QStringLiteral("Whether to list files recursively");
    recursive.required = false;

    return {path, recursive};
}

bool ListFilesTool::runsOnMainThread() const {
    return false;
}

QJsonValue ListFilesTool::invoke(const QJsonObject& args) {
    QString dirArg = args[QStringLiteral("path")].toString().trimmed();
    const bool isRecurse = args[QStringLiteral("recursive")].toBool(false);

    // A bare filesystem root means the agent's OWN root — the project
    // workspace — never the machine's "/". Remap "/" (and normalised
    // equivalents like "//" or "/.") to the project root so an agent that
    // says "/" gets its project, not the entire filesystem. A SPECIFIC
    // absolute path (e.g. "/etc/xdg") is left untouched and browses that
    // exact location — the size cap in FileService is the only backstop.
    if (!dirArg.isEmpty() && QDir::cleanPath(dirArg) == QLatin1String("/")) {
        dirArg.clear();  // → project workspace root
    }
    const QString callerFolderId = args[QStringLiteral("__caller_folder_id")].toString();
    const QString callerClientId = args[QStringLiteral("__caller_client_id")].toString();

    const QStringList paths =
        m_fileService.listDirectory(dirArg, isRecurse, callerFolderId, callerClientId);


    // Compute the local resolution of `dirArg` so we can both:
    //   - relativise absolute file entries that came back from the
    //     local-fallback branch of FileService;
    //   - synthesize a "directory not found" error symmetric with
    //     pre-refactor behaviour for callers without a mount.
    QString localResolved;
    if (QFileInfo(dirArg).isAbsolute()) {
        localResolved = QDir::cleanPath(dirArg);
    } else {
        const QString projectDir = m_fileService.activeProjectDir();
        if (!projectDir.isEmpty()) {
            localResolved = dirArg.isEmpty()
                                ? QDir::cleanPath(projectDir)
                                : QDir::cleanPath(projectDir + QLatin1Char('/') + dirArg);
        }
    }

    if (paths.isEmpty()) {
        // Distinguish "directory not found" from "directory empty".
        // QDir::exists() is the same check pre-refactor. A relative path
        // in a mounted folder is resolved over the wire, where an empty
        // reply is a legitimate empty directory — so the local not-found
        // synthesis stays gated on callerFolderId.isEmpty() for relative
        // paths. An ABSOLUTE path never mount-routes (it resolves
        // locally regardless of folder), so a non-existent absolute path
        // reports not-found even inside a project/mounted folder — that
        // turns a silent empty reply for an invented path (e.g. the model
        // guessing "/project_root") into an actionable error.
        const bool dirArgAbsolute = QFileInfo(dirArg).isAbsolute();
        if (!localResolved.isEmpty() && !QDir(localResolved).exists() &&
            (callerFolderId.isEmpty() || dirArgAbsolute)) {
            return QJsonObject{{QStringLiteral("error"),
                                QStringLiteral("Directory not found: %1")
                                    .arg(dirArg.isEmpty() ? localResolved : dirArg)}};
        }
    }

    const bool dirArgIsAbsolute = QFileInfo(dirArg).isAbsolute();
    QJsonArray files;
    for (const QString& p : paths) {
        QString display = p;
        if (QFileInfo(p).isAbsolute() && !dirArgIsAbsolute && !localResolved.isEmpty()) {
            const QString clean = QDir::cleanPath(p);
            if (clean == localResolved) {
                display = QString();
            } else if (clean.startsWith(localResolved + QLatin1Char('/'))) {
                display = clean.mid(localResolved.length() + 1);
            }
        }
        files.append(display);
    }

    QJsonObject result;
    // `list_files` does NOT include a `path` field in its response.
    // ToolDispatcher uses the generic `result["path"]` slot as the
    // "this tool produced an artifact at <path>" signal — that
    // contract is correct for write_file / canvas tools / submit_result
    // / custom tools / MCP tools / skills that explicitly write files,
    // but list_files returns a directory LISTING, not a produced file.
    // Echoing the listed directory back as `path` (the old behaviour)
    // teaches the dispatcher's artifact-recording hook to surface
    // directory paths in the artifacts panel — "." or "/abs/proj/"
    // entries showed up as if the agent had written something. The
    // agent already knows what it asked for (it sent `path` in args);
    // it does not need it echoed in the response.
    result[QStringLiteral("files")] = files;
    // Surface truncation: FileService caps the walk at kMaxListEntries to
    // bound time/size (an uncapped recursive "/" produced a 319 MB result).
    // Tell the model so it narrows the path or disables recursion rather
    // than assuming it saw everything.
    if (files.size() >= FileService::kMaxListEntries) {
        result[QStringLiteral("truncated")] = true;
        result[QStringLiteral("note")] =
            QStringLiteral("Listing truncated at %1 entries. Narrow the path or set "
                           "recursive=false.")
                .arg(FileService::kMaxListEntries);
    }
    return result;
}

}  // namespace Tools

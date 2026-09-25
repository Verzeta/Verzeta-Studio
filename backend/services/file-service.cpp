// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file file-service.cpp
 * @brief Implementation of FileService: file I/O, MIME detection,
 *        attachment storage, project bundling, and path safety enforcement.
 * @layer Service
 * @dependencies Qt6::Core
 */


#include "file-service.h"

#include "folder-mount-client-wire.h"
#include "folder-mount-registry.h"
#include "i-folder-mount-client.h"
#include "utils/logger.h"

#include <QTextStream>
#include <QThread>

#include <optional>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

// File size caps
static constexpr qint64 kMaxReadBytes = 1024LL * 1024LL;         ///< 1 MiB default read cap
static constexpr qint64 kMaxSaveBytes = 10LL * 1024LL * 1024LL;  ///< 10 MiB save cap

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/**
 * @brief Constructs the FileService and ensures storage directories exist.
 * @param parent Optional Qt parent.
 */
FileService::FileService(QObject* parent) : QObject(parent) {
    m_appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_attachmentDir = m_appDataDir + QStringLiteral("/attachments");
    m_projectDir = m_appDataDir + QStringLiteral("/projects");

    // Ensure directories exist at startup
    QDir dir;
    if (!dir.mkpath(m_attachmentDir)) {
        qCWarning(verzetaUi) << "FileService: failed to create attachment dir" << m_attachmentDir;
    }
    if (!dir.mkpath(m_projectDir)) {
        qCWarning(verzetaUi) << "FileService: failed to create project dir" << m_projectDir;
    }
}

// ---------------------------------------------------------------------------
// File I/O — readFileContent
// ---------------------------------------------------------------------------

/*
 * @brief Reads up to maxBytes of a file's content.
 * @param path     Absolute file path.
 * @param maxBytes Maximum bytes to read (capped at kMaxReadBytes = 1 MiB by default).
 * @return File bytes, or empty QByteArray on error.
 */
QByteArray FileService::readFileContent(const QString& path, qint64 maxBytes) {
    // Enforce the hard cap regardless of what the caller requested
    const qint64 readLimit = qMin(maxBytes, kMaxReadBytes);

    QFile file(path);
    if (!file.exists()) {
        emit error(QStringLiteral("File not found: %1").arg(path));
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        emit error(QStringLiteral("Cannot open file: %1 (%2)").arg(path, file.errorString()));
        return {};
    }

    const QByteArray data = file.read(readLimit);
    file.close();
    return data;
}

// ---------------------------------------------------------------------------
// MIME type detection
// ---------------------------------------------------------------------------

/*
 * @brief Detects the MIME type of a file using content sniffing and extension.
 * @param path File path.
 * @return MIME type string; "application/octet-stream" for unknown.
 */
QString FileService::mimeType(const QString& path) {
    static QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(path, QMimeDatabase::MatchDefault);
    return mime.name();
}

// ---------------------------------------------------------------------------
// Save generated file
// ---------------------------------------------------------------------------

/*
 * @brief Saves LLM-generated text content to a file.
 * @param suggestedName  Filename suggested by the LLM (sanitised before use).
 * @param content        UTF-8 text content.
 * @param destDir        Destination directory; defaults to m_projectDir.
 * @return Absolute path of the saved file, or empty on error.
 */
QString FileService::saveGeneratedFile(const QString& suggestedName,
                                       const QString& content,
                                       const QString& destDir) {
    if (content.size() > kMaxSaveBytes) {
        emit error(QStringLiteral("Content too large to save (%1 bytes, max %2 bytes)")
                       .arg(content.size())
                       .arg(kMaxSaveBytes));
        return {};
    }

    const QString targetDir = destDir.isEmpty() ? m_projectDir : destDir;
    QDir dir;
    if (!dir.mkpath(targetDir)) {
        emit error(QStringLiteral("Cannot create directory: %1").arg(targetDir));
        return {};
    }

    // Resolve the (possibly multi-component) name to a target path.
    // Absolute inputs keep today's whole-string sanitise (they land
    // flattened in the destination directory — the long-standing
    // absolute-name-to-local behaviour). A relative name is resolved
    // component-by-component so subdirectories are PRESERVED instead of
    // being flattened into the filename; a traversal attempt is rejected.
    QString safeName;
    if (QFileInfo(suggestedName).isAbsolute()) {
        safeName = sanitiseFilename(suggestedName);
    } else {
        safeName = sanitiseRelativePath(suggestedName);
        if (safeName.isEmpty()) {
            emit error(QStringLiteral("Refusing to write outside the "
                                      "workspace: %1")
                           .arg(suggestedName));
            return {};
        }
    }
    const QString filePath = targetDir + QStringLiteral("/") + safeName;

    // Create intermediate subdirectories (safeName may contain '/').
    const QString parentDir = QFileInfo(filePath).absolutePath();
    if (!dir.mkpath(parentDir)) {
        emit error(QStringLiteral("Cannot create directory: %1").arg(parentDir));
        return {};
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit error(QStringLiteral("Cannot write file: %1 (%2)").arg(filePath, file.errorString()));
        return {};
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << content;
    file.close();

    qCInfo(verzetaUi) << "FileService: saved generated file" << filePath;
    emit fileSaved(filePath);
    return filePath;
}

// ---------------------------------------------------------------------------
// Per-conversation directory
// ---------------------------------------------------------------------------

void FileService::setActiveConversation(const QString& convId) {
    if (convId.isEmpty()) {
        m_activeConvDir.clear();
        return;
    }
    m_activeConvDir = conversationWorkspaceDir(convId);
}

void FileService::setActiveProjectContext(const QString& projectId, const QString& projectName) {
    if (projectId.isEmpty()) {
        m_activeConvDir.clear();
        return;
    }
    m_activeConvDir = projectWorkspaceDir(projectId, projectName);
}

QString FileService::activeProjectDir() const {
    return m_activeConvDir.isEmpty() ? m_projectDir : m_activeConvDir;
}

QString FileService::projectWorkspaceDir(const QString& projectId,
                                         const QString& projectName) const {
    if (projectId.isEmpty())
        return m_projectDir;
    // Readable directory name: sanitized-name_shortid (the single source of
    // the project-folder naming; setActiveProjectContext uses this too).
    const QString sanitized =
        sanitiseFilename(projectName.isEmpty() ? QStringLiteral("project") : projectName);
    const QString dir =
        m_projectDir + QStringLiteral("/") + sanitized + QStringLiteral("_") + projectId.left(8);
    QDir().mkpath(dir);
    return dir;
}

QString FileService::conversationWorkspaceDir(const QString& convId) const {
    if (convId.isEmpty())
        return m_projectDir;
    // First 8 chars of the UUID for a readable per-conversation directory.
    const QString dir = m_projectDir + QStringLiteral("/") + convId.left(8);
    QDir().mkpath(dir);
    return dir;
}

// ---------------------------------------------------------------------------
// Project bundle (zip)
// ---------------------------------------------------------------------------

/*
 * @brief Creates a zip archive of multiple files using the system zip utility.
 * @param files      Map of relative_path → content.
 * @param bundleName Archive name without extension.
 * @return Absolute path to the .zip, or empty on error.
 */
QString FileService::createProjectBundle(const QMap<QString, QString>& files,
                                         const QString& bundleName) {
    // Create a unique staging directory under the project dir
    const QString stagingDir = m_projectDir + QStringLiteral("/staging-") +
                               QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir dir;
    if (!dir.mkpath(stagingDir)) {
        emit error(QStringLiteral("createProjectBundle: cannot create staging dir"));
        return {};
    }

    // Write each file into the staging area
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const QString relative = it.key();
        const QString content = it.value();

        // Ensure the relative path doesn't escape staging
        if (relative.contains(QStringLiteral("..")) || relative.startsWith(QStringLiteral("/"))) {
            emit error(
                QStringLiteral("createProjectBundle: unsafe path in bundle: %1").arg(relative));
            dir.removeRecursively();
            return {};
        }

        const QString fullPath = stagingDir + QStringLiteral("/") + relative;
        QFileInfo fi(fullPath);
        if (!dir.mkpath(fi.absolutePath())) {
            emit error(
                QStringLiteral("createProjectBundle: cannot create subdir for %1").arg(relative));
            dir.removeRecursively();
            return {};
        }

        QFile f(fullPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            emit error(QStringLiteral("createProjectBundle: cannot write %1").arg(relative));
            dir.removeRecursively();
            return {};
        }
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << content;
        f.close();
    }

    // Run zip to create the archive
    const QString safeName = sanitiseFilename(bundleName);
    const QString zipPath = m_projectDir + QStringLiteral("/") + safeName + QStringLiteral(".zip");

    QProcess zipProc;
    zipProc.setWorkingDirectory(stagingDir);
    zipProc.start(QStringLiteral("/usr/bin/zip"),
                  {QStringLiteral("-r"), zipPath, QStringLiteral(".")});

    if (!zipProc.waitForFinished(30000)) {
        zipProc.kill();
        emit error(QStringLiteral("createProjectBundle: zip timed out"));
        dir.removeRecursively();
        return {};
    }

    if (zipProc.exitCode() != 0) {
        emit error(QStringLiteral("createProjectBundle: zip failed (exit %1): %2")
                       .arg(zipProc.exitCode())
                       .arg(QString::fromUtf8(zipProc.readAllStandardError())));
        dir.removeRecursively();
        return {};
    }

    // Clean up staging directory
    dir.setPath(stagingDir);
    dir.removeRecursively();

    qCInfo(verzetaUi) << "FileService: created bundle" << zipPath;
    emit bundleCreated(zipPath);
    return zipPath;
}

// ---------------------------------------------------------------------------
// Store attachment
// ---------------------------------------------------------------------------

/*
 * @brief Copies a file to the app-managed attachment storage.
 * @param sourcePath Original file path.
 * @param messageId  UUID of the owning message.
 * @return Relative path within the attachment directory (e.g., "msg-uuid/file.png"),
 *         or empty on error.
 */
QString FileService::storeAttachment(const QString& sourcePath, const QString& messageId) {
    if (!QFile::exists(sourcePath)) {
        emit error(QStringLiteral("storeAttachment: source file not found: %1").arg(sourcePath));
        return {};
    }

    // Create a message-specific subdirectory
    const QString subDir = m_attachmentDir + QStringLiteral("/") + messageId;
    QDir dir;
    if (!dir.mkpath(subDir)) {
        emit error(
            QStringLiteral("storeAttachment: cannot create subdir for message %1").arg(messageId));
        return {};
    }

    const QFileInfo fi(sourcePath);
    const QString destPath = subDir + QStringLiteral("/") + fi.fileName();

    if (!QFile::copy(sourcePath, destPath)) {
        emit error(
            QStringLiteral("storeAttachment: failed to copy %1 → %2").arg(sourcePath, destPath));
        return {};
    }

    // Return path relative to m_attachmentDir
    return QStringLiteral("%1/%2").arg(messageId, fi.fileName());
}

// ---------------------------------------------------------------------------
// Project shared documents
// ---------------------------------------------------------------------------

QString FileService::projectDocsDir(const QString& projectId, const QString& projectName) {
    if (projectId.isEmpty())
        return {};
    const QString sanitized =
        sanitiseFilename(projectName.isEmpty() ? QStringLiteral("project") : projectName);
    const QString shortId = projectId.left(8);
    const QString dir = m_projectDir + QStringLiteral("/") + sanitized + QStringLiteral("_") +
                        shortId + QStringLiteral("/shared-docs");
    QDir().mkpath(dir);
    return dir;
}

QStringList FileService::listProjectDocuments(const QString& projectId,
                                              const QString& projectName) {
    const QString dir = projectDocsDir(projectId, projectName);
    if (dir.isEmpty())
        return {};
    QDir d(dir);
    return d.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
}

QString FileService::addProjectDocument(const QString& projectId,
                                        const QString& projectName,
                                        const QString& sourcePath) {
    if (!QFile::exists(sourcePath)) {
        emit error(QStringLiteral("addProjectDocument: source not found: %1").arg(sourcePath));
        return {};
    }
    const QString dir = projectDocsDir(projectId, projectName);
    if (dir.isEmpty()) {
        emit error(QStringLiteral("addProjectDocument: invalid project id"));
        return {};
    }

    const QFileInfo fi(sourcePath);
    const QString safeName = sanitiseFilename(fi.fileName());
    QString destPath = dir + QStringLiteral("/") + safeName;

    // If a file with the same name exists, append a numeric suffix.
    if (QFile::exists(destPath)) {
        const QString base = QFileInfo(safeName).completeBaseName();
        const QString ext = QFileInfo(safeName).suffix();
        for (int i = 2; i < 1000; ++i) {
            const QString candidate =
                ext.isEmpty() ? QStringLiteral("%1/%2_%3").arg(dir, base).arg(i)
                              : QStringLiteral("%1/%2_%3.%4").arg(dir, base).arg(i).arg(ext);
            if (!QFile::exists(candidate)) {
                destPath = candidate;
                break;
            }
        }
    }

    if (!QFile::copy(sourcePath, destPath)) {
        emit error(
            QStringLiteral("addProjectDocument: copy failed %1 → %2").arg(sourcePath, destPath));
        return {};
    }
    qCInfo(verzetaUi) << "FileService: added project document" << destPath;
    return destPath;
}

bool FileService::removeProjectDocument(const QString& projectId,
                                        const QString& projectName,
                                        const QString& fileName) {
    const QString dir = projectDocsDir(projectId, projectName);
    if (dir.isEmpty())
        return false;
    const QString safeName = sanitiseFilename(fileName);
    const QString fullPath = dir + QStringLiteral("/") + safeName;
    if (!QFile::exists(fullPath))
        return false;
    if (!QFile::remove(fullPath)) {
        emit error(QStringLiteral("removeProjectDocument: failed to remove %1").arg(fullPath));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Path safety
// ---------------------------------------------------------------------------

/*
 * @brief Validates that a path does not escape the allowed base directory.
 * @param path    Path to validate.
 * @param baseDir Allowed base directory. Defaults to AppDataLocation.
 * @return true if safe; false if path escapes baseDir or is suspicious.
 *
 * The rules, including the limitation for a path that does not exist
 * yet, are documented on the declaration in file-service.h.
 */
bool FileService::isPathSafe(const QString& path, const QString& baseDir) {
    if (path.contains(QLatin1Char('\0'))) {
        return false;  // Null byte injection
    }

    const QString base = baseDir.isEmpty() ? m_appDataDir : baseDir;
    const QString canonicalBase = QFileInfo(base).canonicalFilePath();

    // Resolve the canonical path of the target
    QString canonicalPath = QFileInfo(path).canonicalFilePath();

    if (canonicalPath.isEmpty()) {
        // File does not exist — QDir::cleanPath() normalises ".." components without
        // requiring the path to exist. Non-existent paths cannot contain live symlinks
        // so this is safe; any traversal attempt (e.g. "../../etc") is caught here.
        const QString cleanedPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        return cleanedPath.startsWith(canonicalBase + QLatin1Char('/')) ||
               cleanedPath == canonicalBase;
    }

    // The path is safe if it's within the base directory
    return canonicalPath.startsWith(canonicalBase + QStringLiteral("/")) ||
           canonicalPath == canonicalBase;
}

/*
 * @brief Copies a file from sourcePath to destPath.
 * @param sourcePath Existing source file path.
 * @param destPath   Destination path.
 * @return true on success.
 */
bool FileService::copyFile(const QString& sourcePath, const QString& destPath) {
    if (!QFile::exists(sourcePath)) {
        emit error(QStringLiteral("copyFile: source not found: %1").arg(sourcePath));
        return false;
    }
    if (!QFile::copy(sourcePath, destPath)) {
        emit error(QStringLiteral("copyFile: failed to copy %1 → %2").arg(sourcePath, destPath));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Sanitises a filename by removing unsafe characters.
 * @param name Raw filename.
 * @return Safe filename suitable for use in a filesystem path.
 *         Falls back to "file.txt" if the sanitised result is empty.
 */
QString FileService::sanitiseFilename(const QString& name) {
    // Allow alphanumerics, dots, hyphens, underscores, and spaces (collapsed to _)
    static const QRegularExpression unsafe(QStringLiteral("[^a-zA-Z0-9.\\-_ ]"));
    QString safe = name;
    safe.replace(unsafe, QString{});
    safe = safe.trimmed();
    safe.replace(QStringLiteral(" "), QStringLiteral("_"));

    // Strip leading dots (hidden files)
    while (!safe.isEmpty() && safe.startsWith(QLatin1Char('.'))) {
        safe.remove(0, 1);
    }

#ifdef Q_OS_WIN
    // Windows silently strips trailing dots/spaces from a filename and refuses
    // to create a file whose base is a reserved device name. Guard both so the
    // write does not fail. Gated on Windows: on Linux "con.txt" and "file." are
    // perfectly valid, so this leaves the Linux result byte-for-byte unchanged.
    while (!safe.isEmpty() && safe.endsWith(QLatin1Char('.'))) {
        safe.chop(1);
    }
    if (isWindowsReservedName(safe)) {
        safe.prepend(QLatin1Char('_'));
    }
#endif

    return safe.isEmpty() ? QStringLiteral("file.txt") : safe;
}

bool FileService::isWindowsReservedName(const QString& name) {
    // Windows reserves the base name (the part before the first '.') regardless
    // of extension. COM/LPT are reserved for device numbers 1-9 only.
    const int dot = name.indexOf(QLatin1Char('.'));
    const QString base = (dot < 0 ? name : name.left(dot)).toLower();
    if (base == QLatin1String("con") || base == QLatin1String("prn") ||
        base == QLatin1String("aux") || base == QLatin1String("nul")) {
        return true;
    }
    if (base.size() == 4 &&
        (base.startsWith(QLatin1String("com")) || base.startsWith(QLatin1String("lpt")))) {
        const QChar d = base.at(3);
        return d >= QLatin1Char('1') && d <= QLatin1Char('9');
    }
    return false;
}

QString FileService::sanitiseRelativePath(const QString& relPath) {
    // Escape guard, matching the mount write path's pathShapeRejection:
    // normalise first, then refuse any traversal out of the base or an
    // absolute input (those callers keep their own, separate behaviour).
    const QString clean = QDir::cleanPath(relPath);
    if (clean.startsWith(QStringLiteral("..")) || clean.contains(QStringLiteral("/../")) ||
        clean.endsWith(QStringLiteral("/..")) || clean == QStringLiteral("..") ||
        QFileInfo(clean).isAbsolute()) {
        return {};
    }
    // Sanitise EACH component but keep the '/' separators so the
    // subdirectory structure survives (a whole-string sanitise would
    // strip the separators and flatten "a/b.md" to "ab.md").
    QStringList out;
    const QStringList segs = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& seg : segs) {
        if (seg == QStringLiteral("."))
            continue;  // collapse "./"
        out.append(sanitiseFilename(seg));
    }
    return out.isEmpty() ? QStringLiteral("file.txt") : out.join(QLatin1Char('/'));
}


void FileService::setMountRegistry(FolderMountRegistry* registry) {
    m_mountRegistry = registry;
}

void FileService::setMountClient(IFolderMountClient* client) {
    m_mountClient = client;
}

void FileService::setMountCommandClient(FolderMountClientWire* client) {
    m_mountCmdClient = client;
}

namespace {

/** Cross-thread helper that runs `fn` on the registry's thread,
 *  blocking the caller until it completes. Worker → main is ONE-WAY
 *  BlockingQueuedConnection; main never waits for worker, so no
 *  deadlock is possible. Mirrors `FolderMountClientWire::resolveClientId`
 *  and the long-standing pattern in `WireHostBridge::handlePropertyGet`. */
template <typename Fn> void runOnRegistryThread(FolderMountRegistry* registry, Fn&& fn) {
    if (QThread::currentThread() == registry->thread()) {
        fn();
    } else {
        QMetaObject::invokeMethod(registry, std::forward<Fn>(fn), Qt::BlockingQueuedConnection);
    }
}

/** Resolves the mount for `folderId` via the registry. Returns
 *  std::nullopt when no mount is registered (caller falls back to
 *  today's local I/O). */
std::optional<FolderMount> resolveMountSnapshot(FolderMountRegistry* registry,
                                                const QString& folderId) {
    if (!registry)
        return std::nullopt;
    std::optional<FolderMount> out;
    runOnRegistryThread(registry,
                        [registry, folderId, &out]() { out = registry->mountForId(folderId); });
    return out;
}

/** Invokes registry->pathIsAllowed across threads as needed. Empty
 *  return means "allowed"; non-empty is the rejection kind. */
QString invokePathIsAllowed(FolderMountRegistry* registry,
                            const QString& folderId,
                            const QString& relPath) {
    if (!registry)
        return QStringLiteral("mount_offline");
    QString reason;
    runOnRegistryThread(registry, [registry, folderId, relPath, &reason]() {
        reason = registry->pathIsAllowed(folderId, relPath);
    });
    return reason;
}

/** Invokes registry->manifestContains across threads as needed. */
bool invokeManifestContains(FolderMountRegistry* registry,
                            const QString& folderId,
                            const QString& relPath) {
    if (!registry)
        return false;
    bool result = false;
    runOnRegistryThread(registry, [registry, folderId, relPath, &result]() {
        result = registry->manifestContains(folderId, relPath);
    });
    return result;
}

/** Marshals FolderMountRegistry::routeForFile onto the registry
 *  thread (it asserts main-thread; the file tools run on workers). */
FolderMountRegistry::MountRoutingDecision invokeRouteForFile(FolderMountRegistry* registry,
                                                             const QString& folderId,
                                                             const QString& callerClientId,
                                                             const QString& relPath) {
    FolderMountRegistry::MountRoutingDecision d;
    d.folderId = folderId;
    d.reason = QStringLiteral("no_registry");
    if (!registry)
        return d;
    runOnRegistryThread(registry, [registry, folderId, callerClientId, relPath, &d]() {
        d = registry->routeForFile(folderId, callerClientId, relPath);
    });
    return d;
}

/** Marshals a full mounts-for-folder snapshot for the list union and
 *  /mount/<short>/ namespace resolution. */
QVector<FolderMount> invokeMountsSnapshot(FolderMountRegistry* registry, const QString& folderId) {
    QVector<FolderMount> out;
    if (!registry)
        return out;
    runOnRegistryThread(
        registry, [registry, folderId, &out]() { out = registry->mountsForFolder(folderId); });
    return out;
}

/**
 * @brief Parsed form of a possibly-namespaced tool path. list_files
 *        emits `/mount/<short_client_id>/<rel>` and `/local/<rel>`
 *        entries; agents echo those namespaced paths back into
 *        read_file / write_file, so every file op must understand
 *        them. Plain paths take the standard mount routing chain.
 */
struct NsPath {
    enum Kind { Plain, Local, Mount } kind = Plain;
    QString shortClient;  ///< first 8 chars of the client UUID
    QString rel;          ///< path inside the namespace
};

NsPath parseNamespacedPath(const QString& path) {
    NsPath out;
    out.rel = path;
    const QString p = path.startsWith(QLatin1Char('/')) ? path.mid(1) : path;
    if (p.startsWith(QStringLiteral("local/")) || p == QStringLiteral("local")) {
        out.kind = NsPath::Local;
        out.rel = p == QStringLiteral("local") ? QString() : p.mid(6);
        return out;
    }
    if (p.startsWith(QStringLiteral("mount/"))) {
        const QString rest = p.mid(6);
        const int slash = rest.indexOf(QLatin1Char('/'));
        out.kind = NsPath::Mount;
        out.shortClient = slash < 0 ? rest : rest.left(slash);
        out.rel = slash < 0 ? QString() : rest.mid(slash + 1);
        return out;
    }
    return out;
}

/**
 * @brief Target-independent path-SHAPE guard: rejects workspace-escape
 *        attempts (`..` traversal) BEFORE routing, because a local
 *        fallback read of `projectDir/../../etc/passwd` would escape
 *        the project directory. Mount-existence and blocklist guards
 *        are mount-scoped and run only on mount-targeted dispatches.
 * @param relPath Relative path from the tool call.
 * @returns "unsafe_path" on an escape attempt; empty when clean.
 */
QString pathShapeRejection(const QString& relPath) {
    const QString canonical = QDir::cleanPath(relPath);
    if (canonical.startsWith(QStringLiteral("..")) || canonical.contains(QStringLiteral("/../")) ||
        canonical.endsWith(QStringLiteral("/..")) || canonical == QStringLiteral("..")) {
        return QStringLiteral("unsafe_path");
    }
    return {};
}

/** Returns the relative path resolved against the active project
 *  directory. This is today's local-fallback behaviour for relative paths
 *  in tools that don't have a registered mount. */
QString anchorAgainstProjectDir(const QString& projectDir, const QString& path) {
    if (projectDir.isEmpty())
        return path;
    if (path.isEmpty())
        return projectDir;
    return projectDir + QLatin1Char('/') + path;
}

}  // namespace

QJsonObject FileService::executeRemoteCommand(const QString& callerFolderId,
                                              const QString& callerConvId,
                                              const QString& command,
                                              int timeoutMs,
                                              qint64 maxOutputBytes,
                                              QString* outError) {
    if (!outError)
        return {};
    outError->clear();

    if (callerFolderId.isEmpty() || !m_mountRegistry || !m_mountCmdClient) {
        *outError = QStringLiteral("no_mount");
        return {};
    }
    const std::optional<FolderMount> mount = resolveMountSnapshot(m_mountRegistry, callerFolderId);
    if (!mount.has_value()) {
        *outError = QStringLiteral("no_mount");
        return {};
    }

    return m_mountCmdClient->executeCommand(
        callerFolderId, mount->mountId, callerConvId, command, timeoutMs, maxOutputBytes, outError);
}

bool FileService::isFolderMounted(const QString& folderId) const {
    if (folderId.isEmpty() || !m_mountRegistry)
        return false;
    // A mount is registered for this folder only for a connected-editor
    // workspace; a plain desktop project folder resolves to nullopt.
    return resolveMountSnapshot(m_mountRegistry, folderId).has_value();
}

QByteArray FileService::readFileContent(const QString& path,
                                        qint64 maxBytes,
                                        const QString& callerFolderId,
                                        const QString& callerClientId) {
    m_lastErrorKind.clear();
    // Absolute paths NEVER route through a mount — today's behaviour
    // is exact.
    if (QFileInfo(path).isAbsolute()) {
        return readFileContent(path, maxBytes);
    }
    // Without a caller-folder hint or a mount registry, we cannot
    // make a routing decision. Anchor against the active project dir
    // (today's behaviour for relative paths via read-file-tool).
    if (callerFolderId.isEmpty() || !m_mountRegistry || !m_mountClient) {
        return readFileContent(anchorAgainstProjectDir(activeProjectDir(), path), maxBytes);
    }

    const NsPath ns = parseNamespacedPath(path);
    if (ns.kind == NsPath::Local) {
        return readFileContent(anchorAgainstProjectDir(activeProjectDir(), ns.rel), maxBytes);
    }

    const QString relPath = ns.kind == NsPath::Mount ? ns.rel : path;

    // Shape guard (escape attempts) is target-independent and runs
    // FIRST — a local fallback must never traverse out of the
    // project directory.
    {
        const QString shapeReject = pathShapeRejection(relPath);
        if (!shapeReject.isEmpty()) {
            m_lastErrorKind = shapeReject;
            emit error(QStringLiteral("mount path rejected: %1").arg(shapeReject));
            return {};
        }
    }

    QString targetClientId;
    QString targetMountId;
    bool pinnedByNamespace = false;
    if (ns.kind == NsPath::Mount) {
        const QVector<FolderMount> mounts = invokeMountsSnapshot(m_mountRegistry, callerFolderId);
        for (const FolderMount& m : mounts) {
            if (m.clientId.startsWith(ns.shortClient)) {
                targetClientId = m.clientId;
                targetMountId = m.mountId;
                pinnedByNamespace = true;
                break;
            }
        }
        if (targetClientId.isEmpty()) {
            m_lastErrorKind = QStringLiteral("mount_namespace_unknown");
            emit error(QStringLiteral("mount namespace not found: /mount/%1/").arg(ns.shortClient));
            return {};
        }
    } else {
        const auto decision =
            invokeRouteForFile(m_mountRegistry, callerFolderId, callerClientId, relPath);
        if (decision.reason == QStringLiteral("caller_client_not_mounted")) {
            m_lastErrorKind = decision.reason;
            emit error(QStringLiteral("caller's workspace mount is not active"));
            return {};
        }
        if (decision.useLocal()) {
            // Step 5, no active mounts: local; not_found
            // semantics ride on the local read's own miss.
            return readFileContent(anchorAgainstProjectDir(activeProjectDir(), relPath), maxBytes);
        }
        targetClientId = decision.clientId;
        targetMountId = decision.mountId;
        // Sticky owner serves in-manifest files over the wire. A
        // most-recent-active decision for a path that is NOT in the
        // owner's manifest would 404 on the wire — that is the
        // overlay case (host-produced artifacts): go local directly.
        if (decision.reason == QStringLiteral("most_recent_active")) {
            return readFileContent(anchorAgainstProjectDir(activeProjectDir(), relPath), maxBytes);
        }
    }

    // Mount-targeted dispatch confirmed — NOW the workspace guards
    // apply (they protect the paired client's workspace; local reads
    // anchor to the project dir and need none of them).
    const QString rejectKind = invokePathIsAllowed(m_mountRegistry, callerFolderId, relPath);
    if (!rejectKind.isEmpty()) {
        m_lastErrorKind = rejectKind;
        emit error(QStringLiteral("mount path rejected: %1").arg(rejectKind));
        return {};
    }

    // Dispatch via the wire-backed client. The client blocks this
    // worker thread on its own semaphore until reply/timeout — safe
    // off-main (the file tools run with runsOnMainThread()==false).
    // Wire errors PROPAGATE for owned reads (L27 — a stale local copy
    // must not masquerade as the authoritative remote); the
    // namespace-pinned path also propagates because the agent
    // explicitly addressed that client.
    QString fingerprint;
    QString err;
    QByteArray bytes =
        m_mountClient->readBytes(callerFolderId,
                                 targetMountId,
                                 relPath,
                                 qMin(maxBytes, static_cast<qint64>(1024LL * 1024LL)),
                                 &fingerprint,
                                 &err);
    Q_UNUSED(pinnedByNamespace);
    if (!err.isEmpty()) {
        m_lastErrorKind = err;
        emit error(err);
        return {};
    }
    return bytes;
}

QString FileService::saveGeneratedFile(const QString& suggestedName,
                                       const QString& content,
                                       const QString& destDir,
                                       const QString& callerFolderId,
                                       const QString& expectedFingerprint,
                                       const QString& callerClientId) {
    m_lastErrorKind.clear();
    // Mount routing applies only when the caller passed a folder id,
    // the registry + mount client are installed, and the name is
    // relative. Otherwise today's local-write path runs verbatim.
    if (callerFolderId.isEmpty() || !m_mountRegistry || !m_mountClient ||
        QFileInfo(suggestedName).isAbsolute()) {
        return saveGeneratedFile(suggestedName, content, destDir);
    }

    const NsPath ns = parseNamespacedPath(suggestedName);
    if (ns.kind == NsPath::Local) {
        return saveGeneratedFile(ns.rel, content, destDir);
    }
    const QString relPath = ns.kind == NsPath::Mount ? ns.rel : suggestedName;

    // Shape guard (escape attempts) is target-independent and runs
    // FIRST, mirroring readFileContent.
    {
        const QString shapeReject = pathShapeRejection(relPath);
        if (!shapeReject.isEmpty()) {
            m_lastErrorKind = shapeReject;
            emit error(QStringLiteral("mount write rejected: %1").arg(shapeReject));
            return {};
        }
    }

    QString targetClientId;
    QString targetMountId;
    if (ns.kind == NsPath::Mount) {
        const QVector<FolderMount> mounts = invokeMountsSnapshot(m_mountRegistry, callerFolderId);
        for (const FolderMount& m : mounts) {
            if (m.clientId.startsWith(ns.shortClient)) {
                targetClientId = m.clientId;
                targetMountId = m.mountId;
                break;
            }
        }
        if (targetClientId.isEmpty()) {
            m_lastErrorKind = QStringLiteral("mount_namespace_unknown");
            emit error(QStringLiteral("mount namespace not found: /mount/%1/").arg(ns.shortClient));
            return {};
        }
    } else {
        const auto decision =
            invokeRouteForFile(m_mountRegistry, callerFolderId, callerClientId, relPath);
        if (decision.reason == QStringLiteral("caller_client_not_mounted")) {
            m_lastErrorKind = decision.reason;
            emit error(QStringLiteral("caller's workspace mount is not active"));
            return {};
        }
        if (decision.useLocal()) {
            return saveGeneratedFile(relPath, content, destDir);
        }
        targetClientId = decision.clientId;
        targetMountId = decision.mountId;
    }

    // Mount-targeted dispatch confirmed — NOW the workspace guard
    // applies (it protects the paired client's workspace; local
    // writes sanitize their own filenames and need none of it).
    const QString rejectKind = invokePathIsAllowed(m_mountRegistry, callerFolderId, relPath);
    if (!rejectKind.isEmpty()) {
        m_lastErrorKind = rejectKind;
        emit error(QStringLiteral("mount write rejected: %1").arg(rejectKind));
        return {};
    }

    QString err;
    const qint64 applied = m_mountClient->writeBytes(
        callerFolderId, targetMountId, relPath, content.toUtf8(), expectedFingerprint, &err);
    if (!err.isEmpty()) {
        const bool inManifest = invokeManifestContains(m_mountRegistry, callerFolderId, relPath);
        const bool transportErr =
            err == QStringLiteral("bridge_offline") || err == QStringLiteral("mount_offline") ||
            err == QStringLiteral("session_superseded") ||
            err == QStringLiteral("session_closed") || err == QStringLiteral("session_destroyed") ||
            err.startsWith(QStringLiteral("timeout"));
        if (!inManifest && transportErr) {
            qCInfo(verzetaUi).noquote() << "FileService: mount unreachable (" << err
                                        << "); falling back to local for new file" << relPath;
            return saveGeneratedFile(relPath, content, destDir);
        }
        m_lastErrorKind = err;
        emit error(err);
        return {};
    }
    if (applied <= 0) {
        m_lastErrorKind = QStringLiteral("zero_bytes_applied");
        emit error(QStringLiteral("mount write applied zero bytes"));
        return {};
    }
    // Opaque marker so the caller can distinguish a mount-routed
    // write from a local one. Local writes return the absolute file
    // path; this returns a stable, non-collidable identifier.
    return QStringLiteral("mount://%1/%2/%3").arg(callerFolderId, targetMountId, relPath);
}

QStringList FileService::listDirectory(const QString& path,
                                       bool recursive,
                                       const QString& callerFolderId,
                                       const QString& callerClientId) {
    Q_UNUSED(callerClientId);
    auto localList = [recursive](const QString& dirPath) -> QStringList {
        QDir dir(dirPath);
        if (!dir.exists())
            return {};
        const QDir::Filters filter = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot;
        QStringList out;
        // Hard cap the walk: stop once kMaxListEntries are collected. An
        // uncapped recursive walk of a large tree (or an absolute path such
        // as "/") produced a 319 MB result over ~60 s that then crashed the
        // UI. Stopping early bounds both time and size; the tool notes the
        // truncation. (Guards every caller: local, mount-local, fallback.)
        if (recursive) {
            QDirIterator it(dirPath, filter, QDirIterator::Subdirectories);
            while (it.hasNext() && out.size() < kMaxListEntries) {
                it.next();
                out.append(it.filePath());
            }
        } else {
            const QStringList entries = dir.entryList(filter);
            for (const QString& e : entries) {
                if (out.size() >= kMaxListEntries)
                    break;
                out.append(dir.filePath(e));
            }
        }
        return out;
    };

    m_lastErrorKind.clear();

    const NsPath ns = parseNamespacedPath(path);
    const QString relPath = (ns.kind == NsPath::Plain) ? path : ns.rel;

    QString resolvedLocal = relPath;
    if (relPath.isEmpty() || QFileInfo(relPath).isRelative()) {
        const QString projectDir = activeProjectDir();
        if (!projectDir.isEmpty()) {
            resolvedLocal =
                relPath.isEmpty() ? projectDir : (projectDir + QLatin1Char('/') + relPath);
        }
    }

    if (ns.kind == NsPath::Plain && QFileInfo(path).isAbsolute()) {
        return localList(resolvedLocal);
    }
    if (callerFolderId.isEmpty() || !m_mountRegistry || !m_mountClient) {
        return localList(resolvedLocal);
    }
    if (ns.kind == NsPath::Local) {
        // Pinned to the host artifact namespace: local only, but keep
        // the namespaced presentation so paths stay echo-safe.
        QStringList out;
        const QString cleanLocal = QDir::cleanPath(resolvedLocal);
        const QStringList locals = localList(resolvedLocal);
        out.reserve(locals.size());
        for (const QString& abs : locals) {
            QString rel = abs;
            if (abs.startsWith(cleanLocal + QLatin1Char('/'))) {
                rel = abs.mid(cleanLocal.length() + 1);
            }
            out.append(QStringLiteral("/local/%1")
                           .arg(relPath.isEmpty() ? rel : relPath + QLatin1Char('/') + rel));
        }
        return out;
    }

    const QVector<FolderMount> mounts = invokeMountsSnapshot(m_mountRegistry, callerFolderId);
    QVector<FolderMount> activeMounts;
    for (const FolderMount& m : mounts) {
        if (!FolderMountRegistry::mountIsActive(m))
            continue;
        if (ns.kind == NsPath::Mount && !m.clientId.startsWith(ns.shortClient))
            continue;
        activeMounts.append(m);
    }
    if (ns.kind == NsPath::Mount && activeMounts.isEmpty()) {
        m_lastErrorKind = QStringLiteral("mount_namespace_unknown");
        emit error(QStringLiteral("mount namespace not found: /mount/%1/").arg(ns.shortClient));
        return {};
    }
    if (activeMounts.isEmpty() && ns.kind == NsPath::Plain) {
        // No active mounts → plain local view (no namespacing needed;
        // there is only one namespace to see).
        return localList(resolvedLocal);
    }

    const QString rejectKind = invokePathIsAllowed(m_mountRegistry, callerFolderId, relPath);
    if (!rejectKind.isEmpty()) {
        m_lastErrorKind = rejectKind;
        emit error(QStringLiteral("mount list rejected: %1").arg(rejectKind));
        return {};
    }

    QStringList out;
    const QString cleanRel = QDir::cleanPath(relPath);
    const bool wholeTree = cleanRel.isEmpty() || cleanRel == QStringLiteral(".");
    for (const FolderMount& m : activeMounts) {
        const QString shortId = m.clientId.left(8);
        const QStringList paths = FolderMountRegistry::mountManifestPaths(m);
        for (const QString& mp : paths) {
            if (!wholeTree) {
                if (mp != cleanRel && !mp.startsWith(cleanRel + QLatin1Char('/'))) {
                    continue;
                }
                if (!recursive) {
                    const QString tail = mp.mid(cleanRel.length() + 1);
                    if (tail.contains(QLatin1Char('/')))
                        continue;
                }
            } else if (!recursive && mp.contains(QLatin1Char('/'))) {
                continue;
            }
            out.append(QStringLiteral("/mount/%1/%2").arg(shortId, mp));
        }
    }

    if (ns.kind == NsPath::Plain) {
        // Host artifact namespace joins the union.
        const QString cleanLocal = QDir::cleanPath(resolvedLocal);
        const QStringList locals = localList(resolvedLocal);
        for (const QString& abs : locals) {
            QString rel = abs;
            if (abs.startsWith(cleanLocal + QLatin1Char('/'))) {
                rel = abs.mid(cleanLocal.length() + 1);
            } else if (abs == cleanLocal) {
                continue;
            }
            const QString prefixed = wholeTree ? rel : (cleanRel + QLatin1Char('/') + rel);
            out.append(QStringLiteral("/local/%1").arg(prefixed));
        }
    }
    return out;
}

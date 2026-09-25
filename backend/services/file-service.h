// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file file-service.h
 * @brief Handles file I/O for chat attachments, LLM-generated files,
 *        and project bundling. Enforces path safety to prevent directory traversal.
 * @layer Service
 * @dependencies Qt6::Core (QFile, QDir, QMimeDatabase), Qt6::Core (QProcess for zip)
 */


#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

class FolderMountRegistry;
class IFolderMountClient;
class FolderMountClientWire;

/**
 * @brief File I/O service for attachments, LLM-generated files, and project bundles.
 *
 * All paths passed to this service are validated against an allowed base directory
 * before any I/O operation. Caller-supplied paths that escape the allowed directories
 * are rejected and an error() signal is emitted.
 */
class FileService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the FileService.
     *        Initialises attachment and project output directories under
     *        QStandardPaths::AppDataLocation.
     * @param parent Optional Qt parent.
     */
    explicit FileService(QObject* parent = nullptr);

    // -----------------------------------------------------------------------
    // File I/O
    // -----------------------------------------------------------------------

    /**
     * @brief Reads file content for attaching to a message.
     * @param path     Absolute file path. Must pass isPathSafe().
     * @param maxBytes Maximum bytes to read (default 1 MiB). Larger values
     *                 are clamped to 1 MiB, and reading is truncated at the
     *                 resulting limit.
     * @return File content as QByteArray. Returns empty on error or path violation.
     * @sideeffects Reads from the filesystem. Emits error() on failure.
     * @complexity O(min(fileSize, maxBytes)).
     */
    QByteArray readFileContent(const QString& path, qint64 maxBytes = 1024 * 1024);

    /**
     * @brief Detects the MIME type of a file by content and extension.
     * @param path File path (does not need to exist; extension-only fallback used).
     * @return MIME type string (e.g., "text/plain", "image/png").
     *         Returns "application/octet-stream" for unknown types.
     * @complexity O(1) (QMimeDatabase lookup).
     */
    Q_INVOKABLE QString mimeType(const QString& path);

    /**
     * @brief Saves LLM-generated content as a file in the project output directory.
     * @param suggestedName Filename suggested by the LLM (sanitised before use).
     * @param content       String content to write (UTF-8 encoded).
     * @param destDir       Destination directory. Defaults to AppDataLocation/projects/.
     *                      Must be inside AppDataLocation if supplied.
     * @return Absolute path of the saved file, or empty string on error.
     * @sideeffects Creates parent directories if needed. Emits fileSaved() on success,
     *              error() on failure. Content capped at 10 MiB.
     * @complexity O(n) on content length.
     */
    QString saveGeneratedFile(const QString& suggestedName,
                              const QString& content,
                              const QString& destDir = {});

    /**
     * @brief Hard cap on entries returned by listDirectory / list_files.
     *        The walk STOPS once this many entries are collected, bounding
     *        both time and result size. A recursive list of a huge tree
     *        (or an absolute path like "/") can otherwise produce hundreds
     *        of MB and stall for a minute. Callers surface a truncation
     *        note when the returned count reaches this cap.
     */
    static constexpr int kMaxListEntries = 5000;

    /**
     * @brief Resolves a relative, possibly multi-component name to a safe
     *        workspace-relative path, preserving subdirectory structure.
     *
     * Each path component is passed through sanitiseFilename (so unsafe
     * characters are stripped) while the '/' separators are kept, so
     * "sub/dir/file.md" survives instead of flattening to
     * "subdirfile.md". Traversal attempts ("..") and absolute inputs are
     * rejected; those are the caller's responsibility to handle.
     * @param relPath Relative path from a tool call (no leading '/').
     * @returns A cleaned workspace-relative path with subdirectories
     *          preserved, or an empty string when the input escapes the
     *          workspace or is absolute.
     */
    static QString sanitiseRelativePath(const QString& relPath);

    /**
     * @brief Reports whether a filename's base component is a Windows reserved
     *        device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9).
     * @param name A filename; the component before the first '.' is tested,
     *             because Windows reserves the name regardless of extension
     *             ("con", "con.txt" and "con.tar.gz" all fail to create).
     * @returns true if the base is a reserved device name (case-insensitive).
     *          Pure predicate; identical on every platform so it is unit-tested
     *          directly. sanitiseFilename only acts on the result on Windows.
     */
    static bool isWindowsReservedName(const QString& name);

    /**
     * @brief Creates a zip archive from a set of named file contents.
     * @param files      Map of relative_path → file_content (UTF-8 strings).
     * @param bundleName Archive name without extension.
     * @return Absolute path to the created .zip file, or empty string on error.
     * @sideeffects Creates a temporary staging directory, writes files, runs
     *              /usr/bin/zip via QProcess. Emits bundleCreated() or error().
     * @complexity O(total content size), I/O bound.
     */
    QString createProjectBundle(const QMap<QString, QString>& files, const QString& bundleName);

    /**
     * @brief Copies an attachment to the app-managed attachment storage directory.
     * @param sourcePath Absolute path to the original file.
     * @param messageId  UUID of the owning message (used for sub-directory naming).
     * @return Relative path within the attachment storage directory (e.g.,
     *         "msg-uuid/filename.png"), or empty string on error.
     * @sideeffects Copies the file. Creates message-specific subdirectory.
     *              Emits error() on failure.
     * @complexity O(fileSize), copy I/O bound.
     */
    QString storeAttachment(const QString& sourcePath, const QString& messageId);

    // -----------------------------------------------------------------------
    // Path safety
    // -----------------------------------------------------------------------

    /**
     * @brief Validates that a path stays inside an allowed base directory.
     *
     *        A path containing a null byte is rejected. An existing path is
     *        resolved with QFileInfo::canonicalFilePath(), which follows
     *        symlinks and ".." components, and must equal or lie inside the
     *        canonical base directory. A path that does not exist yet cannot
     *        be canonicalised, so its absolute form is normalised with
     *        QDir::cleanPath() instead. That removes ".." components but
     *        does not resolve symlinks, so a symlinked parent directory is
     *        not detected for a path that does not exist yet.
     * @param path       Path to validate (absolute or relative).
     * @param baseDir    Allowed base directory. Defaults to the app data location.
     * @return true if the path is inside baseDir; false if it escapes baseDir
     *         or contains a null byte.
     * @complexity O(n) on path component count.
     */
    bool isPathSafe(const QString& path, const QString& baseDir = {});

    /**
     * @brief Sets the active conversation ID for per-conversation file directories.
     * @param convId Conversation UUID. Empty resets to the default project dir.
     * @sideeffects Updates m_activeConvDir. Creates the directory if needed.
     */
    void setActiveConversation(const QString& convId);

    /**
     * @brief Sets a project-level artifact directory, shared across every
     *        conversation inside the same project/organization folder.
     *        Takes precedence over setActiveConversation() when non-empty.
     * @param projectId    Project folder UUID. Empty to clear.
     * @param projectName  Display name used to make the directory readable.
     *                     Sanitized to a filesystem-safe form.
     * @sideeffects Updates m_activeConvDir to point at the project folder.
     *              Creates the directory if needed.
     */
    void setActiveProjectContext(const QString& projectId, const QString& projectName);

    /**
     * @brief Returns the project directory for the active conversation.
     * @return Per-conversation subdirectory path, or default m_projectDir if no conversation set.
     */
    QString activeProjectDir() const;

    /**
     * @brief Composes (and creates) the workspace directory for a PROJECT,
     *        independent of the mutable active context. Same naming as
     *        setActiveProjectContext, so callers can resolve a conversation's
     *        project folder deterministically rather than relying on whatever
     *        the "current" context happens to be (which is unreliable for
     *        async work such as image completions).
     * @param projectId    Project folder UUID. Empty returns the base dir.
     * @param projectName  Display name (sanitised).
     * @returns Absolute project workspace directory.
     */
    QString projectWorkspaceDir(const QString& projectId, const QString& projectName) const;

    /**
     * @brief Composes (and creates) the per-conversation workspace directory,
     *        independent of the mutable active context.
     * @param convId  Conversation UUID. Empty returns the base dir.
     * @returns Absolute per-conversation workspace directory.
     */
    QString conversationWorkspaceDir(const QString& convId) const;


    /**
     * @brief Installs the host's FolderMountRegistry. Non-owning;
     *        AppController retains ownership. Call from the main
     *        thread before any worker thread invokes the mount-aware
     *        overloads below.
     * @param registry  Pointer to the registry; may be null to detach.
     */
    void setMountRegistry(FolderMountRegistry* registry);

    /**
     * @brief Kind of the most recent mount-path
     *        failure (empty when the last op succeeded or never hit
     *        the mount path). The file tools embed this as
     *        `error_kind` in their structured error JSON.
     * @returns Machine-readable error kind string.
     */
    QString lastErrorKind() const { return m_lastErrorKind; }

    /**
     * @brief Installs the wire-backed mount client. Non-owning;
     *        AppController retains ownership.
     * @param client  Pointer to the client; may be null to detach.
     */
    void setMountClient(IFolderMountClient* client);

    /**
     * @brief Installs the CONCRETE wire mount client used for the
     *        out-of-interface `executeCommand` path. Same
     *        object AppController hands to setMountClient, but the
     *        concrete type, because `executeCommand` is not on
     *        `IFolderMountClient`. Non-owning; may be null to detach.
     * @param client  Concrete FolderMountClientWire pointer.
     */
    void setMountCommandClient(FolderMountClientWire* client);

    /**
     * @brief Routes a shell command to the paired client that owns the
     *        conversation's mount via `vfs.execute`. Route-by-
     *        mount: if `callerFolderId` has a registered mount, the
     *        command is dispatched to that mount's client and the
     *        client owns all execution policy (Off/Ask/Allow,
     *        SmartFilter, sandbox). If there is NO mount, no registry,
     *        or no concrete client, this returns an empty object with
     *        `outError == "no_mount"` so `run_shell` falls back to the
     *        host ProcessSandbox path (UNCHANGED for every non-mounted
     *        conversation: desktop, Android, local).
     *
     * @param callerFolderId  Conversation folder (the run_shell
     *                        `__caller_folder_id`); empty disables
     *                        routing.
     * @param callerConvId    Calling conversation id (the run_shell
     *                        `__caller_conv_id`); forwarded to the client
     *                        so it can apply per-conversation execution
     *                        policy. May be empty.
     * @param command         Shell command line.
     * @param timeoutMs       Per-command deadline (clamped downstream).
     * @param maxOutputBytes  Output cap.
     * @param outError        Receives the error kind on failure
     *                        (`no_mount` drives host fallback; other
     *                        kinds are surfaced to the agent). Must not
     *                        be nullptr.
     * @returns `{stdout, stderr, exit_code, sandboxed, timed_out,
     *          truncated}` on success; empty object on failure.
     */
    QJsonObject executeRemoteCommand(const QString& callerFolderId,
                                     const QString& callerConvId,
                                     const QString& command,
                                     int timeoutMs,
                                     qint64 maxOutputBytes,
                                     QString* outError);

    /**
     * @brief Whether a folder is an active workspace MOUNT (connected editor).
     * @param folderId The conversation's folder id (`__caller_folder_id`).
     * @returns true only if a mount is registered for the folder. A plain
     *          desktop project folder has a folder id but NO mount, so this
     *          returns false and its commands run on the local host. Used to
     *          decide whether run_shell(background) must be refused (background
     *          runs on the host only, not over the wire).
     */
    bool isFolderMounted(const QString& folderId) const;

    /**
     * @brief Mount-aware overload of readFileContent. Routing decision:
     *          - Absolute path → today's local QFile read. Mounts NEVER
     *            apply to absolute paths.
     *          - Empty `callerFolderId`, no mount registry, or no mount
     *            registered for the folder → today's local behaviour
     *            (resolve against activeProjectDir + QFile read).
     *          - Mount registered AND `pathIsAllowed` AND
     *            `manifestContains` → dispatched through the installed
     *            IFolderMountClient. Bytes flow over the wire from the
     *            paired client.
     *          - Mount registered but the guards reject → empty result,
     *            `error()` signal emitted with the rejection kind, no
     *            local fallback (predictable semantics: agents see a
     *            clean refusal rather than silently reading a different
     *            file).
     *
     * @param path            Absolute or relative file path.
     * @param maxBytes        Read cap (clamped at 1 MiB upstream).
     * @param callerFolderId  Folder UUID the call originates from.
     *                        Empty disables mount routing and falls
     *                        back to today's behaviour.
     * @param callerClientId  Paired-client id when the call originates
     *                        from an editor extension; empty for
     *                        host/agent calls (routing then falls to
     *                        the sticky / most-recent-active chain).
     * @returns File bytes on success; empty on failure or rejection.
     */
    QByteArray readFileContent(const QString& path,
                               qint64 maxBytes,
                               const QString& callerFolderId,
                               const QString& callerClientId = {});

    /**
     * @brief Mount-aware overload of saveGeneratedFile. Routing decision
     *        mirrors readFileContent: relative paths in a folder with
     *        a registered mount flow through the wire client; everything
     *        else takes today's local-write path.
     *
     * @param suggestedName        Filename the agent supplied; used both
     *                             as the on-wire `rel_path` and as the
     *                             local-fallback sanitised name.
     * @param content              UTF-8 text content. Capped at
     *                             10 MiB upstream.
     * @param destDir              Destination directory for the local
     *                             fallback. Mount-routed writes ignore
     *                             this; the wire client writes to the
     *                             paired client's workspace.
     * @param callerFolderId       Folder UUID; empty disables routing.
     * @param expectedFingerprint  Optimistic-concurrency token from the
     *                             prior read. Empty disables the
     *                             staleness check. Currently only
     *                             plumbed for mount-routed writes;
     *                             local writes ignore this argument.
     * @param callerClientId       Paired-client id when the call
     *                             originates from an editor extension;
     *                             empty for host/agent calls (routing
     *                             then falls to the sticky /
     *                             most-recent-active chain).
     * @returns On local write, the absolute output path; on mount
     *          write, an opaque "mount://<folder>/<mount>/<relPath>"
     *          marker string indicating the wire path was applied.
     *          Empty on failure.
     */
    QString saveGeneratedFile(const QString& suggestedName,
                              const QString& content,
                              const QString& destDir,
                              const QString& callerFolderId,
                              const QString& expectedFingerprint = {},
                              const QString& callerClientId = {});

    /**
     * @brief Lists files (and subdirectories) under `path`. Centralises
     *        directory enumeration so the file-tools subsystem routes
     *        through one chokepoint. Same routing rules as
     *        readFileContent above.
     *
     * @param path            Directory path (absolute or relative).
     * @param recursive       Whether to descend into subdirectories.
     * @param callerFolderId  Folder UUID; empty disables routing.
     * @param callerClientId  Paired-client id when the call originates
     *                        from an editor extension; empty for
     *                        host/agent calls (routing then falls to
     *                        the sticky / most-recent-active chain).
     * @returns List of paths. On local path, returns absolute paths
     *          discovered by QDir/QDirIterator (byte-identical to
     *          today's list-files-tool output). On mount path, returns
     *          the relative paths the wire client listed. Empty on
     *          failure or rejection.
     */
    QStringList listDirectory(const QString& path,
                              bool recursive,
                              const QString& callerFolderId = {},
                              const QString& callerClientId = {});

    // -----------------------------------------------------------------------
    // Project shared documents (persistent, visible to all agents in the project)
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the absolute shared-docs directory for a project folder.
     *        Directory layout: AppDataLocation/projects/{Sanitized}_{shortId}/shared-docs/
     * @param projectId   Project folder UUID.
     * @param projectName Folder display name (sanitized for fs).
     * @return Absolute path. Creates the directory if it does not exist.
     */
    QString projectDocsDir(const QString& projectId, const QString& projectName);

    /**
     * @brief Lists files currently stored in the project shared-docs
     *        directory.
     * @param projectId   Project folder UUID.
     * @param projectName Folder display name (sanitized for fs).
     * @return List of file names (base names only, not full paths).
     */
    QStringList listProjectDocuments(const QString& projectId, const QString& projectName);

    /**
     * @brief Copies a file into the project shared-docs directory.
     * @param projectId   Project folder UUID.
     * @param projectName Folder display name.
     * @param sourcePath  Absolute path of the file to import.
     * @return Absolute destination path on success, empty string on failure.
     * @sideeffects Copies the file. Emits error() on failure.
     */
    QString addProjectDocument(const QString& projectId,
                               const QString& projectName,
                               const QString& sourcePath);

    /**
     * @brief Removes a file from the project shared-docs directory.
     * @param projectId   Project folder UUID.
     * @param projectName Folder display name.
     * @param fileName    Base name of the file to remove.
     * @return true on success.
     */
    bool removeProjectDocument(const QString& projectId,
                               const QString& projectName,
                               const QString& fileName);

    /**
     * @brief Copies a file from sourcePath to destPath.
     *        Used by the ImageAttachment QML component for "Save As…" functionality.
     * @param sourcePath Source file (must exist).
     * @param destPath   Destination path (parent directory must exist).
     * @return true on success.
     * @sideeffects Writes to the filesystem. Emits error() on failure.
     */
    Q_INVOKABLE bool copyFile(const QString& sourcePath, const QString& destPath);

  signals:
    /** @brief Emitted when a file is successfully saved via saveGeneratedFile(). */
    void fileSaved(const QString& path);

    /**
     * @brief Emitted when a project bundle is successfully created.
     * @param path Absolute path of the new bundle archive.
     */
    void bundleCreated(const QString& path);

    /**
     * @brief Emitted on I/O or path-safety errors.
     * @param message Human-readable error description.
     */
    void error(const QString& message);

  private:
    QString m_attachmentDir;  ///< AppDataLocation/attachments/
    QString m_projectDir;     ///< AppDataLocation/projects/
    QString m_appDataDir;     ///< AppDataLocation (root for path safety checks)
    QString m_activeConvDir;  ///< Per-conversation subdir, or empty for default

    /// Non-owning host-side mount registry. Null until AppController
    /// wires it; null disables the mount-aware overloads (they fall
    /// back to today's local I/O). Set on the main thread; read from
    /// worker threads via the cross-thread invoke helpers below.
    FolderMountRegistry* m_mountRegistry = nullptr;

    /** Machine-readable kind of the most recent
     *  mount-path failure on THIS worker call chain
     *  (bridge_offline / mount_offline / unsafe_path /
     *  caller_client_not_mounted / mount_namespace_unknown / …).
     *  Cleared at the top of each mount-aware op. NOT thread-safe
     *  across concurrent callers; the file tools invoke + read
     *  sequentially on one worker, which is the supported pattern. */
    QString m_lastErrorKind;

    /// Non-owning wire-backed mount client. Null until AppController
    /// wires it (after WireHostBridge construction).
    IFolderMountClient* m_mountClient = nullptr;
    FolderMountClientWire* m_mountCmdClient = nullptr;

    /**
     * @brief Sanitises a filename so it contains only safe characters.
     *        Strips path separators, null bytes, and leading dots.
     * @param name  Raw filename.
     * @return Safe filename; falls back to "file.txt" if result is empty.
     */
    static QString sanitiseFilename(const QString& name);
};

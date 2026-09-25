// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file canvas-service.h
 * @brief Top-level service that owns the canvas-artifacts table and the
 *        per-conversation active-canvas state. Registered as the
 *        `Canvas` QML singleton. Sibling of MessageService /
 *        ToolService / TaskGateService; canvas lives OUTSIDE the chat-
 *        session pipeline so it is not a Chat:: collaborator.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Sql, DbManager (non-owning reference);
 *               FileService and ConversationService attach via setters
 *               (used for the disk mirror and folder-chain lookups).
 *
 * Threading: strictly main-thread. Every public method begins with
 *   VERZETA_ASSERT_MAIN_THREAD();
 * SQLite via DbManager is main-thread per project convention.
 *
 * Ownership: AppController owns the single instance via
 *   std::unique_ptr<CanvasService> m_canvasService;
 * declared AFTER the services it depends on (ConversationService,
 * MessageService, FileService) so reverse-declaration destruction
 * keeps those alive through CanvasService's full lifetime.
 */


#pragma once

#include <functional>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class ConversationService;
class DbManager;
class FileService;

/**
 * @brief Canvas-feature top-level service. Q_INVOKABLE surface is the
 *        QML-facing API; signals fan out to ChatController's metadata
 *        cache and to the QML canvas panel for re-render.
 */
class CanvasService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the canvas service.
     * @param db     Non-owning reference; must outlive this service.
     *               AppController owns DbManager via the singleton
     *               pattern, so the lifetime invariant holds for the
     *               whole app run.
     * @param parent Qt parent (AppController).
     */
    explicit CanvasService(DbManager& db, QObject* parent = nullptr);
    ~CanvasService() override;

    // -----------------------------------------------------------------
    // Service attachments — wired by AppController during initialize().
    // Both deferred (set after construction) because their concrete
    // services are constructed AFTER CanvasService in the dependency
    // chain.
    // -----------------------------------------------------------------

    /**
     * @brief Attach the FileService. Used to write the disk mirror
     *        (disk-backed mode) and to subscribe to `FileService::
     *        fileSaved` for the auto-promote path when an agent calls
     *        `write_file`.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setFileService(FileService* svc);

    /**
     * @brief Attach the ConversationService. Used for folder-chain
     *        lookups when resolving the artifact directory for the
     *        disk mirror.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setConversationService(ConversationService* svc);

    /**
     * @brief Install a getter for the UI-active conversation id. Used
     *        by the auto-promote path: when `FileService::fileSaved`
     *        fires, the file is promoted into the currently-active
     *        conversation's canvas (the agent has no other "where did
     *        this fileSave happen" anchor). AppController wires this
     *        after ChatController exists.
     * @param getter Callable returning the active conv id (or empty
     *               string when no conversation is active).
     */
    void setActiveConversationIdGetter(std::function<QString()> getter);

    // -----------------------------------------------------------------
    // Auto-promote
    // -----------------------------------------------------------------

    /**
     * @brief Promote a freshly-written file to canvas iff it meets the
     *        eligibility rules: filename has an extension on the canvas
     *        allowlist, such as .md, .json, .py, .ts, .cpp and .sh;
     *        content is at least 500 chars; conversationId is non-empty.
     *        Callers are the `FileService::fileSaved` subscription
     *        (for write_file) and `ChatController::onTaskArtifactReady`
     *        (for submit_result).
     * @param conversationId Target conversation that should own the
     *                       promoted canvas.
     * @param filename       Source filename (with extension).
     * @param content        Full file content to promote.
     * @returns The new canvas id when promoted, empty when skipped
     *          (ineligible by any rule above).
     */
    Q_INVOKABLE QString tryAutoPromote(const QString& conversationId,
                                       const QString& filename,
                                       const QString& content);

    // -----------------------------------------------------------------
    // Q_INVOKABLE — read paths
    // -----------------------------------------------------------------

    /**
     * @brief Return the conversation's active (non-archived) canvas.
     *
     *        Lookup rule: `is_archived=0` row with the highest
     *        `updated_at`. Each conversation has at most one such row.
     *        The schema does not enforce this; the open/edit/close
     *        write paths maintain the invariant by archiving any
     *        prior active row before inserting a new one.
     * @param conversationId Conversation to inspect.
     * @returns QVariantMap with keys {id, filename, language, content,
     *          revision, lineCount, byteSize, createdAt, updatedAt},
     *          or an empty map if no canvas is active.
     */
    Q_INVOKABLE QVariantMap activeCanvasFor(const QString& conversationId) const;

    /**
     * @brief Return every canvas row for a conversation (active +
     *        archived) in reverse-`updated_at` order. Drives the
     *        canvas-panel header history dropdown.
     * @param conversationId Conversation to enumerate.
     * @returns QVariantList of QVariantMap entries with the same shape as
     *          activeCanvasFor's return value plus an `isArchived`
     *          bool per entry. Empty list when the conversation has
     *          no canvas history.
     */
    Q_INVOKABLE QVariantList historyForConversation(const QString& conversationId) const;

    // -----------------------------------------------------------------
    // Q_INVOKABLE — write paths
    // -----------------------------------------------------------------

    /**
     * @brief Open a new canvas (or update an existing one with the
     *        same filename) in the conversation. If a row with this
     *        filename exists: un-archive it (if archived), archive any
     *        prior active, treat content as a fresh revision++ when
     *        different. Else: archive any current active row, INSERT a
     *        new row with revision=0, is_archived=0. Writes the disk
     *        mirror via FileService and emits `canvasOpened`.
     * @param conversationId Owning conversation.
     * @param filename       Display filename (extension determines
     *                       language when `language` is empty).
     * @param language       Syntax-highlighting language id (may be
     *                       empty; then derived from filename).
     * @param content        Initial canvas content.
     * @param sourceMsgId    Optional id of the assistant message that
     *                       requested the open. Recorded for audit.
     * @returns UUID of the active canvas row, or empty on failure.
     */
    Q_INVOKABLE QString openCanvas(const QString& conversationId,
                                   const QString& filename,
                                   const QString& language,
                                   const QString& content,
                                   const QString& sourceMsgId = {});

    /**
     * @brief Replace the active canvas's content. Increments revision,
     *        refreshes `updated_at`, rewrites the disk mirror, and
     *        emits `canvasUpdated`.
     * @param conversationId Owner of the active canvas to edit.
     * @param newContent     Full replacement content.
     * @returns true on success; false when no active canvas exists or
     *          the DB write failed.
     */
    Q_INVOKABLE bool editCanvas(const QString& conversationId, const QString& newContent);

    /**
     * @brief Read a 1-based line range from the active canvas. Used by
     *        the `read_canvas` tool. `start_line<=0` or beyond the file
     *        returns empty; `end_line == -1` reads to EOF; `end_line <
     *        start_line` returns empty.
     * @param conversationId Owner of the active canvas to read.
     * @param startLine      1-based inclusive start line.
     * @param endLine        1-based inclusive end line, or -1 for EOF.
     * @returns Sliced content as a single string, or empty when the
     *          range is empty / out of bounds / no canvas is active.
     */
    Q_INVOKABLE QString readCanvasSlice(const QString& conversationId,
                                        int startLine,
                                        int endLine) const;

    /**
     * @brief X-close: archive the active canvas (is_archived=1). The
     *        disk file is NOT deleted. Emits `canvasClosed`.
     * @param conversationId Owner of the active canvas to archive.
     * @returns true if a canvas was archived; false if none was active.
     */
    Q_INVOKABLE bool closeCanvas(const QString& conversationId);

    /**
     * @brief Promote an archived canvas back to active (and archive
     *        whatever was active). Used by the history dropdown.
     *        Emits `canvasOpened`.
     * @param conversationId Owner of the canvas to re-activate.
     * @param canvasId       Id of the archived canvas row.
     * @returns true on success; false when the canvas id is not found
     *          for that conversation.
     */
    Q_INVOKABLE bool switchToCanvas(const QString& conversationId, const QString& canvasId);

    /**
     * @brief Open a text file from disk as the conversation's canvas.
     *        User-initiated (the Artifacts overlay's "Open in Canvas"
     *        button), with no auto-promote eligibility rules, only basic
     *        safety: the file must exist, be a regular file, decode as
     *        text (no NUL bytes) and stay under a 10 MB bound.
     *        Delegates to openCanvas() with the file's basename and the
     *        extension-derived language ("plaintext" when the extension
     *        is unmapped), so same-filename upsert / revision++
     *        semantics apply unchanged: re-opening a file replaces the
     *        canvas content with the file's current text as a fresh
     *        revision.
     * @param conversationId Conversation that should own the canvas.
     * @param absolutePath   Absolute local path of the file to open.
     * @returns UUID of the active canvas row, or empty when the path is
     *          not a readable text file or the open failed.
     */
    Q_INVOKABLE QString openCanvasFromFile(const QString& conversationId,
                                           const QString& absolutePath);

    /**
     * @brief Open an EXISTING workspace file into the canvas by name,
     *        reading its content from disk.
     *
     *        Resolves the (relative, subdir-preserving) filename against
     *        the conversation's project workspace directory and, when a
     *        readable local text file exists there, opens it via
     *        openCanvasFromFile. This is the "open the file, then edit it"
     *        path for the open_canvas tool when no content is supplied.
     *        Only LOCAL files are opened. A mount-hosted file has no
     *        local copy, so this returns an empty id and the caller asks
     *        the agent to read_file then re-open WITH content, so no
     *        blocking wire read runs on the main thread.
     * @param conversationId Conversation that should own the canvas.
     * @param filename       Workspace-relative file name to open.
     * @returns UUID of the active canvas row, or empty when the name
     *          escapes the workspace or no readable local file exists.
     */
    Q_INVOKABLE QString openCanvasFromWorkspace(const QString& conversationId,
                                                const QString& filename);

    /**
     * @brief Switch the active canvas to `filename` and replace its content
     *        in one atomic step. This is the "edit this named file" path for
     *        edit_canvas when the requested file is not the open one.
     *
     *        If a canvas for `filename` already exists it is re-activated
     *        (its language reused); otherwise, if a workspace file of that
     *        name exists it is adopted (language inferred from the
     *        extension). The content is then replaced with `newContent`
     *        (edit_canvas is a full-body replace). A name that exists as
     *        neither a canvas nor a workspace file is NOT created here;
     *        the empty return tells the caller to create it via write_file
     *        or open_canvas instead.
     * @param conversationId Conversation that owns the canvas.
     * @param filename       File to switch to and edit.
     * @param newContent     Full replacement body.
     * @returns The active canvas id, or empty when no canvas or workspace
     *          file of that name exists.
     */
    Q_INVOKABLE QString retargetAndEdit(const QString& conversationId,
                                        const QString& filename,
                                        const QString& newContent);

    // -----------------------------------------------------------------
    // Q_INVOKABLE — Export
    // -----------------------------------------------------------------

    /**
     * @brief Write the active canvas's content to a user-chosen file
     *        path (typically picked via a QML FileDialog).
     *
     *        The export is a user-driven copy that does NOT modify the
     *        canvas itself or the project disk-mirror; it just
     *        snapshots the current content to the destination. Atomic
     *        via QSaveFile: either the full content lands or nothing
     *        changes.
     * @param conversationId Active canvas owner.
     * @param absolutePath   Destination on the local filesystem. The
     *                       QML FileDialog returns a `file://` URL; the
     *                       caller is expected to convert via
     *                       `url.toLocalFile()` before invoking. Empty
     *                       paths return false.
     * @returns true on success; false on missing canvas / missing path
     *          / atomic-write failure. Failures emit `errorOccurred`.
     */
    Q_INVOKABLE bool exportCanvasToFile(const QString& conversationId, const QString& absolutePath);

    /**
     * @brief Convenience for QML FileDialog defaults: the active
     *        canvas filename (e.g. `"main.py"`) so the dialog can
     *        pre-fill its name field.
     * @param conversationId Active canvas owner.
     * @returns The filename, or empty when no canvas is active.
     */
    Q_INVOKABLE QString suggestedExportName(const QString& conversationId) const;

    /**
     * @brief Absolute path of the active canvas's disk-mirror file.
     *        Resolution mirrors `writeDiskMirror`'s folder-chain walk
     *        (project artifact dir if the conversation lives inside a
     *        project / org folder; per-conversation artifact
     *        subdirectory otherwise), but does NOT write anything.
     *        Used by CanvasRunner::sendToIde to find the file to hand
     *        off to the OS's default editor.
     * @param conversationId Active canvas owner.
     * @returns The absolute path, or empty when no canvas is active /
     *          FileService is not attached / the disk-mirror file does
     *          not exist on disk.
     */
    Q_INVOKABLE QString diskMirrorPath(const QString& conversationId) const;

    // -----------------------------------------------------------------
    // Q_INVOKABLE — Tools popover
    //
    // The Tools popover on the canvas header lets the user run
    // language-aware in-process actions (Validate JSON, Format JSON,
    // …). v1 ships a tight set; future plans may expand it.
    // -----------------------------------------------------------------

    /**
     * @brief Return the action descriptors available for a given
     *        canvas language. The QML popover Repeater binds to this
     *        list. Disabled entries render greyed-out so the user can
     *        see what's possible per-language without us hiding the
     *        menu items entirely.
     * @param language Canvas language id (e.g. `"json"`, `"python"`).
     * @returns QVariantList of QVariantMap with keys {id, label,
     *          description, enabled, language}.
     */
    Q_INVOKABLE QVariantList availableActionsForLanguage(const QString& language) const;

    /**
     * @brief Run a Tools action on the conversation's active canvas.
     *        Mutating actions (Format) write the new content via the
     *        same path `editCanvas` uses (DB row + disk mirror +
     *        `canvasUpdated` signal). Validation actions (Validate) do
     *        NOT mutate; they return diagnostic info in the result
     *        map.
     * @param conversationId Canvas owner.
     * @param actionId       One of the ids returned by
     *                       `availableActionsForLanguage`; unrecognised
     *                       ids return {ok:false, message:"unknown
     *                       action"}.
     * @returns QVariantMap with keys: ok (bool, action succeeded),
     *          message (string, human-readable result), mutated (bool,
     *          canvas content was changed). Validation may include
     *          extra keys (errorLine, errorOffset).
     */
    Q_INVOKABLE QVariantMap performAction(const QString& conversationId, const QString& actionId);

  signals:
    /**
     * @brief Emitted after `openCanvas` / `switchToCanvas` establishes
     *        a new active canvas. ChatController subscribes to refresh
     *        its metadata cache for the next prompt build; QML
     *        CanvasPanel subscribes to render.
     * @param conversationId Owner of the newly-active canvas.
     * @param canvasId       Id of the now-active canvas row.
     */
    void canvasOpened(const QString& conversationId, const QString& canvasId);

    /**
     * @brief Emitted after `editCanvas` writes a new revision. Same
     *        subscribers as `canvasOpened`: QML re-renders,
     *        ChatController bumps its metadata revision.
     * @param conversationId Owner of the edited canvas.
     * @param canvasId       Id of the edited canvas row.
     * @param revision       New (post-edit) revision number.
     */
    void canvasUpdated(const QString& conversationId, const QString& canvasId, int revision);

    /**
     * @brief Emitted after `closeCanvas` archives the active canvas.
     *        QML hides the panel + the toggle button. ChatController
     *        clears its metadata cache.
     * @param conversationId Owner of the just-archived canvas.
     * @param canvasId       Id of the archived canvas row.
     */
    void canvasClosed(const QString& conversationId, const QString& canvasId);

    /**
     * @brief User-visible error channel. Surfaces DB write failures,
     *        disk write failures, and similar non-fatal issues. QML
     *        binds this alongside other singletons' `errorOccurred`
     *        on the shared error banner.
     * @param message Human-readable error description.
     */
    void errorOccurred(const QString& message);

  private:
    DbManager& m_db;  // non-owning
    FileService* m_fileSvc = nullptr;
    ConversationService* m_convSvc = nullptr;
    std::function<QString()> m_activeConvIdGetter;
    /** Re-entry guard for the FileService::fileSaved subscription.
     *  Auto-promote subscribes to fileSaved + reads back the file +
     *  calls openCanvas → writeDiskMirror → saveGeneratedFile, which
     *  fires fileSaved again. Without the guard the loop is infinite.
     *  Set true around our own writeDiskMirror calls. */
    bool m_inWriteMirror = false;

    // Resolve the on-disk artifact directory for a given conversation
    // (project-scoped if the conversation lives inside a project
    // folder, per-conversation otherwise) and write the canvas content
    // as a UTF-8 text file. Returns the absolute path of the written
    // file, or empty on failure (logged). Mirrors the pre-existing
    // pattern in `ChatController::activeArtifactsPath` so the file
    // ends up in the same artifact directory the user already sees
    // from `openActiveArtifactsFolder` and the existing
    // `read_file` / `write_file` tools.
    QString writeDiskMirror(const QString& conversationId,
                            const QString& filename,
                            const QString& content) const;

    /**
     * @brief Resolve the on-disk workspace directory for a conversation
     *        via its folder chain (project/org artifact dir when inside a
     *        project folder, per-conversation artifact subdirectory
     *        otherwise). This is the single source shared by writeDiskMirror and
     *        openCanvasFromWorkspace. Sets FileService's active context as
     *        a side effect (same as the pre-existing walk it replaces).
     * @param conversationId Conversation whose workspace to resolve.
     * @returns The absolute workspace directory, or empty when
     *          FileService is not attached.
     */
    QString workspaceDirForConversation(const QString& conversationId) const;
};

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file i-folder-mount-client.h
 * @brief Pure abstract interface for the host-side virtual
 *        filesystem indirection. The FileService router (a future
 *        consumer) hands all mount-bound reads / writes / stats /
 *        listings to whichever implementation `AppController`
 *        installs at construction time. The default implementation
 *        is wire-backed (`FolderMountClientWire`); tests inject
 *        stub implementations that record calls and return scripted
 *        payloads without any real IPC.
 *
 *        Every method is SYNCHRONOUS from the caller's perspective:
 *        the calling thread blocks until the reply arrives or the
 *        per-request timeout elapses. Implementations marshal to
 *        the bridge thread internally so the caller (a tool worker
 *        thread) does not need any cross-thread awareness.
 *
 * @layer Service
 * @dependencies Qt6::Core (QByteArray, QString, QJsonObject,
 *               QJsonArray).
 */

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

/**
 * @brief Abstract interface implemented by host-side virtual
 *        filesystem clients. All four methods block the calling
 *        thread until the reply arrives or the request times out.
 *
 * Ownership: callers hold a non-owning `IFolderMountClient*` (or a
 * `std::shared_ptr<IFolderMountClient>` when sharing across threads).
 * Implementations are owned by `AppController` for the real
 * wire-backed impl, or by test fixtures for stubs.
 *
 * Threading: every method is safe to call from worker threads.
 * Implementations marshal to the bridge / IPC layer internally and
 * block the caller until the result is available. The interface
 * itself imposes no thread affinity.
 *
 * Error reporting: every method takes a `QString* outError` that the
 * implementation populates with a machine-readable error kind (e.g.
 * `"client_not_found"`, `"bridge_offline"`, `"timeout"`,
 * `"mount_offline"`, `"stale_fingerprint"`, `"blocked_path"`,
 * `"not_in_workspace"`, `"client_error"`) when the call fails. The
 * primary return value is empty / zero / null on failure. Callers
 * MUST check `*outError` to distinguish a legitimate empty payload
 * from a failure.
 */
class IFolderMountClient {
  public:
    /**
     * @brief Virtual destructor, required for the abstract base.
     */
    virtual ~IFolderMountClient() = default;

    /**
     * @brief Reads `relPath` from the mount owned by `folderId` /
     *        `mountId`. Blocks the caller until the reply arrives or
     *        the request times out.
     *
     * @param folderId         Folder UUID the mount is bound to.
     * @param mountId          Stable workspace UUID provided at
     *                         registration time.
     * @param relPath          Path inside the mount, relative to the
     *                         workspace root. Implementations
     *                         canonicalise + reject path traversal
     *                         before forwarding the request.
     * @param maxBytes         Hard cap on bytes returned (clamped by
     *                         the implementation against the
     *                         protocol-level per-read ceiling).
     * @param outFingerprint   Populated on success with the
     *                         content-SHA256 fingerprint the wire
     *                         client computed for staleness checks
     *                         on subsequent writes. Set to empty on
     *                         failure. May be nullptr if the caller
     *                         does not need it.
     * @param outError         Populated on failure with a machine-
     *                         readable error kind. Set to empty on
     *                         success. Must NOT be nullptr.
     * @returns On success, the bytes read from the mount; empty on
     *          failure (inspect `*outError` to distinguish empty
     *          file from error).
     */
    virtual QByteArray readBytes(const QString& folderId,
                                 const QString& mountId,
                                 const QString& relPath,
                                 qint64 maxBytes,
                                 QString* outFingerprint,
                                 QString* outError) = 0;

    /**
     * @brief Writes `content` to `relPath` in the mount owned by
     *        `folderId` / `mountId`. Blocks until the reply arrives
     *        or the request times out.
     *
     * @param folderId             Folder UUID.
     * @param mountId              Stable workspace UUID.
     * @param relPath              Relative path inside the mount.
     * @param content              Bytes to write.
     * @param expectedFingerprint  Optimistic-concurrency token: the
     *                             SHA-256 fingerprint the caller saw
     *                             on its prior read of the same path.
     *                             The wire client compares this to the
     *                             file's current fingerprint and
     *                             refuses the write with
     *                             `"stale_fingerprint"` on mismatch.
     *                             Empty disables the check.
     * @param outError             Populated on failure. Must not be
     *                             nullptr.
     * @returns On success, the number of bytes the wire client
     *          actually wrote (may match `content.size()` exactly, or
     *          be less if the wire client truncated for protocol
     *          reasons). Zero on failure.
     */
    virtual qint64 writeBytes(const QString& folderId,
                              const QString& mountId,
                              const QString& relPath,
                              const QByteArray& content,
                              const QString& expectedFingerprint,
                              QString* outError) = 0;

    /**
     * @brief Stats `relPath` in the mount. Blocks until reply or
     *        timeout.
     *
     * @param folderId  Folder UUID.
     * @param mountId   Stable workspace UUID.
     * @param relPath   Relative path inside the mount.
     * @param outError  Populated on failure.
     * @returns JSON object with `{exists, kind, size, mtime_ms}` on
     *          success. Empty object on failure.
     */
    virtual QJsonObject statPath(const QString& folderId,
                                 const QString& mountId,
                                 const QString& relPath,
                                 QString* outError) = 0;

    /**
     * @brief Lists entries under `relPath` inside the mount. Blocks
     *        until reply or timeout.
     *
     * @param folderId   Folder UUID.
     * @param mountId    Stable workspace UUID.
     * @param relPath    Relative directory inside the mount; empty
     *                   targets the workspace root.
     * @param recursive  When true the wire client descends; otherwise
     *                   one level only.
     * @param outError   Populated on failure.
     * @returns JSON array of entry objects each shaped
     *          `{path, kind, size, mtime_ms}`. Truncation by the wire
     *          client appears as a `truncated: true` marker on the
     *          last entry (interpreted by the caller). Empty array on
     *          failure.
     */
    virtual QJsonArray listDir(const QString& folderId,
                               const QString& mountId,
                               const QString& relPath,
                               bool recursive,
                               QString* outError) = 0;
};

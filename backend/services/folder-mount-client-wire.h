// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file folder-mount-client-wire.h
 * @brief Wire-backed concrete implementation of
 *        `IFolderMountClient`. Routes every read / write / stat /
 *        list call through `WireHostBridge::dispatchClientRequest`
 *        (the bridge IPC transport) to the paired client
 *        whose `client_id` owns the target mount.
 *
 *        Synchronous from the caller's perspective: each method
 *        blocks the calling thread on a `QSemaphore` until the
 *        bridge's reply callback fires or the safety-margin
 *        wait expires. The shared wait-state is heap-allocated
 *        via `std::shared_ptr` so a callback that fires AFTER
 *        the caller has returned (e.g. unbounded timeout edge
 *        case) cannot dangle.
 *
 * @layer Service
 * @dependencies Qt6::Core, IFolderMountClient, WireHostBridge,
 *               FolderMountRegistry.
 */


#pragma once

#include "i-folder-mount-client.h"

#include <QObject>
#include <QPointer>

class FolderMountRegistry;

namespace Verzeta::Remote {
class WireHostBridge;
}

/**
 * @brief Wire-backed `IFolderMountClient`. Holds a non-owning
 *        `QPointer<WireHostBridge>` (bridge lifetime is
 *        `AppController`) plus a non-owning `FolderMountRegistry*`
 *        (registry lifetime is `AppController`, constructed before
 *        this service). Both pointers may go null at shutdown; every
 *        method checks before use and returns a clean error.
 *
 * Ownership: AppController owns one instance via `std::shared_ptr`
 * so FileService and any future thread-safe consumer can share
 * the pointer.
 *
 * Threading: every method is safe to call from any worker thread.
 * Registry reads cross to main via Qt::BlockingQueuedConnection
 * (one-way; the worker waits for main; main never waits for the
 * worker, so no deadlock is possible). Bridge dispatch uses
 * `WireHostBridge::dispatchClientRequest` which is itself any-
 * thread safe (queues internally onto the bridge worker thread).
 */
class FolderMountClientWire : public IFolderMountClient {
  public:
    /**
     * @brief Constructs the wire client with non-owning references
     *        to the bridge and the registry.
     * @param bridge    Pointer to the host's `WireHostBridge`. May
     *                  be null at construction time (e.g. test stubs
     *                  that never reach the IPC layer). Methods return
     *                  `"bridge_offline"` when the pointer is null
     *                  at call time.
     * @param registry  Non-owning pointer to the
     *                  `FolderMountRegistry`. Must outlive this
     *                  object. May not be null.
     */
    FolderMountClientWire(QPointer<Verzeta::Remote::WireHostBridge> bridge,
                          FolderMountRegistry* registry);
    ~FolderMountClientWire() override = default;

    /**
     * @brief Reads a file's bytes from the paired client over the
     *        wire (`vfs.read` semantics; see i-folder-mount-client.h
     *        for the interface contract).
     * @param folderId        Folder whose mount serves the read.
     * @param mountId         Target mount id within the folder.
     * @param relPath         Workspace-relative file path.
     * @param maxBytes        Read cap forwarded to the client.
     * @param outFingerprint  Optional out-param receiving the file's
     *                        content fingerprint for later
     *                        optimistic-concurrency writes.
     * @param outError        Optional out-param receiving the error
     *                        kind on failure (e.g. "bridge_offline").
     * @returns File bytes on success; empty on failure.
     */
    QByteArray readBytes(const QString& folderId,
                         const QString& mountId,
                         const QString& relPath,
                         qint64 maxBytes,
                         QString* outFingerprint,
                         QString* outError) override;

    /**
     * @brief Writes bytes to the paired client's workspace over the
     *        wire (`vfs.write` semantics).
     * @param folderId             Folder whose mount serves the write.
     * @param mountId              Target mount id within the folder.
     * @param relPath              Workspace-relative file path.
     * @param content              Bytes to write.
     * @param expectedFingerprint  Optimistic-concurrency token from a
     *                             prior read; empty disables the
     *                             staleness check.
     * @param outError             Optional out-param receiving the
     *                             error kind on failure.
     * @returns Number of bytes written, or -1 on failure.
     */
    qint64 writeBytes(const QString& folderId,
                      const QString& mountId,
                      const QString& relPath,
                      const QByteArray& content,
                      const QString& expectedFingerprint,
                      QString* outError) override;

    /**
     * @brief Stats a path on the paired client (`vfs.stat` semantics).
     * @param folderId  Folder whose mount serves the stat.
     * @param mountId   Target mount id within the folder.
     * @param relPath   Workspace-relative path.
     * @param outError  Optional out-param receiving the error kind on
     *                  failure.
     * @returns Stat object (exists/size/mtime fields); empty object
     *          on failure.
     */
    QJsonObject statPath(const QString& folderId,
                         const QString& mountId,
                         const QString& relPath,
                         QString* outError) override;

    /**
     * @brief Lists a directory on the paired client (`vfs.list`
     *        semantics).
     * @param folderId   Folder whose mount serves the list.
     * @param mountId    Target mount id within the folder.
     * @param relPath    Workspace-relative directory path; empty means
     *                   the workspace root.
     * @param recursive  Whether the client should descend into
     *                   subdirectories.
     * @param outError   Optional out-param receiving the error kind on
     *                   failure.
     * @returns Array of entry objects; empty array on failure.
     */
    QJsonArray listDir(const QString& folderId,
                       const QString& mountId,
                       const QString& relPath,
                       bool recursive,
                       QString* outError) override;

    /**
     * @brief Runs a shell command on the paired client's workspace
     *        (`vfs.execute` semantics). CONCRETE-ONLY, and
     *        deliberately NOT on `IFolderMountClient`: command execution
     *        is orthogonal to the file-mount surface, and adding it to
     *        the interface would force every mock to grow a method it
     *        never uses. `FileService::executeRemoteCommand` reaches
     *        this concrete client directly.
     *
     *        Blocks the calling (worker) thread on the same wire round-
     *        trip the file ops use, but with a command-scale timeout
     *        rather than the 15 s file default, because builds/tests legitimately
     *        run long. The client owns all execution policy
     *        (Off/Ask/Allow, SmartFilter, sandbox); this method only
     *        carries the request and surfaces the structured reply.
     *
     * @param folderId        Folder whose mount serves the execution.
     * @param mountId         Target mount id within the folder.
     * @param conversationId  Calling conversation id, forwarded to the
     *                        client so it can apply per-conversation
     *                        execution policy. May be empty.
     * @param command         Shell command line to run in the workspace.
     * @param timeoutMs       Per-command deadline; clamped to
     *                        [1, kExecMaxTimeoutMs]. The wire wait
     *                        extends it by kSweepGraceMs.
     * @param maxOutputBytes  Output cap; the client truncates + flags it.
     * @param outError        Receives the error kind on failure
     *                        (`exec_disabled`, `user_rejected`,
     *                        `blocked_by_smart_filter`, `exec_unsupported`,
     *                        `mount_offline`, `bridge_offline`, …). Must
     *                        NOT be nullptr.
     * @returns `{stdout, stderr, exit_code, sandboxed, timed_out,
     *          truncated}` on success; empty object on failure (inspect
     *          `*outError`).
     */
    QJsonObject executeCommand(const QString& folderId,
                               const QString& mountId,
                               const QString& conversationId,
                               const QString& command,
                               int timeoutMs,
                               qint64 maxOutputBytes,
                               QString* outError);

    /** @brief Default command timeout (longer than the 15 s file ops). */
    static constexpr int kExecDefaultTimeoutMs = 120000;  // 2 min
    /** @brief Hard ceiling so a hung command cannot pin a worker thread. */
    static constexpr int kExecMaxTimeoutMs = 600000;  // 10 min

    /**
     * @brief Per-request deadline forwarded to the bridge dispatcher
     *        (and from there to the wire `request` envelope). Matches
     *        the protocol-level default of 15 s.
     */
    static constexpr int kDefaultTimeoutMs = 15000;

    /**
     * @brief Additional grace beyond the request deadline that the
     *        caller's semaphore-wait allows before declaring an
     *        internal error. Covers timeout-sweep jitter
     *        (500 ms cadence) plus reply-marshal latency. Should
     *        always exceed `kBridgeRpcSweepIntervalMs` from
     *        WireHostBridge by a comfortable margin.
     */
    static constexpr int kSweepGraceMs = 5000;

  private:
    /**
     * @brief Resolves the wire `client_id` that owns the mount
     *        registered for `folderId`. Reads the registry under
     *        single-writer main-thread semantics, marshalling from
     *        worker via `Qt::BlockingQueuedConnection` when needed.
     * @param folderId  Folder UUID.
     * @returns Owner client UUID on hit; empty string on miss.
     */
    QString resolveClientId(const QString& folderId);

    QPointer<Verzeta::Remote::WireHostBridge> m_bridge;
    FolderMountRegistry* m_registry = nullptr;
};

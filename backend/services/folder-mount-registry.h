// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file folder-mount-registry.h
 * @brief Metadata-only registry for client-owned virtual
 *        workspace mounts bound to a given folder. Persists per-
 *        folder mount rows in the `folder_mounts` table, exposes a
 *        Q_INVOKABLE surface for wire-driven CRUD, and signals
 *        lifecycle events so the host bridge can forward them and
 *        AuditService can record them.
 *
 *        Stores ONLY mount metadata (client id, mount id, owner
 *        label, registered_at, last_seen, tree manifest JSON, per-
 *        mount blocklist + allowlist additions, permission tier).
 *        Bytes of workspace files are NEVER persisted here; the
 *        wire layer fetches them on demand from the registered
 *        client at the moment a tool needs them.
 *
 * @layer Service
 * @dependencies DbManager (Data Access), Qt6::Core, Qt6::Sql,
 *               AuditService (optional, for lifecycle audit).
 */


#pragma once

#include <QTimer>

#include <optional>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

class AuditService;
class DbManager;

/**
 * @brief Plain-old-data view of one row in the `folder_mounts` table.
 * Returned by C++ accessors so the FileService router can
 *        resolve mount-owned paths without a QVariantMap marshalling
 *        cost.
 */
struct FolderMount {
    /** Folder this mount is bound to. PRIMARY KEY in storage. */
    QString folderId;
    /** Stable UUID supplied by the registering client; survives
     *  reconnect so a re-register with the same id refreshes in
     *  place rather than emitting `mountReplaced`. */
    QString mountId;
    /** Wire-auth client UUID of the workspace owner. The only
     *  identity authorised to unregister / update this mount. */
    QString clientId;
    /** Human-readable display string for status surfaces ("My
     *  Project on VS Code (laptop)"). */
    QString ownerLabel;
    /** Epoch ms at which `registerMount` first persisted this row.
     *  Re-registers do not reset this. */
    qint64 registeredAtMs = 0;
    /** Epoch ms bumped by the file I/O router on every
     *  successful op. Stale-detection threshold uses this. */
    qint64 lastSeenMs = 0;
    /** Flat tree manifest as JSON. Capability hint only; bytes
     *  flow on demand at agent file-op time, never from this
     *  field. */
    QString treeJson;
    /** Per-mount blocklist additions (JSON array of globs) on top
     *  of the host's default blocklist. Defaults cannot be
     *  removed; this list is append-only. */
    QString blocklistJson;
    /** Per-mount allowlist (JSON array of globs). When non-empty,
     *  ONLY paths matching at least one entry may be accessed. */
    QString allowlistJson;
    /** Permission tier: `ask` (default), `smart`, or `bypass`.
     *  Advisory metadata; client owns the authoritative
     *  confirmation modal. */
    QString permissionTier;
    /** Forward-compat options blob. Unused in V1. */
    QString optionsJson;
};

/**
 * @brief Single-writer registry for client-owned virtual workspace
 *        mounts. Construction populates an in-memory cache from
 *        SQLite so reads are O(1). Every write path goes through
 *        SQLite first, then updates the cache, then emits a
 *        lifecycle signal so the host bridge can forward + the
 *        AuditService can record.
 *
 * Ownership: AppController owns one instance via
 * `std::unique_ptr<FolderMountRegistry>`. Lifetime is tied to the
 * host process; persistence across host restart is via the
 * `folder_mounts` SQLite table.
 *
 * Thread residency: main-thread only. Cross-thread callers MUST
 * marshal via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
 */
class FolderMountRegistry : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct against an open DbManager and populate the
     *        in-memory cache from existing `folder_mounts` rows.
     * @param db     Engine-layer ref; must outlive this service.
     * @param parent Optional Qt parent (typically AppController).
     */
    explicit FolderMountRegistry(DbManager& db, QObject* parent = nullptr);
    ~FolderMountRegistry() override;

    /**
     * @brief Attach an AuditService so lifecycle events
     *        (registered / unregistered / replaced / tree_updated /
     *        tier_changed) get persisted to `activity_log`. Non-
     *        owning; AppController wires this once after both
     *        services are constructed.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setAuditService(AuditService* svc);

    // -----------------------------------------------------------------
    // Q_INVOKABLE wire surface — the wire layer dispatches every
    // `workspace.mount.*` op through these. Each returns a
    // `{ok: bool, error: string}` map so the wire envelope can pick
    // between sendOk(data) and sendErr(kind, detail) cleanly.
    // -----------------------------------------------------------------

    /**
     * @brief Register a new mount, replace an existing mount on the
     *        same folder, or refresh in place when the same client
     *        re-registers the same `mountId`. Last-register-wins
     *        semantics.
     *
     *        Validation:
     *          - `folderId`, `mountId`, `clientId`, `ownerLabel` are
     *            non-empty; tier is `ask` / `smart` / `bypass`.
     *          - The folder must exist in the `folders` table
     *            (referential integrity; CASCADE handles deletion).
     *
     *        Emits exactly one of:
     *          - `mountRegistered` when the folder had no prior mount.
     *          - `mountReplaced` when a different `mountId` /
     *            `clientId` displaces an existing entry.
     *          - `mountTreeUpdated` when the same client re-registers
     *            the same `mountId` (treated as a refresh, only the
     *            mutable fields are updated; `registeredAtMs` is
     *            preserved).
     *
     * @param folderId         Target folder UUID.
     * @param mountId          Stable client-side workspace UUID.
     * @param clientId         Wire-auth client UUID of the workspace owner.
     * @param ownerLabel       Display string.
     * @param treeJson         Flat manifest (see the manifest format docs).
     * @param blocklistJson    JSON array of glob strings.
     * @param allowlistJson    JSON array of glob strings.
     * @param permissionTier   `ask` / `smart` / `bypass`.
     * @param optionsJson      Forward-compat options blob.
     * @returns `{ok:true}` on success, `{ok:false, error:<message>}`
     *          on validation failure or SQL error.
     */
    Q_INVOKABLE QVariantMap registerMount(const QString& folderId,
                                          const QString& mountId,
                                          const QString& clientId,
                                          const QString& ownerLabel,
                                          const QString& treeJson,
                                          const QString& blocklistJson,
                                          const QString& allowlistJson,
                                          const QString& permissionTier,
                                          const QString& optionsJson);

    /**
     * @brief Remove a mount registration. The caller MUST be the
     *        registered owner; foreign `clientId` is rejected with
     *        `{ok:false, error:"forbidden"}` to prevent one client
     *        ejecting another client's mount.
     * @param folderId Target folder UUID.
     * @param clientId Caller's wire-auth client UUID.
     * @returns `{ok:true}` on success, `{ok:false, error:<message>}`
     *          on validation / authorisation / SQL failure.
     */
    Q_INVOKABLE QVariantMap unregisterMount(const QString& folderId, const QString& clientId);

    /**
     * @brief Replace the tree manifest for an existing mount. Used
     *        for the coalesced top-level-structural-change push from
     *        the client. Foreign clientId
     *        rejected.
     * @param folderId  Target folder UUID.
     * @param clientId  Caller's wire-auth client UUID.
     * @param treeJson  New manifest payload.
     * @returns `{ok:true}` on success, `{ok:false, error:<message>}`
     *          on validation / authorisation / SQL failure.
     */
    Q_INVOKABLE QVariantMap updateTree(const QString& folderId,
                                       const QString& clientId,
                                       const QString& treeJson);

    /**
     * @brief Change the advisory permission tier on an existing
     *        mount. Foreign clientId rejected.
     * @param folderId  Target folder UUID.
     * @param clientId  Caller's wire-auth client UUID.
     * @param newTier   `ask` / `smart` / `bypass`.
     * @returns `{ok:true}` on success, `{ok:false, error:<message>}`
     *          on validation / authorisation / SQL failure.
     */
    Q_INVOKABLE QVariantMap updateTier(const QString& folderId,
                                       const QString& clientId,
                                       const QString& newTier);

    /**
     * @brief Look up the mount for a folder and return its full row
     *        as a QVariantMap (camelCase keys, same projection
     *        convention as `WireDbReader::conversationSettings`).
     * @param folderId Target folder UUID.
     * @returns Mount row map on hit, empty map on miss.
     */
    Q_INVOKABLE QVariantMap mountFor(const QString& folderId) const;

    /**
     * @brief Path-safety decision for `relPath` inside the mount
     *        registered for `folderId`. Canonicalises the relative
     *        path (rejecting `..` escape, absolute-path leading `/`,
     *        and null bytes), applies the default blocklist, applies
     *        the per-mount blocklist additions, and applies the
     *        per-mount allowlist when
     *        non-empty.
     *
     * @param folderId Target folder UUID.
     * @param relPath  Relative path inside the mount, as requested
     *                 by the calling tool.
     * @returns Empty string on success (path passes every guard).
     *          On rejection, the machine-readable reason string:
     *          - `"unsafe_path"`: `..` escape or leading `/` or
     *            null byte.
     *          - `"absolute_path_forbidden"`: leading `/`.
     *          - `"mount_offline"`: no mount registered for the
     *            folder.
     *          - `"blocked_path"`: matches default or per-mount
     *            blocklist.
     *          - `"not_allowlisted"`: per-mount allowlist is set
     *            and the path matches none of its patterns.
     */
    Q_INVOKABLE QString pathIsAllowed(const QString& folderId, const QString& relPath) const;

    /**
     * @brief Reports whether the mount's stored tree manifest lists
     *        `relPath`. Used by the FileService router
     *        to reject reads of paths the registering client did
     *        not advertise.
     *
     * @param folderId Target folder UUID.
     * @param relPath  Relative path inside the mount.
     * @returns True iff a mount is registered for `folderId` AND its
     *          tree manifest's `files[]` array contains an entry
     *          whose `path` equals the canonical form of `relPath`.
     */
    /**
     * @brief Outcome of the mount routing algorithm.
     *        Empty clientId means "use the host's local path"
     *        (no active mounts, or explicit /local/ namespace pin).
     */
    struct MountRoutingDecision {
        QString folderId;  ///< Folder the decision was made for.
        QString clientId;  ///< Target client; empty = local fallback.
        QString mountId;   ///< Target mount id (empty when local).
        QString reason;    ///< For logging / structured tool errors.
        /**
         * @brief Whether the host's local path should serve this op.
         * @returns true when no client was selected.
         */
        bool useLocal() const { return clientId.isEmpty(); }
    };

    /**
     * @brief THE routing function, used by FileService
     *        for reads, writes and (per-mount) lists. Locked
     *        semantics, in order:
     *          1. callerClientId non-empty → route exclusively to
     *             that caller's own mount (caller-aware, decision
     *             #1); inactive / missing → reason
     *             "caller_client_not_mounted" with empty clientId
     *             (caller errors out; NO silent fallback to another
     *             client's workspace).
     *          2. Sticky-by-manifest (decision #2): active mounts
     *             whose manifest contains relPath own the file;
     *             most-recent lastSeenMs wins ties.
     *          3. New file (decision #3): most-recent ACTIVE mount.
     *          4. No active mounts: local ("no_active_mount").
     *        Activity = lastSeenMs within kStaleThresholdMs.
     * @param folderId        Folder whose mounts route.
     * @param callerClientId  Originating extension client; empty for
     *                        HOST/agent calls.
     * @param relPath         Canonicalised relative path.
     * @returns Decision; never throws.
     */
    MountRoutingDecision routeForFile(const QString& folderId,
                                      const QString& callerClientId,
                                      const QString& relPath) const;

    /**
     * @brief Per-mount manifest membership (the any-client
     *        manifestContains is too coarse for sticky routing).
     * @param mount   Mount row whose treeJson is checked.
     * @param relPath Canonicalised relative path.
     * @returns true iff this mount's manifest lists the file.
     */
    static bool mountManifestContains(const FolderMount& mount, const QString& relPath);

    /**
     * @brief All manifest paths of one mount, for
     *        the list_files union. Served from the cached treeJson;
     *        zero wire round-trips.
     * @param mount Mount row.
     * @returns Relative paths in manifest order.
     */
    static QStringList mountManifestPaths(const FolderMount& mount);

    /**
     * @brief Whether a mount counts as ACTIVE for routing (lastSeenMs
     *        within kStaleThresholdMs of now).
     * @param mount Mount row.
     * @returns true when fresh enough to route to.
     */
    static bool mountIsActive(const FolderMount& mount);

    /**
     * @brief Whether ANY registered client's manifest on the folder
     *        lists the file. Coarse pre-check only; the routing
     *        function makes the per-client decision.
     * @param folderId Folder UUID.
     * @param relPath  Canonicalised relative path.
     * @returns true if at least one client's manifest has the file.
     */
    Q_INVOKABLE bool manifestContains(const QString& folderId, const QString& relPath) const;


    /**
     * @brief Look up the mount for a folder by id.
     * @param folderId Target folder UUID.
     * @returns Populated FolderMount on hit, std::nullopt on miss.
     *
     * back-compat shim.  When multiple clients are
     * registered on `folderId`, returns the one with the most recent
     * `lastSeenMs`.  New call-sites should use
     * `mountsForFolder` + `mountForClient` directly so they can
     * disambiguate by client identity.
     */
    std::optional<FolderMount> mountForId(const QString& folderId) const;

    /**
     * @brief Lists every mount registered for the folder, ordered by
     *        `lastSeenMs` descending (most-recent first).
     * @param folderId Target folder UUID.
     * @returns Mount rows; empty vector when none registered.
     */
    QVector<FolderMount> mountsForFolder(const QString& folderId) const;

    /**
     * @brief Look up the mount registered by a
     *        specific `(folderId, clientId)` pair.
     * @param folderId Target folder UUID.
     * @param clientId Wire-auth client UUID.
     * @returns Populated FolderMount on hit, std::nullopt on miss.
     */
    std::optional<FolderMount> mountForClient(const QString& folderId,
                                              const QString& clientId) const;

  signals:
    /** Emitted when a folder gains its first mount registration. */
    void mountRegistered(const QString& folderId,
                         const QString& mountId,
                         const QString& clientId,
                         const QString& ownerLabel);
    /**
     * @brief Emitted when a (folder, client) mount row is removed.
     * @param folderId Folder UUID the mount belonged to.
     * @param mountId  Removed mount's id.
     * @param clientId Owning client's UUID.
     */
    void
    mountUnregistered(const QString& folderId, const QString& mountId, const QString& clientId);
    /**
     * @brief Emitted when the SAME client re-registers a folder with
     *        a different mountId (workspace switch on that client).
     * @param folderId    Folder UUID.
     * @param oldMountId  Mount id being displaced.
     * @param oldClientId Owning client (same as newClientId by
     *                    construction post-multi-client).
     * @param newMountId  Replacement mount id.
     * @param newClientId Owning client's UUID.
     */
    void mountReplaced(const QString& folderId,
                       const QString& oldMountId,
                       const QString& oldClientId,
                       const QString& newMountId,
                       const QString& newClientId);
    /**
     * @brief Emitted on a coalesced tree-manifest update (register,
     *        user refresh, reconnect, or top-level structural change).
     * @param folderId Folder UUID.
     * @param mountId  Mount whose manifest changed.
     * @param clientId Owning client's UUID.
     */
    void mountTreeUpdated(const QString& folderId, const QString& mountId, const QString& clientId);
    /**
     * @brief Emitted when the advisory permission tier changes.
     * @param folderId Folder UUID.
     * @param mountId  Mount whose tier changed.
     * @param clientId Owning client's UUID.
     * @param oldTier  Previous tier (ask | smart | bypass).
     * @param newTier  New tier.
     */
    void mountTierChanged(const QString& folderId,
                          const QString& mountId,
                          const QString& clientId,
                          const QString& oldTier,
                          const QString& newTier);

    /**
     * @brief Emitted by the periodic stale-sweep when a mount's
     *        `last_seen_ms` is older than `kStaleThresholdMs`. Fires
     *        exactly once per mount per staleness episode. The
     *        sweeper records the most-recent emission timestamp so
     *        long-stale mounts don't flood the signal on every tick.
     * @param folderId    Folder UUID whose mount went stale.
     * @param mountId     Workspace UUID that hasn't been touched.
     * @param clientId    Wire-auth client UUID owning the mount.
     * @param lastSeenMs  Last successful op timestamp (epoch ms).
     */
    void mountStale(const QString& folderId,
                    const QString& mountId,
                    const QString& clientId,
                    qint64 lastSeenMs);

  public:
    /**
     * @brief Staleness threshold. Mounts whose `last_seen_ms` is
     *        older than `now - kStaleThresholdMs` are considered
     *        stale and surface a `mountStale` signal + audit-log
     *        entry. Fixed at 1 hour; not configurable at runtime.
     */
    static constexpr qint64 kStaleThresholdMs = 60LL * 60LL * 1000LL;

    /**
     * @brief Period of the stale-mount sweep timer (60 s). Mounts
     *        whose last activity drifts past the threshold surface
     *        within one cadence of crossing it. Sub-minute cadence
     *        would be wasteful (mounts that idle are common; the
     *        signal is informational, not life-critical).
     */
    static constexpr int kStaleSweepIntervalMs = 60 * 1000;

    /**
     * @brief Single sweep of the in-memory cache. Walks every mount,
     *        emits `mountStale` + records an audit-log row for any
     *        whose `last_seen_ms` is older than `kStaleThresholdMs`.
     *        Public for two reasons: (a) tests trigger sweeps
     *        synchronously rather than waiting on the 60-second
     *        timer; (b) operators / future maintenance ops may want
     *        to force a sweep on demand. Idempotent: repeat fires
     *        for the same mount are suppressed until the mount's
     *        `last_seen_ms` advances past the previous emission.
     */
    void onStaleSweep();

  private:
    /**
     * @brief Validate `tier` against the SQL CHECK enum.
     * @returns true iff `tier` is exactly one of `ask`, `smart`,
     *          `bypass`.
     */
    static bool isValidTier(const QString& tier);

    /**
     * @brief Project a FolderMount POD into the camelCase
     *        QVariantMap shape returned by `mountFor` and `registerMount`.
     */
    static QVariantMap toVariantMap(const FolderMount& m);

    /**
     * @brief Hydrate `m_cache` from `folder_mounts` rows. Called
     *        exactly once at construction.
     */
    void loadCache();

    /** Engine-layer ref; non-owning. */
    DbManager& m_db;
    /** Audit hook; non-owning, optional. */
    AuditService* m_audit = nullptr;
    /** In-memory mirror of `folder_mounts` keyed by `folder_id`.
     *  Each folder may have multiple registered
     *  mounts, one per (folder_id, client_id) row (schema v19).  The
     *  vector preserves insertion order; same-(folder, client)
     *  upserts replace the matching entry in place rather than
     *  appending a duplicate.  Order within the vector is NOT a
     *  sorting contract; callers needing "most recent" must
     *  sort by lastSeenMs themselves (helpers `mountForId` and
     *  `mountsForFolder` do this when returning). */
    QHash<QString, QVector<FolderMount>> m_cache;

    /** Periodic stale-mount sweep. Parented to this registry so RAII
     *  teardown joins cleanly. Started in the ctor at
     *  `kStaleSweepIntervalMs` cadence. */
    QTimer m_staleSweepTimer;

    /** Per-folder timestamp at which we LAST emitted `mountStale` so
     *  the sweeper can suppress repeats until the mount's
     *  `last_seen_ms` advances past that timestamp again (the only
     *  way a mount can "go stale again" is by being touched, which
     *  bumps `last_seen_ms` past the recorded emission). */
    QHash<QString, qint64> m_lastStaleEmitMs;
};

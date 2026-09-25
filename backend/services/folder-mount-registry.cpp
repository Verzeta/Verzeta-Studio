// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file folder-mount-registry.cpp
 * @brief FolderMountRegistry implementation: SQLite-backed
 *        single-writer for the `folder_mounts` table with an in-
 *        memory cache mirror, lifecycle signals, and (optional)
 *        audit-log hooks.
 * @layer Service
 * @dependencies DbManager (Data Access), Qt6::Core, Qt6::Sql,
 *               AuditService (optional).
 */


#include "folder-mount-registry.h"

#include "../models/activity-event.h"
#include "../models/db-manager.h"
#include "../services/audit-service.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

namespace {

/** Validate UUID shape without depending on QUuid::fromString (some
 *  client SDKs ship lowercase / hyphenated UUIDs without braces; we
 *  accept any of the standard textual forms). Returns true iff the
 *  string is at least 32 hex chars after stripping `{}` and `-`. */
bool isUuidShape(const QString& s) {
    if (s.isEmpty() || s.size() > 64)
        return false;
    int hex = 0;
    for (QChar c : s) {
        if (c == QLatin1Char('-') || c == QLatin1Char('{') || c == QLatin1Char('}'))
            continue;
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
              (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) {
            return false;
        }
        ++hex;
    }
    return hex >= 32;
}

/** Bounded display-string check. Empty rejected; longer than 256
 *  chars rejected to keep audit + UI surfaces bounded. */
bool isBoundedLabel(const QString& s) {
    return !s.isEmpty() && s.size() <= 256;
}

/** Build the standardised `{ok:true}` reply that wire-session
 *  forwards as sendOk(data). */
inline QVariantMap okMap() {
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    return r;
}

/** Build the standardised `{ok:false, error:<message>}` reply. */
inline QVariantMap errMap(const QString& detail) {
    QVariantMap r;
    r.insert(QStringLiteral("ok"), false);
    r.insert(QStringLiteral("error"), detail);
    return r;
}

/**
 * @brief Translates a shell-style glob pattern into an anchored
 *        QRegularExpression. Supports:
 *          - `**`   any sequence of characters including `/`
 *                   (recursive directory wildcard).
 *          - `*`    any sequence of characters EXCEPT `/`
 *                   (single path component wildcard).
 *          - `?`    exactly one character EXCEPT `/`.
 *          - any other character is matched literally (regex
 *            metacharacters are escaped).
 *
 * The returned regex is anchored at both ends (^...$) so a glob
 * matches the WHOLE relative path, not just a prefix.
 */
QRegularExpression globToRegex(const QString& glob) {
    QString re;
    re.reserve(glob.size() * 2 + 4);
    re.append(QLatin1Char('^'));

    int i = 0;

    // Leading `**/` is an OPTIONAL prefix that matches zero or more
    // directories, so `**/.env` matches both `.env` (workspace root)
    // and `nested/.env`. Standard glob semantics.
    if (glob.startsWith(QStringLiteral("**/"))) {
        re.append(QStringLiteral("(?:.*/)?"));
        i = 3;
    }

    while (i < glob.size()) {
        const QChar c = glob.at(i);
        // Embedded `/**/` matches zero or more directories. So `a/**/b`
        // matches `a/b`, `a/x/b`, `a/x/y/b`, ...
        if (c == QLatin1Char('/') && i + 3 < glob.size() && glob.at(i + 1) == QLatin1Char('*') &&
            glob.at(i + 2) == QLatin1Char('*') && glob.at(i + 3) == QLatin1Char('/')) {
            re.append(QStringLiteral("(?:/.*)?/"));
            i += 4;
            continue;
        }
        // Trailing `/**` matches `/x`, `/x/y`, etc. — any non-empty
        // descendant path.
        if (c == QLatin1Char('/') && i + 2 < glob.size() && glob.at(i + 1) == QLatin1Char('*') &&
            glob.at(i + 2) == QLatin1Char('*') && i + 3 == glob.size()) {
            re.append(QStringLiteral("/.*"));
            i += 3;
            continue;
        }
        if (c == QLatin1Char('*')) {
            if (i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('*')) {
                re.append(QStringLiteral(".*"));
                i += 2;
            } else {
                re.append(QStringLiteral("[^/]*"));
                i += 1;
            }
        } else if (c == QLatin1Char('?')) {
            re.append(QStringLiteral("[^/]"));
            i += 1;
        } else if (c == QLatin1Char('.') || c == QLatin1Char('+') || c == QLatin1Char('(') ||
                   c == QLatin1Char(')') || c == QLatin1Char('[') || c == QLatin1Char(']') ||
                   c == QLatin1Char('{') || c == QLatin1Char('}') || c == QLatin1Char('|') ||
                   c == QLatin1Char('^') || c == QLatin1Char('$') || c == QLatin1Char('\\')) {
            re.append(QLatin1Char('\\'));
            re.append(c);
            i += 1;
        } else {
            re.append(c);
            i += 1;
        }
    }
    re.append(QLatin1Char('$'));
    return QRegularExpression(re);
}

/**
 * @brief Default blocklist applied to EVERY mount, regardless of
 *        per-mount additions. Users can ADD to this set per mount;
 *        they cannot REMOVE entries.
 *        Built once at first call and cached for the process
 *        lifetime.
 */
const QList<QRegularExpression>& defaultBlocklist() {
    static const QList<QRegularExpression> kList = []() {
        const QStringList kPatterns{
            QStringLiteral("**/.env"),
            QStringLiteral("**/.env.*"),
            QStringLiteral("**/.env.local"),
            QStringLiteral("**/.git/**"),
            QStringLiteral("**/.ssh/**"),
            QStringLiteral("**/secrets/**"),
            QStringLiteral("**/.netrc"),
            QStringLiteral("**/credentials*"),
            QStringLiteral("**/*.pem"),
            QStringLiteral("**/*.key"),
            QStringLiteral("**/id_rsa*"),
            QStringLiteral("**/id_ed25519*"),
            QStringLiteral("**/.aws/**"),
            QStringLiteral("**/.azure/**"),
            QStringLiteral("**/.docker/config.json"),
            QStringLiteral("**/.kube/**"),
            QStringLiteral("**/.npmrc"),
            QStringLiteral("**/.pypirc"),
            QStringLiteral("**/node_modules/**"),
            QStringLiteral("**/dist/**"),
            QStringLiteral("**/build/**"),
            QStringLiteral("**/target/**"),
            // Root-only versions (the `**/` prefix above only
            // matches paths with at least one directory separator;
            // bare `.env` at workspace root would slip through
            // otherwise).
            QStringLiteral(".env"),
            QStringLiteral(".env.*"),
            QStringLiteral(".env.local"),
            QStringLiteral(".netrc"),
            QStringLiteral(".npmrc"),
            QStringLiteral(".pypirc"),
        };
        QList<QRegularExpression> out;
        out.reserve(kPatterns.size());
        for (const QString& g : kPatterns) {
            out.append(globToRegex(g));
        }
        return out;
    }();
    return kList;
}

/**
 * @brief Parses a JSON array of glob strings into compiled regexes.
 *        Used for per-mount blocklist + allowlist additions stored
 *        as `blocklist_json` / `allowlist_json` in the
 *        `folder_mounts` table.
 */
QList<QRegularExpression> parseGlobList(const QString& json) {
    QList<QRegularExpression> out;
    if (json.isEmpty())
        return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return out;
    }
    const QJsonArray arr = doc.array();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QString pat = v.toString();
        if (!pat.isEmpty())
            out.append(globToRegex(pat));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FolderMountRegistry::FolderMountRegistry(DbManager& db, QObject* parent)
    : QObject(parent), m_db(db) {
    loadCache();
    int totalMounts = 0;
    for (const auto& v : m_cache)
        totalMounts += v.size();
    qCInfo(verzetaDb) << "FolderMountRegistry initialised; folders:" << m_cache.size()
                      << "mounts:" << totalMounts;

    m_staleSweepTimer.setParent(this);
    m_staleSweepTimer.setInterval(kStaleSweepIntervalMs);
    QObject::connect(
        &m_staleSweepTimer, &QTimer::timeout, this, &FolderMountRegistry::onStaleSweep);
    m_staleSweepTimer.start();
}

FolderMountRegistry::~FolderMountRegistry() = default;

void FolderMountRegistry::setAuditService(AuditService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_audit = svc;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

bool FolderMountRegistry::isValidTier(const QString& tier) {
    return tier == QStringLiteral("ask") || tier == QStringLiteral("smart") ||
           tier == QStringLiteral("bypass");
}

QVariantMap FolderMountRegistry::toVariantMap(const FolderMount& m) {
    QVariantMap row;
    row.insert(QStringLiteral("folderId"), m.folderId);
    row.insert(QStringLiteral("mountId"), m.mountId);
    row.insert(QStringLiteral("clientId"), m.clientId);
    row.insert(QStringLiteral("ownerLabel"), m.ownerLabel);
    row.insert(QStringLiteral("registeredAtMs"), m.registeredAtMs);
    row.insert(QStringLiteral("lastSeenMs"), m.lastSeenMs);
    row.insert(QStringLiteral("treeJson"), m.treeJson);
    row.insert(QStringLiteral("blocklistJson"), m.blocklistJson);
    row.insert(QStringLiteral("allowlistJson"), m.allowlistJson);
    row.insert(QStringLiteral("permissionTier"), m.permissionTier);
    row.insert(QStringLiteral("optionsJson"), m.optionsJson);
    return row;
}

void FolderMountRegistry::loadCache() {
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral("SELECT folder_id, mount_id, client_id, owner_label, "
                               "       registered_at_ms, last_seen_ms, tree_json, "
                               "       blocklist_json, allowlist_json, permission_tier, "
                               "       options_json "
                               "FROM folder_mounts"))) {
        qCWarning(verzetaDb) << "FolderMountRegistry::loadCache failed:" << q.lastError().text();
        return;
    }
    while (q.next()) {
        FolderMount m;
        m.folderId = q.value(0).toString();
        m.mountId = q.value(1).toString();
        m.clientId = q.value(2).toString();
        m.ownerLabel = q.value(3).toString();
        m.registeredAtMs = q.value(4).toLongLong();
        m.lastSeenMs = q.value(5).toLongLong();
        m.treeJson = q.value(6).toString();
        m.blocklistJson = q.value(7).toString();
        m.allowlistJson = q.value(8).toString();
        m.permissionTier = q.value(9).toString();
        m.optionsJson = q.value(10).toString();
        m_cache[m.folderId].append(m);
    }
}

// ---------------------------------------------------------------------------
// C++ accessor
// ---------------------------------------------------------------------------

std::optional<FolderMount> FolderMountRegistry::mountForId(const QString& folderId) const {
    // Back-compat shim — return the most-recent mount for the folder
    // (highest `lastSeenMs`).  New call-sites should use
    // mountsForFolder / mountForClient.
    const auto it = m_cache.constFind(folderId);
    if (it == m_cache.constEnd() || it.value().isEmpty()) {
        return std::nullopt;
    }
    const QVector<FolderMount>& vec = it.value();
    const FolderMount* best = &vec.first();
    for (const auto& m : vec) {
        if (m.lastSeenMs > best->lastSeenMs)
            best = &m;
    }
    return *best;
}

QVector<FolderMount> FolderMountRegistry::mountsForFolder(const QString& folderId) const {
    const auto it = m_cache.constFind(folderId);
    if (it == m_cache.constEnd())
        return {};
    // Copy + sort by lastSeenMs descending.
    QVector<FolderMount> out = it.value();
    std::sort(out.begin(), out.end(), [](const FolderMount& a, const FolderMount& b) {
        return a.lastSeenMs > b.lastSeenMs;
    });
    return out;
}

std::optional<FolderMount> FolderMountRegistry::mountForClient(const QString& folderId,
                                                               const QString& clientId) const {
    const auto it = m_cache.constFind(folderId);
    if (it == m_cache.constEnd())
        return std::nullopt;
    for (const auto& m : it.value()) {
        if (m.clientId == clientId)
            return m;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Wire-facing reads
// ---------------------------------------------------------------------------

QVariantMap FolderMountRegistry::mountFor(const QString& folderId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto opt = mountForId(folderId);
    if (!opt)
        return {};
    return toVariantMap(*opt);
}

// ---------------------------------------------------------------------------
// Wire-facing writes
// ---------------------------------------------------------------------------

QVariantMap FolderMountRegistry::registerMount(const QString& folderId,
                                               const QString& mountId,
                                               const QString& clientId,
                                               const QString& ownerLabel,
                                               const QString& treeJson,
                                               const QString& blocklistJson,
                                               const QString& allowlistJson,
                                               const QString& permissionTier,
                                               const QString& optionsJson) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (!isUuidShape(folderId))
        return errMap(QStringLiteral("'folder_id' must be UUID"));
    if (!isUuidShape(mountId))
        return errMap(QStringLiteral("'mount_id' must be UUID"));
    if (!isUuidShape(clientId))
        return errMap(QStringLiteral("'client_id' must be UUID"));
    if (!isBoundedLabel(ownerLabel))
        return errMap(QStringLiteral("'owner_label' empty or too long"));
    if (!isValidTier(permissionTier))
        return errMap(QStringLiteral("'permission_tier' must be one of: ask, smart, bypass"));

    QSqlDatabase db = m_db.db();
    if (!db.transaction()) {
        qCWarning(verzetaDb) << "FolderMountRegistry::registerMount transaction failed:"
                             << db.lastError().text();
        return errMap(QStringLiteral("transaction failed"));
    }

    const std::optional<FolderMount> prior = mountForClient(folderId, clientId);
    enum class Kind { Fresh, Refresh, Replace } kind = Kind::Fresh;
    if (prior) {
        // Same client_id but mount_id changed → Replace.  Same client
        // re-registering with same mount_id → Refresh.
        kind = (prior->mountId == mountId) ? Kind::Refresh : Kind::Replace;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 keepRegisteredAtMs = (kind == Kind::Refresh) ? prior->registeredAtMs : nowMs;

    QSqlQuery up(db);
    up.prepare(QStringLiteral("INSERT INTO folder_mounts ("
                              "  folder_id, client_id, mount_id, owner_label, "
                              "  registered_at_ms, last_seen_ms, tree_json, blocklist_json, "
                              "  allowlist_json, permission_tier, options_json"
                              ") VALUES (?,?,?,?,?,?,?,?,?,?,?) "
                              // v19 composite PK lets ON CONFLICT key off both columns so
                              // re-registers update the SAME row.  Different clients on the
                              // same folder insert distinct rows side by side.
                              "ON CONFLICT(folder_id, client_id) DO UPDATE SET "
                              "  mount_id         = excluded.mount_id, "
                              "  owner_label      = excluded.owner_label, "
                              "  registered_at_ms = excluded.registered_at_ms, "
                              "  last_seen_ms     = excluded.last_seen_ms, "
                              "  tree_json        = excluded.tree_json, "
                              "  blocklist_json   = excluded.blocklist_json, "
                              "  allowlist_json   = excluded.allowlist_json, "
                              "  permission_tier  = excluded.permission_tier, "
                              "  options_json     = excluded.options_json"));
    up.addBindValue(folderId);
    up.addBindValue(clientId);
    up.addBindValue(mountId);
    up.addBindValue(ownerLabel);
    up.addBindValue(keepRegisteredAtMs);
    up.addBindValue(nowMs);
    up.addBindValue(treeJson);
    up.addBindValue(blocklistJson);
    up.addBindValue(allowlistJson);
    up.addBindValue(permissionTier);
    up.addBindValue(optionsJson);
    if (!up.exec()) {
        qCWarning(verzetaDb) << "FolderMountRegistry::registerMount UPSERT failed:"
                             << up.lastError().text();
        db.rollback();
        return errMap(QStringLiteral("sql error"));
    }
    if (!db.commit()) {
        qCWarning(verzetaDb) << "FolderMountRegistry::registerMount commit failed:"
                             << db.lastError().text();
        db.rollback();
        return errMap(QStringLiteral("commit failed"));
    }

    // Cache mirror.
    FolderMount cached;
    cached.folderId = folderId;
    cached.mountId = mountId;
    cached.clientId = clientId;
    cached.ownerLabel = ownerLabel;
    cached.registeredAtMs = keepRegisteredAtMs;
    cached.lastSeenMs = nowMs;
    cached.treeJson = treeJson;
    cached.blocklistJson = blocklistJson;
    cached.allowlistJson = allowlistJson;
    cached.permissionTier = permissionTier;
    cached.optionsJson = optionsJson;
    {
        QVector<FolderMount>& vec = m_cache[folderId];
        bool replaced = false;
        for (auto& m : vec) {
            if (m.clientId == clientId) {
                m = cached;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            vec.append(cached);
    }

    // Signal + audit fan-out.
    if (kind == Kind::Fresh) {
        emit mountRegistered(folderId, mountId, clientId, ownerLabel);
        if (m_audit) {
            m_audit->record(ActivityEvent::forWorkspaceMountRegistered(
                folderId, mountId, clientId, ownerLabel));
        }
    } else if (kind == Kind::Replace) {
        emit mountReplaced(folderId, prior->mountId, prior->clientId, mountId, clientId);
        if (m_audit) {
            m_audit->record(ActivityEvent::forWorkspaceMountReplaced(
                folderId, prior->mountId, prior->clientId, mountId, clientId));
        }
    } else {  // Kind::Refresh
        emit mountTreeUpdated(folderId, mountId, clientId);
        if (m_audit) {
            m_audit->record(
                ActivityEvent::forWorkspaceMountTreeUpdated(folderId, mountId, clientId));
        }
    }

    return okMap();
}

QVariantMap FolderMountRegistry::unregisterMount(const QString& folderId, const QString& clientId) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (!isUuidShape(folderId))
        return errMap(QStringLiteral("'folder_id' must be UUID"));
    if (!isUuidShape(clientId))
        return errMap(QStringLiteral("'client_id' must be UUID"));

    const std::optional<FolderMount> prior = mountForClient(folderId, clientId);
    if (!prior)
        return errMap(QStringLiteral("no mount registered for folder/client"));

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM folder_mounts "
                             "WHERE folder_id = ? AND client_id = ?"));
    q.addBindValue(folderId);
    q.addBindValue(clientId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "FolderMountRegistry::unregisterMount DELETE failed:"
                             << q.lastError().text();
        return errMap(QStringLiteral("sql error"));
    }

    const QString mountId = prior->mountId;

    // Remove this client's entry from the per-folder vector; drop the
    // folder key entirely if the vector becomes empty.
    auto it = m_cache.find(folderId);
    if (it != m_cache.end()) {
        QVector<FolderMount>& vec = it.value();
        vec.erase(
            std::remove_if(vec.begin(),
                           vec.end(),
                           [&clientId](const FolderMount& m) { return m.clientId == clientId; }),
            vec.end());
        if (vec.isEmpty())
            m_cache.erase(it);
    }

    emit mountUnregistered(folderId, mountId, clientId);
    if (m_audit) {
        m_audit->record(ActivityEvent::forWorkspaceMountUnregistered(folderId, mountId, clientId));
    }
    return okMap();
}

QVariantMap FolderMountRegistry::updateTree(const QString& folderId,
                                            const QString& clientId,
                                            const QString& treeJson) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (!isUuidShape(folderId))
        return errMap(QStringLiteral("'folder_id' must be UUID"));
    if (!isUuidShape(clientId))
        return errMap(QStringLiteral("'client_id' must be UUID"));

    const std::optional<FolderMount> prior = mountForClient(folderId, clientId);
    if (!prior)
        return errMap(QStringLiteral("no mount registered for folder/client"));

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE folder_mounts "
                             "   SET tree_json    = ?, "
                             "       last_seen_ms = ? "
                             " WHERE folder_id    = ? AND client_id = ?"));
    q.addBindValue(treeJson);
    q.addBindValue(nowMs);
    q.addBindValue(folderId);
    q.addBindValue(clientId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "FolderMountRegistry::updateTree UPDATE failed:"
                             << q.lastError().text();
        return errMap(QStringLiteral("sql error"));
    }

    // Update the matching client's entry in the per-folder vector.
    {
        QVector<FolderMount>& vec = m_cache[folderId];
        for (auto& m : vec) {
            if (m.clientId == clientId) {
                m.treeJson = treeJson;
                m.lastSeenMs = nowMs;
                break;
            }
        }
    }
    FolderMount updated = *prior;
    updated.treeJson = treeJson;
    updated.lastSeenMs = nowMs;

    emit mountTreeUpdated(folderId, updated.mountId, clientId);
    if (m_audit) {
        m_audit->record(
            ActivityEvent::forWorkspaceMountTreeUpdated(folderId, updated.mountId, clientId));
    }
    return okMap();
}

QVariantMap FolderMountRegistry::updateTier(const QString& folderId,
                                            const QString& clientId,
                                            const QString& newTier) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (!isUuidShape(folderId))
        return errMap(QStringLiteral("'folder_id' must be UUID"));
    if (!isUuidShape(clientId))
        return errMap(QStringLiteral("'client_id' must be UUID"));
    if (!isValidTier(newTier))
        return errMap(QStringLiteral("'permission_tier' must be one of: ask, smart, bypass"));

    const std::optional<FolderMount> prior = mountForClient(folderId, clientId);
    if (!prior)
        return errMap(QStringLiteral("no mount registered for folder/client"));
    if (prior->permissionTier == newTier)
        return okMap();  // no-op, but still success

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE folder_mounts "
                             "   SET permission_tier = ?, "
                             "       last_seen_ms    = ? "
                             " WHERE folder_id       = ? AND client_id = ?"));
    q.addBindValue(newTier);
    q.addBindValue(nowMs);
    q.addBindValue(folderId);
    q.addBindValue(clientId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "FolderMountRegistry::updateTier UPDATE failed:"
                             << q.lastError().text();
        return errMap(QStringLiteral("sql error"));
    }

    const QString oldTier = prior->permissionTier;
    FolderMount updated = *prior;
    updated.permissionTier = newTier;
    updated.lastSeenMs = nowMs;
    // Update the matching client's entry in the per-folder vector.
    {
        QVector<FolderMount>& vec = m_cache[folderId];
        for (auto& m : vec) {
            if (m.clientId == clientId) {
                m.permissionTier = newTier;
                m.lastSeenMs = nowMs;
                break;
            }
        }
    }

    emit mountTierChanged(folderId, updated.mountId, clientId, oldTier, newTier);
    if (m_audit) {
        m_audit->record(ActivityEvent::forWorkspaceMountTierChanged(
            folderId, updated.mountId, clientId, oldTier, newTier));
    }
    return okMap();
}


QString FolderMountRegistry::pathIsAllowed(const QString& folderId, const QString& relPath) const {
    VERZETA_ASSERT_MAIN_THREAD();

    // Cheap structural rejections first — no need to consult the
    // mount for clearly-malformed input.
    //
    // Empty `relPath` is NOT a rejection: it denotes the workspace
    // ROOT (the natural target for `list_files` discovering what's
    // in the workspace). Reads of "" fail downstream via the
    // manifest gate (manifest carries file entries, not the empty
    // string); writes of "" are caught earlier by the tools' own
    // arg validation. Removing the empty-path rejection here does
    // not widen the attack surface — the canonical `..` escape
    // check + the absolute-path check + the blocklist still cover
    // every actual traversal.
    if (relPath.contains(QChar(0))) {
        return QStringLiteral("unsafe_path");
    }
    if (relPath.startsWith(QLatin1Char('/'))) {
        return QStringLiteral("absolute_path_forbidden");
    }
    // QDir::cleanPath collapses `.` and `..`; if the result still
    // contains `..` it means the path escaped the workspace root.
    const QString canonical = QDir::cleanPath(relPath);
    if (canonical.startsWith(QStringLiteral("..")) || canonical.contains(QStringLiteral("/../")) ||
        canonical.endsWith(QStringLiteral("/..")) || canonical == QStringLiteral("..")) {
        return QStringLiteral("unsafe_path");
    }

    const std::optional<FolderMount> mount = mountForId(folderId);
    if (!mount) {
        return QStringLiteral("mount_offline");
    }

    // Default blocklist.
    for (const QRegularExpression& re : defaultBlocklist()) {
        if (re.match(canonical).hasMatch()) {
            return QStringLiteral("blocked_path");
        }
    }

    // Per-mount blocklist additions.
    const QList<QRegularExpression> perMountBlock = parseGlobList(mount->blocklistJson);
    for (const QRegularExpression& re : perMountBlock) {
        if (re.match(canonical).hasMatch()) {
            return QStringLiteral("blocked_path");
        }
    }

    // Per-mount allowlist (when non-empty). Path MUST match at least
    // one entry.
    const QList<QRegularExpression> perMountAllow = parseGlobList(mount->allowlistJson);
    if (!perMountAllow.isEmpty()) {
        bool matched = false;
        for (const QRegularExpression& re : perMountAllow) {
            if (re.match(canonical).hasMatch()) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return QStringLiteral("not_allowlisted");
        }
    }

    return QString();  // empty == ok
}

void FolderMountRegistry::onStaleSweep() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_cache.isEmpty())
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    const QList<QString> folderIds = m_cache.keys();
    for (const QString& folderId : folderIds) {
        const auto it = m_cache.constFind(folderId);
        if (it == m_cache.constEnd())
            continue;
        for (const FolderMount& m : it.value()) {
            const QString suppKey = folderId + QLatin1Char('|') + m.clientId;

            // Skip mounts inside the freshness window.
            if (nowMs - m.lastSeenMs < kStaleThresholdMs) {
                // If this mount was previously stale but recently
                // refreshed, drop the suppression entry so a future
                // staleness episode can fire cleanly.
                const auto sit = m_lastStaleEmitMs.constFind(suppKey);
                if (sit != m_lastStaleEmitMs.constEnd() && m.lastSeenMs > sit.value()) {
                    m_lastStaleEmitMs.remove(suppKey);
                }
                continue;
            }

            // Suppress repeat fires until the mount has been touched
            // again since the last emission. Otherwise the sweeper would
            // spam the signal every 60 s for a permanently-idle mount.
            const auto sit = m_lastStaleEmitMs.constFind(suppKey);
            if (sit != m_lastStaleEmitMs.constEnd() && m.lastSeenMs <= sit.value()) {
                continue;
            }

            m_lastStaleEmitMs.insert(suppKey, nowMs);

            emit mountStale(m.folderId, m.mountId, m.clientId, m.lastSeenMs);
            if (m_audit) {
                m_audit->record(ActivityEvent::forWorkspaceMountStale(
                    m.folderId, m.mountId, m.clientId, m.lastSeenMs));
            }
        }
    }
}

bool FolderMountRegistry::mountIsActive(const FolderMount& mount) {
    return QDateTime::currentMSecsSinceEpoch() - mount.lastSeenMs <= kStaleThresholdMs;
}

bool FolderMountRegistry::mountManifestContains(const FolderMount& mount, const QString& relPath) {
    const QString canonical = QDir::cleanPath(relPath);
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(mount.treeJson.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonArray files = doc.object().value(QStringLiteral("files")).toArray();
    for (const QJsonValue& v : files) {
        if (v.toObject().value(QStringLiteral("path")).toString() == canonical) {
            return true;
        }
    }
    return false;
}

QStringList FolderMountRegistry::mountManifestPaths(const FolderMount& mount) {
    QStringList out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(mount.treeJson.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return out;
    }
    const QJsonArray files = doc.object().value(QStringLiteral("files")).toArray();
    out.reserve(files.size());
    for (const QJsonValue& v : files) {
        const QString p = v.toObject().value(QStringLiteral("path")).toString();
        if (!p.isEmpty())
            out.append(p);
    }
    return out;
}

FolderMountRegistry::MountRoutingDecision FolderMountRegistry::routeForFile(
    const QString& folderId, const QString& callerClientId, const QString& relPath) const {
    VERZETA_ASSERT_MAIN_THREAD();
    MountRoutingDecision d;
    d.folderId = folderId;

    const QVector<FolderMount> mounts = mountsForFolder(folderId);

    // Step 2, caller-aware routing: an
    // extension client operating on its own folder is the
    // authoritative owner of its own context. Route exclusively
    // there; if its mount is gone/stale, the CALLER errors — we never
    // silently write into a different client's workspace.
    if (!callerClientId.isEmpty()) {
        for (const FolderMount& m : mounts) {
            if (m.clientId == callerClientId) {
                if (!mountIsActive(m))
                    break;
                d.clientId = m.clientId;
                d.mountId = m.mountId;
                d.reason = QStringLiteral("caller_owned");
                return d;
            }
        }
        d.reason = QStringLiteral("caller_client_not_mounted");
        return d;  // clientId empty + this reason = caller error.
    }

    // Step 3, sticky-by-manifest: the
    // client whose manifest contains the file owns reads AND writes
    // for it. Most-recent active owner wins the rare multi-claim tie
    // (mountsForFolder is already lastSeenMs-desc sorted).
    for (const FolderMount& m : mounts) {
        if (mountIsActive(m) && mountManifestContains(m, relPath)) {
            d.clientId = m.clientId;
            d.mountId = m.mountId;
            d.reason = QStringLiteral("manifest_owner");
            return d;
        }
    }

    // Step 4, new file: most-recent ACTIVE
    // client takes it.
    for (const FolderMount& m : mounts) {
        if (mountIsActive(m)) {
            d.clientId = m.clientId;
            d.mountId = m.mountId;
            d.reason = QStringLiteral("most_recent_active");
            return d;
        }
    }

    // Step 5, no active mounts: host-local.
    d.reason = QStringLiteral("no_active_mount");
    return d;
}

bool FolderMountRegistry::manifestContains(const QString& folderId, const QString& relPath) const {
    VERZETA_ASSERT_MAIN_THREAD();

    const auto it = m_cache.constFind(folderId);
    if (it == m_cache.constEnd())
        return false;

    const QString canonical = QDir::cleanPath(relPath);

    for (const FolderMount& mount : it.value()) {
        // Tree manifest shape: {v:1, files:[{path, size, mtime_ms}, ...]}
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(mount.treeJson.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        const QJsonArray files = doc.object().value(QStringLiteral("files")).toArray();
        for (const QJsonValue& v : files) {
            if (v.toObject().value(QStringLiteral("path")).toString() == canonical) {
                return true;
            }
        }
    }
    return false;
}

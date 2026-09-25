// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file db-manager.cpp
 * @brief Implementation of the SQLite singleton database manager.
 *        Handles schema creation (version 1), migrations, and PRAGMA configuration.
 * @layer Data Access
 * @dependencies Qt6::Sql, SQLite3
 */

#include "db-manager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSet>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

#if defined(Q_OS_UNIX) && !defined(Q_OS_LINUX)
#include <errno.h>
#include <signal.h>
#endif
#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#ifdef VERZETA_HAVE_SQLITE3
#include <sqlite3.h>
#endif

#include "../utils/filesystem-check.h"
#include "../utils/logger.h"

namespace {

/**
 * @brief Best-effort liveness check for the PID recorded in a
 *        QLockFile.
 *
 * Returns true ONLY when the platform can prove the PID is gone.
 * Unknown/permission-denied cases return false (conservative: we'd
 * rather block startup than corrupt the DB by stomping a live
 * process's lockfile).
 *
 * Why this exists: Qt's QLockFile is supposed to auto-reclaim a
 * lockfile whose stored PID is dead.  In production we have observed
 * that reclaim NOT firing reliably. A host crash leaves a stuck
 * lockfile that blocks every subsequent restart until the user
 * deletes the file manually.  The headless host is meant to recover
 * automatically, so this helper restores that behaviour by unlinking
 * the file ourselves when we can prove the holder is dead.
 */
bool isLockHolderPidDead(qint64 pid) {
    if (pid <= 0)
        return true;
#if defined(Q_OS_LINUX)
    return !QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid));
#elif defined(Q_OS_MACOS) || defined(Q_OS_BSD4) || defined(Q_OS_UNIX)
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return false;
    return errno == ESRCH;
#elif defined(Q_OS_WIN)
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (h) {
        ::CloseHandle(h);
        return false;
    }
    return ::GetLastError() == ERROR_INVALID_PARAMETER;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

/**
 * @brief Reads the first line of the QLockFile (the holder PID) and
 *        returns it as a qint64.  Returns 0 if the file is unreadable
 *        or the first line cannot be parsed.
 */
qint64 readLockHolderPid(const QString& lockPath) {
    QFile f(lockPath);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QString line = QString::fromUtf8(f.readLine()).trimmed();
    f.close();
    bool ok = false;
    const qint64 pid = line.toLongLong(&ok);
    return ok ? pid : 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

/**
 * @brief Returns the singleton DbManager instance.
 * @return Reference to the global DbManager.
 * @complexity O(1)
 */
DbManager& DbManager::instance() {
    static DbManager s_instance;
    return s_instance;
}

// Out-of-line ctor / dtor so the std::unique_ptr<QLockFile> member can
// see the complete QLockFile type (header forward-declares it).
DbManager::DbManager() = default;
DbManager::~DbManager() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * @brief Opens the SQLite database at the given path.
 *        Creates parent directories and the file if they don't exist.
 * @param path Absolute file path to the .db file.
 * @return true if the database was opened successfully.
 * @sideeffects Creates the file and parent directories if they don't exist.
 */
bool DbManager::open(const QString& path) {
    m_path = path;

    // Resolve parent directory for filesystem-type inspection. We check
    // BEFORE mkpath so refusal surfaces without creating any directories.
    QDir dir = QFileInfo(path).absoluteDir();

    // -----------------------------------------------------------------
    // Refuse network / distributed / VM-shared / FUSE-network paths.
    // SQLite WAL is documented (sqlite.org/wal.html) as not working
    // over network filesystems — opening here would silently corrupt
    // data later. Refusing cleanly at startup is the only safe option.
    // -----------------------------------------------------------------
    const auto fsInfo = Verzeta::Utils::inspectFilesystem(dir.absolutePath());
    if (fsInfo.isNetwork) {
        const QString msg =
            QStringLiteral("Database directory is on a network / distributed filesystem "
                           "(detected: %1 at %2).  SQLite WAL mode requires local "
                           "block-storage; running over a network filesystem causes "
                           "silent data corruption.  Move the data directory to a local "
                           "disk, or set XDG_DATA_HOME to a local path.")
                .arg(fsInfo.typeName, dir.absolutePath());
        qCCritical(verzetaDb).noquote() << msg;
        if (!fsInfo.humanReason.isEmpty()) {
            qCCritical(verzetaDb).noquote() << fsInfo.humanReason;
        }
        emit errorOccurred(msg);
        return false;
    }

    // Ensure parent directory exists.
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qCCritical(verzetaDb) << "Failed to create database directory:" << dir.absolutePath();
            emit errorOccurred(QStringLiteral("Cannot create database directory"));
            return false;
        }
    }

    // -----------------------------------------------------------------
    // Acquire the engine lock BEFORE opening the SQLite connection so a
    // second engine attempt refuses without ever touching the DB file.
    // QLockFile is PID-aware: a crashed previous process leaves a stale
    // lockfile that Qt automatically reclaims on the next tryLock call
    // — users do NOT need to manually delete anything.
    //
    // Belt-and-braces stale-lock recovery: read the holder PID ourselves
    // BEFORE handing the file to QLockFile.  If we can prove the holder
    // process is gone, unlink the stale lockfile so QLockFile can take
    // it cleanly.  Qt's own reclaim has been observed not firing in
    // production — a host crash would otherwise leave a stuck lockfile
    // that blocks every subsequent restart until the user deletes it
    // by hand.
    // -----------------------------------------------------------------
    const QString lockPath = path + QStringLiteral(".engine.lock");
    if (QFileInfo::exists(lockPath)) {
        const qint64 staleHolderPid = readLockHolderPid(lockPath);
        if (staleHolderPid > 0 && isLockHolderPidDead(staleHolderPid)) {
            if (QFile::remove(lockPath)) {
                qCWarning(verzetaDb).noquote()
                    << QStringLiteral("Removed stale engine lockfile from dead PID %1 "
                                      "(holder process no longer exists).")
                           .arg(staleHolderPid);
            } else {
                qCWarning(verzetaDb)
                    << "Stale engine lockfile detected for dead PID" << staleHolderPid
                    << "but could not be removed — QLockFile will retry.";
            }
        }
    }
    m_engineLock = std::make_unique<QLockFile>(lockPath);
    m_engineLock->setStaleLockTime(30000);  // 30s — Qt's default
    if (!m_engineLock->tryLock(0)) {
        qint64 holderPid = 0;
        QString holderHost, holderApp;
        m_engineLock->getLockInfo(&holderPid, &holderHost, &holderApp);
        const QString msg =
            QStringLiteral("Another Verzeta Studio engine instance is already running on "
                           "this database (holder PID: %1, hostname: %2).  Close the "
                           "existing instance and try again.")
                .arg(holderPid)
                .arg(holderHost.isEmpty() ? QHostInfo::localHostName() : holderHost);
        qCCritical(verzetaDb).noquote() << msg;
        emit errorOccurred(msg);
        m_engineLock.reset();
        return false;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("verzeta_main"));
    m_db.setDatabaseName(path);

    if (!m_db.open()) {
        qCCritical(verzetaDb) << "Failed to open database:" << m_db.lastError().text();
        emit errorOccurred(QStringLiteral("Cannot open database: ") + m_db.lastError().text());
        // Release the lock — open failed; another process may legitimately
        // want to retry after the user fixes whatever broke.
        m_engineLock.reset();
        return false;
    }

    configurePragmas();
    qCInfo(verzetaDb) << "Database opened at" << path << "(filesystem:" << fsInfo.typeName << ")";
    return true;
}

/**
 * @brief Applies schema migrations from current version to latest.
 * @return true if all migrations succeeded.
 * @sideeffects Modifies database schema, writes to settings table.
 */
bool DbManager::runMigrations() {
    if (!m_db.isOpen()) {
        qCCritical(verzetaDb) << "Cannot run migrations: database is not open";
        return false;
    }

    int version = currentSchemaVersion();
    qCInfo(verzetaDb) << "Current schema version:" << version;

    if (version < 1) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start migration transaction";
            return false;
        }
        applySchema();
        setSchemaVersion(1);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit migration transaction";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 1";
    }

    // Migration v1 → v2: Agent support
    // Adds: agents table, folder metadata (type/goal/description/agent_ids),
    //       conversation primary_agent_id + is_group, message agent_id.
    if (version < 2) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v2 migration transaction";
            return false;
        }
        if (!applySchemaV2()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(2);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v2 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 2 (agent support)";
    }

    // Migration v2 → v3: Group chats
    // Adds: conversations.group_agent_ids (JSON array of agent UUIDs).
    if (version < 3) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v3 migration transaction";
            return false;
        }
        if (!applySchemaV3()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(3);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v3 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 3 (group chats)";
    }

    // Migration v3 → v4: Aliased memberships
    // Adds: project_members, conversation_members tables;
    //       agents.is_coordinator; messages.member_alias;
    //       migrates existing agent_ids / group_agent_ids JSON into the
    //       new tables using each agent's name as the default alias.
    if (version < 4) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v4 migration transaction";
            return false;
        }
        if (!applySchemaV4()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(4);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v4 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 4 (aliased memberships)";
    }

    if (version < 5) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v5 migration transaction";
            return false;
        }
        if (!applySchemaV5()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(5);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v5 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 5 (task execution loop)";
    }

    if (version < 6) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v6 migration transaction";
            return false;
        }
        if (!applySchemaV6()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(6);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v6 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 6 (message/tool-call separation)";
    }

    // Migration v6 → v7: tool_calls.plan_step_id lineage FK.
    if (version < 7) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v7 migration transaction";
            return false;
        }
        if (!applySchemaV7()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(7);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v7 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 7 (tool-call plan lineage)";
    }

    if (version < 8) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v8 migration transaction";
            return false;
        }
        if (!applySchemaV8()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(8);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v8 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 8 (canvas artifacts)";
    }

    if (version < 9) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v9 migration transaction";
            return false;
        }
        if (!applySchemaV9()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(9);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v9 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 9 (conversation pinning)";
    }

    if (version < 10) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v10 migration transaction";
            return false;
        }
        if (!applySchemaV10()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(10);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v10 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 10 (heartbeat subagents)";
    }

    if (version < 11) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v11 migration transaction";
            return false;
        }
        if (!applySchemaV11()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(11);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v11 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 11 (heartbeat self-config + audit)";
    }

    if (version < 12) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v12 migration transaction";
            return false;
        }
        if (!applySchemaV12()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(12);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v12 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 12 (membership provenance)";
    }

    if (version < 13) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v13 migration transaction";
            return false;
        }
        if (!applySchemaV13()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(13);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v13 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 13 (polls / voting)";
    }

    if (version < 14) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v14 migration transaction";
            return false;
        }
        if (!applySchemaV14()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(14);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v14 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 14 (per-member model override)";
    }

    if (version < 15) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v15 migration transaction";
            return false;
        }
        if (!applySchemaV15()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(15);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v15 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 15 (activity_log audit trail)";
    }

    // v16: additive `thinking_content` column on `messages` for the
    // display-only reasoning sidecar.  NOT NULL with DEFAULT '' so
    // every existing row backfills to empty without a data migration
    // step (and so any future INSERT that forgets to bind the column
    // gets an explicit empty string rather than NULL).  The column
    // is render-only by contract — RequestBuilder + HistoryBudgeter +
    // ContentSanitizer + SearchService NEVER read it (verified by a
    // dedicated invariant test in tests/integration/).
    if (version < 16) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v16 migration transaction";
            return false;
        }
        if (!applySchemaV16()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(16);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v16 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 16 "
                             "(messages.thinking_content sidecar)";
    }

    // Migration v16 → v17: additive folder_mounts table for client-
    // owned virtual workspace mounts. Pure metadata — no file bytes
    // persisted, no existing table altered, no existing row touched.
    // Folder deletion cascades the mount row via the table's own
    // ON DELETE CASCADE on folder_id.
    if (version < 17) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v17 migration transaction";
            return false;
        }
        if (!applySchemaV17()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(17);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v17 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 17 (folder_mounts)";
    }

    if (version < 18) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v18 migration transaction";
            return false;
        }
        if (!applySchemaV18()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(18);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v18 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 18 (activity_log.actor_kind "
                             "CHECK widened to include 'client')";
    }

    if (version < 19) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v19 migration transaction";
            return false;
        }
        if (!applySchemaV19()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(19);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v19 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 19 (folder_mounts PK widened "
                             "to (folder_id, client_id) for multi-client mounts)";
    }

    if (version < 20) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v20 migration transaction";
            return false;
        }
        if (!applySchemaV20()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(20);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v20 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 20 (conversation_summaries "
                             "for dynamic compaction)";
    }

    if (version < 21) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v21 migration transaction";
            return false;
        }
        if (!applySchemaV21()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(21);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v21 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 21 (subagent runs + "
                             "transcripts)";
    }

    if (version < 22) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v22 migration transaction";
            return false;
        }
        if (!applySchemaV22()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(22);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v22 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 22 (RAG corpus scope + model id)";
    }

    // Migration v22 → v23: per-agent memory (AIM).
    // Adds: agent_memories (durable per-agent / per-conversation memory entries
    //       with embedding + scope, sharing the VectorStore substrate) and its
    //       agent_memories_fts FTS5 index for lexical recall. No data migration.
    if (version < 23) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v23 migration transaction";
            return false;
        }
        if (!applySchemaV23()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(23);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v23 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 23 (agent memory / AIM)";
    }

    // Migration v23 → v24: team/project memory (ACN).
    // Adds: acn_entries (project/org-scoped memory written at compaction, with
    //       embedding + scope, sharing the VectorStore substrate) + its
    //       acn_entries_fts FTS5 index, and folders.acn_enabled (master switch,
    //       default on). No data migration.
    if (version < 24) {
        if (!m_db.transaction()) {
            qCCritical(verzetaDb) << "Failed to start v24 migration transaction";
            return false;
        }
        if (!applySchemaV24()) {
            m_db.rollback();
            return false;
        }
        setSchemaVersion(24);
        if (!m_db.commit()) {
            qCCritical(verzetaDb) << "Failed to commit v24 migration";
            m_db.rollback();
            return false;
        }
        qCInfo(verzetaDb) << "Schema migrated to version 24 (team memory / ACN)";
    }

    if (!repairMembershipProvenanceColumns()) {
        return false;
    }

    return true;
}

bool DbManager::repairMembershipProvenanceColumns() {
    auto hasColumn = [this](const QString& table, const QString& column) -> bool {
        QSqlQuery q(m_db);
        if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            qCWarning(verzetaDb) << "repairMembershipProvenanceColumns: PRAGMA failed for" << table
                                 << ":" << q.lastError().text();
            return true;  // assume present to avoid spurious ALTER attempts.
        }
        while (q.next()) {
            if (q.value(1).toString() == column)
                return true;
        }
        return false;
    };

    /**
     * @brief One column the migration must add. Lookup-only struct.
     */
    struct Needed {
        QString table;
        QString column;
        QString defaultLiteral;  // SQL literal incl. quotes for strings
    };
    const QList<Needed> needed = {
        {QStringLiteral("project_members"),
         QStringLiteral("added_by_kind"),
         QStringLiteral("'user'")},
        {QStringLiteral("project_members"),
         QStringLiteral("added_by_agent_id"),
         QStringLiteral("''")},
        {QStringLiteral("conversation_members"),
         QStringLiteral("added_by_kind"),
         QStringLiteral("'user'")},
        {QStringLiteral("conversation_members"),
         QStringLiteral("added_by_agent_id"),
         QStringLiteral("''")},
    };

    for (const Needed& n : needed) {
        if (hasColumn(n.table, n.column))
            continue;
        QSqlQuery alter(m_db);
        const QString stmt = QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 TEXT NOT NULL DEFAULT %3")
                                 .arg(n.table, n.column, n.defaultLiteral);
        if (!alter.exec(stmt)) {
            const QString err = alter.lastError().text();
            // applySchemaV12 has the same duplicate-column tolerance —
            // any race where the column appeared between the PRAGMA
            // and the ALTER is harmless.
            if (err.contains(QStringLiteral("duplicate column"), Qt::CaseInsensitive)) {
                continue;
            }
            qCCritical(verzetaDb) << "repairMembershipProvenanceColumns: ALTER failed:" << err
                                  << "stmt:" << stmt;
            return false;
        }
        qCInfo(verzetaDb) << "repairMembershipProvenanceColumns: added missing column" << n.table
                          << "." << n.column;
    }
    return true;
}

/**
 * @brief Adds agent-related tables and columns to an existing schema v1 DB.
 * @return true on success, false if any statement failed.
 *
 * SQLite ALTER TABLE can add columns but not constraints. Adding columns with
 * CHECK constraints requires a table rebuild. Since we use nullable columns
 * with defaults, simple ALTER TABLE statements are sufficient.
 */
bool DbManager::applySchemaV2() {
    QSqlQuery q(m_db);

    const QStringList statements = {
        // Create agents table
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS agents (
                id              TEXT PRIMARY KEY,
                name            TEXT NOT NULL UNIQUE,
                description     TEXT,
                icon_name       TEXT,
                system_prompt   TEXT NOT NULL,
                default_pattern TEXT NOT NULL DEFAULT 'direct',
                model_provider  TEXT,
                model_name      TEXT,
                allowed_tools   TEXT,
                is_builtin      INTEGER NOT NULL DEFAULT 0,
                created_at      INTEGER NOT NULL
            )
        )"),

        // Extend folders
        QStringLiteral(
            "ALTER TABLE folders ADD COLUMN folder_type TEXT NOT NULL DEFAULT 'regular'"),
        QStringLiteral("ALTER TABLE folders ADD COLUMN goal TEXT"),
        QStringLiteral("ALTER TABLE folders ADD COLUMN description TEXT"),
        QStringLiteral("ALTER TABLE folders ADD COLUMN agent_ids TEXT"),

        // Extend conversations
        QStringLiteral(
            "ALTER TABLE conversations ADD COLUMN primary_agent_id TEXT REFERENCES agents(id)"),
        QStringLiteral("ALTER TABLE conversations ADD COLUMN is_group INTEGER NOT NULL DEFAULT 0"),

        // Extend messages
        QStringLiteral("ALTER TABLE messages ADD COLUMN agent_id TEXT REFERENCES agents(id)"),
    };

    for (const QString& stmt : statements) {
        if (!q.exec(stmt)) {
            // ALTER TABLE on an already-added column returns "duplicate column"
            // which is fine — it means a previous migration attempt succeeded.
            const QString errText = q.lastError().text().toLower();
            if (errText.contains(QStringLiteral("duplicate column"))) {
                continue;
            }
            qCCritical(verzetaDb) << "v2 migration failed:" << q.lastError().text()
                                  << "statement:" << stmt;
            return false;
        }
    }

    return true;
}

/**
 * @brief Adds group-chat support (schema v2 → v3).
 *        Single column: conversations.group_agent_ids (JSON array of agent UUIDs).
 */
bool DbManager::applySchemaV3() {
    QSqlQuery q(m_db);

    const QStringList statements = {
        QStringLiteral("ALTER TABLE conversations ADD COLUMN group_agent_ids TEXT"),
    };

    for (const QString& stmt : statements) {
        if (!q.exec(stmt)) {
            const QString errText = q.lastError().text().toLower();
            if (errText.contains(QStringLiteral("duplicate column"))) {
                continue;
            }
            qCCritical(verzetaDb) << "v3 migration failed:" << q.lastError().text()
                                  << "statement:" << stmt;
            return false;
        }
    }

    return true;
}

/**
 * @brief Adds aliased-membership tables and columns (schema v3 → v4).
 *        Creates project_members + conversation_members tables,
 *        adds agents.is_coordinator and messages.member_alias,
 *        then migrates existing JSON agent_ids / group_agent_ids into
 *        the new tables using the agent's name as the default alias.
 */
bool DbManager::applySchemaV4() {
    QSqlQuery q(m_db);

    const QStringList ddl = {
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS project_members (
                folder_id          TEXT NOT NULL REFERENCES folders(id) ON DELETE CASCADE,
                agent_id           TEXT NOT NULL REFERENCES agents(id),
                alias              TEXT NOT NULL,
                is_coordinator     INTEGER NOT NULL DEFAULT 0,
                joined_at          INTEGER NOT NULL,
                added_by_kind      TEXT NOT NULL DEFAULT 'user',
                added_by_agent_id  TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (folder_id, alias)
            )
        )"),
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS conversation_members (
                conversation_id    TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
                agent_id           TEXT NOT NULL REFERENCES agents(id),
                alias              TEXT NOT NULL,
                is_coordinator     INTEGER NOT NULL DEFAULT 0,
                joined_at          INTEGER NOT NULL,
                added_by_kind      TEXT NOT NULL DEFAULT 'user',
                added_by_agent_id  TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (conversation_id, alias)
            )
        )"),
        QStringLiteral("ALTER TABLE agents ADD COLUMN is_coordinator INTEGER NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE messages ADD COLUMN member_alias TEXT"),
    };

    for (const QString& stmt : ddl) {
        if (!q.exec(stmt)) {
            const QString errText = q.lastError().text().toLower();
            if (errText.contains(QStringLiteral("duplicate column")))
                continue;
            qCCritical(verzetaDb) << "v4 migration DDL failed:" << q.lastError().text()
                                  << "statement:" << stmt;
            return false;
        }
    }

    // Data migration: read folders.agent_ids JSON → project_members rows
    {
        QSqlQuery sel(m_db);
        if (!sel.exec(QStringLiteral("SELECT id, agent_ids FROM folders WHERE agent_ids IS NOT "
                                     "NULL AND agent_ids != ''"))) {
            qCCritical(verzetaDb) << "v4 migration: SELECT folders failed:"
                                  << sel.lastError().text();
            return false;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        while (sel.next()) {
            const QString folderId = sel.value(0).toString();
            const QString jsonStr = sel.value(1).toString();
            const QJsonArray arr = QJsonDocument::fromJson(jsonStr.toUtf8()).array();

            QSet<QString> usedAliases;
            for (const QJsonValue& v : arr) {
                const QString agentId = v.toString();
                if (agentId.isEmpty())
                    continue;

                // Look up agent.name for alias default
                QSqlQuery nameQ(m_db);
                nameQ.prepare(QStringLiteral("SELECT name FROM agents WHERE id = ?"));
                nameQ.addBindValue(agentId);
                if (!nameQ.exec() || !nameQ.next())
                    continue;

                QString alias = nameQ.value(0).toString();
                // Ensure uniqueness within this folder
                QString candidate = alias;
                int suffix = 2;
                while (usedAliases.contains(candidate)) {
                    candidate = QStringLiteral("%1 %2").arg(alias).arg(suffix++);
                }
                alias = candidate;
                usedAliases.insert(alias);

                QSqlQuery ins(m_db);
                ins.prepare(QStringLiteral(
                    "INSERT OR IGNORE INTO project_members(folder_id, agent_id, alias, "
                    "is_coordinator, joined_at) VALUES(?, ?, ?, 0, ?)"));
                ins.addBindValue(folderId);
                ins.addBindValue(agentId);
                ins.addBindValue(alias);
                ins.addBindValue(now);
                if (!ins.exec()) {
                    qCWarning(verzetaDb)
                        << "v4 migration: project_members insert failed:" << ins.lastError().text();
                }
            }
        }
    }

    // Data migration: read conversations.group_agent_ids JSON → conversation_members rows
    {
        QSqlQuery sel(m_db);
        if (!sel.exec(
                QStringLiteral("SELECT id, group_agent_ids FROM conversations "
                               "WHERE group_agent_ids IS NOT NULL AND group_agent_ids != ''"))) {
            qCCritical(verzetaDb) << "v4 migration: SELECT conversations failed:"
                                  << sel.lastError().text();
            return false;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        while (sel.next()) {
            const QString convId = sel.value(0).toString();
            const QString jsonStr = sel.value(1).toString();
            const QJsonArray arr = QJsonDocument::fromJson(jsonStr.toUtf8()).array();

            QSet<QString> usedAliases;
            bool firstMember = true;
            for (const QJsonValue& v : arr) {
                const QString agentId = v.toString();
                if (agentId.isEmpty())
                    continue;

                QSqlQuery nameQ(m_db);
                nameQ.prepare(QStringLiteral("SELECT name FROM agents WHERE id = ?"));
                nameQ.addBindValue(agentId);
                if (!nameQ.exec() || !nameQ.next())
                    continue;

                QString alias = nameQ.value(0).toString();
                QString candidate = alias;
                int suffix = 2;
                while (usedAliases.contains(candidate)) {
                    candidate = QStringLiteral("%1 %2").arg(alias).arg(suffix++);
                }
                alias = candidate;
                usedAliases.insert(alias);

                QSqlQuery ins(m_db);
                ins.prepare(QStringLiteral(
                    "INSERT OR IGNORE INTO conversation_members(conversation_id, agent_id, "
                    "alias, is_coordinator, joined_at) VALUES(?, ?, ?, ?, ?)"));
                ins.addBindValue(convId);
                ins.addBindValue(agentId);
                ins.addBindValue(alias);
                ins.addBindValue(firstMember ? 1 : 0);  // first member becomes coordinator
                ins.addBindValue(now);
                if (!ins.exec()) {
                    qCWarning(verzetaDb) << "v4 migration: conversation_members insert failed:"
                                         << ins.lastError().text();
                }
                firstMember = false;
            }
        }
    }

    return true;
}

/**
 * @brief Returns a reference to the underlying QSqlDatabase.
 * @return QSqlDatabase reference.
 */
QSqlDatabase& DbManager::db() {
    return m_db;
}

/**
 * @brief Begins a database transaction.
 * @return true if transaction started successfully.
 */
bool DbManager::transaction() {
    return m_db.transaction();
}

/**
 * @brief Commits the current transaction.
 * @return true if commit succeeded.
 */
bool DbManager::commit() {
    return m_db.commit();
}

/**
 * @brief Rolls back the current transaction.
 * @return true if rollback succeeded.
 */
bool DbManager::rollback() {
    return m_db.rollback();
}

/**
 * @brief Returns whether the database is currently open and valid.
 * @return true if database is open.
 */
bool DbManager::isOpen() const {
    return m_db.isOpen();
}

/**
 * @brief Closes the database connection, releasing the SQLite file lock.
 * @sideeffects Releases SQLite file lock.
 */
void DbManager::close() {
    if (m_db.isOpen()) {
        m_db.close();
        qCInfo(verzetaDb) << "Database closed";
    }
    QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    // Release the engine lock AFTER the SQLite connection is closed so
    // no other process can race in before SQLite has finished flushing
    // its file-level locks. Destructor-driven via unique_ptr — the
    // QLockFile destructor calls unlock() automatically.
    m_engineLock.reset();
}

/**
 * @brief Returns the path to the database file.
 * @return Absolute path string.
 */
QString DbManager::databasePath() const {
    return m_path;
}

bool DbManager::isVectorSearchAvailable() const {
    return m_vectorSearchAvailable;
}

bool DbManager::loadVectorExtension(const QString& loadablePath) {
    m_vectorSearchAvailable = false;

#ifndef VERZETA_HAVE_SQLITE3
    Q_UNUSED(loadablePath)
    qCInfo(verzetaDb) << "DbManager: sqlite3 C API not linked — vector search "
                         "uses brute-force fallback";
    return false;
#else
    if (loadablePath.isEmpty()) {
        qCInfo(verzetaDb) << "DbManager: no sqlite-vec loadable provided — "
                             "vector search uses brute-force fallback";
        return false;
    }
    if (!m_db.isOpen()) {
        qCWarning(verzetaDb) << "DbManager::loadVectorExtension called before open()";
        return false;
    }
    if (!QFileInfo::exists(loadablePath)) {
        qCInfo(verzetaDb) << "DbManager: sqlite-vec loadable not found at" << loadablePath
                          << "— brute-force fallback";
        return false;
    }

    // Guard against an ABI mismatch between OUR sqlite3 and the one Qt's
    // QSQLITE driver was built against (e.g. a Qt that bundles its own).
    const QString driverVersion = [this]() {
        QSqlQuery q(m_db);
        if (q.exec(QStringLiteral("SELECT sqlite_version()")) && q.next()) {
            return q.value(0).toString();
        }
        return QString();
    }();
    const QString ourVersion = QString::fromLatin1(sqlite3_libversion());
    if (driverVersion.isEmpty() || driverVersion != ourVersion) {
        qCWarning(verzetaDb) << "DbManager: sqlite3 version mismatch (driver" << driverVersion
                             << "vs linked" << ourVersion
                             << ") — skipping vec0 load to avoid an "
                                "ABI mismatch; vector search uses brute-force fallback";
        return false;
    }

    const QVariant handleVar = m_db.driver()->handle();
    if (handleVar.isNull() || qstrcmp(handleVar.typeName(), "sqlite3*") != 0) {
        qCWarning(verzetaDb) << "DbManager: could not obtain sqlite3* handle — "
                                "brute-force fallback";
        return false;
    }
    sqlite3* handle = *static_cast<sqlite3* const*>(handleVar.constData());
    if (handle == nullptr) {
        qCWarning(verzetaDb) << "DbManager: null sqlite3 handle — brute-force fallback";
        return false;
    }

    if (sqlite3_enable_load_extension(handle, 1) != SQLITE_OK) {
        qCWarning(verzetaDb) << "DbManager: sqlite3_enable_load_extension failed — "
                                "brute-force fallback";
        return false;
    }

    char* errMsg = nullptr;
    const int rc =
        sqlite3_load_extension(handle, loadablePath.toUtf8().constData(), nullptr, &errMsg);
    // Re-disable extension loading immediately — defence in depth so untrusted
    // SQL can never load arbitrary extensions later.
    sqlite3_enable_load_extension(handle, 0);

    if (rc != SQLITE_OK) {
        qCWarning(verzetaDb) << "DbManager: sqlite-vec load failed:"
                             << (errMsg ? errMsg : "(no message)") << "— brute-force fallback";
        sqlite3_free(errMsg);
        return false;
    }
    sqlite3_free(errMsg);

    // Confirm the extension actually registered.
    QSqlQuery verify(m_db);
    if (!verify.exec(QStringLiteral("SELECT vec_version()")) || !verify.next()) {
        qCWarning(verzetaDb) << "DbManager: vec_version() unavailable after load — "
                                "brute-force fallback";
        return false;
    }

    m_vectorSearchAvailable = true;
    qCInfo(verzetaDb) << "DbManager: sqlite-vec" << verify.value(0).toString()
                      << "loaded — vector KNN enabled";
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Enables SQLite WAL mode and foreign key enforcement.
 * @sideeffects Sets PRAGMA journal_mode=WAL and PRAGMA foreign_keys=ON.
 */
void DbManager::configurePragmas() {
    QSqlQuery q(m_db);

    // WAL mode for better concurrent read performance
    if (!q.exec(QStringLiteral("PRAGMA journal_mode=WAL"))) {
        qCWarning(verzetaDb) << "PRAGMA journal_mode=WAL failed:" << q.lastError().text();
    }

    // Enforce foreign key constraints (off by default in SQLite)
    if (!q.exec(QStringLiteral("PRAGMA foreign_keys=ON"))) {
        qCWarning(verzetaDb) << "PRAGMA foreign_keys=ON failed:" << q.lastError().text();
    }

    // Synchronous = NORMAL for balanced safety/performance
    if (!q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"))) {
        qCWarning(verzetaDb) << "PRAGMA synchronous=NORMAL failed:" << q.lastError().text();
    }
}

/**
 * @brief Creates all tables and indexes for schema version 1.
 *        Must be called inside an active transaction.
 * @sideeffects Executes CREATE TABLE, CREATE INDEX, and CREATE VIRTUAL TABLE statements.
 */
void DbManager::applySchema() {
    QSqlQuery q(m_db);

    const QStringList statements = {
        // folders table (must be created before conversations due to FK reference)
        // folder_type: "regular" | "project" | "organization" (schema v2+)
        // goal, description, agent_ids: populated when folder_type != "regular"
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS folders (
                id          TEXT PRIMARY KEY,
                name        TEXT NOT NULL,
                parent_id   TEXT REFERENCES folders(id),
                created_at  INTEGER NOT NULL,
                folder_type TEXT NOT NULL DEFAULT 'regular'
                            CHECK(folder_type IN ('regular','project','organization')),
                goal        TEXT,
                description TEXT,
                agent_ids   TEXT
            )
        )"),

        // agents table — named agents with roles (schema v2+)
        // is_coordinator: schema v4+; used by group-chat UI to flag agents
        // suitable as group coordinators (Team Lead, Manager, etc.)
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS agents (
                id              TEXT PRIMARY KEY,
                name            TEXT NOT NULL UNIQUE,
                description     TEXT,
                icon_name       TEXT,
                system_prompt   TEXT NOT NULL,
                default_pattern TEXT NOT NULL DEFAULT 'direct',
                model_provider  TEXT,
                model_name      TEXT,
                allowed_tools   TEXT,
                is_builtin      INTEGER NOT NULL DEFAULT 0,
                is_coordinator  INTEGER NOT NULL DEFAULT 0,
                created_at      INTEGER NOT NULL
            )
        )"),

        // project_members — per-folder agent memberships with aliases (schema v4+)
        // Each row represents one "seat" in a project. The same agent template
        // may be assigned multiple times with different aliases (e.g. two
        // Engineers named "Alice" and "Bob").
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS project_members (
                folder_id          TEXT NOT NULL REFERENCES folders(id) ON DELETE CASCADE,
                agent_id           TEXT NOT NULL REFERENCES agents(id),
                alias              TEXT NOT NULL,
                is_coordinator     INTEGER NOT NULL DEFAULT 0,
                joined_at          INTEGER NOT NULL,
                added_by_kind      TEXT NOT NULL DEFAULT 'user',
                added_by_agent_id  TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (folder_id, alias)
            )
        )"),

        // conversation_members — per-conversation memberships for group chats
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS conversation_members (
                conversation_id    TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
                agent_id           TEXT NOT NULL REFERENCES agents(id),
                alias              TEXT NOT NULL,
                is_coordinator     INTEGER NOT NULL DEFAULT 0,
                joined_at          INTEGER NOT NULL,
                added_by_kind      TEXT NOT NULL DEFAULT 'user',
                added_by_agent_id  TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (conversation_id, alias)
            )
        )"),

        // conversations table
        // primary_agent_id: nullable — legacy conversations have no assigned agent
        // is_group: 0 for 1:1 chat, 1 for group chat with multiple agents
        // group_agent_ids: JSON array of agent UUIDs (group chats only)
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS conversations (
                id              TEXT PRIMARY KEY,
                title           TEXT NOT NULL,
                folder_id       TEXT REFERENCES folders(id),
                created_at      INTEGER NOT NULL,
                updated_at      INTEGER NOT NULL,
                system_prompt   TEXT,
                llm_config      TEXT,
                token_total     INTEGER DEFAULT 0,
                primary_agent_id TEXT REFERENCES agents(id),
                is_group        INTEGER NOT NULL DEFAULT 0,
                group_agent_ids TEXT
            )
        )"),

        // messages table
        // agent_id: which agent template produced this message (schema v2+)
        // member_alias: the alias used at send time (schema v4+) — because the
        //               same agent template may appear multiple times in a
        //               group chat under different aliases.
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS messages (
                id              TEXT PRIMARY KEY,
                conversation_id TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
                role            TEXT NOT NULL CHECK(role IN ('user','assistant','system','tool')),
                content         TEXT NOT NULL,
                content_html    TEXT,
                created_at      INTEGER NOT NULL,
                token_count     INTEGER DEFAULT 0,
                model_used      TEXT,
                finish_reason   TEXT,
                metadata        TEXT,
                agent_id        TEXT REFERENCES agents(id),
                member_alias    TEXT
            )
        )"),

        // attachments table
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS attachments (
                id          TEXT PRIMARY KEY,
                message_id  TEXT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
                type        TEXT NOT NULL CHECK(type IN ('image','audio','file','code')),
                filename    TEXT NOT NULL,
                mime_type   TEXT,
                data_path   TEXT,
                data_inline BLOB,
                created_at  INTEGER NOT NULL
            )
        )"),

        // tool_calls table
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS tool_calls (
                id           TEXT PRIMARY KEY,
                message_id   TEXT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
                tool_name    TEXT NOT NULL,
                arguments    TEXT NOT NULL,
                result       TEXT,
                status       TEXT NOT NULL CHECK(status IN ('pending','running','success','error')),
                started_at   INTEGER,
                completed_at INTEGER,
                plan_step_id TEXT
            )
        )"),

        // embeddings table
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS embeddings (
                id          TEXT PRIMARY KEY,
                source_id   TEXT NOT NULL,
                source_type TEXT NOT NULL CHECK(source_type IN ('message','document')),
                chunk_text  TEXT NOT NULL,
                embedding   BLOB NOT NULL,
                model_used  TEXT NOT NULL,
                created_at  INTEGER NOT NULL
            )
        )"),

        // documents table
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS documents (
                id          TEXT PRIMARY KEY,
                name        TEXT NOT NULL,
                path        TEXT,
                content     TEXT NOT NULL,
                mime_type   TEXT,
                created_at  INTEGER NOT NULL,
                indexed_at  INTEGER
            )
        )"),

        // settings table
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS settings (
                key        TEXT PRIMARY KEY,
                value      TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            )
        )"),

        // Full-text search virtual table (FTS5)
        QStringLiteral(R"(
            CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
                content,
                message_id UNINDEXED,
                conversation_id UNINDEXED
            )
        )"),

        // Performance indexes
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_conv "
                       "ON messages(conversation_id, created_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_attachments_msg ON attachments(message_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tool_calls_msg ON tool_calls(message_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_embeddings_source "
                       "ON embeddings(source_id, source_type)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_conversations_folder "
                       "ON conversations(folder_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id)"),
    };

    for (const QString& stmt : statements) {
        if (!q.exec(stmt)) {
            qCCritical(verzetaDb) << "Schema creation failed:" << q.lastError().text();
            // Rollback is handled by the caller (runMigrations)
            return;
        }
    }

    qCInfo(verzetaDb) << "Schema version 1 applied successfully";
}

/**
 * @brief Adds state-driven execution loop tables (schema v4 → v5).
 *
 * Tables:
 *   agent_plans:    one row per task plan, bound to a home conversation.
 *   plan_steps:     ordered child rows, one per step owned by an alias.
 *   step_artifacts: submit_result outputs, one row per artifact (rejected
 *                   artifacts are kept for audit; rework creates a new row).
 *
 * Timestamps are stored as INTEGER (ms-since-epoch), matching the existing
 * schema conventions.
 */
bool DbManager::applySchemaV5() {
    QSqlQuery q(m_db);

    const QStringList ddl = {
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS agent_plans (
                id                     TEXT PRIMARY KEY,
                conversation_id        TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
                project_folder_id      TEXT,
                organization_folder_id TEXT,
                goal                   TEXT NOT NULL,
                status                 TEXT NOT NULL DEFAULT 'planning',
                started_by             TEXT NOT NULL,
                created_at             INTEGER NOT NULL,
                updated_at             INTEGER NOT NULL,
                turns_used             INTEGER NOT NULL DEFAULT 0,
                heartbeats_used        INTEGER NOT NULL DEFAULT 0
            )
        )"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_agent_plans_conv "
                       "ON agent_plans(conversation_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_agent_plans_status "
                       "ON agent_plans(status)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_agent_plans_project "
                       "ON agent_plans(project_folder_id)"),

        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS plan_steps (
                id                    TEXT PRIMARY KEY,
                plan_id               TEXT NOT NULL REFERENCES agent_plans(id) ON DELETE CASCADE,
                ordering              INTEGER NOT NULL,
                title                 TEXT NOT NULL,
                description           TEXT,
                owner_alias           TEXT NOT NULL,
                acceptance_criteria   TEXT NOT NULL,
                status                TEXT NOT NULL DEFAULT 'pending',
                rejection_count       INTEGER NOT NULL DEFAULT 0,
                tool_retry_count      INTEGER NOT NULL DEFAULT 0,
                executor_turns_used   INTEGER NOT NULL DEFAULT 0,
                last_rejection_reason TEXT,
                created_at            INTEGER NOT NULL,
                updated_at            INTEGER NOT NULL
            )
        )"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_plan_steps_plan "
                       "ON plan_steps(plan_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_plan_steps_plan_status "
                       "ON plan_steps(plan_id, status)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_plan_steps_owner "
                       "ON plan_steps(owner_alias, status)"),

        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS step_artifacts (
                id                 TEXT PRIMARY KEY,
                step_id            TEXT NOT NULL REFERENCES plan_steps(id) ON DELETE CASCADE,
                artifact_type      TEXT NOT NULL,
                file_path          TEXT,
                content            TEXT,
                summary            TEXT NOT NULL,
                submitted_by_alias TEXT NOT NULL,
                created_at         INTEGER NOT NULL,
                approved           INTEGER NOT NULL DEFAULT 0,
                critic_reasons     TEXT
            )
        )"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_step_artifacts_step "
                       "ON step_artifacts(step_id)"),
    };

    for (const QString& stmt : ddl) {
        if (!q.exec(stmt)) {
            qCCritical(verzetaDb) << "v5 migration failed:" << q.lastError().text()
                                  << "statement:" << stmt;
            return false;
        }
    }

    return true;
}

/**
 * @brief Adds message/tool-call separation column + cleans legacy data
 *        (schema v5 → v6).
 *
 * Three things happen, in order:
 *   1. ALTER TABLE messages ADD COLUMN turn_id TEXT (nullable).
 *   2. DELETE legacy rows that should never have been in messages:
 *      role='tool' rows (these were a hack; they belong in tool_calls)
 *      and orphan empty assistant rows whose finish_reason='tool_calls'
 *      with no associated tool_calls entry (the same hack's anchor).
 *      The user has explicitly approved this clean wipe for the test
 *      environment.
 *   3. Backfill turn_id by walking each conversation chronologically:
 *      every user message opens a new turn (turn_id = its own id);
 *      subsequent assistant/system rows inherit until the next user
 *      message. Implemented in C++ rather than pure SQL so the logic is
 *      traceable and testable.
 *   4. CREATE INDEX idx_messages_turn so the per-turn UI grouping
 *      query is O(log n) rather than a full scan.
 */
bool DbManager::applySchemaV6() {
    QSqlQuery q(m_db);

    // --- Step 1: add the column (idempotent if already present) -----
    if (!q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN turn_id TEXT"))) {
        const QString errText = q.lastError().text().toLower();
        if (!errText.contains(QStringLiteral("duplicate column"))) {
            qCCritical(verzetaDb) << "v6 migration: ALTER TABLE failed:" << q.lastError().text();
            return false;
        }
    }

    // --- Step 2a: delete legacy role='tool' rows --------------------
    if (!q.exec(QStringLiteral("DELETE FROM messages WHERE role = 'tool'"))) {
        qCCritical(verzetaDb) << "v6 migration: DELETE role='tool' failed:" << q.lastError().text();
        return false;
    }
    const int toolDeletes = q.numRowsAffected();

    // --- Step 2b: delete orphan empty-assistant tool-call anchors ---
    if (!q.exec(QStringLiteral("DELETE FROM messages "
                               "WHERE role = 'assistant' "
                               "  AND TRIM(COALESCE(content, '')) = '' "
                               "  AND finish_reason = 'tool_calls' "
                               "  AND NOT EXISTS ( "
                               "      SELECT 1 FROM tool_calls tc "
                               "      WHERE tc.message_id = messages.id "
                               "  )"))) {
        qCCritical(verzetaDb) << "v6 migration: DELETE empty assistant orphans failed:"
                              << q.lastError().text();
        return false;
    }
    const int orphanDeletes = q.numRowsAffected();

    // --- Step 3: backfill turn_id -----------------------------------
    //
    // Strategy: walk every conversation in chronological order, track
    // the most recent user message id, set turn_id on every row to
    // that id. User messages get turn_id = their own id (self-anchor).
    // Rows that arrive before any user message in their conversation
    // (extremely rare — only happens with seeded system prompts) get
    // a synthesized turn_id so they're never NULL.
    {
        QSqlQuery convQ(m_db);
        if (!convQ.exec(QStringLiteral("SELECT id FROM conversations"))) {
            qCCritical(verzetaDb) << "v6 migration: convQ failed:" << convQ.lastError().text();
            return false;
        }
        int backfilled = 0;
        while (convQ.next()) {
            const QString convId = convQ.value(0).toString();

            QSqlQuery msgQ(m_db);
            msgQ.prepare(QStringLiteral("SELECT id, role FROM messages "
                                        "WHERE conversation_id = ? "
                                        "  AND (turn_id IS NULL OR turn_id = '') "
                                        "ORDER BY created_at ASC"));
            msgQ.addBindValue(convId);
            if (!msgQ.exec()) {
                qCWarning(verzetaDb) << "v6 migration: msgQ failed for conv" << convId << ":"
                                     << msgQ.lastError().text();
                continue;
            }

            QString currentTurn;
            QSqlQuery upd(m_db);
            upd.prepare(QStringLiteral("UPDATE messages SET turn_id = ? WHERE id = ?"));

            while (msgQ.next()) {
                const QString id = msgQ.value(0).toString();
                const QString role = msgQ.value(1).toString();
                if (role == QStringLiteral("user")) {
                    currentTurn = id;
                } else if (currentTurn.isEmpty()) {
                    // Pre-user system or assistant row — anchor it on
                    // its own id so turn_id is never null.
                    currentTurn = id;
                }
                upd.bindValue(0, currentTurn);
                upd.bindValue(1, id);
                if (!upd.exec()) {
                    qCWarning(verzetaDb)
                        << "v6 migration: row update failed:" << upd.lastError().text();
                    continue;
                }
                ++backfilled;
            }
        }
        qCInfo(verzetaDb) << "v6 migration: backfilled turn_id on" << backfilled << "rows";
    }

    // --- Step 4: create the per-turn index --------------------------
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_turn "
                               "ON messages(turn_id, agent_id)"))) {
        qCWarning(verzetaDb) << "v6 migration: CREATE INDEX idx_messages_turn failed:"
                             << q.lastError().text();
        // Non-fatal — the index is an optimization, queries still work.
    }

    qCInfo(verzetaDb) << "v6 cleanup: deleted" << toolDeletes << "legacy role='tool' rows,"
                      << orphanDeletes << "orphan empty-assistant anchors";
    return true;
}

bool DbManager::applySchemaV7() {
    QSqlQuery q(m_db);

    // Add plan_step_id to tool_calls. Nullable — tool calls that fire
    // outside of a task step simply leave it NULL. Existing rows get
    // NULL too; the lineage is filled in going forward.
    //
    // Note: SQLite ALTER TABLE ADD COLUMN cannot declare an FK REFERENCES
    // constraint on a column added to an existing table — you can only
    // get a declarative FK by rebuilding the table. For now we store
    // plan_step_id as plain TEXT and rely on ON DELETE behaviour being
    // handled by application code (PlanService::deletePlan deletes the
    // whole plan + steps cascade, and those cascades happen via the
    // already-existing plan_steps FK, so orphan tool_calls rows are
    // acceptable as they reference a dead step id). A full table
    // rebuild migration is deferred to v8 if FK enforcement is needed.
    if (!q.exec(QStringLiteral("ALTER TABLE tool_calls ADD COLUMN plan_step_id TEXT"))) {
        const QString errText = q.lastError().text().toLower();
        if (!errText.contains(QStringLiteral("duplicate column"))) {
            qCCritical(verzetaDb) << "v7 migration: ALTER TABLE tool_calls failed:"
                                  << q.lastError().text();
            return false;
        }
    }

    // Index for "which tool calls did this step run" lookups.
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tool_calls_plan_step "
                               "ON tool_calls(plan_step_id)"))) {
        qCWarning(verzetaDb) << "v7 migration: CREATE INDEX idx_tool_calls_plan_step failed:"
                             << q.lastError().text();
        // Non-fatal — queries still work.
    }
    return true;
}

bool DbManager::applySchemaV8() {
    QSqlQuery q(m_db);

    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS canvas_artifacts ("
                               "  id              TEXT PRIMARY KEY,"
                               "  conversation_id TEXT NOT NULL,"
                               "  filename        TEXT NOT NULL,"
                               "  language        TEXT NOT NULL,"
                               "  content         TEXT NOT NULL,"
                               "  revision        INTEGER NOT NULL DEFAULT 0,"
                               "  is_archived     INTEGER NOT NULL DEFAULT 0,"
                               "  source_msg_id   TEXT,"
                               "  created_at      TEXT NOT NULL,"
                               "  updated_at      TEXT NOT NULL,"
                               "  FOREIGN KEY (conversation_id) REFERENCES conversations(id) "
                               "    ON DELETE CASCADE"
                               ")"))) {
        qCCritical(verzetaDb) << "v8 migration: CREATE TABLE canvas_artifacts failed:"
                              << q.lastError().text();
        return false;
    }

    // Composite index keyed for both `activeCanvasFor(convId)` (which
    // wants the highest-updated_at non-archived row) and
    // `historyForConversation(convId)` (which wants every row in
    // reverse-chronological order). The leading is_archived column
    // means SQLite can satisfy the active lookup with an index seek
    // instead of a scan.
    if (!q.exec(
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_canvas_active "
                           "ON canvas_artifacts(conversation_id, is_archived, updated_at DESC)"))) {
        qCWarning(verzetaDb) << "v8 migration: CREATE INDEX idx_canvas_active failed:"
                             << q.lastError().text();
        // Non-fatal — queries still work without the index.
    }

    return true;
}

/**
 * @brief Heartbeat subagent infrastructure tables.
 *
 * Creates heartbeat_configs (per-(agent, scope, alias) row) and
 * heartbeat_reports (per-Tier-1-run audit + report storage) plus their
 * supporting indexes. See db-manager.h applySchemaV10 doc-comment for
 * the full identity / status-field rationale.
 */
bool DbManager::applySchemaV10() {
    QSqlQuery q(m_db);

    // heartbeat_configs — one row per (agent, scope, alias) tuple.
    //
    // Why scope_id is NOT a foreign key: the column is polymorphic
    // (references conversations.id when scope_type starts with
    // 'conversation_', references folders.id when scope_type='folder').
    // SQLite has no native polymorphic FK; cleanup of orphan rows on
    // conversation/folder deletion happens in the application layer
    // (HeartbeatConfigService listens for the relevant deletion signals).
    if (!q.exec(QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS heartbeat_configs (
                id                                  TEXT PRIMARY KEY,
                agent_id                            TEXT NOT NULL
                                                    REFERENCES agents(id) ON DELETE CASCADE,
                scope_type                          TEXT NOT NULL
                                                    CHECK(scope_type IN
                                                          ('conversation_1to1',
                                                           'conversation_group',
                                                           'folder')),
                scope_id                            TEXT NOT NULL,
                alias                               TEXT NOT NULL DEFAULT '',
                enabled                             INTEGER NOT NULL DEFAULT 0,
                schedule                            TEXT NOT NULL DEFAULT '',
                goal                                TEXT NOT NULL DEFAULT '',
                surface_criteria                    TEXT NOT NULL DEFAULT '',
                max_runs_per_day                    INTEGER NOT NULL DEFAULT 24,
                auto_surface_target_conversation_id TEXT NOT NULL DEFAULT '',
                self_config_allowed                 INTEGER NOT NULL DEFAULT 0,
                last_fire_at                        TEXT,
                last_fire_outcome                   TEXT,
                created_at                          TEXT NOT NULL,
                updated_at                          TEXT NOT NULL,
                UNIQUE(agent_id, scope_type, scope_id, alias)
            )
        )"))) {
        qCCritical(verzetaDb) << "v10 migration: CREATE TABLE heartbeat_configs failed:"
                              << q.lastError().text();
        return false;
    }

    // Scope-lookup index — drives "list all heartbeat configs in this
    // conversation" (HB activity overlay) and "list all heartbeat
    // configs in this folder" (project overlay) queries.
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_heartbeat_configs_scope "
                               "ON heartbeat_configs(scope_type, scope_id)"))) {
        qCWarning(verzetaDb) << "v10 migration: CREATE INDEX idx_heartbeat_configs_scope failed:"
                             << q.lastError().text();
    }

    // Scheduler-tick index — the queue's nextFireTime() sweep walks
    // enabled=1 rows ordered by last_fire_at to decide which configs
    // are due. With this index the sweep is O(log N + k) where k is
    // the number of rows actually due, instead of O(N) full-table scan.
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_heartbeat_configs_enabled "
                               "ON heartbeat_configs(enabled, last_fire_at)"))) {
        qCWarning(verzetaDb) << "v10 migration: CREATE INDEX idx_heartbeat_configs_enabled failed:"
                             << q.lastError().text();
    }

    // heartbeat_reports — one row per Tier-1 subagent invocation.
    if (!q.exec(QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS heartbeat_reports (
                id                       TEXT PRIMARY KEY,
                config_id                TEXT NOT NULL
                                         REFERENCES heartbeat_configs(id) ON DELETE CASCADE,
                started_at               TEXT NOT NULL,
                completed_at             TEXT,
                outcome                  TEXT NOT NULL DEFAULT 'pending',
                title                    TEXT NOT NULL DEFAULT '',
                body                     TEXT NOT NULL DEFAULT '',
                summary                  TEXT NOT NULL DEFAULT '',
                parent_review_status     TEXT NOT NULL DEFAULT 'pending',
                surface_status           TEXT NOT NULL DEFAULT 'pending',
                surfaced_message_id      TEXT NOT NULL DEFAULT '',
                error                    TEXT NOT NULL DEFAULT ''
            )
        )"))) {
        qCCritical(verzetaDb) << "v10 migration: CREATE TABLE heartbeat_reports failed:"
                              << q.lastError().text();
        return false;
    }

    // Per-config history index — drives the activity overlay's
    // per-config "show me the last N runs" query.
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_heartbeat_reports_config_started "
                               "ON heartbeat_reports(config_id, started_at DESC)"))) {
        qCWarning(verzetaDb)
            << "v10 migration: CREATE INDEX idx_heartbeat_reports_config_started failed:"
            << q.lastError().text();
    }

    // Pending-review index — drives the cross-conv "any reports awaiting
    // parent review" sweep. parent_review_status='pending' is the
    // selectivity filter (most rows are 'reviewed' or 'not_reviewable_yet').
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_heartbeat_reports_pending_review "
                               "ON heartbeat_reports(parent_review_status, started_at DESC)"))) {
        qCWarning(verzetaDb)
            << "v10 migration: CREATE INDEX idx_heartbeat_reports_pending_review failed:"
            << q.lastError().text();
    }

    return true;
}

/**
 * @brief Schema v11: Heartbeat self-config audit
 *        log + AgentEditor "default heartbeat suggestion" pre-fill
 *        columns. See db-manager.h applySchemaV11 doc-comment for
 *        the full rationale.
 */
bool DbManager::applySchemaV11() {
    QSqlQuery q(m_db);

    // heartbeat_config_changes — append-only audit row per mutation
    // of a heartbeat_configs row.
    //
    // source ∈ {'agent', 'user'} — agent for self-config tool calls
    // (the agent invoked set_heartbeat_goal / etc.), user for any
    // mutation made via the user-facing UI surfaces (AddChatMember-
    // Dialog, RightSettingsPanel, FolderSettingsDialog).
    if (!q.exec(QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS heartbeat_config_changes (
                id          TEXT PRIMARY KEY,
                config_id   TEXT NOT NULL
                            REFERENCES heartbeat_configs(id) ON DELETE CASCADE,
                changed_at  TEXT NOT NULL,
                field       TEXT NOT NULL,
                old_value   TEXT NOT NULL DEFAULT '',
                new_value   TEXT NOT NULL DEFAULT '',
                source      TEXT NOT NULL CHECK(source IN ('agent', 'user'))
            )
        )"))) {
        qCCritical(verzetaDb) << "v11 migration: CREATE TABLE heartbeat_config_changes failed:"
                              << q.lastError().text();
        return false;
    }

    // Per-config audit history index — drives the diagnostics tab's
    // "history of self-config changes for this config" view, plus
    // the global "recent self-config audit" sweep.
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_heartbeat_config_changes_cfg "
                               "ON heartbeat_config_changes(config_id, changed_at DESC)"))) {
        qCWarning(verzetaDb)
            << "v11 migration: CREATE INDEX idx_heartbeat_config_changes_cfg failed:"
            << q.lastError().text();
    }

    // Three optional hint columns on agents — pre-fill the
    // AddChatMemberDialog heartbeat expander when the user picks
    // this agent template. Empty default = no pre-fill, which is
    // the historical behaviour pre-H7.
    const QStringList alters = {
        QStringLiteral("ALTER TABLE agents ADD COLUMN default_heartbeat_goal "
                       "TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE agents ADD COLUMN default_heartbeat_schedule "
                       "TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE agents ADD COLUMN default_heartbeat_surface_criteria "
                       "TEXT NOT NULL DEFAULT ''"),
    };
    for (const QString& alterSql : alters) {
        if (!q.exec(alterSql)) {
            // Tolerate "duplicate column" — re-running the migration
            // on a partially-applied DB should be safe. Other errors
            // are real failures.
            const QString errText = q.lastError().text();
            if (errText.contains(QStringLiteral("duplicate column"), Qt::CaseInsensitive)) {
                continue;
            }
            qCCritical(verzetaDb) << "v11 migration: ALTER TABLE agents failed:" << errText
                                  << "stmt:" << alterSql;
            return false;
        }
    }

    return true;
}

/**
 * @brief Schema v12 adds added_by_kind +
 *        added_by_agent_id to project_members AND conversation_members.
 *
 *        Defaults: 'user' / '' on existing rows, so memberships that
 *        predate this migration are protected from agent-driven
 *        removal by the
 *        new restriction in remove_project_member. Purely additive;
 *        every existing INSERT path that doesn't pass the columns
 *        gets the defaults (UI writes unchanged).
 */
bool DbManager::applySchemaV12() {
    QSqlQuery q(m_db);

    const QStringList alters = {
        QStringLiteral("ALTER TABLE project_members ADD COLUMN added_by_kind "
                       "TEXT NOT NULL DEFAULT 'user'"),
        QStringLiteral("ALTER TABLE project_members ADD COLUMN added_by_agent_id "
                       "TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE conversation_members ADD COLUMN added_by_kind "
                       "TEXT NOT NULL DEFAULT 'user'"),
        QStringLiteral("ALTER TABLE conversation_members ADD COLUMN added_by_agent_id "
                       "TEXT NOT NULL DEFAULT ''"),
    };
    for (const QString& stmt : alters) {
        if (!q.exec(stmt)) {
            const QString errText = q.lastError().text();
            if (errText.contains(QStringLiteral("duplicate column"), Qt::CaseInsensitive)) {
                continue;
            }
            qCCritical(verzetaDb) << "v12 migration: ALTER TABLE failed:" << errText
                                  << "stmt:" << stmt;
            return false;
        }
    }
    return true;
}

/**
 * @brief Schema v13: polls and voting.
 *
 * Three new tables:
 *
 *   polls(
 *     id              TEXT PK,
 *     conversation_id TEXT FK conversations(id) CASCADE,
 *     creator_kind    TEXT  CHECK in ('agent','user'),
 *     creator_alias   TEXT,
 *     question        TEXT,
 *     mode            TEXT  CHECK in ('single','multi','ranked')  [v1 rejects 'ranked'],
 *     status          TEXT  CHECK in ('open','closed') DEFAULT 'open',
 *     created_at      TEXT,
 *     closes_at       TEXT  nullable,
 *     closed_at       TEXT  nullable
 *   )
 *
 *   poll_options(
 *     id        TEXT PK,
 *     poll_id   TEXT FK polls(id) CASCADE,
 *     ordering  INTEGER,
 *     text      TEXT,
 *     UNIQUE(poll_id, ordering)
 *   )
 *
 *   poll_votes(
 *     id          TEXT PK,
 *     poll_id     TEXT FK polls(id) CASCADE,
 *     voter_kind  TEXT  CHECK in ('agent','user'),
 *     voter_alias TEXT,
 *     option_id   TEXT FK poll_options(id) CASCADE,
 *     rank        INTEGER nullable  [reserved for future ranked mode],
 *     cast_at     TEXT,
 *     UNIQUE(poll_id, voter_alias, option_id)
 *   )
 *
 * Indexes: idx_polls_conv_open on polls(conversation_id, status,
 * created_at DESC) and idx_poll_votes_poll on poll_votes(poll_id).
 *
 * Auto-close is LAZY: pollResults() detects past-closes_at and
 * promotes 'open' → 'closed' on read. No background sweep.
 */
bool DbManager::applySchemaV13() {
    QSqlQuery q(m_db);

    const QStringList statements = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS polls ("
            "    id              TEXT PRIMARY KEY,"
            "    conversation_id TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,"
            "    creator_kind    TEXT NOT NULL CHECK(creator_kind IN ('agent','user')),"
            "    creator_alias   TEXT NOT NULL,"
            "    question        TEXT NOT NULL,"
            "    mode            TEXT NOT NULL CHECK(mode IN ('single','multi','ranked')) DEFAULT "
            "'single',"
            "    status          TEXT NOT NULL CHECK(status IN ('open','closed')) DEFAULT 'open',"
            "    created_at      TEXT NOT NULL,"
            "    closes_at       TEXT,"
            "    closed_at       TEXT"
            ")"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_polls_conv_open "
                       "ON polls(conversation_id, status, created_at DESC)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS poll_options ("
                       "    id        TEXT PRIMARY KEY,"
                       "    poll_id   TEXT NOT NULL REFERENCES polls(id) ON DELETE CASCADE,"
                       "    ordering  INTEGER NOT NULL,"
                       "    text      TEXT NOT NULL,"
                       "    UNIQUE(poll_id, ordering)"
                       ")"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_poll_options_poll "
                       "ON poll_options(poll_id, ordering)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS poll_votes ("
            "    id          TEXT PRIMARY KEY,"
            "    poll_id     TEXT NOT NULL REFERENCES polls(id) ON DELETE CASCADE,"
            "    voter_kind  TEXT NOT NULL CHECK(voter_kind IN ('agent','user')),"
            "    voter_alias TEXT NOT NULL,"
            "    option_id   TEXT NOT NULL REFERENCES poll_options(id) ON DELETE CASCADE,"
            "    rank        INTEGER,"
            "    cast_at     TEXT NOT NULL,"
            "    UNIQUE(poll_id, voter_alias, option_id)"
            ")"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_poll_votes_poll "
                       "ON poll_votes(poll_id)"),
    };
    for (const QString& stmt : statements) {
        if (!q.exec(stmt)) {
            qCCritical(verzetaDb) << "v13 migration: statement failed:" << q.lastError().text()
                                  << "stmt:" << stmt;
            return false;
        }
    }
    return true;
}

/**
 * @brief Schema v14: per-member provider/model/tools
 *        override.
 *
 * Adds three columns to BOTH project_members and conversation_members:
 *   model_provider  TEXT NOT NULL DEFAULT '':   per-member provider override
 *   model_name      TEXT NOT NULL DEFAULT '':   per-member model override
 *   allowed_tools   TEXT NOT NULL DEFAULT '':   JSON array string ('' = no
 *                                              restriction), mirroring
 *                                              agents.allowed_tools
 *
 * Adds one nullable column to conversations:
 *   member_alias    TEXT:                       for a project/org 1:1 direct
 *                                              chat, the project member alias
 *                                              this chat is the channel for.
 *                                              NULL for group / plain /
 *                                              non-project conversations.
 *
 * Verify at source: after the ALTERs, this function
 * re-queries PRAGMA table_info and returns false if ANY expected column
 * is missing. runMigrations then rolls back and does NOT bump
 * schema_version, so a DB recorded at v14 with columns missing, the
 * failure class previously seen with the v12 columns, is structurally
 * impossible.
 */
bool DbManager::applySchemaV14() {
    QSqlQuery q(m_db);

    const QStringList alters = {
        QStringLiteral("ALTER TABLE project_members ADD COLUMN "
                       "model_provider TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE project_members ADD COLUMN "
                       "model_name TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE project_members ADD COLUMN "
                       "allowed_tools TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE conversation_members ADD COLUMN "
                       "model_provider TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE conversation_members ADD COLUMN "
                       "model_name TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE conversation_members ADD COLUMN "
                       "allowed_tools TEXT NOT NULL DEFAULT ''"),
        QStringLiteral("ALTER TABLE conversations ADD COLUMN member_alias TEXT"),
    };
    for (const QString& stmt : alters) {
        if (!q.exec(stmt)) {
            const QString errText = q.lastError().text();
            if (errText.contains(QStringLiteral("duplicate column"), Qt::CaseInsensitive)) {
                continue;  // idempotent — column already present
            }
            qCCritical(verzetaDb) << "v14 migration: ALTER TABLE failed:" << errText
                                  << "stmt:" << stmt;
            return false;
        }
    }

    // Verify-at-source — confirm every expected column is actually
    // present before this migration is allowed to commit.
    /**
     * @brief Name of one column the migration must end up with.
     *        Lookup-only struct.
     */
    struct ExpectedColumn {
        QString table;
        QString column;
    };
    const QList<ExpectedColumn> expected = {
        {QStringLiteral("project_members"), QStringLiteral("model_provider")},
        {QStringLiteral("project_members"), QStringLiteral("model_name")},
        {QStringLiteral("project_members"), QStringLiteral("allowed_tools")},
        {QStringLiteral("conversation_members"), QStringLiteral("model_provider")},
        {QStringLiteral("conversation_members"), QStringLiteral("model_name")},
        {QStringLiteral("conversation_members"), QStringLiteral("allowed_tools")},
        {QStringLiteral("conversations"), QStringLiteral("member_alias")},
    };
    for (const ExpectedColumn& e : expected) {
        QSqlQuery check(m_db);
        if (!check.exec(QStringLiteral("PRAGMA table_info(%1)").arg(e.table))) {
            qCCritical(verzetaDb) << "v14 migration: verify PRAGMA failed for" << e.table << ":"
                                  << check.lastError().text();
            return false;
        }
        bool found = false;
        while (check.next()) {
            if (check.value(1).toString() == e.column) {
                found = true;
                break;
            }
        }
        if (!found) {
            qCCritical(verzetaDb) << "v14 migration: verify-at-source FAILED — column"
                                  << (e.table + QLatin1Char('.') + e.column)
                                  << "missing after ALTER; refusing to bump schema_version";
            return false;
        }
    }
    return true;
}

/**
 * @brief Schema v15: unified audit trail.
 *
 * Creates the append-only `activity_log` table with its 13 columns and
 * 3 indexes:
 *
 *   activity_log(
 *     id                 TEXT PK,
 *     created_at         TEXT NOT NULL,   -- ISODateWithMs UTC
 *     project_folder_id  TEXT,            -- nullable; null for app-level
 *     conversation_id    TEXT,            -- nullable
 *     turn_id            TEXT,            -- nullable; reuses messages.turn_id
 *     actor_kind         TEXT NOT NULL CHECK in ('user','agent','system'),
 *                        -- Schema v18 widens this enum to also include
 *                        -- 'client' (paired-client-driven events such
 *                        -- as workspace.mount.*). See
 *                        -- applySchemaV18.
 *     actor_alias        TEXT NOT NULL,
 *     actor_agent_id     TEXT,            -- nullable
 *     actor_client_id    TEXT,            -- nullable; "" for local
 *     event_type         TEXT NOT NULL,   -- stable enum-string
 *     tool_name          TEXT,            -- nullable (first-class column)
 *     event_summary      TEXT NOT NULL,   -- one human-readable line
 *     event_detail       TEXT             -- nullable JSON payload
 *   )
 *
 *   idx_activity_project (project_folder_id, created_at DESC)
 *   idx_activity_conv    (conversation_id, created_at DESC)
 *   idx_activity_turn    (turn_id)
 *
 * No FK CASCADE on project_folder_id / conversation_id / turn_id /
 * actor_agent_id / actor_client_id. Audit rows survive the deletion
 * of their tagged conversation / project so the user can still see
 * "what happened in the project I just archived".
 *
 * Verify at source: after CREATE, re-queries
 * PRAGMA table_info(activity_log) and returns false if ANY expected
 * column is missing. runMigrations then rolls back and does NOT bump
 * schema_version, so a DB recorded at v15 with columns missing, the
 * failure class previously seen with the v12 columns, is structurally
 * impossible.
 */
bool DbManager::applySchemaV15() {
    QSqlQuery q(m_db);

    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS activity_log ("
                       "    id                TEXT PRIMARY KEY,"
                       "    created_at        TEXT NOT NULL,"
                       "    project_folder_id TEXT,"
                       "    conversation_id   TEXT,"
                       "    turn_id           TEXT,"
                       "    actor_kind        TEXT NOT NULL "
                       "                      CHECK(actor_kind IN ('user','agent','system')),"
                       "    actor_alias       TEXT NOT NULL,"
                       "    actor_agent_id    TEXT,"
                       "    actor_client_id   TEXT,"
                       "    event_type        TEXT NOT NULL,"
                       "    tool_name         TEXT,"
                       "    event_summary     TEXT NOT NULL,"
                       "    event_detail      TEXT"
                       ")"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_project "
                       "ON activity_log(project_folder_id, created_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_conv "
                       "ON activity_log(conversation_id, created_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_turn "
                       "ON activity_log(turn_id)"),
    };
    for (const QString& stmt : statements) {
        if (!q.exec(stmt)) {
            qCCritical(verzetaDb) << "v15 migration: statement failed:" << q.lastError().text()
                                  << "stmt:" << stmt;
            return false;
        }
    }

    const QStringList expectedColumns = {
        QStringLiteral("id"),
        QStringLiteral("created_at"),
        QStringLiteral("project_folder_id"),
        QStringLiteral("conversation_id"),
        QStringLiteral("turn_id"),
        QStringLiteral("actor_kind"),
        QStringLiteral("actor_alias"),
        QStringLiteral("actor_agent_id"),
        QStringLiteral("actor_client_id"),
        QStringLiteral("event_type"),
        QStringLiteral("tool_name"),
        QStringLiteral("event_summary"),
        QStringLiteral("event_detail"),
    };
    QSet<QString> present;
    QSqlQuery check(m_db);
    if (!check.exec(QStringLiteral("PRAGMA table_info(activity_log)"))) {
        qCCritical(verzetaDb) << "v15 migration: verify PRAGMA failed:" << check.lastError().text();
        return false;
    }
    while (check.next()) {
        present.insert(check.value(1).toString());
    }
    for (const QString& col : expectedColumns) {
        if (!present.contains(col)) {
            qCCritical(verzetaDb) << "v15 migration: verify-at-source FAILED — column"
                                  << QStringLiteral("activity_log.%1").arg(col)
                                  << "missing after CREATE; refusing to bump schema_version";
            return false;
        }
    }
    return true;
}

/**
 * @brief Adds `messages.thinking_content` for the display-only reasoning
 *        sidecar.  NOT NULL with DEFAULT '' so existing rows backfill to
 *        empty without a data migration step and any INSERT that forgets
 *        to bind gets an explicit empty string rather than NULL.
 *
 * Verifies the column landed via PRAGMA table_info before returning true
 * so a partial ALTER does not bump schema_version (same self-heal
 * pattern v15 carries forward).
 */
bool DbManager::applySchemaV16() {
    QSqlQuery q(m_db);

    if (!q.exec(QStringLiteral("ALTER TABLE messages "
                               "ADD COLUMN thinking_content TEXT NOT NULL DEFAULT ''"))) {
        qCCritical(verzetaDb) << "v16 migration: ALTER TABLE messages ADD thinking_content "
                                 "failed:"
                              << q.lastError().text();
        return false;
    }

    // Verify-at-source — confirm the column actually landed before
    // schema_version is bumped.  Matches the v14 / v15 pattern.
    QSqlQuery check(m_db);
    if (!check.exec(QStringLiteral("PRAGMA table_info(messages)"))) {
        qCCritical(verzetaDb) << "v16 migration: PRAGMA table_info(messages) failed:"
                              << check.lastError().text();
        return false;
    }
    bool found = false;
    while (check.next()) {
        if (check.value(1).toString() == QStringLiteral("thinking_content")) {
            found = true;
            break;
        }
    }
    if (!found) {
        qCCritical(verzetaDb) << "v16 migration: ALTER TABLE reported success but "
                                 "thinking_content column is not present in PRAGMA";
        return false;
    }

    return true;
}

/**
 * @brief Creates the `folder_mounts` table, the metadata-only substrate
 *        for client-owned virtual workspace mounts. Verify-at-source:
 *        PRAGMA re-checks the column set after CREATE so a half-
 *        applied migration cannot advance `schema_version`.
 *
 * Column shape:
 *   folder_id        TEXT PRIMARY KEY REFERENCES folders(id) ON DELETE CASCADE
 *   mount_id         TEXT NOT NULL  -- stable UUID provided by the registering client
 *   client_id        TEXT NOT NULL  -- wire-auth client UUID (workspace owner)
 *   owner_label      TEXT NOT NULL  -- display string
 *   registered_at_ms INTEGER NOT NULL  -- epoch ms
 *   last_seen_ms     INTEGER NOT NULL  -- bumped on every successful op
 *   tree_json        TEXT NOT NULL DEFAULT '{}'
 *   blocklist_json   TEXT NOT NULL DEFAULT '[]'
 *   allowlist_json   TEXT NOT NULL DEFAULT '[]'
 *   permission_tier  TEXT NOT NULL DEFAULT 'ask'
 *                     CHECK (permission_tier IN ('ask','smart','bypass'))
 *   options_json     TEXT NOT NULL DEFAULT '{}'
 *
 * Index:
 *   idx_folder_mounts_client(client_id): supports per-client sweep
 *   (drop every mount owned by a wire-revoked client without a full
 *   table scan).
 */
bool DbManager::applySchemaV17() {
    QSqlQuery q(m_db);

    if (!q.exec(QStringLiteral("CREATE TABLE folder_mounts ("
                               "  folder_id        TEXT PRIMARY KEY "
                               "                       REFERENCES folders(id) ON DELETE CASCADE,"
                               "  mount_id         TEXT NOT NULL,"
                               "  client_id        TEXT NOT NULL,"
                               "  owner_label      TEXT NOT NULL,"
                               "  registered_at_ms INTEGER NOT NULL,"
                               "  last_seen_ms     INTEGER NOT NULL,"
                               "  tree_json        TEXT NOT NULL DEFAULT '{}',"
                               "  blocklist_json   TEXT NOT NULL DEFAULT '[]',"
                               "  allowlist_json   TEXT NOT NULL DEFAULT '[]',"
                               "  permission_tier  TEXT NOT NULL DEFAULT 'ask'"
                               "                       CHECK (permission_tier IN "
                               "                              ('ask','smart','bypass')),"
                               "  options_json     TEXT NOT NULL DEFAULT '{}'"
                               ")"))) {
        qCCritical(verzetaDb) << "v17 migration: CREATE TABLE folder_mounts failed:"
                              << q.lastError().text();
        return false;
    }

    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_folder_mounts_client "
                               "ON folder_mounts(client_id)"))) {
        qCWarning(verzetaDb) << "v17 migration: CREATE INDEX idx_folder_mounts_client failed:"
                             << q.lastError().text();
        // Non-fatal — queries still work without the index.
    }

    // Verify-at-source — confirm every column actually exists before
    // schema_version is bumped. Matches the v14 / v15 / v16 pattern.
    static const QStringList kExpected = {
        QStringLiteral("folder_id"),
        QStringLiteral("mount_id"),
        QStringLiteral("client_id"),
        QStringLiteral("owner_label"),
        QStringLiteral("registered_at_ms"),
        QStringLiteral("last_seen_ms"),
        QStringLiteral("tree_json"),
        QStringLiteral("blocklist_json"),
        QStringLiteral("allowlist_json"),
        QStringLiteral("permission_tier"),
        QStringLiteral("options_json"),
    };
    QSqlQuery check(m_db);
    if (!check.exec(QStringLiteral("PRAGMA table_info(folder_mounts)"))) {
        qCCritical(verzetaDb) << "v17 migration: PRAGMA table_info(folder_mounts) failed:"
                              << check.lastError().text();
        return false;
    }
    QSet<QString> seen;
    while (check.next())
        seen.insert(check.value(1).toString());
    for (const QString& col : kExpected) {
        if (!seen.contains(col)) {
            qCCritical(verzetaDb) << "v17 migration: CREATE TABLE reported success but" << col
                                  << "is not present in PRAGMA";
            return false;
        }
    }

    return true;
}

/**
 * @brief Schema v18: widen `activity_log.actor_kind`'s CHECK
 *        constraint to include `'client'`. See db-manager.h for the
 *        full design rationale + the rename-recreate-copy-drop
 *        recipe.
 *
 * SQLite cannot ALTER TABLE to modify a CHECK constraint; the
 * widening is structural. Verify-at-source: pre / post row count
 * parity AND a sqlite_master query confirming the new SQL contains
 * the literal `'client'` before the migration is allowed to bump
 * schema_version.
 */
bool DbManager::applySchemaV18() {
    QSqlQuery q(m_db);

    // Step 1: record current row count so the post-copy verify
    // can refuse to advance schema_version on a mismatched copy.
    qint64 preCount = -1;
    {
        QSqlQuery count(m_db);
        if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log"))) {
            qCCritical(verzetaDb) << "v18 migration: pre-copy COUNT(*) failed:"
                                  << count.lastError().text();
            return false;
        }
        if (count.next())
            preCount = count.value(0).toLongLong();
    }
    if (preCount < 0) {
        qCCritical(verzetaDb) << "v18 migration: pre-copy COUNT(*) returned no row";
        return false;
    }

    // Step 2: rename the existing table out of the way. The new
    // table will be created under the original name so callers
    // (AuditService, wire-session activity.* handlers) reference the
    // same path before and after the migration.
    if (!q.exec(QStringLiteral("ALTER TABLE activity_log RENAME TO activity_log_v17_backup"))) {
        qCCritical(verzetaDb) << "v18 migration: RENAME failed:" << q.lastError().text();
        return false;
    }

    // Step 3: create the new table with the widened CHECK enum.
    // Schema is otherwise byte-identical to applySchemaV15 — same
    // columns, same NOT NULL rules, same nullability on the
    // identity / scope columns.
    if (!q.exec(QStringLiteral("CREATE TABLE activity_log ("
                               "    id                TEXT PRIMARY KEY,"
                               "    created_at        TEXT NOT NULL,"
                               "    project_folder_id TEXT,"
                               "    conversation_id   TEXT,"
                               "    turn_id           TEXT,"
                               "    actor_kind        TEXT NOT NULL "
                               "                      CHECK(actor_kind IN "
                               "                            ('user','agent','system','client')),"
                               "    actor_alias       TEXT NOT NULL,"
                               "    actor_agent_id    TEXT,"
                               "    actor_client_id   TEXT,"
                               "    event_type        TEXT NOT NULL,"
                               "    tool_name         TEXT,"
                               "    event_summary     TEXT NOT NULL,"
                               "    event_detail      TEXT"
                               ")"))) {
        qCCritical(verzetaDb) << "v18 migration: CREATE TABLE activity_log (new CHECK) failed:"
                              << q.lastError().text();
        return false;
    }

    // Step 4: copy every existing row. Column order matches between
    // the renamed backup and the freshly-created table, so the bare
    // SELECT * is safe.
    if (!q.exec(QStringLiteral("INSERT INTO activity_log SELECT * FROM activity_log_v17_backup"))) {
        qCCritical(verzetaDb) << "v18 migration: INSERT FROM SELECT failed:"
                              << q.lastError().text();
        return false;
    }

    // Step 5: row-count parity guard. The new CHECK is a SUPERSET of
    // the old enum, so every row should re-insert; a mismatch means
    // either the schema diverged elsewhere or a row violates some
    // OTHER constraint we did not preserve.
    qint64 postCount = -1;
    {
        QSqlQuery count(m_db);
        if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log"))) {
            qCCritical(verzetaDb) << "v18 migration: post-copy COUNT(*) failed:"
                                  << count.lastError().text();
            return false;
        }
        if (count.next())
            postCount = count.value(0).toLongLong();
    }
    if (postCount != preCount) {
        qCCritical(verzetaDb) << "v18 migration: row count mismatch — pre:" << preCount
                              << "post:" << postCount << "— refusing to bump schema_version";
        return false;
    }

    // Step 6: drop the renamed backup. This MUST happen after the
    // row-count gate because the rollback path needs the backup in
    // place if the migration fails before reaching here.
    if (!q.exec(QStringLiteral("DROP TABLE activity_log_v17_backup"))) {
        qCCritical(verzetaDb) << "v18 migration: DROP TABLE activity_log_v17_backup failed:"
                              << q.lastError().text();
        return false;
    }

    const QStringList indexStatements = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_project "
                       "ON activity_log(project_folder_id, created_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_conv "
                       "ON activity_log(conversation_id, created_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_turn "
                       "ON activity_log(turn_id)"),
    };
    for (const QString& stmt : indexStatements) {
        if (!q.exec(stmt)) {
            qCCritical(verzetaDb) << "v18 migration: CREATE INDEX failed:" << q.lastError().text()
                                  << "stmt:" << stmt;
            return false;
        }
    }

    // Step 8: verify-at-source — query sqlite_master and confirm the
    // new CHECK is structurally present in the table definition.
    // This catches the pathological case where the CREATE TABLE
    // somehow used the OLD enum (no migration path inside the
    // codebase does this today, but the guard is cheap).
    QSqlQuery verify(m_db);
    if (!verify.exec(QStringLiteral("SELECT sql FROM sqlite_master WHERE name = 'activity_log'"))) {
        qCCritical(verzetaDb) << "v18 migration: sqlite_master verify failed:"
                              << verify.lastError().text();
        return false;
    }
    if (!verify.next()) {
        qCCritical(verzetaDb) << "v18 migration: sqlite_master returned no row for activity_log";
        return false;
    }
    const QString actualSql = verify.value(0).toString();
    if (!actualSql.contains(QStringLiteral("'client'"))) {
        qCCritical(verzetaDb) << "v18 migration: post-migration CHECK does NOT include 'client';"
                              << "refusing to bump schema_version. sqlite_master.sql:" << actualSql;
        return false;
    }

    qCInfo(verzetaDb) << "v18 migration: activity_log.actor_kind CHECK widened;"
                      << "rows preserved:" << postCount;
    return true;
}

bool DbManager::applySchemaV19() {
    QSqlQuery q(m_db);

    // Step 1 — pre-copy row count guard.
    qint64 preCount = -1;
    {
        QSqlQuery count(m_db);
        if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM folder_mounts"))) {
            qCCritical(verzetaDb) << "v19 migration: pre-copy COUNT(*) failed:"
                                  << count.lastError().text();
            return false;
        }
        if (count.next())
            preCount = count.value(0).toLongLong();
    }
    if (preCount < 0) {
        qCCritical(verzetaDb) << "v19 migration: pre-copy COUNT(*) returned no row";
        return false;
    }

    // Step 2 — rename the old table out of the way.
    if (!q.exec(QStringLiteral("ALTER TABLE folder_mounts RENAME TO folder_mounts_v18_backup"))) {
        qCCritical(verzetaDb) << "v19 migration: RENAME failed:" << q.lastError().text();
        return false;
    }

    // Step 3 — create the new table.  Same columns, same NOT NULL +
    // DEFAULT + CHECK rules, same FOREIGN KEY.  The only difference
    // is the composite PRIMARY KEY: `(folder_id, client_id)`.
    if (!q.exec(QStringLiteral("CREATE TABLE folder_mounts ("
                               "  folder_id        TEXT NOT NULL "
                               "                       REFERENCES folders(id) ON DELETE CASCADE,"
                               "  client_id        TEXT NOT NULL,"
                               "  mount_id         TEXT NOT NULL,"
                               "  owner_label      TEXT NOT NULL,"
                               "  registered_at_ms INTEGER NOT NULL,"
                               "  last_seen_ms     INTEGER NOT NULL,"
                               "  tree_json        TEXT NOT NULL DEFAULT '{}',"
                               "  blocklist_json   TEXT NOT NULL DEFAULT '[]',"
                               "  allowlist_json   TEXT NOT NULL DEFAULT '[]',"
                               "  permission_tier  TEXT NOT NULL DEFAULT 'ask'"
                               "                       CHECK (permission_tier IN "
                               "                              ('ask','smart','bypass')),"
                               "  options_json     TEXT NOT NULL DEFAULT '{}',"
                               "  PRIMARY KEY (folder_id, client_id)"
                               ")"))) {
        qCCritical(verzetaDb) << "v19 migration: CREATE TABLE folder_mounts (composite PK) failed:"
                              << q.lastError().text();
        return false;
    }

    // Step 4 — copy existing rows.  Column order in the rename'd
    // backup matches the new layout EXCEPT mount_id / client_id are
    // swapped (v17/v18 had `mount_id` before `client_id`).  Use an
    // explicit column list to avoid relying on ordinal mapping.
    if (!q.exec(QStringLiteral("INSERT INTO folder_mounts ("
                               "    folder_id, client_id, mount_id, owner_label,"
                               "    registered_at_ms, last_seen_ms,"
                               "    tree_json, blocklist_json, allowlist_json,"
                               "    permission_tier, options_json"
                               ") SELECT "
                               "    folder_id, client_id, mount_id, owner_label,"
                               "    registered_at_ms, last_seen_ms,"
                               "    tree_json, blocklist_json, allowlist_json,"
                               "    permission_tier, options_json "
                               "FROM folder_mounts_v18_backup"))) {
        qCCritical(verzetaDb) << "v19 migration: INSERT FROM SELECT failed:"
                              << q.lastError().text();
        return false;
    }

    // Step 5 — row-count parity guard.  Composite PK is a relaxation
    // (more rows can coexist) so every pre-existing row should fit;
    // a mismatch means a column we depended on diverged.
    qint64 postCount = -1;
    {
        QSqlQuery count(m_db);
        if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM folder_mounts"))) {
            qCCritical(verzetaDb) << "v19 migration: post-copy COUNT(*) failed:"
                                  << count.lastError().text();
            return false;
        }
        if (count.next())
            postCount = count.value(0).toLongLong();
    }
    if (postCount != preCount) {
        qCCritical(verzetaDb) << "v19 migration: row count mismatch — pre:" << preCount
                              << "post:" << postCount << "— refusing to bump schema_version";
        return false;
    }

    // Step 6 — drop the renamed backup.  Must run after the parity
    // guard since rollback needs it in place if anything earlier in
    // the migration failed.
    if (!q.exec(QStringLiteral("DROP TABLE folder_mounts_v18_backup"))) {
        qCCritical(verzetaDb) << "v19 migration: DROP TABLE folder_mounts_v18_backup failed:"
                              << q.lastError().text();
        return false;
    }

    // Step 7 — recreate the by-client index dropped along with the
    // old table.  This index is referenced by mount lookups and
    // stale-sweep queries; v19's composite PK already covers
    // (folder_id, client_id) so a separate index on client_id alone
    // continues to serve "find every mount this client owns" queries.
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_folder_mounts_client "
                               "ON folder_mounts(client_id)"))) {
        qCWarning(verzetaDb) << "v19 migration: CREATE INDEX idx_folder_mounts_client failed:"
                             << q.lastError().text();
        // Index is a performance optimisation, not a correctness
        // guard.  Do not fail migration if the index can't be
        // recreated; the next startup's defensive self-heal could
        // also handle it.
    }

    // Step 8 — verify-at-source.  SQLite's sqlite_master holds the
    // canonical CREATE TABLE SQL; if it doesn't contain the composite
    // PK, the migration silently no-op'd somehow.
    {
        QSqlQuery mq(m_db);
        if (!mq.exec(QStringLiteral("SELECT sql FROM sqlite_master "
                                    "WHERE type='table' AND name='folder_mounts'"))) {
            qCCritical(verzetaDb) << "v19 migration: sqlite_master query failed:"
                                  << mq.lastError().text();
            return false;
        }
        if (!mq.next()) {
            qCCritical(verzetaDb) << "v19 migration: sqlite_master returned no row for "
                                     "folder_mounts after migration";
            return false;
        }
        const QString actualSql = mq.value(0).toString();
        if (!actualSql.contains(QStringLiteral("PRIMARY KEY (folder_id, client_id)"),
                                Qt::CaseInsensitive)) {
            qCCritical(verzetaDb) << "v19 migration: sqlite_master CREATE TABLE does NOT "
                                     "contain composite PRIMARY KEY — refusing to bump "
                                     "schema_version. Actual SQL:"
                                  << actualSql;
            return false;
        }
    }

    qCInfo(verzetaDb) << "v19 migration: folder_mounts PK widened to (folder_id, client_id);"
                      << "rows preserved:" << postCount;
    return true;
}


bool DbManager::applySchemaV20() {
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS conversation_summaries ("
                               "  conversation_id      TEXT PRIMARY KEY "
                               "                       REFERENCES conversations(id) "
                               "                       ON DELETE CASCADE,"
                               "  summary_text         TEXT NOT NULL,"
                               "  last_message_id      TEXT NOT NULL,"
                               "  last_message_idx     INTEGER NOT NULL,"
                               "  covered_count        INTEGER NOT NULL,"
                               "  generated_at_ms      INTEGER NOT NULL,"
                               "  token_count          INTEGER NOT NULL,"
                               "  model_used           TEXT NOT NULL,"
                               "  trigger_reason       TEXT NOT NULL "
                               "                       CHECK(trigger_reason IN ('auto','manual')),"
                               "  invalidation_reason  TEXT NOT NULL DEFAULT 'none' "
                               "                       CHECK(invalidation_reason IN "
                               "                             ('none','stale','member_changed',"
                               "                              'folder_changed','flashmemory',"
                               "                              'user_regenerated'))"
                               ")"))) {
        qCCritical(verzetaDb) << "v20 migration: CREATE conversation_summaries failed:"
                              << q.lastError().text();
        return false;
    }
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_conv_summary_conv_id "
                               "ON conversation_summaries(conversation_id)"))) {
        qCCritical(verzetaDb) << "v20 migration: CREATE INDEX failed:" << q.lastError().text();
        return false;
    }
    // Verify-at-source: the table must expose exactly the columns the
    // service layer will bind. A partial CREATE (disk full, etc.)
    // must fail the migration, not surface later as bind errors.
    {
        QSqlQuery info(m_db);
        if (!info.exec(QStringLiteral("PRAGMA table_info(conversation_summaries)"))) {
            qCCritical(verzetaDb) << "v20 migration: PRAGMA table_info failed:"
                                  << info.lastError().text();
            return false;
        }
        QSet<QString> cols;
        while (info.next())
            cols.insert(info.value(1).toString());
        const QSet<QString> expected = {
            QStringLiteral("conversation_id"),
            QStringLiteral("summary_text"),
            QStringLiteral("last_message_id"),
            QStringLiteral("last_message_idx"),
            QStringLiteral("covered_count"),
            QStringLiteral("generated_at_ms"),
            QStringLiteral("token_count"),
            QStringLiteral("model_used"),
            QStringLiteral("trigger_reason"),
            QStringLiteral("invalidation_reason"),
        };
        if (cols != expected) {
            qCCritical(verzetaDb) << "v20 migration: column verify failed — got" << cols;
            return false;
        }
    }
    return true;
}


bool DbManager::applySchemaV21() {
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS subagent_runs ("
                               "  id               TEXT PRIMARY KEY,"
                               "  conversation_id  TEXT NOT NULL,"
                               "  parent_msg_id    TEXT NOT NULL,"
                               "  requester_alias  TEXT NOT NULL,"
                               "  task             TEXT NOT NULL,"
                               "  tools_whitelist  TEXT NOT NULL DEFAULT '[]',"
                               "  provider_id      TEXT NOT NULL,"
                               "  model_name       TEXT NOT NULL,"
                               "  status           TEXT NOT NULL CHECK(status IN "
                               "    ('queued','running','done','failed','cancelled')),"
                               "  result_text      TEXT NOT NULL DEFAULT '',"
                               "  fail_reason      TEXT NOT NULL DEFAULT '',"
                               "  total_tokens     INTEGER NOT NULL DEFAULT 0,"
                               "  created_at_ms    INTEGER NOT NULL,"
                               "  started_at_ms    INTEGER,"
                               "  finished_at_ms   INTEGER"
                               ")"))) {
        qCCritical(verzetaDb) << "v21: CREATE subagent_runs failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS subagent_messages ("
                               "  id         TEXT PRIMARY KEY,"
                               "  run_id     TEXT NOT NULL "
                               "             REFERENCES subagent_runs(id) ON DELETE CASCADE,"
                               "  seq        INTEGER NOT NULL,"
                               "  role       TEXT NOT NULL,"
                               "  content    TEXT NOT NULL,"
                               "  tool_name  TEXT NOT NULL DEFAULT '',"
                               "  created_at_ms INTEGER NOT NULL"
                               ")"))) {
        qCCritical(verzetaDb) << "v21: CREATE subagent_messages failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_subagent_runs_conv "
                               "ON subagent_runs(conversation_id)")) ||
        !q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_subagent_messages_run "
                               "ON subagent_messages(run_id, seq)"))) {
        qCCritical(verzetaDb) << "v21: CREATE INDEX failed:" << q.lastError().text();
        return false;
    }
    // Verify-at-source.
    {
        QSqlQuery info(m_db);
        if (!info.exec(QStringLiteral("PRAGMA table_info(subagent_runs)"))) {
            qCCritical(verzetaDb) << "v21: PRAGMA failed:" << info.lastError().text();
            return false;
        }
        QSet<QString> cols;
        while (info.next())
            cols.insert(info.value(1).toString());
        const QSet<QString> expected = {
            QStringLiteral("id"),
            QStringLiteral("conversation_id"),
            QStringLiteral("parent_msg_id"),
            QStringLiteral("requester_alias"),
            QStringLiteral("task"),
            QStringLiteral("tools_whitelist"),
            QStringLiteral("provider_id"),
            QStringLiteral("model_name"),
            QStringLiteral("status"),
            QStringLiteral("result_text"),
            QStringLiteral("fail_reason"),
            QStringLiteral("total_tokens"),
            QStringLiteral("created_at_ms"),
            QStringLiteral("started_at_ms"),
            QStringLiteral("finished_at_ms"),
        };
        if (cols != expected) {
            qCCritical(verzetaDb) << "v21: column verify failed —" << cols;
            return false;
        }
    }
    return true;
}

bool DbManager::applySchemaV22() {
    // RAG corpus scope + model identity. owner_scope lets retrieval restrict
    // to {conversation} ∪ {project} ∪ {global} (and {agent} later for AIM)
    // instead of leaking across every conversation. ADD COLUMN is the only
    // way to extend an existing table; each ALTER is guarded by a column
    // existence check so a defensive re-run is a no-op.
    QSqlQuery q(m_db);

    const auto hasColumn = [this](const QString& table, const QString& col) -> bool {
        QSqlQuery info(m_db);
        if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            return false;
        }
        while (info.next()) {
            if (info.value(1).toString() == col) {
                return true;
            }
        }
        return false;
    };

    if (!hasColumn(QStringLiteral("embeddings"), QStringLiteral("owner_scope"))) {
        if (!q.exec(QStringLiteral("ALTER TABLE embeddings ADD COLUMN owner_scope TEXT NOT NULL "
                                   "DEFAULT 'global'"))) {
            qCCritical(verzetaDb) << "v22: ALTER embeddings ADD owner_scope failed:"
                                  << q.lastError().text();
            return false;
        }
    }
    if (!hasColumn(QStringLiteral("documents"), QStringLiteral("owner_scope"))) {
        if (!q.exec(QStringLiteral("ALTER TABLE documents ADD COLUMN owner_scope TEXT NOT NULL "
                                   "DEFAULT 'global'"))) {
            qCCritical(verzetaDb) << "v22: ALTER documents ADD owner_scope failed:"
                                  << q.lastError().text();
            return false;
        }
    }

    // Backfill existing message-embedding rows to their source message's
    // conversation scope; document rows keep the 'global' default. Guarded by
    // EXISTS so a chunk whose source message is gone is left untouched.
    if (!q.exec(QStringLiteral("UPDATE embeddings SET owner_scope = "
                               "  'conversation:' || (SELECT m.conversation_id FROM messages m "
                               "                      WHERE m.id = embeddings.source_id) "
                               "WHERE source_type = 'message' AND EXISTS "
                               "  (SELECT 1 FROM messages m WHERE m.id = embeddings.source_id)"))) {
        qCCritical(verzetaDb) << "v22: backfill embeddings.owner_scope failed:"
                              << q.lastError().text();
        return false;
    }

    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_embeddings_scope "
                               "ON embeddings(owner_scope)"))) {
        qCCritical(verzetaDb) << "v22: CREATE INDEX idx_embeddings_scope failed:"
                              << q.lastError().text();
        return false;
    }

    // Verify-at-source.
    if (!hasColumn(QStringLiteral("embeddings"), QStringLiteral("owner_scope")) ||
        !hasColumn(QStringLiteral("documents"), QStringLiteral("owner_scope"))) {
        qCCritical(verzetaDb) << "v22: owner_scope column verify failed";
        return false;
    }
    return true;
}

bool DbManager::applySchemaV23() {
    // Per-agent memory (AIM). agent_memories holds durable memory entries; it
    // satisfies the VectorStore schema contract (embedding BLOB + model_used +
    // owner_scope) so the shared substrate provides KNN/brute-force ranking over
    // a companion vec0 table (vec_agent_memories). owner_scope is the visibility
    // key: 'agent:<agentId>' for an agent's own memory, or 'conversation:<id>'
    // for an agentless direct chat; ACN later writes 'project:<folderId>'.
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS agent_memories (
                id              TEXT PRIMARY KEY,
                owner_scope     TEXT NOT NULL,
                kind            TEXT NOT NULL DEFAULT 'fact'
                                CHECK(kind IN ('fact','preference','correction',
                                               'observation','explicit')),
                text            TEXT NOT NULL,
                embedding       BLOB,
                model_used      TEXT,
                source_conv_id  TEXT,
                source_msg_id   TEXT,
                confidence      REAL,
                use_count       INTEGER NOT NULL DEFAULT 0,
                last_used_at_ms INTEGER,
                superseded_by   TEXT,
                created_at_ms   INTEGER NOT NULL
            )
        )"))) {
        qCCritical(verzetaDb) << "v23: CREATE agent_memories failed:" << q.lastError().text();
        return false;
    }

    // Scope index for fast scope-filtered reads/purges.
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_agent_memories_scope "
                               "ON agent_memories(owner_scope)"))) {
        qCCritical(verzetaDb) << "v23: CREATE idx_agent_memories_scope failed:"
                              << q.lastError().text();
        return false;
    }

    // Content-less FTS5 index for the lexical half of hybrid recall, maintained
    // by AgentMemoryService (manual insert/delete, mirroring messages_fts — no
    // triggers). memory_id + owner_scope are UNINDEXED row keys.
    if (!q.exec(QStringLiteral(R"(
            CREATE VIRTUAL TABLE IF NOT EXISTS agent_memories_fts USING fts5(
                text,
                memory_id UNINDEXED,
                owner_scope UNINDEXED
            )
        )"))) {
        qCCritical(verzetaDb) << "v23: CREATE agent_memories_fts failed:" << q.lastError().text();
        return false;
    }

    return true;
}

bool DbManager::applySchemaV24() {
    // Team/project memory (ACN). acn_entries holds memory written at compaction,
    // scoped 'project:<folderId>' (the conversation's nearest project/org
    // ancestor). It satisfies the VectorStore schema contract (embedding BLOB +
    // model_used + owner_scope) so the shared substrate ranks over a companion
    // vec0 table (vec_acn_entries). folders.acn_enabled is the per-project/org
    // master switch (default ON).
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS acn_entries (
                id             TEXT PRIMARY KEY,
                owner_scope    TEXT NOT NULL,
                entry_kind     TEXT NOT NULL DEFAULT 'summary'
                               CHECK(entry_kind IN ('summary','decision','fact',
                                                    'open_question')),
                text           TEXT NOT NULL,
                embedding      BLOB,
                model_used     TEXT,
                source_conv_id TEXT,
                covered_range  TEXT,
                created_at_ms  INTEGER NOT NULL
            )
        )"))) {
        qCCritical(verzetaDb) << "v24: CREATE acn_entries failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_acn_entries_scope "
                               "ON acn_entries(owner_scope)"))) {
        qCCritical(verzetaDb) << "v24: CREATE idx_acn_entries_scope failed:"
                              << q.lastError().text();
        return false;
    }
    if (!q.exec(QStringLiteral(R"(
            CREATE VIRTUAL TABLE IF NOT EXISTS acn_entries_fts USING fts5(
                text,
                entry_id UNINDEXED,
                owner_scope UNINDEXED
            )
        )"))) {
        qCCritical(verzetaDb) << "v24: CREATE acn_entries_fts failed:" << q.lastError().text();
        return false;
    }

    // folders.acn_enabled — the per-project/org ACN master switch (default ON).
    // ADD COLUMN guarded by a column-existence check so a re-run is a no-op.
    const auto hasColumn = [this](const QString& table, const QString& col) -> bool {
        QSqlQuery info(m_db);
        if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            return false;
        }
        while (info.next()) {
            if (info.value(1).toString() == col) {
                return true;
            }
        }
        return false;
    };
    if (!hasColumn(QStringLiteral("folders"), QStringLiteral("acn_enabled"))) {
        if (!q.exec(QStringLiteral("ALTER TABLE folders ADD COLUMN acn_enabled INTEGER NOT NULL "
                                   "DEFAULT 1"))) {
            qCCritical(verzetaDb) << "v24: ALTER folders ADD acn_enabled failed:"
                                  << q.lastError().text();
            return false;
        }
    }
    if (!hasColumn(QStringLiteral("folders"), QStringLiteral("acn_enabled"))) {
        qCCritical(verzetaDb) << "v24: folders.acn_enabled verify failed";
        return false;
    }
    return true;
}

bool DbManager::applySchemaV9() {
    QSqlQuery q(m_db);

    if (!q.exec(QStringLiteral("ALTER TABLE conversations "
                               "ADD COLUMN is_pinned INTEGER NOT NULL DEFAULT 0"))) {
        qCCritical(verzetaDb) << "v9 migration: ALTER TABLE conversations ADD is_pinned failed:"
                              << q.lastError().text();
        return false;
    }

    // Composite index that supports the sidebar's "list all pinned"
    // sweep (WHERE is_pinned = 1 ORDER BY updated_at DESC) with an
    // index seek instead of a full scan. The leading is_pinned column
    // means SQLite resolves the WHERE with the prefix and uses the
    // trailing updated_at DESC for the sort.
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_conversations_pinned "
                               "ON conversations(is_pinned, updated_at DESC)"))) {
        qCWarning(verzetaDb) << "v9 migration: CREATE INDEX idx_conversations_pinned failed:"
                             << q.lastError().text();
        // Non-fatal — queries still work without the index.
    }

    return true;
}

/**
 * @brief Retrieves the current schema version from the settings table.
 * @return Current schema version integer, or 0 if not set / table doesn't exist yet.
 * @complexity O(1)
 */
int DbManager::currentSchemaVersion() {
    // The settings table may not exist yet on first run
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT value FROM settings WHERE key='schema_version' LIMIT 1"))) {
        return 0;
    }
    if (q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

/**
 * @brief Stores the schema version in the settings table.
 * @param version The schema version to record.
 * @sideeffects Inserts or replaces the schema_version row in settings.
 */
void DbManager::setSchemaVersion(int version) {
    QSqlQuery q(m_db);
    q.prepare(
        QStringLiteral("INSERT OR REPLACE INTO settings(key, value, updated_at) VALUES(?, ?, ?)"));
    q.addBindValue(QStringLiteral("schema_version"));
    q.addBindValue(QString::number(version));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qCWarning(verzetaDb) << "Failed to set schema_version:" << q.lastError().text();
    }
}

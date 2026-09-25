// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/activity-event.h"
#include "models/db-manager.h"
#include "services/audit-service.h"
#include "services/folder-mount-registry.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <optional>
#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QUuid>

namespace {

QString freshUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void seedFolder(QSqlDatabase& db, const QString& folderId, const QString& name) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO folders (id, name, parent_id, created_at, "
                             "folder_type) VALUES (?, ?, NULL, ?, 'project')"));
    q.addBindValue(folderId);
    q.addBindValue(name);
    q.addBindValue(static_cast<qint64>(0));
    if (!q.exec()) {
        qWarning() << "seedFolder failed:" << q.lastError().text();
    }
}

}  // namespace

class TestFolderMountRegistry : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() + QStringLiteral("/test_%1.db").arg(freshUuid());

        DbManager::instance().close();
        QFile::remove(m_dbPath);
        QVERIFY2(DbManager::instance().open(m_dbPath), "DbManager::open failed");
        QVERIFY2(DbManager::instance().runMigrations(), "DbManager::runMigrations failed");

        QSqlQuery v(DbManager::instance().db());
        QVERIFY(v.exec(QStringLiteral("SELECT value FROM settings WHERE key = 'schema_version'")));
        QVERIFY(v.next());
        QCOMPARE(v.value(0).toInt(), 24);

        m_registry = std::make_unique<FolderMountRegistry>(DbManager::instance());
    }

    void cleanup() {
        m_registry.reset();
        DbManager::instance().close();
        QFile::remove(m_dbPath);
    }

    void registerFreshMount_succeedsAndEmits() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Test Project"));

        QSignalSpy spyRegistered(m_registry.get(), &FolderMountRegistry::mountRegistered);
        QSignalSpy spyReplaced(m_registry.get(), &FolderMountRegistry::mountReplaced);
        QSignalSpy spyRefreshed(m_registry.get(), &FolderMountRegistry::mountTreeUpdated);

        const auto reply = m_registry->registerMount(folderId,
                                                     mountId,
                                                     clientId,
                                                     QStringLiteral("VS Code on laptop"),
                                                     QStringLiteral("{\"v\":1,\"files\":[]}"),
                                                     QStringLiteral("[]"),
                                                     QStringLiteral("[]"),
                                                     QStringLiteral("ask"),
                                                     QStringLiteral("{}"));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), true);

        QCOMPARE(spyRegistered.size(), 1);
        QCOMPARE(spyReplaced.size(), 0);
        QCOMPARE(spyRefreshed.size(), 0);

        const auto row = m_registry->mountFor(folderId);
        QCOMPARE(row.value(QStringLiteral("mountId")).toString(), mountId);
        QCOMPARE(row.value(QStringLiteral("clientId")).toString(), clientId);
        QCOMPARE(row.value(QStringLiteral("permissionTier")).toString(), QStringLiteral("ask"));
    }

    void reRegisterSameClientAndMount_refreshes() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Refresh Project"));

        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner A"),
                                  QStringLiteral("{\"v\":1,\"files\":[]}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));
        const qint64 firstRegisteredAt =
            m_registry->mountFor(folderId).value(QStringLiteral("registeredAtMs")).toLongLong();

        QSignalSpy spyRegistered(m_registry.get(), &FolderMountRegistry::mountRegistered);
        QSignalSpy spyReplaced(m_registry.get(), &FolderMountRegistry::mountReplaced);
        QSignalSpy spyRefreshed(m_registry.get(), &FolderMountRegistry::mountTreeUpdated);

        QTest::qWait(2);

        const auto reply =
            m_registry->registerMount(folderId,
                                      mountId,
                                      clientId,
                                      QStringLiteral("Owner A v2"),
                                      QStringLiteral("{\"v\":1,\"files\":[{\"path\":\"a\"}]}"),
                                      QStringLiteral("[]"),
                                      QStringLiteral("[]"),
                                      QStringLiteral("smart"),
                                      QStringLiteral("{}"));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), true);

        QCOMPARE(spyRegistered.size(), 0);
        QCOMPARE(spyReplaced.size(), 0);
        QCOMPARE(spyRefreshed.size(), 1);

        const auto row = m_registry->mountFor(folderId);
        QCOMPARE(row.value(QStringLiteral("registeredAtMs")).toLongLong(), firstRegisteredAt);
        QVERIFY(row.value(QStringLiteral("lastSeenMs")).toLongLong() >= firstRegisteredAt);
        QCOMPARE(row.value(QStringLiteral("ownerLabel")).toString(), QStringLiteral("Owner A v2"));
        QCOMPARE(row.value(QStringLiteral("permissionTier")).toString(), QStringLiteral("smart"));
    }

    void registerDifferentOwner_emitsReplaced() {
        const QString folderId = freshUuid();
        const QString oldMount = freshUuid();
        const QString oldClient = freshUuid();
        const QString newMount = freshUuid();
        const QString newClient = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Replace Project"));

        m_registry->registerMount(folderId,
                                  oldMount,
                                  oldClient,
                                  QStringLiteral("First Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QSignalSpy spyReplaced(m_registry.get(), &FolderMountRegistry::mountReplaced);
        QSignalSpy spyRegistered(m_registry.get(), &FolderMountRegistry::mountRegistered);

        const auto reply = m_registry->registerMount(folderId,
                                                     newMount,
                                                     newClient,
                                                     QStringLiteral("Second Owner"),
                                                     QStringLiteral("{}"),
                                                     QStringLiteral("[]"),
                                                     QStringLiteral("[]"),
                                                     QStringLiteral("bypass"),
                                                     QStringLiteral("{}"));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), true);

        QCOMPARE(spyReplaced.size(), 0);
        QCOMPARE(spyRegistered.size(), 1);

        QCOMPARE(m_registry->mountsForFolder(folderId).size(), 2);
        m_registry->registerMount(folderId,
                                  freshUuid(),
                                  newClient,
                                  QStringLiteral("Second Owner v2"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("bypass"),
                                  QStringLiteral("{}"));
        QCOMPARE(spyReplaced.size(), 1);
    }

    void unregisterForeignClient_rejected() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString ownerClient = freshUuid();
        const QString stranger = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Owned Project"));

        m_registry->registerMount(folderId,
                                  mountId,
                                  ownerClient,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        const auto reply = m_registry->unregisterMount(folderId, stranger);
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(reply.value(QStringLiteral("error")).toString(),
                 QStringLiteral("no mount registered for folder/client"));

        QVERIFY(m_registry->mountForId(folderId).has_value());
        QVERIFY(m_registry->mountForClient(folderId, ownerClient).has_value());
    }

    void unregisterOwner_succeedsAndEmits() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Owner Project"));

        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QSignalSpy spyUnregistered(m_registry.get(), &FolderMountRegistry::mountUnregistered);

        const auto reply = m_registry->unregisterMount(folderId, clientId);
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(spyUnregistered.size(), 1);
        QVERIFY(!m_registry->mountForId(folderId).has_value());
    }

    void updateTier_emitsWithBeforeAndAfter() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Tier Project"));

        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QSignalSpy spyTier(m_registry.get(), &FolderMountRegistry::mountTierChanged);

        const auto r1 = m_registry->updateTier(folderId, clientId, QStringLiteral("smart"));
        QCOMPARE(r1.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(spyTier.size(), 1);
        QCOMPARE(spyTier.at(0).at(3).toString(), QStringLiteral("ask"));
        QCOMPARE(spyTier.at(0).at(4).toString(), QStringLiteral("smart"));

        const auto r2 = m_registry->updateTier(folderId, clientId, QStringLiteral("smart"));
        QCOMPARE(r2.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(spyTier.size(), 1);
    }

    void registerInvalidTier_rejected() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Invalid Tier Project"));

        const auto reply = m_registry->registerMount(folderId,
                                                     mountId,
                                                     clientId,
                                                     QStringLiteral("Owner"),
                                                     QStringLiteral("{}"),
                                                     QStringLiteral("[]"),
                                                     QStringLiteral("[]"),
                                                     QStringLiteral("paranoid"),
                                                     QStringLiteral("{}"));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), false);
        QVERIFY(reply.value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("permission_tier")));
        QVERIFY(!m_registry->mountForId(folderId).has_value());
    }

    void folderDelete_cascadesMountRow() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Doomed Project"));

        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));
        QVERIFY(m_registry->mountForId(folderId).has_value());

        QSqlQuery del(DbManager::instance().db());
        QVERIFY(del.exec(QStringLiteral("PRAGMA foreign_keys = ON")));
        del.prepare(QStringLiteral("DELETE FROM folders WHERE id = ?"));
        del.addBindValue(folderId);
        QVERIFY(del.exec());

        m_registry.reset();
        m_registry = std::make_unique<FolderMountRegistry>(DbManager::instance());
        QVERIFY(!m_registry->mountForId(folderId).has_value());
    }

    void pathIsAllowed_canonicalisesAndRejectsEscape() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Path Decision Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("../etc/passwd")),
                 QStringLiteral("unsafe_path"));
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("src/../../etc/shadow")),
                 QStringLiteral("unsafe_path"));
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("/etc/hosts")),
                 QStringLiteral("absolute_path_forbidden"));
    }

    void pathIsAllowed_emptyPathIsWorkspaceRoot() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Root Listing Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("")), QString());
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral(".")), QString());
    }

    void pathIsAllowed_nullBytePathStillRejected() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Null Byte Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QString withNull = QStringLiteral("a");
        withNull.append(QChar(0));
        withNull.append(QStringLiteral("b"));
        QCOMPARE(m_registry->pathIsAllowed(folderId, withNull), QStringLiteral("unsafe_path"));
    }

    void pathIsAllowed_blocklistMatchRejected() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Blocklist Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[\"**/*.secret\"]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("src/.env")),
                 QStringLiteral("blocked_path"));
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral(".env")),
                 QStringLiteral("blocked_path"));
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("config/.git/HEAD")),
                 QStringLiteral("blocked_path"));
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("server.pem")),
                 QStringLiteral("blocked_path"));
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("node_modules/foo/index.js")),
                 QStringLiteral("blocked_path"));

        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("data/api.secret")),
                 QStringLiteral("blocked_path"));

        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("src/foo.ts")), QString());
    }

    void pathIsAllowed_allowlistEnforcedWhenSet() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Allowlist Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[\"src/**\", \"docs/**\"]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("src/main.ts")), QString());
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("docs/intro.md")), QString());

        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("tests/spec.ts")),
                 QStringLiteral("not_allowlisted"));
        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("README.md")),
                 QStringLiteral("not_allowlisted"));

        QCOMPARE(m_registry->pathIsAllowed(folderId, QStringLiteral("src/.env")),
                 QStringLiteral("blocked_path"));
    }

    void manifestContains_reportsTreeMembership() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Manifest Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{\"v\":1,\"files\":[{\"path\":\"src/foo.ts\","
                                                 "\"size\":42,\"mtime_ms\":1717000000000},"
                                                 "{\"path\":\"docs/README.md\","
                                                 "\"size\":120,\"mtime_ms\":1717000000001}]}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QVERIFY(m_registry->manifestContains(folderId, QStringLiteral("src/foo.ts")));
        QVERIFY(m_registry->manifestContains(folderId, QStringLiteral("docs/README.md")));
        QVERIFY(!m_registry->manifestContains(folderId, QStringLiteral("src/bar.ts")));

        const QString other = freshUuid();
        QVERIFY(!m_registry->manifestContains(other, QStringLiteral("src/foo.ts")));
    }

    void onStaleSweep_emitsForBackdatedMount() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Stale Sweep Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        const qint64 ancientMs =
            QDateTime::currentMSecsSinceEpoch() - FolderMountRegistry::kStaleThresholdMs - 60000;
        QSqlQuery up(DbManager::instance().db());
        up.prepare(QStringLiteral("UPDATE folder_mounts SET last_seen_ms = ? WHERE folder_id = ?"));
        up.addBindValue(ancientMs);
        up.addBindValue(folderId);
        QVERIFY(up.exec());

        m_registry.reset();
        m_registry = std::make_unique<FolderMountRegistry>(DbManager::instance());

        QSignalSpy spy(m_registry.get(), &FolderMountRegistry::mountStale);
        m_registry->onStaleSweep();
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), folderId);
        QCOMPARE(spy.at(0).at(1).toString(), mountId);
        QCOMPARE(spy.at(0).at(2).toString(), clientId);
        QCOMPARE(spy.at(0).at(3).toLongLong(), ancientMs);
    }

    void onStaleSweep_suppressesRepeatsUntilTouched() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Stale Repeat Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        const qint64 ancientMs =
            QDateTime::currentMSecsSinceEpoch() - FolderMountRegistry::kStaleThresholdMs - 60000;
        QSqlQuery up(DbManager::instance().db());
        up.prepare(QStringLiteral("UPDATE folder_mounts SET last_seen_ms = ? WHERE folder_id = ?"));
        up.addBindValue(ancientMs);
        up.addBindValue(folderId);
        QVERIFY(up.exec());
        m_registry.reset();
        m_registry = std::make_unique<FolderMountRegistry>(DbManager::instance());

        QSignalSpy spy(m_registry.get(), &FolderMountRegistry::mountStale);
        m_registry->onStaleSweep();
        m_registry->onStaleSweep();
        m_registry->onStaleSweep();
        QCOMPARE(spy.size(), 1);
    }

    void onStaleSweep_freshMountSilent() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Fresh Sweep Project"));
        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        QSignalSpy spy(m_registry.get(), &FolderMountRegistry::mountStale);
        m_registry->onStaleSweep();
        QCOMPARE(spy.size(), 0);
    }

    void persistence_acrossRegistryReconstruct() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Persistent Project"));

        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Persistent Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("smart"),
                                  QStringLiteral("{}"));

        m_registry.reset();
        m_registry = std::make_unique<FolderMountRegistry>(DbManager::instance());

        const auto reloaded = m_registry->mountForId(folderId);
        QVERIFY(reloaded.has_value());
        QCOMPARE(reloaded->mountId, mountId);
        QCOMPARE(reloaded->clientId, clientId);
        QCOMPARE(reloaded->permissionTier, QStringLiteral("smart"));
    }

    void registryLifecycle_persistsActivityRowsViaAudit() {
        const QString folderId = freshUuid();
        const QString mountId = freshUuid();
        const QString clientId = freshUuid();
        const QString clientId2 = freshUuid();
        const QString mountId2 = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Audit Trail Project"));

        AuditService audit(DbManager::instance());
        m_registry->setAuditService(&audit);

        m_registry->registerMount(folderId,
                                  mountId,
                                  clientId,
                                  QStringLiteral("Owner Label"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        m_registry->registerMount(folderId,
                                  mountId2,
                                  clientId2,
                                  QStringLiteral("New Owner"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        m_registry->registerMount(folderId,
                                  freshUuid(),
                                  clientId2,
                                  QStringLiteral("New Owner (workspace switch)"),
                                  QStringLiteral("{}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        m_registry->updateTier(folderId, clientId2, QStringLiteral("smart"));

        m_registry->updateTree(folderId, clientId2, QStringLiteral("{\"v\":1}"));

        m_registry->unregisterMount(folderId, clientId2);

        const auto rows = audit.recentActivityForProject(folderId, 50);
        QStringList eventTypes;
        QStringList actorKinds;
        for (const auto& v : rows) {
            const auto map = v.toMap();
            eventTypes.append(map.value(QStringLiteral("eventType")).toString());
            actorKinds.append(map.value(QStringLiteral("actorKind")).toString());
        }
        QVERIFY2(eventTypes.contains(QStringLiteral("workspace.mount.registered")),
                 qPrintable(QStringLiteral("no workspace.mount.registered row in activity_log; "
                                           "rows: ") +
                            eventTypes.join(QStringLiteral(", "))));
        QVERIFY2(eventTypes.contains(QStringLiteral("workspace.mount.replaced")),
                 "no workspace.mount.replaced row");
        QVERIFY2(eventTypes.contains(QStringLiteral("workspace.mount.tier_changed")),
                 "no workspace.mount.tier_changed row");
        QVERIFY2(eventTypes.contains(QStringLiteral("workspace.mount.tree_updated")),
                 "no workspace.mount.tree_updated row");
        QVERIFY2(eventTypes.contains(QStringLiteral("workspace.mount.unregistered")),
                 "no workspace.mount.unregistered row");
        for (const QString& kind : actorKinds) {
            QCOMPARE(kind, QStringLiteral("client"));
        }
    }

    void routeForFile_lockedAlgorithm() {
        const QString folderId = freshUuid();
        const QString clientA = freshUuid();
        const QString clientB = freshUuid();
        seedFolder(DbManager::instance().db(), folderId, QStringLiteral("Routing Project"));

        m_registry->registerMount(folderId,
                                  freshUuid(),
                                  clientA,
                                  QStringLiteral("A"),
                                  QStringLiteral("{\"v\":1,\"files\":[{\"path\":\"a.md\","
                                                 "\"size\":1,\"mtime_ms\":1}]}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));
        QTest::qSleep(2);
        m_registry->registerMount(folderId,
                                  freshUuid(),
                                  clientB,
                                  QStringLiteral("B"),
                                  QStringLiteral("{\"v\":1,\"files\":[{\"path\":\"b.md\","
                                                 "\"size\":1,\"mtime_ms\":1}]}"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("[]"),
                                  QStringLiteral("ask"),
                                  QStringLiteral("{}"));

        auto d = m_registry->routeForFile(folderId, clientB, QStringLiteral("a.md"));
        QCOMPARE(d.clientId, clientB);
        QCOMPARE(d.reason, QStringLiteral("caller_owned"));

        d = m_registry->routeForFile(folderId, freshUuid(), QStringLiteral("a.md"));
        QVERIFY(d.useLocal());
        QCOMPARE(d.reason, QStringLiteral("caller_client_not_mounted"));

        d = m_registry->routeForFile(folderId, QString(), QStringLiteral("a.md"));
        QCOMPARE(d.clientId, clientA);
        QCOMPARE(d.reason, QStringLiteral("manifest_owner"));

        d = m_registry->routeForFile(folderId, QString(), QStringLiteral("brand-new.md"));
        QCOMPARE(d.clientId, clientB);
        QCOMPARE(d.reason, QStringLiteral("most_recent_active"));

        const QString emptyFolder = freshUuid();
        seedFolder(DbManager::instance().db(), emptyFolder, QStringLiteral("Empty"));
        d = m_registry->routeForFile(emptyFolder, QString(), QStringLiteral("x.md"));
        QVERIFY(d.useLocal());
        QCOMPARE(d.reason, QStringLiteral("no_active_mount"));
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<FolderMountRegistry> m_registry;
};

QTEST_MAIN(TestFolderMountRegistry)
#include "test-folder-mount-registry.moc"

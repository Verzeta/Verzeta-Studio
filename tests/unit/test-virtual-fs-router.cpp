// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/file-service.h"
#include "services/folder-mount-registry.h"
#include "services/i-folder-mount-client.h"

#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <memory>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <utility>
#include <vector>

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

class StubMountClient : public IFolderMountClient {
  public:
    struct Call {
        QString op;
        QString folderId;
        QString mountId;
        QString relPath;
        QByteArray writeContent;
        QString expectedFingerprint;
    };

    QByteArray scriptedRead;
    qint64 scriptedWriteApplied = 0;
    QString scriptedReadError;
    QString scriptedWriteError;
    QJsonArray scriptedListEntries;

    std::vector<Call> calls;

    QByteArray readBytes(const QString& folderId,
                         const QString& mountId,
                         const QString& relPath,
                         qint64,
                         QString* outFingerprint,
                         QString* outError) override {
        calls.push_back({QStringLiteral("read"), folderId, mountId, relPath, {}, {}});
        if (!scriptedReadError.isEmpty()) {
            if (outError)
                *outError = scriptedReadError;
            return {};
        }
        if (outFingerprint)
            *outFingerprint = QStringLiteral("stub-fingerprint");
        return scriptedRead;
    }

    qint64 writeBytes(const QString& folderId,
                      const QString& mountId,
                      const QString& relPath,
                      const QByteArray& content,
                      const QString& expectedFingerprint,
                      QString* outError) override {
        calls.push_back(
            {QStringLiteral("write"), folderId, mountId, relPath, content, expectedFingerprint});
        if (!scriptedWriteError.isEmpty()) {
            if (outError)
                *outError = scriptedWriteError;
            return 0;
        }
        return scriptedWriteApplied > 0 ? scriptedWriteApplied
                                        : static_cast<qint64>(content.size());
    }

    QJsonObject statPath(const QString& folderId,
                         const QString& mountId,
                         const QString& relPath,
                         QString*) override {
        calls.push_back({QStringLiteral("stat"), folderId, mountId, relPath, {}, {}});
        return {};
    }

    QJsonArray listDir(const QString& folderId,
                       const QString& mountId,
                       const QString& relPath,
                       bool,
                       QString*) override {
        calls.push_back({QStringLiteral("list"), folderId, mountId, relPath, {}, {}});
        return scriptedListEntries;
    }
};

}  // namespace

class TestVirtualFsRouter : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<FolderMountRegistry> m_registry;
    std::unique_ptr<StubMountClient> m_stub;
    std::unique_ptr<FileService> m_fs;

    QString m_folderId;
    QString m_mountId;
    QString m_clientId;

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() + QStringLiteral("/test_%1.db").arg(freshUuid());
        DbManager::instance().close();
        QFile::remove(m_dbPath);
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_registry = std::make_unique<FolderMountRegistry>(DbManager::instance());
        m_stub = std::make_unique<StubMountClient>();
        m_fs = std::make_unique<FileService>();
        m_fs->setMountRegistry(m_registry.get());
        m_fs->setMountClient(m_stub.get());

        m_folderId = freshUuid();
        m_mountId = freshUuid();
        m_clientId = freshUuid();
        seedFolder(DbManager::instance().db(), m_folderId, QStringLiteral("Router Test Folder"));
        const auto reply =
            m_registry->registerMount(m_folderId,
                                      m_mountId,
                                      m_clientId,
                                      QStringLiteral("Owner"),
                                      QStringLiteral("{\"v\":1,\"files\":[{\"path\":\"src/foo.ts\","
                                                     "\"size\":42,\"mtime_ms\":1717000000000}]}"),
                                      QStringLiteral("[]"),
                                      QStringLiteral("[]"),
                                      QStringLiteral("ask"),
                                      QStringLiteral("{}"));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), true);
    }

    void cleanup() {
        m_fs.reset();
        m_stub.reset();
        m_registry.reset();
        DbManager::instance().close();
        QFile::remove(m_dbPath);
    }

    void absolutePath_neverRoutesThroughMount() {
        QString fingerprint, err;
        const QByteArray result =
            m_fs->readFileContent(QStringLiteral("/etc/hosts"), 1024, m_folderId);
        QCOMPARE(m_stub->calls.size(), size_t(0));
        Q_UNUSED(result);
    }

    void emptyCallerFolderId_neverRoutesThroughMount() {
        QString fingerprint, err;
        m_fs->readFileContent(QStringLiteral("src/foo.ts"), 1024, QString());
        QCOMPARE(m_stub->calls.size(), size_t(0));
    }

    void folderWithoutMount_neverRoutesThroughMount() {
        const QString other = freshUuid();
        seedFolder(DbManager::instance().db(), other, QStringLiteral("Other Folder (no mount)"));
        m_fs->readFileContent(QStringLiteral("src/foo.ts"), 1024, other);
        QCOMPARE(m_stub->calls.size(), size_t(0));
        QVERIFY2(
            m_fs->lastErrorKind().isEmpty(),
            qPrintable(QStringLiteral("unexpected mount error: %1").arg(m_fs->lastErrorKind())));
    }

    void folderWithoutMount_writeFallsBackToLocal() {
        const QString other = freshUuid();
        seedFolder(DbManager::instance().db(), other, QStringLiteral("Other Folder (no mount)"));
        QTemporaryDir dest;
        QVERIFY(dest.isValid());
        const QString written = m_fs->saveGeneratedFile(
            QStringLiteral("README.md"), QStringLiteral("# hello"), dest.path(), other);
        QVERIFY2(
            !written.isEmpty(),
            qPrintable(
                QStringLiteral("local write failed; lastErrorKind=%1").arg(m_fs->lastErrorKind())));
        QVERIFY(!written.startsWith(QStringLiteral("mount://")));
        QVERIFY(QFile::exists(written));
        QCOMPARE(m_stub->calls.size(), size_t(0));
        QVERIFY(m_fs->lastErrorKind().isEmpty());
    }

    void staleMount_writeFallsBackToLocal() {
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("UPDATE folder_mounts SET last_seen_ms = ? WHERE folder_id = ?"));
        q.addBindValue(QDateTime::currentMSecsSinceEpoch() -
                       2 * FolderMountRegistry::kStaleThresholdMs);
        q.addBindValue(m_folderId);
        QVERIFY(q.exec());

        auto staleRegistry = std::make_unique<FolderMountRegistry>(DbManager::instance());
        auto fs = std::make_unique<FileService>();
        fs->setMountRegistry(staleRegistry.get());
        fs->setMountClient(m_stub.get());

        QTemporaryDir dest;
        QVERIFY(dest.isValid());
        const QString written = fs->saveGeneratedFile(
            QStringLiteral("README.md"), QStringLiteral("# hello"), dest.path(), m_folderId);
        QVERIFY2(!written.isEmpty(),
                 qPrintable(QStringLiteral("stale-mount write failed; lastErrorKind=%1")
                                .arg(fs->lastErrorKind())));
        QVERIFY(!written.startsWith(QStringLiteral("mount://")));
        QVERIFY(QFile::exists(written));
        QCOMPARE(m_stub->calls.size(), size_t(0));
    }

    void mountRegistered_pathNotInManifest_overlaysToLocal() {
        QSignalSpy errSpy(m_fs.get(), &FileService::error);
        const QByteArray result =
            m_fs->readFileContent(QStringLiteral("src/bar.ts"), 1024, m_folderId);
        QCOMPARE(result.size(), 0);
        QCOMPARE(m_stub->calls.size(), size_t(0));
        for (int i = 0; i < errSpy.size(); ++i) {
            const QString msg = errSpy.at(i).at(0).toString();
            QVERIFY2(!msg.contains(QStringLiteral("not part of registered")),
                     msg.toUtf8().constData());
        }
    }

    void mountRegistered_blocklistedPath_rejectedNoDispatch() {
        m_fs->readFileContent(QStringLiteral(".env"), 1024, m_folderId);
        QCOMPARE(m_stub->calls.size(), size_t(0));
        QVERIFY2(m_fs->lastErrorKind().isEmpty(), qPrintable(m_fs->lastErrorKind()));

        const QString client2 = freshUuid();
        const auto reply =
            m_registry->registerMount(m_folderId,
                                      freshUuid(),
                                      client2,
                                      QStringLiteral("Owner2"),
                                      QStringLiteral("{\"v\":1,\"files\":[{\"path\":\".env\","
                                                     "\"size\":10,\"mtime_ms\":1717000000000}]}"),
                                      QStringLiteral("[]"),
                                      QStringLiteral("[]"),
                                      QStringLiteral("ask"),
                                      QStringLiteral("{}"));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), true);

        QSignalSpy errSpy(m_fs.get(), &FileService::error);
        m_fs->readFileContent(QStringLiteral(".env"), 1024, m_folderId);
        QCOMPARE(m_stub->calls.size(), size_t(0));
        QVERIFY(errSpy.size() >= 1);
        QVERIFY(errSpy.at(0).at(0).toString().contains(QStringLiteral("blocked_path")));
    }

    void mountRegistered_escapeAttempt_rejectedNoDispatch() {
        QSignalSpy errSpy(m_fs.get(), &FileService::error);
        m_fs->readFileContent(QStringLiteral("../../etc/passwd"), 1024, m_folderId);
        QCOMPARE(m_stub->calls.size(), size_t(0));
        QVERIFY(errSpy.size() >= 1);
        QVERIFY(errSpy.at(0).at(0).toString().contains(QStringLiteral("unsafe_path")));
    }

    void mountRegistered_passesGuards_dispatchesToMountClient() {
        m_stub->scriptedRead = QByteArrayLiteral("hello world");
        QString fingerprint, err;
        const QByteArray bytes =
            m_fs->readFileContent(QStringLiteral("src/foo.ts"), 1024, m_folderId);
        QCOMPARE(bytes, QByteArrayLiteral("hello world"));
        QCOMPARE(m_stub->calls.size(), size_t(1));
        QCOMPARE(m_stub->calls.at(0).op, QStringLiteral("read"));
        QCOMPARE(m_stub->calls.at(0).folderId, m_folderId);
        QCOMPARE(m_stub->calls.at(0).mountId, m_mountId);
        QCOMPARE(m_stub->calls.at(0).relPath, QStringLiteral("src/foo.ts"));
    }

    void mountClientError_propagatesAsErrorSignal() {
        m_stub->scriptedReadError = QStringLiteral("stale_fingerprint");
        QSignalSpy errSpy(m_fs.get(), &FileService::error);
        const QByteArray bytes =
            m_fs->readFileContent(QStringLiteral("src/foo.ts"), 1024, m_folderId);
        QCOMPARE(bytes.size(), 0);
        QVERIFY(errSpy.size() >= 1);
        QVERIFY(errSpy.at(0).at(0).toString().contains(QStringLiteral("stale_fingerprint")));
    }

    void saveGeneratedFile_underMount_dispatchesWrite() {
        const QString marker = m_fs->saveGeneratedFile(QStringLiteral("src/foo.ts"),
                                                       QStringLiteral("new file content"),
                                                       QString(),
                                                       m_folderId,
                                                       QStringLiteral("prior-fp"));
        QVERIFY(marker.startsWith(QStringLiteral("mount://")));
        QCOMPARE(m_stub->calls.size(), size_t(1));
        QCOMPARE(m_stub->calls.at(0).op, QStringLiteral("write"));
        QCOMPARE(m_stub->calls.at(0).relPath, QStringLiteral("src/foo.ts"));
        QCOMPARE(m_stub->calls.at(0).expectedFingerprint, QStringLiteral("prior-fp"));
    }

    void saveGeneratedFile_blocklisted_rejectedNoDispatch() {
        QSignalSpy errSpy(m_fs.get(), &FileService::error);
        const QString result = m_fs->saveGeneratedFile(
            QStringLiteral(".env"), QStringLiteral("FOO=bar"), QString(), m_folderId, QString());
        QVERIFY(result.isEmpty());
        QCOMPARE(m_stub->calls.size(), size_t(0));
        QVERIFY(errSpy.size() >= 1);
        QVERIFY(errSpy.at(0).at(0).toString().contains(QStringLiteral("blocked_path")));
    }

    void listDirectory_underMount_dispatchesAndReturnsEntries() {
        m_stub->scriptedListEntries = QJsonArray{
            QJsonObject{{QStringLiteral("path"), QStringLiteral("src/ignored-by-design.ts")}},
        };
        const QStringList paths = m_fs->listDirectory(QStringLiteral("src"), false, m_folderId);
        const QStringList expected{
            QStringLiteral("/mount/%1/src/foo.ts").arg(m_clientId.left(8)),
        };
        QCOMPARE(paths, expected);
        QCOMPARE(m_stub->calls.size(), size_t(0));
    }

    void listDirectory_emptyCallerFolderId_localPath() {
        m_fs->listDirectory(QStringLiteral("nope"), false, QString());
        QCOMPARE(m_stub->calls.size(), size_t(0));
    }
};

QTEST_MAIN(TestVirtualFsRouter)
#include "test-virtual-fs-router.moc"

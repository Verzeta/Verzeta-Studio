// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/file-service.h"
#include "services/folder-mount-registry.h"
#include "services/i-folder-mount-client.h"
#include "tools/file/list-files-tool.h"
#include "tools/file/read-file-tool.h"
#include "tools/file/write-file-tool.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
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
    QJsonArray scriptedListEntries;
    std::vector<QString> listedPaths;

    QByteArray readBytes(const QString&,
                         const QString&,
                         const QString& relPath,
                         qint64,
                         QString* outFingerprint,
                         QString* outError) override {
        readPaths.push_back(relPath);
        if (outError)
            outError->clear();
        if (outFingerprint)
            *outFingerprint = QStringLiteral("stub-fp");
        return scriptedRead;
    }
    qint64 writeBytes(const QString&,
                      const QString&,
                      const QString& relPath,
                      const QByteArray& content,
                      const QString&,
                      QString* outError) override {
        writePaths.push_back(relPath);
        if (outError)
            outError->clear();
        return scriptedWriteApplied > 0 ? scriptedWriteApplied
                                        : static_cast<qint64>(content.size());
    }
    QJsonObject
    statPath(const QString&, const QString&, const QString&, QString* outError) override {
        if (outError)
            outError->clear();
        return {};
    }
    QJsonArray listDir(
        const QString&, const QString&, const QString& relPath, bool, QString* outError) override {
        listedPaths.push_back(relPath);
        if (outError)
            outError->clear();
        return scriptedListEntries;
    }

    QByteArray scriptedRead;
    qint64 scriptedWriteApplied = 0;
    std::vector<QString> readPaths;
    std::vector<QString> writePaths;
};

}  // namespace

class TestVirtualFsRoundtrip : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<FolderMountRegistry> m_registry;
    std::unique_ptr<StubMountClient> m_stub;
    std::unique_ptr<FileService> m_fs;
    std::unique_ptr<Tools::ListFilesTool> m_tool;
    std::unique_ptr<Tools::ReadFileTool> m_readTool;
    std::unique_ptr<Tools::WriteFileTool> m_writeTool;

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

        m_tool = std::make_unique<Tools::ListFilesTool>(*m_fs);
        m_readTool = std::make_unique<Tools::ReadFileTool>(*m_fs);
        m_writeTool = std::make_unique<Tools::WriteFileTool>(*m_fs);

        m_folderId = freshUuid();
        m_mountId = freshUuid();
        m_clientId = freshUuid();
        seedFolder(DbManager::instance().db(), m_folderId, QStringLiteral("Roundtrip Folder"));
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
    }

    void cleanup() {
        m_writeTool.reset();
        m_readTool.reset();
        m_tool.reset();
        m_fs.reset();
        m_stub.reset();
        m_registry.reset();
        DbManager::instance().close();
        QFile::remove(m_dbPath);
    }

    void listFilesTool_noCallerFolderId_localFallback() {
        m_fs->setActiveConversation(QStringLiteral(""));

        const QJsonObject args{
            {QStringLiteral("path"), QStringLiteral("")},
            {QStringLiteral("recursive"), false},
        };
        const QJsonValue r = m_tool->invoke(args);
        const QJsonObject obj = r.toObject();

        QCOMPARE(m_stub->listedPaths.size(), size_t(0));
        QVERIFY(!obj.contains(QStringLiteral("path")));
        QVERIFY(obj.contains(QStringLiteral("files")));
        QVERIFY(obj.value(QStringLiteral("files")).isArray());
    }

    void listFilesTool_withCallerFolderId_routesThroughMount() {
        m_stub->scriptedListEntries = QJsonArray{
            QJsonObject{{QStringLiteral("path"), QStringLiteral("src/foo.ts")}},
            QJsonObject{{QStringLiteral("path"), QStringLiteral("src/bar.ts")}},
        };

        const QJsonObject args{
            {QStringLiteral("path"), QStringLiteral("src")},
            {QStringLiteral("recursive"), false},
            {QStringLiteral("__caller_folder_id"), m_folderId},
        };
        const QJsonValue r = m_tool->invoke(args);
        const QJsonObject obj = r.toObject();

        QCOMPARE(m_stub->listedPaths.size(), size_t(0));

        const QJsonArray files = obj.value(QStringLiteral("files")).toArray();
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.at(0).toString(),
                 QStringLiteral("/mount/%1/src/foo.ts").arg(m_clientId.left(8)));
    }

    void readFileTool_withCallerFolderId_routesThroughMount() {
        m_stub->scriptedRead = QByteArrayLiteral("hello from the mount");

        const QJsonObject args{
            {QStringLiteral("path"), QStringLiteral("src/foo.ts")},
            {QStringLiteral("__caller_folder_id"), m_folderId},
        };
        const QJsonValue r = m_readTool->invoke(args);
        const QJsonObject obj = r.toObject();

        QCOMPARE(m_stub->readPaths.size(), size_t(1));
        QCOMPARE(m_stub->readPaths.at(0), QStringLiteral("src/foo.ts"));
        QCOMPARE(obj.value(QStringLiteral("path")).toString(), QStringLiteral("src/foo.ts"));
        QCOMPARE(obj.value(QStringLiteral("content")).toString(),
                 QStringLiteral("hello from the mount"));
        QCOMPARE(obj.value(QStringLiteral("size")).toInt(),
                 static_cast<int>(m_stub->scriptedRead.size()));
    }

    void readFileTool_emptyCallerFolderId_localFallback() {
        const QJsonObject args{
            {QStringLiteral("path"), QStringLiteral("src/foo.ts")},
        };
        m_readTool->invoke(args);
        QCOMPARE(m_stub->readPaths.size(), size_t(0));
    }

    void writeFileTool_withCallerFolderId_routesThroughMount() {
        const QJsonObject args{
            {QStringLiteral("filename"), QStringLiteral("src/foo.ts")},
            {QStringLiteral("content"), QStringLiteral("new content via mount")},
            {QStringLiteral("__caller_folder_id"), m_folderId},
        };
        const QJsonValue r = m_writeTool->invoke(args);
        const QJsonObject obj = r.toObject();

        QCOMPARE(m_stub->writePaths.size(), size_t(1));
        QCOMPARE(m_stub->writePaths.at(0), QStringLiteral("src/foo.ts"));
        QCOMPARE(obj.value(QStringLiteral("path")).toString(), QStringLiteral("src/foo.ts"));
        QCOMPARE(obj.value(QStringLiteral("written")).toInt(),
                 QStringLiteral("new content via mount").length());
    }

    void writeFileTool_emptyCallerFolderId_localFallback() {
        const QJsonObject args{
            {QStringLiteral("filename"), QStringLiteral("artifact.md")},
            {QStringLiteral("content"), QStringLiteral("local-only artifact")},
        };
        m_writeTool->invoke(args);
        QCOMPARE(m_stub->writePaths.size(), size_t(0));
    }

    void listFilesTool_publicSurfaceUnchanged() {
        QCOMPARE(m_tool->name(), QStringLiteral("list_files"));
        QVERIFY(!m_tool->description().isEmpty());
        const auto params = m_tool->parameters();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params.at(0).name, QStringLiteral("path"));
        QCOMPARE(params.at(0).type, QStringLiteral("string"));
        QVERIFY(!params.at(0).required);
        QCOMPARE(params.at(1).name, QStringLiteral("recursive"));
        QCOMPARE(params.at(1).type, QStringLiteral("boolean"));
        QVERIFY(!params.at(1).required);
        QCOMPARE(m_tool->runsOnMainThread(), false);
    }
};

QTEST_MAIN(TestVirtualFsRoundtrip)
#include "test-virtual-fs-roundtrip.moc"

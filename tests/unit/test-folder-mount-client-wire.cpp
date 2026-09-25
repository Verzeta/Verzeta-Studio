// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "remote/wire-host-bridge.h"
#include "remote/wire-protocol.h"
#include "services/folder-mount-client-wire.h"
#include "services/folder-mount-registry.h"

#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QUuid>

namespace IpcType = Verzeta::Remote::IpcType;
using Verzeta::Remote::WireHostBridge;

namespace {

QString freshUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

template <typename Predicate> bool spinUntil(Predicate predicate, int timeoutMs) {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (predicate())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return predicate();
}

QByteArray frameOf(const QJsonObject& obj) {
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray out;
    out.reserve(4 + payload.size());
    const quint32 len = static_cast<quint32>(payload.size());
    out.append(static_cast<char>((len >> 24) & 0xff));
    out.append(static_cast<char>((len >> 16) & 0xff));
    out.append(static_cast<char>((len >> 8) & 0xff));
    out.append(static_cast<char>(len & 0xff));
    out.append(payload);
    return out;
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

class TestFolderMountClientWire : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<FolderMountRegistry> m_registry;
    std::unique_ptr<WireHostBridge> m_bridge;
    std::unique_ptr<QLocalSocket> m_daemonSocket;
    std::unique_ptr<FolderMountClientWire> m_client;

    QString m_folderId;
    QString m_mountId;
    QString m_clientId;

    QByteArray m_rxBuf;
    QStringList m_receivedFrames;

    void onDaemonReadyRead() {
        if (!m_daemonSocket)
            return;
        m_rxBuf.append(m_daemonSocket->readAll());
        while (true) {
            if (m_rxBuf.size() < 4)
                return;
            const quint32 len =
                (static_cast<quint8>(m_rxBuf[0]) << 24) | (static_cast<quint8>(m_rxBuf[1]) << 16) |
                (static_cast<quint8>(m_rxBuf[2]) << 8) | static_cast<quint8>(m_rxBuf[3]);
            if (m_rxBuf.size() < static_cast<int>(4 + len))
                return;
            const QByteArray payload = m_rxBuf.mid(4, len);
            m_rxBuf.remove(0, 4 + len);
            m_receivedFrames.append(QString::fromUtf8(payload));
        }
    }

    QJsonObject takeFrame(const QString& expectedType) {
        for (int i = 0; i < m_receivedFrames.size(); ++i) {
            QJsonParseError err;
            const QJsonDocument doc =
                QJsonDocument::fromJson(m_receivedFrames.at(i).toUtf8(), &err);
            if (err.error != QJsonParseError::NoError)
                continue;
            if (!doc.isObject())
                continue;
            if (doc.object().value(QStringLiteral("type")).toString() == expectedType) {
                const QJsonObject result = doc.object();
                m_receivedFrames.removeAt(i);
                return result;
            }
        }
        return {};
    }

    void sendDaemonReply(const QString& requestId,
                         bool ok,
                         const QJsonValue& data,
                         const QString& errKind = {},
                         const QString& errDetail = {}) {
        QJsonObject payload{
            {QStringLiteral("type"), QString::fromLatin1(IpcType::ClientRpcReply)},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("ok"), ok},
        };
        if (ok) {
            payload.insert(QStringLiteral("data"), data);
        } else {
            payload.insert(QStringLiteral("error"),
                           QJsonObject{
                               {QStringLiteral("kind"), errKind},
                               {QStringLiteral("detail"), errDetail},
                           });
        }
        m_daemonSocket->write(frameOf(payload));
        m_daemonSocket->flush();
    }

  private slots:
    void initTestCase() {
        qputenv("VERZETA_BRIDGE_SOCKET",
                QByteArrayLiteral("verzeta-host-bridge-test-fmc-") +
                    QByteArray::number(QCoreApplication::applicationPid()));
    }

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() + QStringLiteral("/test_%1.db").arg(freshUuid());

        DbManager::instance().close();
        QFile::remove(m_dbPath);
        QVERIFY2(DbManager::instance().open(m_dbPath), "DbManager::open failed");
        QVERIFY2(DbManager::instance().runMigrations(), "DbManager::runMigrations failed");

        m_registry = std::make_unique<FolderMountRegistry>(DbManager::instance());

        m_folderId = freshUuid();
        m_mountId = freshUuid();
        m_clientId = freshUuid();
        seedFolder(
            DbManager::instance().db(), m_folderId, QStringLiteral("Wire Client Test Folder"));
        const auto reply =
            m_registry->registerMount(m_folderId,
                                      m_mountId,
                                      m_clientId,
                                      QStringLiteral("Test Owner"),
                                      QStringLiteral("{\"v\":1,\"files\":[{\"path\":\"src/foo.ts\","
                                                     "\"size\":42,\"mtime_ms\":1717000000000}]}"),
                                      QStringLiteral("[]"),
                                      QStringLiteral("[]"),
                                      QStringLiteral("ask"),
                                      QStringLiteral("{}"));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), true);

        m_bridge = std::make_unique<WireHostBridge>(WireHostBridge::Services{});

        m_daemonSocket = std::make_unique<QLocalSocket>();
        m_rxBuf.clear();
        m_receivedFrames.clear();
        QObject::connect(m_daemonSocket.get(),
                         &QLocalSocket::readyRead,
                         this,
                         &TestFolderMountClientWire::onDaemonReadyRead);
        const QString name = Verzeta::Remote::hostBridgeSocketName();
        QVERIFY2(spinUntil(
                     [this, &name] {
                         if (m_daemonSocket->state() == QLocalSocket::ConnectedState) {
                             return true;
                         }
                         if (m_daemonSocket->state() == QLocalSocket::UnconnectedState) {
                             m_daemonSocket->connectToServer(name);
                         }
                         return m_daemonSocket->state() == QLocalSocket::ConnectedState;
                     },
                     3000),
                 "stub daemon failed to connect to bridge QLocalServer");

        (void)spinUntil(
            [this]() { return !takeFrame(QString::fromLatin1(IpcType::Hello)).isEmpty(); }, 1000);

        m_client = std::make_unique<FolderMountClientWire>(QPointer<WireHostBridge>(m_bridge.get()),
                                                           m_registry.get());
    }

    void cleanup() {
        m_client.reset();
        if (m_daemonSocket) {
            m_daemonSocket->disconnectFromServer();
            m_daemonSocket->waitForDisconnected(1000);
            m_daemonSocket.reset();
        }
        m_bridge.reset();
        m_registry.reset();
        DbManager::instance().close();
        QFile::remove(m_dbPath);
        m_rxBuf.clear();
        m_receivedFrames.clear();
    }

    void readBytesRoundTrip() {
        QString fingerprint, err;
        QByteArray bytes;

        auto fut = std::async(std::launch::async, [&]() {
            bytes = m_client->readBytes(
                m_folderId, m_mountId, QStringLiteral("src/foo.ts"), 4096, &fingerprint, &err);
        });

        QJsonObject dispatchFrame;
        QVERIFY2(spinUntil(
                     [&]() {
                         dispatchFrame = takeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
                         return !dispatchFrame.isEmpty();
                     },
                     3000),
                 "dispatch frame did not arrive");

        QCOMPARE(dispatchFrame.value(QStringLiteral("op")).toString(), QStringLiteral("vfs.read"));
        QCOMPARE(dispatchFrame.value(QStringLiteral("client_id")).toString(), m_clientId);
        const QJsonObject args = dispatchFrame.value(QStringLiteral("args")).toObject();
        QCOMPARE(args.value(QStringLiteral("folder_id")).toString(), m_folderId);
        QCOMPARE(args.value(QStringLiteral("mount_id")).toString(), m_mountId);
        QCOMPARE(args.value(QStringLiteral("rel_path")).toString(), QStringLiteral("src/foo.ts"));

        const QString reqId = dispatchFrame.value(QStringLiteral("request_id")).toString();
        QVERIFY(!reqId.isEmpty());

        const QByteArray serverBytes = QByteArrayLiteral("hello world");
        sendDaemonReply(
            reqId,
            true,
            QJsonObject{
                {QStringLiteral("content"), QString::fromLatin1(serverBytes.toBase64())},
                {QStringLiteral("fingerprint"), QStringLiteral("abc123fingerprint")},
            });

        QVERIFY2(spinUntil(
                     [&]() {
                         return fut.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
                     },
                     3000),
                 "readBytes did not complete after reply");
        fut.get();

        QVERIFY(err.isEmpty());
        QCOMPARE(bytes, serverBytes);
        QCOMPARE(fingerprint, QStringLiteral("abc123fingerprint"));
    }

    void executeCommandRoundTrip() {
        QString err;
        QJsonObject result;

        auto fut = std::async(std::launch::async, [&]() {
            result = m_client->executeCommand(m_folderId,
                                              m_mountId,
                                              QStringLiteral("conv-abc"),
                                              QStringLiteral("grep -r foo ."),
                                              0,
                                              1024 * 1024,
                                              &err);
        });

        QJsonObject dispatchFrame;
        QVERIFY2(spinUntil(
                     [&]() {
                         dispatchFrame = takeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
                         return !dispatchFrame.isEmpty();
                     },
                     3000),
                 "dispatch frame did not arrive");

        QCOMPARE(dispatchFrame.value(QStringLiteral("op")).toString(),
                 QStringLiteral("vfs.execute"));
        const QJsonObject args = dispatchFrame.value(QStringLiteral("args")).toObject();
        QCOMPARE(args.value(QStringLiteral("folder_id")).toString(), m_folderId);
        QCOMPARE(args.value(QStringLiteral("mount_id")).toString(), m_mountId);
        QCOMPARE(args.value(QStringLiteral("conversation_id")).toString(),
                 QStringLiteral("conv-abc"));
        QCOMPARE(args.value(QStringLiteral("command")).toString(), QStringLiteral("grep -r foo ."));

        const QString reqId = dispatchFrame.value(QStringLiteral("request_id")).toString();
        QVERIFY(!reqId.isEmpty());

        sendDaemonReply(reqId,
                        true,
                        QJsonObject{
                            {QStringLiteral("stdout"), QStringLiteral("match\n")},
                            {QStringLiteral("stderr"), QString()},
                            {QStringLiteral("exit_code"), 0},
                            {QStringLiteral("sandboxed"), true},
                            {QStringLiteral("timed_out"), false},
                            {QStringLiteral("truncated"), false},
                        });

        QVERIFY2(spinUntil(
                     [&]() {
                         return fut.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
                     },
                     3000),
                 "executeCommand did not complete after reply");
        fut.get();

        QVERIFY(err.isEmpty());
        QCOMPARE(result.value(QStringLiteral("stdout")).toString(), QStringLiteral("match\n"));
        QCOMPARE(result.value(QStringLiteral("exit_code")).toInt(), 0);
    }

    void readBytesErrorPropagates() {
        QString fingerprint, err;
        QByteArray bytes;

        auto fut = std::async(std::launch::async, [&]() {
            bytes = m_client->readBytes(
                m_folderId, m_mountId, QStringLiteral("src/foo.ts"), 4096, &fingerprint, &err);
        });

        QJsonObject dispatchFrame;
        QVERIFY2(spinUntil(
                     [&]() {
                         dispatchFrame = takeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
                         return !dispatchFrame.isEmpty();
                     },
                     3000),
                 "dispatch frame did not arrive");

        sendDaemonReply(dispatchFrame.value(QStringLiteral("request_id")).toString(),
                        false,
                        {},
                        QStringLiteral("stale_fingerprint"),
                        QStringLiteral("file changed since prior read"));

        QVERIFY2(spinUntil(
                     [&]() {
                         return fut.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
                     },
                     3000),
                 "readBytes did not complete after error reply");
        fut.get();

        QCOMPARE(err, QStringLiteral("stale_fingerprint"));
        QCOMPARE(bytes.size(), 0);
        QVERIFY(fingerprint.isEmpty());
    }

    void readBytesMountOfflineNoDispatch() {
        QString fingerprint, err;
        const QString unregisteredFolder = freshUuid();

        auto fut = std::async(std::launch::async, [&]() {
            return m_client->readBytes(unregisteredFolder,
                                       freshUuid(),
                                       QStringLiteral("src/foo.ts"),
                                       4096,
                                       &fingerprint,
                                       &err);
        });

        QVERIFY2(spinUntil(
                     [&]() {
                         return fut.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
                     },
                     3000),
                 "readBytes did not return for offline mount");
        const QByteArray bytes = fut.get();

        QCOMPARE(err, QStringLiteral("mount_offline"));
        QCOMPARE(bytes.size(), 0);

        for (const QString& f : m_receivedFrames) {
            QJsonParseError jerr;
            const QJsonDocument doc = QJsonDocument::fromJson(f.toUtf8(), &jerr);
            if (jerr.error != QJsonParseError::NoError)
                continue;
            if (!doc.isObject())
                continue;
            QVERIFY2(doc.object().value(QStringLiteral("type")).toString() !=
                         QString::fromLatin1(IpcType::ClientRpcDispatch),
                     "mount-offline rejection must not emit dispatch");
        }
    }

    void writeBytesRoundTrip() {
        QString err;
        qint64 applied = -1;
        const QByteArray payload = QByteArrayLiteral("new content");

        auto fut = std::async(std::launch::async, [&]() {
            applied = m_client->writeBytes(m_folderId,
                                           m_mountId,
                                           QStringLiteral("src/foo.ts"),
                                           payload,
                                           QStringLiteral("prior-fingerprint"),
                                           &err);
        });

        QJsonObject dispatchFrame;
        QVERIFY2(spinUntil(
                     [&]() {
                         dispatchFrame = takeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
                         return !dispatchFrame.isEmpty();
                     },
                     3000),
                 "dispatch frame did not arrive");

        QCOMPARE(dispatchFrame.value(QStringLiteral("op")).toString(), QStringLiteral("vfs.write"));
        const QJsonObject args = dispatchFrame.value(QStringLiteral("args")).toObject();
        QCOMPARE(
            QByteArray::fromBase64(args.value(QStringLiteral("content")).toString().toLatin1()),
            payload);
        QCOMPARE(args.value(QStringLiteral("expected_fingerprint")).toString(),
                 QStringLiteral("prior-fingerprint"));

        sendDaemonReply(dispatchFrame.value(QStringLiteral("request_id")).toString(),
                        true,
                        QJsonObject{
                            {QStringLiteral("applied_bytes"), static_cast<double>(payload.size())},
                        });

        QVERIFY2(spinUntil(
                     [&]() {
                         return fut.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
                     },
                     3000),
                 "writeBytes did not complete after reply");
        fut.get();

        QVERIFY(err.isEmpty());
        QCOMPARE(applied, static_cast<qint64>(payload.size()));
    }

    void listDirRoundTrip() {
        QString err;
        QJsonArray entries;

        auto fut = std::async(std::launch::async, [&]() {
            entries = m_client->listDir(m_folderId, m_mountId, QStringLiteral("src"), false, &err);
        });

        QJsonObject dispatchFrame;
        QVERIFY2(spinUntil(
                     [&]() {
                         dispatchFrame = takeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
                         return !dispatchFrame.isEmpty();
                     },
                     3000),
                 "dispatch frame did not arrive");

        QCOMPARE(dispatchFrame.value(QStringLiteral("op")).toString(), QStringLiteral("vfs.list"));
        const QJsonObject args = dispatchFrame.value(QStringLiteral("args")).toObject();
        QCOMPARE(args.value(QStringLiteral("recursive")).toBool(), false);

        const QJsonArray scriptedEntries{
            QJsonObject{
                {QStringLiteral("path"), QStringLiteral("src/foo.ts")},
                {QStringLiteral("kind"), QStringLiteral("file")},
                {QStringLiteral("size"), 42},
            },
            QJsonObject{
                {QStringLiteral("path"), QStringLiteral("src/bar.ts")},
                {QStringLiteral("kind"), QStringLiteral("file")},
                {QStringLiteral("size"), 99},
            },
        };
        sendDaemonReply(dispatchFrame.value(QStringLiteral("request_id")).toString(),
                        true,
                        QJsonObject{{QStringLiteral("entries"), scriptedEntries}});

        QVERIFY2(spinUntil(
                     [&]() {
                         return fut.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
                     },
                     3000),
                 "listDir did not complete after reply");
        fut.get();

        QVERIFY(err.isEmpty());
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).toObject().value(QStringLiteral("path")).toString(),
                 QStringLiteral("src/foo.ts"));
    }

    void statPathRoundTrip() {
        QString err;
        QJsonObject stat;

        auto fut = std::async(std::launch::async, [&]() {
            stat = m_client->statPath(m_folderId, m_mountId, QStringLiteral("src/foo.ts"), &err);
        });

        QJsonObject dispatchFrame;
        QVERIFY2(spinUntil(
                     [&]() {
                         dispatchFrame = takeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
                         return !dispatchFrame.isEmpty();
                     },
                     3000),
                 "dispatch frame did not arrive");

        QCOMPARE(dispatchFrame.value(QStringLiteral("op")).toString(), QStringLiteral("vfs.stat"));

        sendDaemonReply(dispatchFrame.value(QStringLiteral("request_id")).toString(),
                        true,
                        QJsonObject{
                            {QStringLiteral("exists"), true},
                            {QStringLiteral("kind"), QStringLiteral("file")},
                            {QStringLiteral("size"), 42},
                            {QStringLiteral("mtime_ms"), static_cast<qint64>(1717000000000LL)},
                        });

        QVERIFY2(spinUntil(
                     [&]() {
                         return fut.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
                     },
                     3000),
                 "statPath did not complete after reply");
        fut.get();

        QVERIFY(err.isEmpty());
        QCOMPARE(stat.value(QStringLiteral("kind")).toString(), QStringLiteral("file"));
        QCOMPARE(stat.value(QStringLiteral("size")).toInt(), 42);
    }
};

QTEST_MAIN(TestFolderMountClientWire)
#include "test-folder-mount-client-wire.moc"

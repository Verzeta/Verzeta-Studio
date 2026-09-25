// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "remote/server/wire-auth.h"
#include "remote/server/wire-host-client.h"
#include "remote/server/wire-server.h"
#include "remote/wire-protocol.h"

#include <QTcpServer>
#include <QtTest>

#include <memory>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QWebSocket>

using Verzeta::Remote::WireAuth;
using Verzeta::Remote::WireHostClient;
using Verzeta::Remote::WireServer;
namespace IpcType = Verzeta::Remote::IpcType;
namespace WsType = Verzeta::Remote::WsType;

namespace {

QByteArray frameOf(const QJsonObject& obj) {
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray out;
    const quint32 len = static_cast<quint32>(payload.size());
    out.append(static_cast<char>((len >> 24) & 0xff));
    out.append(static_cast<char>((len >> 16) & 0xff));
    out.append(static_cast<char>((len >> 8) & 0xff));
    out.append(static_cast<char>(len & 0xff));
    out.append(payload);
    return out;
}

quint16 reserveFreePort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return 0;
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

template <typename Pred> bool spinUntil(Pred pred, int timeoutMs = 2000) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

}  // namespace

class TestWireHostClientExecGate : public QObject {
    Q_OBJECT

  private:
    QString m_bridgeName;
    std::unique_ptr<WireAuth> m_auth;
    std::unique_ptr<QLocalServer> m_bridgeServer;
    QLocalSocket* m_bridgeSide = nullptr;
    std::unique_ptr<WireHostClient> m_host;
    std::unique_ptr<WireServer> m_server;
    std::unique_ptr<QWebSocket> m_client;

    QByteArray m_bridgeRx;
    QList<QJsonObject> m_bridgeFrames;
    QStringList m_clientFrames;

    void drainBridgeRx() {
        while (m_bridgeRx.size() >= 4) {
            const quint32 len = (static_cast<quint8>(m_bridgeRx[0]) << 24) |
                                (static_cast<quint8>(m_bridgeRx[1]) << 16) |
                                (static_cast<quint8>(m_bridgeRx[2]) << 8) |
                                static_cast<quint8>(m_bridgeRx[3]);
            if (m_bridgeRx.size() < static_cast<int>(4 + len))
                return;
            const QByteArray payload = m_bridgeRx.mid(4, len);
            m_bridgeRx.remove(0, 4 + len);
            const QJsonDocument doc = QJsonDocument::fromJson(payload);
            if (doc.isObject())
                m_bridgeFrames.append(doc.object());
        }
    }

    bool hasBridgeFrame(const QString& type) const {
        for (const QJsonObject& f : m_bridgeFrames) {
            if (f.value(QStringLiteral("type")).toString() == type)
                return true;
        }
        return false;
    }

    QJsonObject takeBridgeFrame(const QString& type) {
        for (int i = 0; i < m_bridgeFrames.size(); ++i) {
            if (m_bridgeFrames.at(i).value(QStringLiteral("type")).toString() == type) {
                return m_bridgeFrames.takeAt(i);
            }
        }
        return {};
    }

    bool clientGotRequestFor(const QString& op) const {
        for (const QString& raw : m_clientFrames) {
            const QJsonObject o = QJsonDocument::fromJson(raw.toUtf8()).object();
            if (o.value(QStringLiteral("type")).toString() == WsType::Request &&
                o.value(QStringLiteral("op")).toString() == op) {
                return true;
            }
        }
        return false;
    }

    QString buildAuthedSession(bool advertiseExec) {
        m_auth = std::make_unique<WireAuth>();
        if (!m_auth->open())
            return {};

        m_bridgeServer = std::make_unique<QLocalServer>();
        QLocalServer::removeServer(m_bridgeName);
        if (!m_bridgeServer->listen(m_bridgeName))
            return {};
        QObject::connect(m_bridgeServer.get(), &QLocalServer::newConnection, this, [this]() {
            m_bridgeSide = m_bridgeServer->nextPendingConnection();
            if (m_bridgeSide) {
                QObject::connect(m_bridgeSide, &QLocalSocket::readyRead, this, [this]() {
                    m_bridgeRx.append(m_bridgeSide->readAll());
                    drainBridgeRx();
                });
            }
        });

        const quint16 wsPort = reserveFreePort();
        if (wsPort == 0)
            return {};

        m_host = std::make_unique<WireHostClient>();
        m_server = std::make_unique<WireServer>(m_auth.get(), m_host.get(), nullptr);
        m_host->setWireServer(m_server.get());
        if (!m_server->listen(QStringLiteral("127.0.0.1"), wsPort))
            return {};
        m_host->start();

        if (!spinUntil([&] { return m_bridgeSide != nullptr; }))
            return {};

        m_client = std::make_unique<QWebSocket>();
        QObject::connect(m_client.get(),
                         &QWebSocket::textMessageReceived,
                         this,
                         [this](const QString& f) { m_clientFrames.append(f); });
        m_client->open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(wsPort)));
        if (!spinUntil([&] { return m_client->state() == QAbstractSocket::ConnectedState; })) {
            return {};
        }

        const auto issued = m_auth->issueToken(QStringLiteral("ext-test"));
        if (!issued.has_value())
            return {};
        QJsonObject params{{QStringLiteral("token"), issued->token}};
        if (advertiseExec) {
            params.insert(QStringLiteral("capabilities"),
                          QJsonArray{QStringLiteral("vfs.execute")});
        }
        const QJsonObject authFrame{
            {QStringLiteral("op"), QStringLiteral("auth.token")},
            {QStringLiteral("request_id"), QStringLiteral("a1")},
            {QStringLiteral("params"), params},
        };
        m_client->sendTextMessage(
            QString::fromUtf8(QJsonDocument(authFrame).toJson(QJsonDocument::Compact)));

        const QString clientId = issued->clientId;
        if (!spinUntil([&] { return m_server->sessionForClientId(clientId) != nullptr; })) {
            return {};
        }
        m_clientFrames.clear();
        return clientId;
    }

    void dispatch(const QString& reqId, const QString& clientId, const QString& op) {
        const QJsonObject frame{
            {QStringLiteral("type"), QString::fromLatin1(IpcType::ClientRpcDispatch)},
            {QStringLiteral("request_id"), reqId},
            {QStringLiteral("client_id"), clientId},
            {QStringLiteral("op"), op},
            {QStringLiteral("args"), QJsonObject{}},
            {QStringLiteral("timeout_ms"), 5000},
        };
        m_bridgeSide->write(frameOf(frame));
        m_bridgeSide->flush();
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        m_bridgeName = QStringLiteral("verzeta-test-bridge-%1").arg(reserveFreePort());
        qputenv("VERZETA_BRIDGE_SOCKET", m_bridgeName.toUtf8());
    }

    void cleanup() {
        m_client.reset();
        m_server.reset();
        m_host.reset();
        m_bridgeServer.reset();
        m_bridgeSide = nullptr;
        m_auth.reset();
        m_bridgeRx.clear();
        m_bridgeFrames.clear();
        m_clientFrames.clear();
    }

    void test_execRefusedWhenCapabilityAbsent() {
        const QString cid = buildAuthedSession(false);
        QVERIFY(!cid.isEmpty());

        dispatch(QStringLiteral("r1"), cid, QStringLiteral("vfs.execute"));

        QVERIFY(spinUntil(
            [&] { return hasBridgeFrame(QString::fromLatin1(IpcType::ClientRpcReply)); }));
        const QJsonObject reply = takeBridgeFrame(QString::fromLatin1(IpcType::ClientRpcReply));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(reply.value(QStringLiteral("error"))
                     .toObject()
                     .value(QStringLiteral("kind"))
                     .toString(),
                 QStringLiteral("exec_unsupported"));
        QVERIFY(!clientGotRequestFor(QStringLiteral("vfs.execute")));
    }

    void test_execForwardedWhenCapabilityPresent() {
        const QString cid = buildAuthedSession(true);
        QVERIFY(!cid.isEmpty());

        dispatch(QStringLiteral("r2"), cid, QStringLiteral("vfs.execute"));

        QVERIFY(spinUntil([&] { return clientGotRequestFor(QStringLiteral("vfs.execute")); }));
        QVERIFY(takeBridgeFrame(QString::fromLatin1(IpcType::ClientRpcReply)).isEmpty());
    }

    void test_otherOpForwardedWithoutCapability() {
        const QString cid = buildAuthedSession(false);
        QVERIFY(!cid.isEmpty());

        dispatch(QStringLiteral("r3"), cid, QStringLiteral("vfs.read"));

        QVERIFY(spinUntil([&] { return clientGotRequestFor(QStringLiteral("vfs.read")); }));
    }
};

QTEST_MAIN(TestWireHostClientExecGate)
#include "test-wire-host-client-exec-gate.moc"

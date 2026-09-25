// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "remote/server/wire-auth.h"
#include "remote/server/wire-session.h"

#include <QTcpServer>
#include <QTimer>
#include <QtTest>

#include <memory>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QWebSocket>
#include <QWebSocketServer>

using Verzeta::Remote::WireAuth;
using Verzeta::Remote::WireSession;

namespace {

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

class TestWireSessionAuthCapabilities : public QObject {
    Q_OBJECT

  private:
    quint16 m_port = 0;
    std::unique_ptr<WireAuth> m_auth;
    std::unique_ptr<QWebSocketServer> m_server;
    std::unique_ptr<QWebSocket> m_clientSide;
    std::unique_ptr<WireSession> m_session;

    bool buildSession() {
        m_auth = std::make_unique<WireAuth>();
        if (!m_auth->open())
            return false;

        m_port = reserveFreePort();
        if (m_port == 0)
            return false;

        m_server = std::make_unique<QWebSocketServer>(QStringLiteral("test-wire-auth-caps"),
                                                      QWebSocketServer::NonSecureMode);
        if (!m_server->listen(QHostAddress::LocalHost, m_port))
            return false;

        QWebSocket* serverSideRaw = nullptr;
        QObject::connect(
            m_server.get(), &QWebSocketServer::newConnection, this, [&serverSideRaw, this]() {
                serverSideRaw = m_server->nextPendingConnection();
            });

        m_clientSide = std::make_unique<QWebSocket>();
        m_clientSide->open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_port)));

        if (!spinUntil([&] { return serverSideRaw != nullptr; }))
            return false;
        if (!spinUntil([&] { return m_clientSide->state() == QAbstractSocket::ConnectedState; })) {
            return false;
        }

        m_session = std::make_unique<WireSession>(serverSideRaw, m_auth.get(), nullptr, nullptr);
        return true;
    }

    void sendOp(const QString& op, const QJsonObject& params) {
        const QJsonObject frame{
            {QStringLiteral("op"), op},
            {QStringLiteral("request_id"), QStringLiteral("r1")},
            {QStringLiteral("params"), params},
        };
        m_clientSide->sendTextMessage(
            QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanup() {
        m_session.reset();
        m_clientSide.reset();
        m_server.reset();
        m_auth.reset();
    }

    void test_authPair_recordsAdvertisedCapabilities() {
        QVERIFY(buildSession());
        const QString code = m_auth->generatePairingCode();
        QVERIFY(!code.isEmpty());

        sendOp(QStringLiteral("auth.pair"),
               QJsonObject{
                   {QStringLiteral("code"), code},
                   {QStringLiteral("client_name"), QStringLiteral("ext-test")},
                   {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("vfs.execute")}},
               });

        QVERIFY(spinUntil([&] { return m_session->isAuthenticated(); }));
        QVERIFY(m_session->hasCapability(QStringLiteral("vfs.execute")));
        QVERIFY(!m_session->hasCapability(QStringLiteral("vfs.unknown")));
    }

    void test_authToken_recordsAdvertisedCapabilities() {
        QVERIFY(buildSession());
        const auto issued = m_auth->issueToken(QStringLiteral("ext-test"));
        QVERIFY(issued.has_value());

        sendOp(QStringLiteral("auth.token"),
               QJsonObject{
                   {QStringLiteral("token"), issued->token},
                   {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("vfs.execute")}},
               });

        QVERIFY(spinUntil([&] { return m_session->isAuthenticated(); }));
        QVERIFY(m_session->hasCapability(QStringLiteral("vfs.execute")));
    }

    void test_authToken_noCapabilities_hasNone() {
        QVERIFY(buildSession());
        const auto issued = m_auth->issueToken(QStringLiteral("legacy-test"));
        QVERIFY(issued.has_value());

        sendOp(QStringLiteral("auth.token"),
               QJsonObject{
                   {QStringLiteral("token"), issued->token},
               });

        QVERIFY(spinUntil([&] { return m_session->isAuthenticated(); }));
        QVERIFY(!m_session->hasCapability(QStringLiteral("vfs.execute")));
    }
};

QTEST_MAIN(TestWireSessionAuthCapabilities)
#include "test-wire-session-auth-capabilities.moc"

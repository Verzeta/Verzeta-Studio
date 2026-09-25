// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "remote/server/wire-auth.h"
#include "remote/server/wire-session.h"
#include "remote/wire-protocol.h"

#include <QTcpServer>
#include <QTest>

#include <memory>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QWebSocket>
#include <QWebSocketServer>

using Verzeta::Remote::kMaxStdinBytes;
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

class TestWireSessionCanvasStdin : public QObject {
    Q_OBJECT

  private:
    quint16 m_port = 0;
    std::unique_ptr<WireAuth> m_auth;
    std::unique_ptr<QWebSocketServer> m_server;
    std::unique_ptr<QWebSocket> m_clientSide;
    std::unique_ptr<WireSession> m_session;
    QList<QJsonObject> m_responses;

    bool buildAuthedSession() {
        m_auth = std::make_unique<WireAuth>();
        if (!m_auth->open())
            return false;

        m_port = reserveFreePort();
        if (m_port == 0)
            return false;

        m_server = std::make_unique<QWebSocketServer>(QStringLiteral("test-canvas-stdin"),
                                                      QWebSocketServer::NonSecureMode);
        if (!m_server->listen(QHostAddress::LocalHost, m_port))
            return false;

        QWebSocket* serverSideRaw = nullptr;
        QObject::connect(
            m_server.get(), &QWebSocketServer::newConnection, this, [&serverSideRaw, this]() {
                serverSideRaw = m_server->nextPendingConnection();
            });

        m_clientSide = std::make_unique<QWebSocket>();
        QObject::connect(
            m_clientSide.get(), &QWebSocket::textMessageReceived, this, [this](const QString& msg) {
                m_responses.append(QJsonDocument::fromJson(msg.toUtf8()).object());
            });
        m_clientSide->open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_port)));

        if (!spinUntil([&] { return serverSideRaw != nullptr; }))
            return false;
        if (!spinUntil([&] { return m_clientSide->state() == QAbstractSocket::ConnectedState; })) {
            return false;
        }

        m_session = std::make_unique<WireSession>(serverSideRaw, m_auth.get(), nullptr, nullptr);

        const auto issued = m_auth->issueToken(QStringLiteral("stdin-test"));
        if (!issued.has_value())
            return false;
        sendOp(QStringLiteral("auth.token"), QJsonObject{{QStringLiteral("token"), issued->token}});
        if (!spinUntil([&] { return m_session->isAuthenticated(); }))
            return false;

        m_responses.clear();
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

    QString errorKindFor(const QString& op, const QJsonObject& params) {
        m_responses.clear();
        sendOp(op, params);
        if (!spinUntil([&] { return !m_responses.isEmpty(); }))
            return {};
        const QJsonObject r = m_responses.constLast();
        if (r.value(QStringLiteral("ok")).toBool())
            return {};
        return r.value(QStringLiteral("error")).toObject().value(QStringLiteral("kind")).toString();
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanup() {
        m_session.reset();
        m_clientSide.reset();
        m_server.reset();
        m_auth.reset();
        m_responses.clear();
    }

    void test_control_unknownOp_reportsUnknownOp() {
        QVERIFY(buildAuthedSession());
        QCOMPARE(errorKindFor(QStringLiteral("canvas.run.no_such_op"), {}),
                 QStringLiteral("unknown_op"));
    }

    void test_sendInput_missingText_invalidParams() {
        QVERIFY(buildAuthedSession());
        QCOMPARE(errorKindFor(QStringLiteral("canvas.run.send_input"), {}),
                 QStringLiteral("invalid_params"));
    }

    void test_sendInput_nonStringText_invalidParams() {
        QVERIFY(buildAuthedSession());
        QCOMPARE(errorKindFor(QStringLiteral("canvas.run.send_input"),
                              QJsonObject{{QStringLiteral("text"), 42}}),
                 QStringLiteral("invalid_params"));
    }

    void test_sendInput_oversizedText_payloadTooLarge() {
        QVERIFY(buildAuthedSession());
        const QString big(static_cast<int>(kMaxStdinBytes) + 1, QLatin1Char('x'));
        QCOMPARE(errorKindFor(QStringLiteral("canvas.run.send_input"),
                              QJsonObject{{QStringLiteral("text"), big}}),
                 QStringLiteral("payload_too_large"));
    }

    void test_sendInput_capIsMeasuredInUtf8Bytes() {
        QVERIFY(buildAuthedSession());
        const QString multibyte(static_cast<int>(kMaxStdinBytes / 2) + 1, QChar(0x00E9));
        QVERIFY(multibyte.size() < kMaxStdinBytes);
        QVERIFY(multibyte.toUtf8().size() > kMaxStdinBytes);
        QCOMPARE(errorKindFor(QStringLiteral("canvas.run.send_input"),
                              QJsonObject{{QStringLiteral("text"), multibyte}}),
                 QStringLiteral("payload_too_large"));
    }

    void test_sendInput_valid_passesValidation_reachesHostGuard() {
        QVERIFY(buildAuthedSession());
        const QString kind =
            errorKindFor(QStringLiteral("canvas.run.send_input"),
                         QJsonObject{{QStringLiteral("text"), QStringLiteral("Ada")}});
        QCOMPARE(kind, QStringLiteral("server_error"));
        QVERIFY(kind != QStringLiteral("unknown_op"));
    }

    void test_sendInput_multilineText_isAccepted() {
        QVERIFY(buildAuthedSession());
        QCOMPARE(errorKindFor(
                     QStringLiteral("canvas.run.send_input"),
                     QJsonObject{{QStringLiteral("text"), QStringLiteral("line one\nline two")}}),
                 QStringLiteral("server_error"));
    }

    void test_eof_isDispatched_reachesHostGuard() {
        QVERIFY(buildAuthedSession());
        const QString kind = errorKindFor(QStringLiteral("canvas.run.eof"), {});
        QCOMPARE(kind, QStringLiteral("server_error"));
        QVERIFY(kind != QStringLiteral("unknown_op"));
    }
};

QTEST_MAIN(TestWireSessionCanvasStdin)
#include "test-wire-session-canvas-stdin.moc"

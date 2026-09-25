// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "remote/server/wire-session.h"

#include <QTcpServer>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <memory>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>
#include <vector>

using Verzeta::Remote::ClientRpcReply;
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

template <typename Predicate> bool spinUntil(Predicate predicate, int timeoutMs) {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (predicate())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return predicate();
}

}  // namespace

class TestWireSessionClientRpc : public QObject {
    Q_OBJECT

  private:
    quint16 m_port = 0;

    std::unique_ptr<QWebSocketServer> m_server;

    std::unique_ptr<QWebSocket> m_clientSide;

    std::unique_ptr<WireSession> m_session;

    QStringList m_clientReceivedFrames;

    QJsonObject waitForClientFrame(const QString& expectedType, int timeoutMs = 2000) {
        const bool got = spinUntil(
            [&] {
                for (const QString& frame : m_clientReceivedFrames) {
                    QJsonParseError err;
                    const QJsonDocument doc = QJsonDocument::fromJson(frame.toUtf8(), &err);
                    if (err.error != QJsonParseError::NoError)
                        continue;
                    if (!doc.isObject())
                        continue;
                    if (doc.object().value(QStringLiteral("type")).toString() == expectedType) {
                        return true;
                    }
                }
                return false;
            },
            timeoutMs);
        if (!got)
            return {};

        for (int i = 0; i < m_clientReceivedFrames.size(); ++i) {
            QJsonParseError err;
            const QJsonDocument doc =
                QJsonDocument::fromJson(m_clientReceivedFrames.at(i).toUtf8(), &err);
            if (err.error != QJsonParseError::NoError)
                continue;
            if (!doc.isObject())
                continue;
            if (doc.object().value(QStringLiteral("type")).toString() == expectedType) {
                const QJsonObject result = doc.object();
                m_clientReceivedFrames.removeAt(i);
                return result;
            }
        }
        return {};
    }

    void sendClientResponse(const QString& reqId,
                            bool ok,
                            const QJsonValue& data,
                            const QString& errKind = {},
                            const QString& errDetail = {}) {
        QJsonObject payload{
            {QStringLiteral("type"), QStringLiteral("client_response")},
            {QStringLiteral("request_id"), reqId},
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
        m_clientSide->sendTextMessage(
            QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    }

  private slots:
    void init() {
        m_clientReceivedFrames.clear();
        m_port = reserveFreePort();
        QVERIFY2(m_port != 0, "failed to reserve a free TCP port");

        m_server = std::make_unique<QWebSocketServer>(QStringLiteral("test-wire-rpc"),
                                                      QWebSocketServer::NonSecureMode);
        QVERIFY2(m_server->listen(QHostAddress::LocalHost, m_port), "WS server failed to bind");

        m_clientSide = std::make_unique<QWebSocket>();
        QObject::connect(m_clientSide.get(),
                         &QWebSocket::textMessageReceived,
                         this,
                         [this](const QString& frame) { m_clientReceivedFrames.append(frame); });

        QWebSocket* serverSideRaw = nullptr;
        QObject::connect(
            m_server.get(), &QWebSocketServer::newConnection, this, [&serverSideRaw, this]() {
                serverSideRaw = m_server->nextPendingConnection();
            });

        m_clientSide->open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_port)));

        QVERIFY2(spinUntil([&] { return serverSideRaw != nullptr; }, 2000),
                 "server failed to accept connection");
        QVERIFY2(
            spinUntil([this] { return m_clientSide->state() == QAbstractSocket::ConnectedState; },
                      2000),
            "client failed to reach Connected state");

        m_session = std::make_unique<WireSession>(serverSideRaw, nullptr, nullptr, nullptr);

        (void)waitForClientFrame(QStringLiteral("event"), 1000);
    }

    void cleanup() {
        m_session.reset();
        if (m_clientSide)
            m_clientSide->close();
        m_clientSide.reset();
        if (m_server)
            m_server->close();
        m_server.reset();
        m_clientReceivedFrames.clear();
    }

    void roundTripFiresCallback() {
        std::atomic<int> callCount{0};
        QJsonValue receivedData;
        bool receivedOk = false;

        const qint64 deadlineMs = m_session->sendClientRequest(
            QStringLiteral("vfs.echo"),
            QJsonObject{{QStringLiteral("n"), 42}},
            [&](const ClientRpcReply& reply) {
                ++callCount;
                receivedOk = reply.ok;
                receivedData = reply.data;
            },
            5000);
        QVERIFY2(deadlineMs > 0, "sendClientRequest should accept a valid call");

        const QJsonObject reqFrame = waitForClientFrame(QStringLiteral("request"));
        QCOMPARE(reqFrame.value(QStringLiteral("op")).toString(), QStringLiteral("vfs.echo"));
        const QString reqId = reqFrame.value(QStringLiteral("request_id")).toString();
        QVERIFY(!reqId.isEmpty());

        sendClientResponse(reqId, true, QJsonObject{{QStringLiteral("n"), 42}});

        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 2000),
                 "callback did not fire within 2 s");
        QVERIFY(receivedOk);
        QCOMPARE(receivedData.toObject().value(QStringLiteral("n")).toInt(), 42);
    }

    void timeoutFiresCallback() {
        std::atomic<int> callCount{0};
        QString receivedKind;

        const qint64 deadlineMs = m_session->sendClientRequest(
            QStringLiteral("vfs.never_replies"),
            QJsonObject{},
            [&](const ClientRpcReply& reply) {
                ++callCount;
                receivedKind = reply.errorKind;
            },
            150);
        QVERIFY(deadlineMs > 0);

        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 2500),
                 "timeout callback did not fire within budget");
        QCOMPARE(receivedKind, QStringLiteral("timeout"));
    }

    void sessionCloseDrainsPending() {
        std::atomic<int> callCount{0};
        QString receivedKind;
        m_session->sendClientRequest(
            QStringLiteral("vfs.will_be_drained"),
            QJsonObject{},
            [&](const ClientRpcReply& reply) {
                ++callCount;
                receivedKind = reply.errorKind;
            },
            10000);

        (void)waitForClientFrame(QStringLiteral("request"));

        m_clientSide->close();

        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 3000),
                 "session-close drain did not fire callback");
        QCOMPARE(receivedKind, QStringLiteral("session_closed"));
    }

    void capacityRejectionAtCap() {
        std::atomic<int> rejections{0};
        QString lastKind;
        for (int i = 0; i < WireSession::kMaxPendingClientRequests; ++i) {
            const qint64 deadline = m_session->sendClientRequest(
                QStringLiteral("vfs.fill"), QJsonObject{}, [](const ClientRpcReply&) {}, 30000);
            QVERIFY2(deadline > 0, "fill-phase sendClientRequest should succeed");
        }

        const qint64 deadline = m_session->sendClientRequest(
            QStringLiteral("vfs.overflow"),
            QJsonObject{},
            [&](const ClientRpcReply& reply) {
                ++rejections;
                lastKind = reply.errorKind;
            },
            30000);
        QCOMPARE(deadline, qint64(0));
        QCOMPARE(rejections.load(), 1);
        QCOMPARE(lastKind, QStringLiteral("too_many_pending"));
    }

    void unknownRequestIdIgnored() {
        sendClientResponse(QStringLiteral("not-a-real-uuid"), true, QJsonObject{});
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        QVERIFY(true);
    }

    void emptyOpRejectsSynchronously() {
        std::atomic<int> rejections{0};
        QString lastKind;
        const qint64 deadline = m_session->sendClientRequest(
            QString(),
            QJsonObject{},
            [&](const ClientRpcReply& reply) {
                ++rejections;
                lastKind = reply.errorKind;
            },
            5000);
        QCOMPARE(deadline, qint64(0));
        QCOMPARE(rejections.load(), 1);
        QCOMPARE(lastKind, QStringLiteral("invalid_argument"));
    }

    void nullCallbackRejects() {
        const qint64 deadline = m_session->sendClientRequest(
            QStringLiteral("vfs.no_callback"), QJsonObject{}, nullptr, 5000);
        QCOMPARE(deadline, qint64(0));
    }
};

QTEST_MAIN(TestWireSessionClientRpc)
#include "test-wire-session-client-rpc.moc"

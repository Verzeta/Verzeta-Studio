// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "remote/wire-host-bridge.h"
#include "remote/wire-protocol.h"

#include <QTimer>
#include <QtTest>

#include <atomic>
#include <memory>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>
#include <vector>

using Verzeta::Remote::BridgeClientRpcCallback;
using Verzeta::Remote::BridgeClientRpcReply;
using Verzeta::Remote::WireHostBridge;
namespace IpcType = Verzeta::Remote::IpcType;

namespace {

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

}  // namespace

class TestWireHostBridgeClientRpc : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<WireHostBridge> m_bridge;

    std::unique_ptr<QLocalSocket> m_daemonSocket;

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

    QJsonObject waitForBridgeFrame(const QString& expectedType, int timeoutMs = 2000) {
        const bool got = spinUntil(
            [&] {
                for (const QString& frame : m_receivedFrames) {
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
                         const QJsonValue& data = {},
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
                QByteArrayLiteral("verzeta-host-bridge-test-rpc-") +
                    QByteArray::number(QCoreApplication::applicationPid()));
    }

    void init() {
        m_rxBuf.clear();
        m_receivedFrames.clear();

        m_bridge = std::make_unique<WireHostBridge>(WireHostBridge::Services{});

        m_daemonSocket = std::make_unique<QLocalSocket>();
        QObject::connect(m_daemonSocket.get(),
                         &QLocalSocket::readyRead,
                         this,
                         &TestWireHostBridgeClientRpc::onDaemonReadyRead);

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

        (void)waitForBridgeFrame(QString::fromLatin1(IpcType::Hello), 1000);
    }

    void cleanup() {
        if (m_daemonSocket) {
            m_daemonSocket->disconnectFromServer();
            m_daemonSocket->waitForDisconnected(1000);
            m_daemonSocket.reset();
        }
        m_bridge.reset();
        m_rxBuf.clear();
        m_receivedFrames.clear();
    }

    void roundTripFiresCallback() {
        std::atomic<int> callCount{0};
        QJsonValue receivedData;
        bool receivedOk = false;

        m_bridge->dispatchClientRequest(
            QStringLiteral("client-uuid-abc"),
            QStringLiteral("vfs.echo"),
            QJsonObject{{QStringLiteral("n"), 42}},
            [&](const BridgeClientRpcReply& reply) {
                receivedOk = reply.ok;
                receivedData = reply.data;
                ++callCount;
            },
            5000);

        const QJsonObject dispatchFrame =
            waitForBridgeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
        QVERIFY(!dispatchFrame.isEmpty());
        QCOMPARE(dispatchFrame.value(QStringLiteral("op")).toString(), QStringLiteral("vfs.echo"));
        QCOMPARE(dispatchFrame.value(QStringLiteral("client_id")).toString(),
                 QStringLiteral("client-uuid-abc"));
        const QString reqId = dispatchFrame.value(QStringLiteral("request_id")).toString();
        QVERIFY(!reqId.isEmpty());

        sendDaemonReply(reqId, true, QJsonObject{{QStringLiteral("n"), 42}});

        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 3000),
                 "bridge callback did not fire after daemon reply");
        QVERIFY(receivedOk);
        QCOMPARE(receivedData.toObject().value(QStringLiteral("n")).toInt(), 42);
    }

    void timeoutFiresCallback() {
        std::atomic<int> callCount{0};
        QString receivedKind;

        m_bridge->dispatchClientRequest(
            QStringLiteral("client-uuid-xyz"),
            QStringLiteral("vfs.never_replies"),
            QJsonObject{},
            [&](const BridgeClientRpcReply& reply) {
                receivedKind = reply.errorKind;
                ++callCount;
            },
            150);

        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 2500),
                 "timeout callback did not fire within budget");
        QCOMPARE(receivedKind, QStringLiteral("timeout"));
    }

    void ipcDisconnectDrainsPending() {
        std::atomic<int> callCount{0};
        QString receivedKind;

        m_bridge->dispatchClientRequest(
            QStringLiteral("client-uuid-drain"),
            QStringLiteral("vfs.will_be_drained"),
            QJsonObject{},
            [&](const BridgeClientRpcReply& reply) {
                receivedKind = reply.errorKind;
                ++callCount;
            },
            30000);

        (void)waitForBridgeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));

        m_daemonSocket->disconnectFromServer();
        m_daemonSocket->waitForDisconnected(1000);

        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 3000),
                 "IPC-disconnect drain did not fire callback");
        QCOMPARE(receivedKind, QStringLiteral("bridge_disconnected"));
    }

    void capacityRejectionAtCap() {
        std::atomic<int> rejections{0};
        QString lastKind;

        for (int i = 0; i < WireHostBridge::kMaxPendingBridgeRpc; ++i) {
            m_bridge->dispatchClientRequest(
                QStringLiteral("client-uuid-fill"),
                QStringLiteral("vfs.fill"),
                QJsonObject{},
                [](const BridgeClientRpcReply&) {},
                30000);
        }

        QVERIFY2(
            spinUntil(
                [this] { return m_receivedFrames.size() >= WireHostBridge::kMaxPendingBridgeRpc; },
                5000),
            "fill phase did not produce 256 outbound frames");

        m_receivedFrames.clear();

        m_bridge->dispatchClientRequest(
            QStringLiteral("client-uuid-overflow"),
            QStringLiteral("vfs.overflow"),
            QJsonObject{},
            [&](const BridgeClientRpcReply& reply) {
                lastKind = reply.errorKind;
                ++rejections;
            },
            30000);

        QVERIFY2(spinUntil([&] { return rejections.load() == 1; }, 3000),
                 "capacity-overflow rejection did not fire");
        QCOMPARE(lastKind, QStringLiteral("too_many_pending"));
    }

    void clientNotFoundPropagates() {
        std::atomic<int> callCount{0};
        QString receivedKind;
        QString receivedDetail;

        m_bridge->dispatchClientRequest(
            QStringLiteral("client-uuid-missing"),
            QStringLiteral("vfs.read"),
            QJsonObject{},
            [&](const BridgeClientRpcReply& reply) {
                receivedKind = reply.errorKind;
                receivedDetail = reply.errorDetail;
                ++callCount;
            },
            5000);

        const QJsonObject dispatchFrame =
            waitForBridgeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
        QVERIFY(!dispatchFrame.isEmpty());
        const QString reqId = dispatchFrame.value(QStringLiteral("request_id")).toString();
        QVERIFY(!reqId.isEmpty());

        sendDaemonReply(reqId,
                        false,
                        {},
                        QStringLiteral("client_not_found"),
                        QStringLiteral("no paired client matches client_id"));

        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 2000),
                 "client_not_found callback did not fire");
        QCOMPARE(receivedKind, QStringLiteral("client_not_found"));
        QVERIFY(!receivedDetail.isEmpty());
    }

    void emptyOpRejectsSynchronously() {
        std::atomic<int> rejections{0};
        QString lastKind;

        m_bridge->dispatchClientRequest(
            QStringLiteral("client-uuid-empty-op"),
            QString(),
            QJsonObject{},
            [&](const BridgeClientRpcReply& reply) {
                lastKind = reply.errorKind;
                ++rejections;
            },
            5000);

        QCOMPARE(rejections.load(), 1);
        QCOMPARE(lastKind, QStringLiteral("invalid_argument"));

        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        for (const QString& f : m_receivedFrames) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(f.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError)
                continue;
            if (!doc.isObject())
                continue;
            QVERIFY2(doc.object().value(QStringLiteral("type")).toString() !=
                         QString::fromLatin1(IpcType::ClientRpcDispatch),
                     "empty-op rejection must not emit a dispatch frame");
        }
    }

    void emptyClientIdRejectsSynchronously() {
        std::atomic<int> rejections{0};
        QString lastKind;
        m_bridge->dispatchClientRequest(
            QString(),
            QStringLiteral("vfs.read"),
            QJsonObject{},
            [&](const BridgeClientRpcReply& reply) {
                lastKind = reply.errorKind;
                ++rejections;
            },
            5000);
        QCOMPARE(rejections.load(), 1);
        QCOMPARE(lastKind, QStringLiteral("invalid_argument"));
    }

    void nullCallbackRejects() {
        m_bridge->dispatchClientRequest(QStringLiteral("client-uuid-no-cb"),
                                        QStringLiteral("vfs.no_callback"),
                                        QJsonObject{},
                                        nullptr,
                                        5000);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        for (const QString& f : m_receivedFrames) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(f.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError)
                continue;
            if (!doc.isObject())
                continue;
            QVERIFY2(doc.object().value(QStringLiteral("type")).toString() !=
                         QString::fromLatin1(IpcType::ClientRpcDispatch),
                     "null-callback rejection must not emit a frame");
        }
    }

    void unknownReplyIdSilentlyDropped() {
        sendDaemonReply(
            QStringLiteral("not-a-real-uuid"), true, QJsonObject{{QStringLiteral("n"), 1}});

        QCoreApplication::processEvents(QEventLoop::AllEvents, 500);

        std::atomic<int> callCount{0};
        bool receivedOk = false;
        m_bridge->dispatchClientRequest(
            QStringLiteral("client-uuid-after-stale"),
            QStringLiteral("vfs.echo"),
            QJsonObject{{QStringLiteral("n"), 7}},
            [&](const BridgeClientRpcReply& reply) {
                receivedOk = reply.ok;
                ++callCount;
            },
            5000);
        const QJsonObject f = waitForBridgeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
        QVERIFY(!f.isEmpty());
        const QString reqId = f.value(QStringLiteral("request_id")).toString();
        sendDaemonReply(reqId, true, QJsonObject{{QStringLiteral("n"), 7}});
        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 2000),
                 "post-stale round trip did not complete");
        QVERIFY(receivedOk);
    }

    void unknownIpcTypeSafelyIgnored() {
        const QJsonObject bogus{
            {QStringLiteral("type"), QStringLiteral("some_future_type_v99")},
            {QStringLiteral("request_id"), QStringLiteral("ignored")},
            {QStringLiteral("opaque"), QStringLiteral("data the bridge cannot understand")},
        };
        m_daemonSocket->write(frameOf(bogus));
        m_daemonSocket->flush();

        QCoreApplication::processEvents(QEventLoop::AllEvents, 500);

        std::atomic<int> callCount{0};
        bool receivedOk = false;
        m_bridge->dispatchClientRequest(
            QStringLiteral("client-uuid-fwd-compat"),
            QStringLiteral("vfs.echo"),
            QJsonObject{{QStringLiteral("n"), 99}},
            [&](const BridgeClientRpcReply& reply) {
                receivedOk = reply.ok;
                ++callCount;
            },
            5000);
        const QJsonObject f = waitForBridgeFrame(QString::fromLatin1(IpcType::ClientRpcDispatch));
        QVERIFY(!f.isEmpty());
        const QString reqId = f.value(QStringLiteral("request_id")).toString();
        sendDaemonReply(reqId, true, QJsonObject{{QStringLiteral("n"), 99}});
        QVERIFY2(spinUntil([&] { return callCount.load() == 1; }, 2000),
                 "post-unknown-type round trip did not complete");
        QVERIFY(receivedOk);
    }
};

QTEST_MAIN(TestWireHostBridgeClientRpc)
#include "test-wire-host-bridge-client-rpc.moc"

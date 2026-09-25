// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-host-client.cpp
 * @brief Implementation of the verzeta-remote-side QLocalSocket client
 *        to the host bridge. Handles framing, reconnect backoff, and
 *        async-RPC correlation.
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::Network (QLocalSocket).
 */

#include "wire-host-client.h"

#include "../wire-protocol.h"
#include "wire-server.h"
#include "wire-session.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QUuid>

Q_LOGGING_CATEGORY(wireRemote, "verzeta.remote.client", QtInfoMsg)

namespace Verzeta::Remote {

namespace {

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

WireHostClient::WireHostClient(QObject* parent) : QObject(parent) {
    m_socket = new QLocalSocket(this);
    QObject::connect(m_socket, &QLocalSocket::connected, this, &WireHostClient::onSocketConnected);
    QObject::connect(
        m_socket, &QLocalSocket::disconnected, this, &WireHostClient::onSocketDisconnected);
    QObject::connect(m_socket, &QLocalSocket::readyRead, this, &WireHostClient::onSocketReadyRead);

    m_reconnectTimer.setSingleShot(true);
    QObject::connect(&m_reconnectTimer, &QTimer::timeout, this, &WireHostClient::onReconnectTick);

    m_timeoutSweepTimer.setInterval(500);
    QObject::connect(&m_timeoutSweepTimer, &QTimer::timeout, this, &WireHostClient::onTimeoutSweep);
}

WireHostClient::~WireHostClient() = default;

void WireHostClient::start() {
    if (m_socket->state() == QLocalSocket::ConnectedState)
        return;
    m_socket->connectToServer(hostBridgeSocketName());
    m_timeoutSweepTimer.start();
}

bool WireHostClient::isConnected() const {
    return m_socket && m_socket->state() == QLocalSocket::ConnectedState;
}

void WireHostClient::onSocketConnected() {
    qCInfo(wireRemote) << "WireHostClient: bridge connected at" << m_socket->fullServerName();
    m_backoffMs = 250;
    m_rxBuf.clear();
    emit connectedChanged(true);
}

void WireHostClient::onSocketDisconnected() {
    qCInfo(wireRemote) << "WireHostClient: bridge disconnected; will retry";
    m_rxBuf.clear();

    // Fail every pending request — host can't service them.
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it.value().callback) {
            it.value().callback(false, QJsonValue(), QStringLiteral("bridge disconnected"));
        }
    }
    m_pending.clear();

    emit connectedChanged(false);
    scheduleReconnect();
}

void WireHostClient::scheduleReconnect() {
    m_reconnectTimer.start(m_backoffMs);
    m_backoffMs = qMin(m_backoffMs * 2, 5000);
}

void WireHostClient::onReconnectTick() {
    if (m_socket->state() == QLocalSocket::UnconnectedState) {
        m_socket->connectToServer(hostBridgeSocketName());
    }
    if (m_socket->state() == QLocalSocket::UnconnectedState) {
        // Connect attempt failed sync; retry with backoff.
        scheduleReconnect();
    }
}

void WireHostClient::onTimeoutSweep() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList expired;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        if (it.value().expiresAtMs <= now)
            expired.append(it.key());
    }
    for (const QString& id : expired) {
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            continue;
        if (it->callback) {
            it->callback(false, QJsonValue(), QStringLiteral("timeout"));
        }
        m_pending.erase(it);
    }
}

void WireHostClient::onSocketReadyRead() {
    m_rxBuf.append(m_socket->readAll());
    while (true) {
        if (m_rxBuf.size() < 4)
            return;
        const quint32 len =
            (static_cast<quint8>(m_rxBuf[0]) << 24) | (static_cast<quint8>(m_rxBuf[1]) << 16) |
            (static_cast<quint8>(m_rxBuf[2]) << 8) | static_cast<quint8>(m_rxBuf[3]);
        if (len > kMaxWireFrameBytes) {
            qCWarning(wireRemote) << "WireHostClient: oversized frame; dropping connection";
            m_socket->disconnectFromServer();
            return;
        }
        if (m_rxBuf.size() < static_cast<int>(4 + len))
            return;
        const QByteArray payload = m_rxBuf.mid(4, len);
        m_rxBuf.remove(0, 4 + len);
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qCWarning(wireRemote) << "WireHostClient: malformed frame:" << err.errorString();
            continue;
        }
        handleFrame(doc.object());
    }
}

void WireHostClient::handleFrame(const QJsonObject& obj) {
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type == QString::fromLatin1(IpcType::Hello)) {
        return;  // diagnostics only
    }
    if (type == QString::fromLatin1(IpcType::Signal)) {
        emit hostSignal(obj.value(QStringLiteral("name")).toString(),
                        obj.value(QStringLiteral("args")).toObject());
        return;
    }
    if (type == QString::fromLatin1(IpcType::InvokeResponse)) {
        const QString id = obj.value(QStringLiteral("request_id")).toString();
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            return;
        const bool ok = obj.value(QStringLiteral("ok")).toBool();
        const QJsonValue data = obj.value(QStringLiteral("data"));
        const QString err = obj.value(QStringLiteral("error")).toString();
        if (it->callback)
            it->callback(ok, data, err);
        m_pending.erase(it);
        return;
    }
    if (type == QString::fromLatin1(IpcType::ClientRpcDispatch)) {
        handleClientRpcDispatch(obj);
        return;
    }
    // Unknown inbound IPC type — silently ignored for forward-compat.
    // The bridge may add new types in future releases; this daemon
    // ignores anything it doesn't understand rather than tearing down
    // the connection.
}

void WireHostClient::setWireServer(WireServer* server) {
    m_wireServer = server;
}

void WireHostClient::handleClientRpcDispatch(const QJsonObject& obj) {
    const QString requestId = obj.value(QStringLiteral("request_id")).toString();
    const QString clientId = obj.value(QStringLiteral("client_id")).toString();
    const QString op = obj.value(QStringLiteral("op")).toString();
    const QJsonObject args = obj.value(QStringLiteral("args")).toObject();
    const int timeoutMs = static_cast<int>(obj.value(QStringLiteral("timeout_ms")).toInt(15000));

    if (requestId.isEmpty()) {
        // Malformed frame — the bridge always includes a request_id;
        // a missing one means the bridge will never wait for a reply.
        // Drop silently rather than echo back a reply that nobody is
        // listening for.
        qCWarning(wireRemote) << "WireHostClient: client_rpc_dispatch missing request_id";
        return;
    }
    if (op.isEmpty()) {
        sendClientRpcReply(requestId,
                           false,
                           {},
                           QStringLiteral("invalid_argument"),
                           QStringLiteral("op is empty"));
        return;
    }
    if (m_wireServer == nullptr) {
        sendClientRpcReply(requestId,
                           false,
                           {},
                           QStringLiteral("client_not_found"),
                           QStringLiteral("daemon has no WireServer registered"));
        return;
    }

    WireSession* session = m_wireServer->sessionForClientId(clientId);
    if (session == nullptr) {
        sendClientRpcReply(requestId,
                           false,
                           {},
                           QStringLiteral("client_not_found"),
                           QStringLiteral("no paired client matches client_id"));
        return;
    }

    // Capability-gated routing: `vfs.execute` runs an agent shell
    // command on the client, which only clients that advertised the
    // capability at auth handle. A client that did not (legacy / opt-out
    // clients, and clients that simply never register a workspace) is
    // refused here so the host can fall back to its own ProcessSandbox
    // rather than dispatching a command the client cannot run. Every
    // other op is unaffected.
    if (op == QStringLiteral("vfs.execute") &&
        !session->hasCapability(QStringLiteral("vfs.execute"))) {
        sendClientRpcReply(requestId,
                           false,
                           {},
                           QStringLiteral("exec_unsupported"),
                           QStringLiteral("client did not advertise vfs.execute"));
        return;
    }

    // The wire-session callback fires on the daemon's main thread (same
    // thread we're on). Capture `this` and `requestId` by value; the
    // session's per-pending entry owns the std::function until it
    // resolves, so the closure's captures are safe across the async wait.
    session->sendClientRequest(
        op,
        args,
        [this, requestId](const ClientRpcReply& reply) {
            if (reply.ok) {
                sendClientRpcReply(requestId, true, reply.data, {}, {});
            } else {
                sendClientRpcReply(requestId, false, {}, reply.errorKind, reply.errorDetail);
            }
        },
        timeoutMs);
}

void WireHostClient::sendClientRpcReply(const QString& requestId,
                                        bool ok,
                                        const QJsonValue& data,
                                        const QString& errorKind,
                                        const QString& errorDetail) {
    QJsonObject frame{
        {QStringLiteral("type"), QString::fromLatin1(IpcType::ClientRpcReply)},
        {QStringLiteral("request_id"), requestId},
        {QStringLiteral("ok"), ok},
    };
    if (ok) {
        frame.insert(QStringLiteral("data"), data);
    } else {
        frame.insert(QStringLiteral("error"),
                     QJsonObject{
                         {QStringLiteral("kind"), errorKind},
                         {QStringLiteral("detail"), errorDetail},
                     });
    }
    sendFrame(frame);
}

void WireHostClient::sendFrame(const QJsonObject& obj) {
    if (!isConnected())
        return;
    m_socket->write(frameOf(obj));
}

void WireHostClient::invokeFireAndForget(const QString& target,
                                         const QString& method,
                                         const QJsonArray& args,
                                         const QString& clientId) {
    QJsonObject frame{
        {QStringLiteral("type"), QString::fromLatin1(IpcType::Invoke)},
        {QStringLiteral("target"), target},
        {QStringLiteral("method"), method},
        {QStringLiteral("args"), args},
    };
    if (!clientId.isEmpty()) {
        frame.insert(QStringLiteral("client_id"), clientId);
    }
    sendFrame(frame);
}

void WireHostClient::invokeAsync(
    const QString& target,
    const QString& method,
    const QJsonArray& args,
    const QString& returnType,
    int timeoutMs,
    std::function<void(bool, const QJsonValue&, const QString&)> callback,
    const QString& clientId) {
    if (!isConnected()) {
        if (callback)
            callback(false, QJsonValue(), QStringLiteral("bridge offline"));
        return;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingInvoke pi;
    pi.requestId = id;
    pi.expiresAtMs = QDateTime::currentMSecsSinceEpoch() + qMax(timeoutMs, 100);
    pi.callback = std::move(callback);
    m_pending.insert(id, pi);

    QJsonObject frame{
        {QStringLiteral("type"), QString::fromLatin1(IpcType::Invoke)},
        {QStringLiteral("request_id"), id},
        {QStringLiteral("target"), target},
        {QStringLiteral("method"), method},
        {QStringLiteral("args"), args},
        {QStringLiteral("return_type"), returnType},
    };
    if (!clientId.isEmpty()) {
        frame.insert(QStringLiteral("client_id"), clientId);
    }
    sendFrame(frame);
}

void WireHostClient::invokePropertyGet(
    const QString& target,
    const QString& property,
    int timeoutMs,
    std::function<void(bool, const QJsonValue&, const QString&)> callback,
    const QString& clientId) {
    if (!isConnected()) {
        if (callback)
            callback(false, QJsonValue(), QStringLiteral("bridge offline"));
        return;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingInvoke pi;
    pi.requestId = id;
    pi.expiresAtMs = QDateTime::currentMSecsSinceEpoch() + qMax(timeoutMs, 100);
    pi.callback = std::move(callback);
    m_pending.insert(id, pi);

    QJsonObject frame{
        {QStringLiteral("type"), QString::fromLatin1(IpcType::PropertyGet)},
        {QStringLiteral("request_id"), id},
        {QStringLiteral("target"), target},
        {QStringLiteral("property"), property},
    };
    if (!clientId.isEmpty()) {
        frame.insert(QStringLiteral("client_id"), clientId);
    }
    sendFrame(frame);
}

void WireHostClient::invokePropertySet(
    const QString& target,
    const QString& property,
    const QString& valueType,
    const QJsonValue& value,
    int timeoutMs,
    std::function<void(bool, const QJsonValue&, const QString&)> callback,
    const QString& clientId) {
    if (!isConnected()) {
        if (callback)
            callback(false, QJsonValue(), QStringLiteral("bridge offline"));
        return;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingInvoke pi;
    pi.requestId = id;
    pi.expiresAtMs = QDateTime::currentMSecsSinceEpoch() + qMax(timeoutMs, 100);
    pi.callback = std::move(callback);
    m_pending.insert(id, pi);

    QJsonObject frame{
        {QStringLiteral("type"), QString::fromLatin1(IpcType::PropertySet)},
        {QStringLiteral("request_id"), id},
        {QStringLiteral("target"), target},
        {QStringLiteral("property"), property},
        {QStringLiteral("value"),
         QJsonObject{
             {QStringLiteral("type"), valueType},
             {QStringLiteral("value"), value},
         }},
    };
    if (!clientId.isEmpty()) {
        frame.insert(QStringLiteral("client_id"), clientId);
    }
    sendFrame(frame);
}

}  // namespace Verzeta::Remote

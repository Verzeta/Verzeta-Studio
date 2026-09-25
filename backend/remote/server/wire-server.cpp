// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-server.cpp
 * @brief Implementation of the verzeta-remote WebSocket server. Owns
 *        the `QWebSocketServer`, accepts incoming WebSocket connections,
 *        and constructs one WireSession per socket.
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::Network, Qt6::WebSockets.
 */

#include "wire-server.h"

#include "../wire-protocol.h"
#include "wire-auth.h"
#include "wire-host-client.h"
#include "wire-session.h"

#include <QThread>

#include <QFile>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QWebSocket>
#include <QWebSocketServer>

Q_LOGGING_CATEGORY(wireServer, "verzeta.remote.server", QtInfoMsg)

namespace Verzeta::Remote {

WireServer::WireServer(WireAuth* auth, WireHostClient* host, WireDbReader* db, QObject* parent)
    : QObject(parent), m_auth(auth), m_host(host), m_db(db) {}

WireServer::~WireServer() {
    close();
}

bool WireServer::listen(const QString& bindAddr, quint16 port) {
    if (m_ws)
        return false;
    m_ws = new QWebSocketServer(
        QStringLiteral("Verzeta Remote API"), QWebSocketServer::NonSecureMode, this);
    QObject::connect(m_ws, &QWebSocketServer::newConnection, this, &WireServer::onNewConnection);
    const QHostAddress addr = bindAddr == QStringLiteral("0.0.0.0")
                                  ? QHostAddress(QHostAddress::Any)
                                  : QHostAddress(bindAddr);
    if (!m_ws->listen(addr, port)) {
        qCCritical(wireServer) << "WireServer: listen() failed on" << bindAddr << ":" << port << "—"
                               << m_ws->errorString();
        delete m_ws;
        m_ws = nullptr;
        return false;
    }
    qCInfo(wireServer) << "WireServer: listening on ws://" << bindAddr << ":" << port << "/";
    return true;
}

bool WireServer::listenTls(const QString& bindAddr,
                           quint16 port,
                           const QString& certPath,
                           const QString& keyPath) {
    if (m_ws)
        return false;
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        qCCritical(wireServer) << "listenTls: cannot open cert file" << certPath << ":"
                               << certFile.errorString();
        return false;
    }
    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        qCCritical(wireServer) << "listenTls: cannot open key file" << keyPath << ":"
                               << keyFile.errorString();
        return false;
    }
    const QSslCertificate cert(&certFile, QSsl::Pem);
    if (cert.isNull()) {
        qCCritical(wireServer) << "listenTls: failed to parse certificate" << certPath;
        return false;
    }
    keyFile.seek(0);
    // Try multiple key encodings — RSA is most common but Ed25519 / ECDSA
    // are supported by openssl-generated keys too.
    QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem);
    if (key.isNull()) {
        keyFile.seek(0);
        key = QSslKey(&keyFile, QSsl::Ec, QSsl::Pem);
    }
    if (key.isNull()) {
        keyFile.seek(0);
        key = QSslKey(&keyFile, QSsl::Dsa, QSsl::Pem);
    }
    if (key.isNull()) {
        qCCritical(wireServer) << "listenTls: failed to parse private key" << keyPath
                               << "(tried RSA, EC, DSA — is the file PEM-encoded?)";
        return false;
    }
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setLocalCertificate(cert);
    cfg.setPrivateKey(key);

    m_ws = new QWebSocketServer(
        QStringLiteral("Verzeta Remote API"), QWebSocketServer::SecureMode, this);
    m_ws->setSslConfiguration(cfg);
    QObject::connect(m_ws, &QWebSocketServer::newConnection, this, &WireServer::onNewConnection);
    const QHostAddress addr = bindAddr == QStringLiteral("0.0.0.0")
                                  ? QHostAddress(QHostAddress::Any)
                                  : QHostAddress(bindAddr);
    if (!m_ws->listen(addr, port)) {
        qCCritical(wireServer) << "listenTls: listen() failed on" << bindAddr << ":" << port << "—"
                               << m_ws->errorString();
        delete m_ws;
        m_ws = nullptr;
        return false;
    }
    qCInfo(wireServer) << "WireServer: listening on wss://" << bindAddr << ":" << port << "/";
    return true;
}

void WireServer::close() {
    for (auto* s : m_sessions)
        if (s)
            s->deleteLater();
    m_sessions.clear();
    if (m_ws) {
        m_ws->close();
        m_ws->deleteLater();
        m_ws = nullptr;
    }
}

void WireServer::onNewConnection() {
    if (!m_ws)
        return;
    while (m_ws->hasPendingConnections()) {
        QWebSocket* sock = m_ws->nextPendingConnection();
        if (!sock)
            break;
        // One transport cap (kMaxWireFrameBytes) governs every WebSocket
        // client — the VS Code extension and Android alike. Content inside
        // a frame is separately bounded by kMaxContentBytes; see
        // wire-protocol.h.
        sock->setMaxAllowedIncomingMessageSize(kMaxWireFrameBytes);
        auto* sess = new WireSession(sock, m_auth, m_host, m_db, this);
        QObject::connect(
            sess, &WireSession::disconnected, this, &WireServer::onSessionDisconnected);
        QObject::connect(sess, &WireSession::clientIdRebound, this, &WireServer::onClientIdRebound);
        m_sessions.append(sess);
        qCInfo(wireServer) << "WireServer: new client from" << sock->peerAddress().toString();
    }
}

void WireServer::onSessionDisconnected(WireSession* session) {
    if (!session)
        return;
    m_sessions.removeAll(session);
    session->deleteLater();
    if (qEnvironmentVariableIntValue("VERZETA_MOUNT_TRACE") > 0) {
        qCInfo(wireServer).noquote()
            << "MOUNT-TRACE: session disconnected, removed from pool. session="
            << static_cast<void*>(session) << "remaining=" << m_sessions.size();
    }
}

void WireServer::onClientIdRebound(const QString& clientId, WireSession* self) {
    Q_ASSERT(QThread::currentThread() == this->thread());
    if (clientId.isEmpty() || !self)
        return;
    // Collect first; close after.  Force-close during iteration would
    // invalidate the QList we're walking.
    QList<WireSession*> evict;
    for (WireSession* s : m_sessions) {
        if (s && s != self && s->clientId() == clientId) {
            evict.append(s);
        }
    }
    for (WireSession* zombie : evict) {
        if (qEnvironmentVariableIntValue("VERZETA_MOUNT_TRACE") > 0) {
            qCInfo(wireServer).noquote()
                << "MOUNT-TRACE: superseding zombie session for clientId=" << clientId
                << "zombie=" << static_cast<void*>(zombie) << "new=" << static_cast<void*>(self);
        } else {
            qCInfo(wireServer) << "WireServer: superseding old session for client" << clientId;
        }
        // forceClose() drains pending requests with `session_superseded`
        // and tears down the WebSocket; the session's `disconnected`
        // signal then fires onSessionDisconnected which removes from
        // m_sessions and schedules deleteLater.
        zombie->forceClose(QStringLiteral("session_superseded"));
    }
}

WireSession* WireServer::sessionForClientId(const QString& clientId) const {
    // The daemon is single-threaded: WireServer, every WireSession, and
    // the WireHostClient receive path all live on the daemon's main
    // event-loop thread. The lookup is a plain linear scan of the
    // current session container. Defence-in-depth assertion against
    // accidental cross-thread call sites; Q_ASSERT compiles out in
    // release builds.
    Q_ASSERT(QThread::currentThread() == this->thread());
    if (clientId.isEmpty())
        return nullptr;
    for (WireSession* s : m_sessions) {
        if (s && s->clientId() == clientId)
            return s;
    }
    return nullptr;
}

}  // namespace Verzeta::Remote

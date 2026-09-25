// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-server.h
 * @brief Top-level WebSocket server inside the verzeta-remote process.
 *
 *        Owns the `QWebSocketServer`, accepts incoming WebSocket
 *        connections, constructs one WireSession per socket, and
 *        forwards disconnect notifications.
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::WebSockets, WireAuth, WireHostClient,
 *               WireDbReader.
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>

class QWebSocketServer;
class QWebSocket;

namespace Verzeta::Remote {

class WireAuth;
class WireDbReader;
class WireHostClient;
class WireSession;

/**
 * @brief WebSocket server that owns the QWebSocketServer instance and
 *        the collection of per-client WireSession objects.
 */
class WireServer : public QObject {
    Q_OBJECT
  public:
    /**
     * @brief Constructs the server with the three non-owning service
     *        references each new session will receive.
     * @param auth    Auth + paired-client store.
     * @param host    Local-socket client to the host bridge.
     * @param db      Read-only SQLite reader.
     * @param parent  Optional Qt parent.
     */
    WireServer(WireAuth* auth, WireHostClient* host, WireDbReader* db, QObject* parent = nullptr);
    ~WireServer() override;

    /**
     * @brief Starts listening for plain WebSocket connections.
     * @param bindAddr  Bind address (e.g. "0.0.0.0", "127.0.0.1").
     * @param port      TCP port to listen on.
     * @returns True on successful bind; false otherwise.
     */
    bool listen(const QString& bindAddr, quint16 port);

    /**
     * @brief Starts listening for TLS-wrapped WebSocket connections.
     * @param bindAddr  Bind address.
     * @param port      TCP port to listen on.
     * @param certPath  PEM file containing the server certificate.
     * @param keyPath   PEM file containing the matching private key
     *                  (PKCS#8 or traditional RSA).
     * @returns True on successful bind + cert load.
     */
    bool listenTls(const QString& bindAddr,
                   quint16 port,
                   const QString& certPath,
                   const QString& keyPath);

    /**
     * @brief Stops the server and tears down all live sessions.
     */
    void close();

    /**
     * @brief Looks up the currently-connected WireSession owned by the
     *        paired-client whose authenticated UUID matches `clientId`.
     *        Used by `WireHostClient` when a host-initiated
     *        ClientRpcDispatch IPC frame arrives, because the daemon must
     *        route the corresponding wire `request` frame to the
     *        session that owns the target client's WebSocket.
     * @param clientId  Wire-auth paired-client UUID. Empty / unknown
     *                  ids return nullptr.
     * @returns Pointer to the session on hit; nullptr on miss. Caller
     *          MUST dereference within the same main-thread invocation;
     *          the returned pointer is invalidated by any subsequent
     *          session add/remove path. Single-threaded; entry
     *          enforced via `VERZETA_ASSERT_MAIN_THREAD()`.
     */
    WireSession* sessionForClientId(const QString& clientId) const;

  private slots:
    /**
     * @brief Accepts a pending connection and constructs a WireSession.
     */
    void onNewConnection();

    /**
     * @brief Removes a session from the live list and schedules it for
     *        deletion when its socket disconnects.
     * @param session  Session that just emitted `disconnected`.
     */
    void onSessionDisconnected(WireSession* session);

    /**
     * @brief When a session binds (or rebinds) its
     *        `m_clientId` via auth.pair / auth.token, scan
     *        `m_sessions` for ANY other live session whose
     *        `clientId()` already matches and force-close it.  Without
     *        this, an extension reconnect leaves a zombie session in
     *        `m_sessions` whose socket has not yet been observed as
     *        disconnected; `sessionForClientId` then returns the dead
     *        session first on its linear scan and every host-initiated
     *        RPC dispatches into a closed socket.
     *
     * @param clientId Newly bound identity.
     * @param self     The session that just bound (NOT to be evicted).
     */
    void onClientIdRebound(const QString& clientId, WireSession* self);

  private:
    WireAuth* m_auth = nullptr;
    WireHostClient* m_host = nullptr;
    WireDbReader* m_db = nullptr;
    QWebSocketServer* m_ws = nullptr;
    QList<WireSession*> m_sessions;
};

}  // namespace Verzeta::Remote

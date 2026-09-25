// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-host-client.h
 * @brief verzeta-remote-side QLocalSocket client to the host bridge.
 *
 *        Maintains a connection to the running verzeta-studio process
 *        via the QLocalSocket bound by Verzeta::Remote::WireHostBridge.
 *        Reconnects automatically with exponential backoff when the
 *        host is down. Forwards Q_INVOKABLE invocations to the host
 *        and emits Qt signals for every host-side signal received.
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::Network (QLocalSocket).
 */

#pragma once

#include <QTimer>

#include <memory>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QLocalSocket;

namespace Verzeta::Remote {

class WireServer;

/**
 * @brief Async-RPC reply token. The remote-side caller awaits this;
 *        WireHostClient resolves it when the matching `invoke_response`
 *        frame arrives over the QLocalSocket.
 *
 * The callback runs on the WireHostClient's thread when the response
 * arrives. `ok=false` means the host rejected the call (unknown
 * method, etc.) OR the deadline elapsed.
 */
struct PendingInvoke {
    QString requestId;   ///< Id matched against the response frame.
    qint64 expiresAtMs;  ///< Deadline, ms since epoch.
    /// Called once with the result, or with ok=false on rejection or
    /// timeout.
    std::function<void(bool ok, const QJsonValue& data, const QString& error)> callback;
};

/**
 * @brief verzeta-remote-side QLocalSocket client to the host bridge.
 *
 * Owns the socket, the request/response correlation table, and the
 * reconnect / timeout sweepers.
 */
class WireHostClient : public QObject {
    Q_OBJECT
  public:
    /**
     * @brief Constructs the client; the socket is opened by start().
     * @param parent  Optional Qt parent.
     */
    explicit WireHostClient(QObject* parent = nullptr);
    ~WireHostClient() override;

    /**
     * @brief Begins the (re)connect loop. Idempotent.
     */
    void start();

    /**
     * @brief Reports whether the local socket is currently connected.
     * @returns True when the host bridge socket is open.
     */
    bool isConnected() const;

    /**
     * @brief Fire-and-forget invoke against an existing host Q_INVOKABLE.
     * @param target    Target object name registered in wire-protocol.h
     *                  Targets::.
     * @param method    Method name on the target.
     * @param args      JSON-encoded argument list.
     * @param clientId  When non-empty, threaded into the IPC frame as
     *                  `client_id`; the bridge routes per-client
     *                  ChatController / AgentSettings invokes through
     *                  SessionRouter. Empty (default) dispatches to the
     *                  shared local target.
     *
     * Frames are dropped silently when not connected; callers surface
     * "host offline" via isConnected().
     */
    void invokeFireAndForget(const QString& target,
                             const QString& method,
                             const QJsonArray& args,
                             const QString& clientId = QString());

    /**
     * @brief Async invoke with a callback for the typed response.
     * @param target      Target object name.
     * @param method      Method name on the target.
     * @param args        JSON-encoded argument list.
     * @param returnType  Qt type name the bridge converts the return
     *                    value to before responding.
     * @param timeoutMs   Deadline in milliseconds; on expiry the
     *                    callback fires with `ok=false`, `error="timeout"`.
     * @param callback    Invoked on this object's thread when the
     *                    response arrives (or the deadline elapses).
     * @param clientId    Optional per-client routing key (see
     *                    invokeFireAndForget).
     */
    void invokeAsync(const QString& target,
                     const QString& method,
                     const QJsonArray& args,
                     const QString& returnType,
                     int timeoutMs,
                     std::function<void(bool, const QJsonValue&, const QString&)> callback,
                     const QString& clientId = QString());

    /**
     * @brief Reads an EXISTING Q_PROPERTY on a host object via IPC.
     * @param target    Target object name.
     * @param property  Property name.
     * @param timeoutMs Deadline in milliseconds.
     * @param callback  Invoked with the JSON-converted value.
     * @param clientId  Optional per-client routing key.
     *
     * The bridge reads `target->property(name)` under
     * BlockingQueuedConnection on main; main never waits on worker,
     * so no deadlock is possible.
     */
    void invokePropertyGet(const QString& target,
                           const QString& property,
                           int timeoutMs,
                           std::function<void(bool, const QJsonValue&, const QString&)> callback,
                           const QString& clientId = QString());

    /**
     * @brief Writes an EXISTING Q_PROPERTY on a host object via IPC.
     * @param target     Target object name.
     * @param property   Property name.
     * @param valueType  One of "string", "bool", "int", "qint64",
     *                   "double", "stringlist", "variantmap",
     *                   "variantlist".
     * @param value      JSON value, interpreted per `valueType`.
     * @param timeoutMs  Deadline in milliseconds.
     * @param callback   When non-null, fires when the bridge
     *                   acknowledges dispatch.
     * @param clientId   Optional per-client routing key.
     *
     * The bridge calls `target->setProperty(name, value)` under
     * QueuedConnection on main (fire-and-forget).
     */
    void invokePropertySet(const QString& target,
                           const QString& property,
                           const QString& valueType,
                           const QJsonValue& value,
                           int timeoutMs,
                           std::function<void(bool, const QJsonValue&, const QString&)> callback,
                           const QString& clientId = QString());

    /**
     * @brief Registers the WireServer instance whose session container
     *        is consulted when a host-initiated ClientRpcDispatch IPC
     *        frame arrives. The daemon's main.cpp wires this once
     *        after constructing both the WireServer and the
     *        WireHostClient. Non-owning; the server outlives this
     *        client by construction order.
     * @param server  Pointer to the WireServer. nullptr disables
     *                client-RPC handling entirely (any inbound
     *                ClientRpcDispatch is rejected with
     *                error.kind="client_not_found").
     */
    void setWireServer(WireServer* server);

  signals:
    /**
     * @brief Emitted whenever the connection state flips.
     * @param connected  True when the socket has just opened.
     */
    void connectedChanged(bool connected);

    /**
     * @brief Emitted for every host-side signal forwarded by the bridge.
     * @param name  Registered Signals::* string.
     * @param args  JSON payload object.
     */
    void hostSignal(const QString& name, const QJsonObject& args);

  private slots:
    /** @brief Internal: socket-connected handler. */
    void onSocketConnected();

    /** @brief Internal: socket-disconnected handler. */
    void onSocketDisconnected();

    /** @brief Internal: drain bytes from the socket and dispatch frames. */
    void onSocketReadyRead();

    /** @brief Internal: reconnect-tick callback. */
    void onReconnectTick();

    /** @brief Internal: sweeps expired pending callbacks. */
    void onTimeoutSweep();

  private:
    /** @brief Schedules a reconnect attempt under exponential backoff. */
    void scheduleReconnect();

    /** @brief Encodes and writes a length-prefixed JSON frame. */
    void sendFrame(const QJsonObject& obj);

    /** @brief Dispatches one decoded inbound frame. */
    void handleFrame(const QJsonObject& obj);

    /**
     * @brief Handles an inbound `client_rpc_dispatch` IPC frame. Looks
     *        up the WireSession by client_id via the registered
     *        WireServer, calls `WireSession::sendClientRequest`, and
     *        translates the matching `ClientRpcReply` callback into
     *        an outbound `client_rpc_reply` IPC frame. Single-threaded;
     *        everything runs on the daemon's main event-loop thread.
     * @param obj  Decoded frame body. `type` is `client_rpc_dispatch`.
     */
    void handleClientRpcDispatch(const QJsonObject& obj);

    /**
     * @brief Encodes and writes a `client_rpc_reply` IPC frame back to
     *        the bridge.
     * @param requestId  The bridge-generated request_id this reply
     *                   corresponds to.
     * @param ok         True iff the wire client responded successfully.
     * @param data       Response payload on success; ignored on failure.
     * @param errorKind  Machine error tag on failure ("timeout",
     *                   "client_not_found", "session_closed",
     *                   "invalid_argument", "client_error",
     *                   "malformed_response").
     * @param errorDetail Human-readable error detail on failure.
     */
    void sendClientRpcReply(const QString& requestId,
                            bool ok,
                            const QJsonValue& data,
                            const QString& errorKind,
                            const QString& errorDetail);

    QLocalSocket* m_socket = nullptr;
    QByteArray m_rxBuf;
    QTimer m_reconnectTimer;
    QTimer m_timeoutSweepTimer;
    int m_backoffMs = 250;

    QHash<QString, PendingInvoke> m_pending;  // request_id → callback

    /**
     * @brief Optional non-owning back-pointer to the WireServer. When
     *        set, inbound ClientRpcDispatch frames look up sessions
     *        via `server->sessionForClientId(clientId)`. When unset
     *        (or set to nullptr), every inbound ClientRpcDispatch is
     *        immediately rejected with `client_not_found`.
     */
    WireServer* m_wireServer = nullptr;
};

}  // namespace Verzeta::Remote

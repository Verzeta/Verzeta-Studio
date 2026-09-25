// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-session.cpp
 * @brief Implementation of the per-WebSocket-client wire session.
 *
 *        Validates inbound frames, dispatches the ~150 wire ops, and
 *        forwards filtered host signals back to the client. Threads
 *        the per-client routing id through every IPC invoke wrapper.
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::WebSockets, WireAuth, WireHostClient,
 *               WireDbReader.
 */

#include "wire-session.h"

#include "../wire-protocol.h"
#include "wire-auth.h"
#include "wire-db-reader.h"
#include "wire-host-client.h"
#include "wire-validator.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>
#include <QWebSocket>

Q_LOGGING_CATEGORY(wireSess, "verzeta.remote.session", QtInfoMsg)

namespace Verzeta::Remote {

namespace V = Validate;

namespace {

/**
 * Extract the optional `capabilities` string array a client sends at
 * auth into a set (e.g. {"vfs.execute"}). Non-string entries are
 * ignored; a missing or malformed field yields an empty set, so clients
 * that advertise nothing simply have no capabilities.
 */
inline QSet<QString> parseCapabilities(const QJsonObject& params) {
    QSet<QString> caps;
    const QJsonArray arr = params.value(QStringLiteral("capabilities")).toArray();
    for (const QJsonValue& v : arr) {
        const QString cap = v.toString();
        if (!cap.isEmpty())
            caps.insert(cap);
    }
    return caps;
}

/** Build a JSON arg block of the form {type, value}. */
inline QJsonObject argStr(const QString& s) {
    return {{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("value"), s}};
}
inline QJsonObject argBool(bool b) {
    return {{QStringLiteral("type"), QStringLiteral("bool")}, {QStringLiteral("value"), b}};
}
inline QJsonObject argInt(int i) {
    return {{QStringLiteral("type"), QStringLiteral("int")}, {QStringLiteral("value"), i}};
}
inline QJsonObject argInt64(qint64 i) {
    return {{QStringLiteral("type"), QStringLiteral("qint64")},
            {QStringLiteral("value"), static_cast<double>(i)}};
}
inline QJsonObject argDouble(double d) {
    return {{QStringLiteral("type"), QStringLiteral("double")}, {QStringLiteral("value"), d}};
}
inline QJsonObject argStringList(const QStringList& sl) {
    QJsonArray a;
    for (const QString& s : sl)
        a.append(s);
    return {{QStringLiteral("type"), QStringLiteral("stringlist")}, {QStringLiteral("value"), a}};
}
inline QJsonObject argMap(const QJsonObject& obj) {
    return {{QStringLiteral("type"), QStringLiteral("variantmap")}, {QStringLiteral("value"), obj}};
}
inline QJsonObject argList(const QJsonArray& arr) {
    return {{QStringLiteral("type"), QStringLiteral("variantlist")},
            {QStringLiteral("value"), arr}};
}

/** Upper bound on one member's tool allowlist sent over the wire. */
constexpr int kMaxMemberAllowedTools = 512;

/**
 * @brief Copies the optional per-member override and provenance fields
 *        of a wire member object into the camelCase map
 *        MembershipService reads. Accepts snake_case or camelCase keys.
 *        Absent keys are not written, so the service applies its own
 *        defaults exactly as it does for a client that sends none.
 * @param in  Member object from the client.
 * @param out Member map passed to MembershipService; receives the fields.
 * @returns false when a present field has the wrong type or is too long.
 */
bool copyMemberOverrideFields(const QJsonObject& in, QJsonObject& out) {
    auto pick = [&in](const char* snake, const char* camel) -> QJsonValue {
        const QString s = QString::fromLatin1(snake);
        return in.contains(s) ? in.value(s) : in.value(QString::fromLatin1(camel));
    };
    const struct {
        const char* snake;
        const char* camel;
    } strings[] = {
        {"model_provider", "modelProvider"},
        {"model_name", "modelName"},
        {"added_by_kind", "addedByKind"},
        {"added_by_agent_id", "addedByAgentId"},
    };
    for (const auto& f : strings) {
        const QJsonValue v = pick(f.snake, f.camel);
        if (v.isUndefined() || v.isNull())
            continue;
        if (!v.isString() || !V::optionalBounded(v.toString()))
            return false;
        out.insert(QString::fromLatin1(f.camel), v.toString());
    }
    // Provenance decides which members an agent may remove, so only the
    // two kinds the host itself writes are accepted.
    const QString kind = out.value(QStringLiteral("addedByKind")).toString();
    if (out.contains(QStringLiteral("addedByKind")) && kind != QStringLiteral("user") &&
        kind != QStringLiteral("agent"))
        return false;
    const QJsonValue tools = pick("allowed_tools", "allowedTools");
    if (!tools.isUndefined() && !tools.isNull()) {
        if (!tools.isArray())
            return false;
        const QJsonArray arr = tools.toArray();
        if (arr.size() > kMaxMemberAllowedTools)
            return false;
        for (const QJsonValue& t : arr) {
            if (!t.isString() || !V::nonEmptyBounded(t.toString()))
                return false;
        }
        out.insert(QStringLiteral("allowedTools"), arr);
    }
    return true;
}
inline QJsonObject argByteArrayB64(const QString& b64) {
    return {{QStringLiteral("type"), QStringLiteral("bytearray-base64")},
            {QStringLiteral("value"), b64}};
}

/** Read a generated-media file from disk, capped at the WS frame size.
 *  The path must be absolute and inside AppDataLocation; anything else
 *  is rejected to prevent traversal. Returns empty object on rejection
 *  or read failure. */
inline QJsonObject readMediaFile(const QString& absPath) {
    QFileInfo fi(absPath);
    const QString canonical = fi.canonicalFilePath();
    if (canonical.isEmpty())
        return {};
    const QString appDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appDir.isEmpty() || !canonical.startsWith(appDir))
        return {};

    if (fi.size() > kMaxContentBytes)
        return {};  // see wire-protocol.h

    QFile f(canonical);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = f.readAll();
    if (bytes.isEmpty())
        return {};

    return QJsonObject{
        {QStringLiteral("filename"), fi.fileName()},
        {QStringLiteral("size_bytes"), bytes.size()},
        {QStringLiteral("content_base64"), QString::fromLatin1(bytes.toBase64())},
    };
}

/** Stage a base64 payload to the wire-process-owned uploads dir. The
 *  path is returned for handing to the host's existing
 *  sendMessageWithAttachments / addProjectDocument Q_INVOKABLEs. The
 *  host then copies the bytes into its own location, after which the
 *  staged temp file is no longer needed. */
inline QString stageBase64Upload(const QString& filename, const QString& base64) {
    if (filename.isEmpty() || base64.isEmpty())
        return {};
    const QByteArray bytes =
        QByteArray::fromBase64(base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (bytes.isEmpty())
        return {};
    if (bytes.size() > kMaxContentBytes)
        return {};  // see wire-protocol.h

    const QString tempBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString uploadDir = tempBase + QStringLiteral("/verzeta-remote-uploads");
    QDir().mkpath(uploadDir);

    // Sanitize filename: strip directory components, allow only basic chars.
    QFileInfo fi(filename);
    QString safe = fi.fileName();
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    if (safe.isEmpty() || safe.startsWith(QChar('.'))) {
        safe = QStringLiteral("upload");
    }

    const QString uniq = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString out = uploadDir + QStringLiteral("/") + uniq + QStringLiteral("-") + safe;
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    if (f.write(bytes) != bytes.size()) {
        f.close();
        QFile::remove(out);
        return {};
    }
    f.close();
    return out;
}

}  // namespace

WireSession::WireSession(QWebSocket* socket,
                         WireAuth* auth,
                         WireHostClient* hostClient,
                         WireDbReader* db,
                         QObject* parent)
    : QObject(parent), m_socket(socket), m_auth(auth), m_host(hostClient), m_db(db) {
    if (m_socket) {
        m_socket->setParent(this);
        QObject::connect(
            m_socket, &QWebSocket::textMessageReceived, this, &WireSession::onTextMessageReceived);
        QObject::connect(
            m_socket, &QWebSocket::disconnected, this, &WireSession::onSocketDisconnected);
    }
    if (m_host) {
        QObject::connect(m_host, &WireHostClient::hostSignal, this, &WireSession::onHostSignal);
    }

    // Periodic timeout sweeper for host-initiated requests. Parent
    // ties the timer's lifetime to this session so RAII teardown
    // joins cleanly. Single-shot retries-on-tick semantics; we
    // restart on every interval until destruction.
    m_clientRpcSweepTimer.setParent(this);
    m_clientRpcSweepTimer.setInterval(kClientRpcSweepIntervalMs);
    QObject::connect(
        &m_clientRpcSweepTimer, &QTimer::timeout, this, &WireSession::onClientRpcTimeoutSweep);
    m_clientRpcSweepTimer.start();

    sendHello();
}

WireSession::~WireSession() {
    // Defence in depth: drain any callbacks still pending so std::function
    // captures release deterministically. In well-formed lifecycles the
    // socket-disconnect drain has already cleared the map; this guard
    // covers destruction paths that bypass onSocketDisconnected (e.g.
    // direct ~WireSession from a server-side error).
    drainPendingClientRequests(QStringLiteral("session_destroyed"),
                               QStringLiteral("session destroyed"));
}

void WireSession::onSocketDisconnected() {
    // Drain BEFORE emit so callers blocked on the request's future see
    // the resolution before their owning service tears the session
    // down in response to `disconnected(this)`.
    drainPendingClientRequests(QStringLiteral("session_closed"),
                               QStringLiteral("WebSocket disconnected"));
    emit disconnected(this);
}

void WireSession::forceClose(const QString& kind) {
    drainPendingClientRequests(kind, QStringLiteral("session superseded"));
    if (m_socket) {
        // close() initiates a graceful WebSocket close handshake; if
        // the peer's TCP is wedged, the OS-level socket teardown takes
        // over after the keep-alive window.  abort() would tear down
        // immediately but skips the close frame; close() is the right
        // default for a clean supersede.
        m_socket->close();
    }
}

void WireSession::sendHello() {
    sendEvent(QStringLiteral("hello"),
              QJsonObject{
                  {QStringLiteral("host_name"), QStringLiteral("Verzeta Studio")},
                  {QStringLiteral("requires_auth"), true},
                  {QStringLiteral("protocol"), QStringLiteral("verzeta-ws-2")},
                  {QStringLiteral("host_alive"), m_host && m_host->isConnected()},
              });
}

void WireSession::sendOk(const QString& reqId, const QJsonValue& data) {
    if (!m_socket)
        return;
    m_socket->sendTextMessage(
        QString::fromUtf8(QJsonDocument(QJsonObject{
                                            {QStringLiteral("type"), QStringLiteral("response")},
                                            {QStringLiteral("request_id"), reqId},
                                            {QStringLiteral("ok"), true},
                                            {QStringLiteral("data"), data},
                                        })
                              .toJson(QJsonDocument::Compact)));
}


void WireSession::invokeFire(const QString& target, const QString& method, const QJsonArray& args) {
    if (!m_host)
        return;
    m_host->invokeFireAndForget(target, method, args, m_clientId);
}

bool WireSession::requireHostOnline(const QString& reqId) {
    if (m_host && m_host->isConnected())
        return true;
    sendErr(reqId,
            QStringLiteral("server_error"),
            QStringLiteral("Verzeta Studio is not connected to the remote server. Check that it is "
                           "running on the host."));
    return false;
}

void WireSession::invokeAsyncSession(
    const QString& target,
    const QString& method,
    const QJsonArray& args,
    const QString& returnType,
    int timeoutMs,
    std::function<void(bool, const QJsonValue&, const QString&)> callback) {
    if (!m_host) {
        if (callback)
            callback(false,
                     QJsonValue(),
                     QStringLiteral("Verzeta Studio is not connected to the remote server. Check "
                                    "that it is running on the host."));
        return;
    }
    m_host->invokeAsync(
        target, method, args, returnType, timeoutMs, std::move(callback), m_clientId);
}

void WireSession::invokePropertyGetSession(
    const QString& target,
    const QString& property,
    int timeoutMs,
    std::function<void(bool, const QJsonValue&, const QString&)> callback) {
    if (!m_host) {
        if (callback)
            callback(false,
                     QJsonValue(),
                     QStringLiteral("Verzeta Studio is not connected to the remote server. Check "
                                    "that it is running on the host."));
        return;
    }
    m_host->invokePropertyGet(target, property, timeoutMs, std::move(callback), m_clientId);
}

void WireSession::invokePropertySetSession(
    const QString& target,
    const QString& property,
    const QString& valueType,
    const QJsonValue& value,
    int timeoutMs,
    std::function<void(bool, const QJsonValue&, const QString&)> callback) {
    if (!m_host) {
        if (callback)
            callback(false,
                     QJsonValue(),
                     QStringLiteral("Verzeta Studio is not connected to the remote server. Check "
                                    "that it is running on the host."));
        return;
    }
    m_host->invokePropertySet(
        target, property, valueType, value, timeoutMs, std::move(callback), m_clientId);
}

void WireSession::sendErr(const QString& reqId, const QString& kind, const QString& detail) {
    if (!m_socket)
        return;
    m_socket->sendTextMessage(
        QString::fromUtf8(QJsonDocument(QJsonObject{
                                            {QStringLiteral("type"), QStringLiteral("response")},
                                            {QStringLiteral("request_id"), reqId},
                                            {QStringLiteral("ok"), false},
                                            {QStringLiteral("error"),
                                             QJsonObject{
                                                 {QStringLiteral("kind"), kind},
                                                 {QStringLiteral("detail"), detail},
                                             }},
                                        })
                              .toJson(QJsonDocument::Compact)));
}

void WireSession::sendEvent(const QString& event, const QJsonObject& data) {
    if (!m_socket)
        return;
    m_socket->sendTextMessage(
        QString::fromUtf8(QJsonDocument(QJsonObject{
                                            {QStringLiteral("type"), QStringLiteral("event")},
                                            {QStringLiteral("event"), event},
                                            {QStringLiteral("data"), data},
                                        })
                              .toJson(QJsonDocument::Compact)));
}

bool WireSession::requireAuth(const QString& reqId) {
    if (isAuthenticated())
        return true;
    sendErr(reqId, QStringLiteral("auth_required"), QStringLiteral("op requires authentication"));
    return false;
}

bool WireSession::checkRateLimit() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (!m_recentOpsMs.isEmpty() && m_recentOpsMs.head() < now - 1000) {
        m_recentOpsMs.dequeue();
    }
    if (m_recentOpsMs.size() >= kMaxOpsPerSecond)
        return false;
    m_recentOpsMs.enqueue(now);
    return true;
}

bool WireSession::checkOutboundRateLimit() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (!m_recentOutboundOpsMs.isEmpty() && m_recentOutboundOpsMs.head() < now - 1000) {
        m_recentOutboundOpsMs.dequeue();
    }
    if (m_recentOutboundOpsMs.size() >= kMaxOutboundOpsPerSecond)
        return false;
    m_recentOutboundOpsMs.enqueue(now);
    return true;
}

qint64 WireSession::sendClientRequest(const QString& op,
                                      const QJsonObject& args,
                                      ClientRpcCallback callback,
                                      int timeoutMs) {
    // Fail-closed: reject every call that cannot be honoured rather
    // than queue silently. Each rejection still fires the callback
    // exactly once so the caller's future resolves and any captured
    // resources release.
    if (!callback)
        return 0;
    if (op.isEmpty()) {
        callback(ClientRpcReply{false,
                                QStringLiteral("invalid_argument"),
                                QStringLiteral("op is empty"),
                                QJsonValue()});
        return 0;
    }
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        callback(ClientRpcReply{false,
                                QStringLiteral("session_closed"),
                                QStringLiteral("WebSocket not connected"),
                                QJsonValue()});
        return 0;
    }
    if (m_pendingClientRequests.size() >= kMaxPendingClientRequests) {
        callback(ClientRpcReply{false,
                                QStringLiteral("too_many_pending"),
                                QStringLiteral("session pending-request map at capacity"),
                                QJsonValue()});
        return 0;
    }
    if (!checkOutboundRateLimit()) {
        callback(ClientRpcReply{false,
                                QStringLiteral("rate_limited"),
                                QStringLiteral("outbound request rate exceeded"),
                                QJsonValue()});
        return 0;
    }

    const int clampedTimeoutMs = qMax(100, timeoutMs);
    const QString reqId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 expiresAtMs =
        QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(clampedTimeoutMs);

    m_pendingClientRequests.insert(reqId,
                                   PendingClientRequest{
                                       op,
                                       expiresAtMs,
                                       std::move(callback),
                                   });

    m_socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(QJsonObject{
                          {QStringLiteral("type"), QString::fromLatin1(WsType::Request)},
                          {QStringLiteral("request_id"), reqId},
                          {QStringLiteral("op"), op},
                          {QStringLiteral("args"), args},
                      })
            .toJson(QJsonDocument::Compact)));

    return expiresAtMs;
}

void WireSession::onClientResponse(const QJsonObject& root) {
    const QString reqId = root.value(QStringLiteral("request_id")).toString();
    if (reqId.isEmpty()) {
        // Malformed reply — log and drop. The originating callback
        // (if any) already resolved via the timeout sweeper.
        qCWarning(wireSess) << "client_response missing request_id";
        return;
    }
    const auto it = m_pendingClientRequests.find(reqId);
    if (it == m_pendingClientRequests.end()) {
        // Already resolved (timeout) or never issued by this session.
        // Either way the reply is informational; we drop it silently
        // to keep the protocol forward-compatible.
        return;
    }

    PendingClientRequest pending = std::move(it.value());
    m_pendingClientRequests.erase(it);

    ClientRpcReply reply;
    const bool ok = root.value(QStringLiteral("ok")).toBool(false);
    if (ok) {
        reply.ok = true;
        reply.data = root.value(QStringLiteral("data"));
    } else {
        reply.ok = false;
        const QJsonObject errObj = root.value(QStringLiteral("error")).toObject();
        reply.errorKind =
            errObj.value(QStringLiteral("kind")).toString(QStringLiteral("client_error"));
        reply.errorDetail = errObj.value(QStringLiteral("detail")).toString();
    }

    if (pending.callback)
        pending.callback(reply);
}

void WireSession::onClientRpcTimeoutSweep() {
    if (m_pendingClientRequests.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Two-pass: collect expired ids first so we don't invalidate the
    // hash iterator if a callback re-enters and mutates the map (e.g.
    // by issuing a follow-up request).
    QStringList expired;
    expired.reserve(m_pendingClientRequests.size());
    for (auto it = m_pendingClientRequests.constBegin(); it != m_pendingClientRequests.constEnd();
         ++it) {
        if (it.value().expiresAtMs <= now) {
            expired.append(it.key());
        }
    }
    for (const QString& reqId : expired) {
        const auto it = m_pendingClientRequests.find(reqId);
        if (it == m_pendingClientRequests.end())
            continue;
        PendingClientRequest pending = std::move(it.value());
        m_pendingClientRequests.erase(it);
        if (pending.callback) {
            pending.callback(
                ClientRpcReply{false,
                               QStringLiteral("timeout"),
                               QStringLiteral("client_response not received within deadline"),
                               QJsonValue()});
        }
    }
}

void WireSession::drainPendingClientRequests(const QString& kind, const QString& detail) {
    if (m_pendingClientRequests.isEmpty())
        return;
    // Move the whole map out so callbacks that re-enter (issuing a
    // brand-new request during drain) cannot observe inconsistent
    // state. After this point the per-session map is empty and any
    // re-entrant sendClientRequest will see a fresh capacity budget.
    QHash<QString, PendingClientRequest> drain;
    drain.swap(m_pendingClientRequests);
    for (auto it = drain.begin(); it != drain.end(); ++it) {
        if (it.value().callback) {
            it.value().callback(ClientRpcReply{false, kind, detail, QJsonValue()});
        }
    }
}

void WireSession::asyncInvoke(const QString& reqId,
                              const QString& target,
                              const QString& method,
                              const QJsonArray& args,
                              const QString& returnType,
                              int timeoutMs) {
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    const QString rid = reqId;
    invokeAsyncSession(target,
                       method,
                       args,
                       returnType,
                       timeoutMs,
                       [self, rid](bool ok, const QJsonValue& data, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid, data);
                       });
}

void WireSession::asyncInvokeWrapId(const QString& reqId,
                                    const QString& target,
                                    const QString& method,
                                    const QJsonArray& args,
                                    const QString& idKey,
                                    int timeoutMs) {
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    const QString rid = reqId;
    const QString key = idKey;
    invokeAsyncSession(target,
                       method,
                       args,
                       QStringLiteral("QString"),
                       timeoutMs,
                       [self, rid, key](bool ok, const QJsonValue& data, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid, QJsonObject{{key, data.toString()}});
                       });
}

void WireSession::onTextMessageReceived(const QString& text) {
    if (text.size() > kMaxWireFrameBytes) {
        sendEvent(QStringLiteral("error"),
                  QJsonObject{
                      {QStringLiteral("kind"), QStringLiteral("malformed_message")},
                      {QStringLiteral("detail"), QStringLiteral("frame exceeds size limit")},
                  });
        return;
    }

    QString reqId;
    try {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            sendEvent(QStringLiteral("error"),
                      QJsonObject{
                          {QStringLiteral("kind"), QStringLiteral("malformed_message")},
                          {QStringLiteral("detail"), QStringLiteral("expected JSON object")},
                      });
            return;
        }
        const QJsonObject root = doc.object();

        // Reply envelope to a previously-issued host-initiated request.
        // Routed BEFORE the inbound rate-limit check and op extraction:
        // this frame is elicited transport, not a new op, and must not
        // be billed against the inbound 200 ops/sec budget.
        if (root.value(QStringLiteral("type")).toString() == WsType::ClientResponse) {
            onClientResponse(root);
            return;
        }

        if (!checkRateLimit()) {
            sendEvent(
                QStringLiteral("error"),
                QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("rate_limited")},
                    {QStringLiteral("detail"),
                     QStringLiteral("too many ops per second (max %1)").arg(kMaxOpsPerSecond)},
                });
            return;
        }
        const QString op = root.value(QStringLiteral("op")).toString();
        reqId = root.value(QStringLiteral("request_id")).toString();
        const QJsonObject params = root.value(QStringLiteral("params")).toObject();
        if (op.isEmpty()) {
            sendErr(
                reqId, QStringLiteral("malformed_message"), QStringLiteral("missing 'op' field"));
            return;
        }
        dispatchOp(op, reqId, params);
    } catch (const std::exception& ex) {
        qCWarning(wireSess) << "exception:" << ex.what();
        if (!reqId.isEmpty()) {
            sendErr(reqId, QStringLiteral("server_error"), QStringLiteral("internal error"));
        }
    } catch (...) {
        qCWarning(wireSess) << "unknown exception";
        if (!reqId.isEmpty()) {
            sendErr(reqId, QStringLiteral("server_error"), QStringLiteral("internal error"));
        }
    }
}

// ===========================================================================
// Op dispatch
// ===========================================================================

void WireSession::dispatchOp(const QString& op, const QString& reqId, const QJsonObject& params) {
    // Auth-bypass ops.
    if (op == QStringLiteral("ping")) {
        opPing(reqId, params);
        return;
    }
    if (op == QStringLiteral("auth.pair")) {
        opAuthPair(reqId, params);
        return;
    }
    if (op == QStringLiteral("auth.token")) {
        opAuthToken(reqId, params);
        return;
    }

    if (!requireAuth(reqId))
        return;

    if (op == QStringLiteral("auth.me")) {
        opAuthMe(reqId, params);
        return;
    }
    if (op == QStringLiteral("auth.revoke_self")) {
        opAuthRevokeSelf(reqId, params);
        return;
    }
    if (op == QStringLiteral("clients.list")) {
        opClientsList(reqId, params);
        return;
    }
    if (op == QStringLiteral("clients.revoke")) {
        opClientsRevoke(reqId, params);
        return;
    }

    if (op == QStringLiteral("msg.subscribe")) {
        opMsgSubscribe(reqId, params);
        return;
    }
    if (op == QStringLiteral("msg.unsubscribe")) {
        opMsgUnsubscribe(reqId, params);
        return;
    }

    if (op == QStringLiteral("conv.list")) {
        opConvList(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.get")) {
        opConvGet(reqId, params);
        return;
    }
    if (op == QStringLiteral("msg.list")) {
        opMsgList(reqId, params);
        return;
    }
    if (op == QStringLiteral("msg.send")) {
        opMsgSend(reqId, params);
        return;
    }
    if (op == QStringLiteral("msg.stop")) {
        opMsgStop(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.create")) {
        opConvCreate(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.create_with_agent")) {
        opConvCreateWithAgent(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.rename")) {
        opConvRename(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.set_pinned")) {
        opConvSetPinned(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.delete")) {
        opConvDelete(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.move_to_folder")) {
        opConvMoveToFolder(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.is_group")) {
        opConvIsGroup(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.group_members")) {
        opConvGroupMembers(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.folder_id")) {
        opConvFolderId(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.open")) {
        opConvOpen(reqId, params);
        return;
    }

    if (op == QStringLiteral("folder.list_projects")) {
        opFolderListProjects(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.list_all")) {
        opFolderListAll(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.conversations")) {
        opFolderConversations(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.create")) {
        opFolderCreate(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.rename")) {
        opFolderRename(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.delete")) {
        opFolderDelete(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.info")) {
        opFolderInfo(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.update_metadata")) {
        opFolderUpdateMetadata(reqId, params);
        return;
    }
    if (op == QStringLiteral("group.create")) {
        opGroupCreate(reqId, params);
        return;
    }

    if (op == QStringLiteral("models.catalog")) {
        opModelsCatalog(reqId, params);
        return;
    }
    if (op == QStringLiteral("models.set_active")) {
        opModelsSetActive(reqId, params);
        return;
    }
    if (op == QStringLiteral("models.active")) {
        opModelsActive(reqId, params);
        return;
    }
    if (op == QStringLiteral("models.for_provider")) {
        opModelsForProvider(reqId, params);
        return;
    }
    if (op == QStringLiteral("search.providers")) {
        opSearchProviders(reqId, params);
        return;
    }
    if (op == QStringLiteral("search.active")) {
        opSearchActive(reqId, params);
        return;
    }
    if (op == QStringLiteral("search.set_active")) {
        opSearchSetActive(reqId, params);
        return;
    }

    if (op == QStringLiteral("agent.list")) {
        opAgentList(reqId, params);
        return;
    }
    if (op == QStringLiteral("agent.get")) {
        opAgentGet(reqId, params);
        return;
    }
    if (op == QStringLiteral("tool.list")) {
        opToolList(reqId, params);
        return;
    }
    if (op == QStringLiteral("mcp.list")) {
        opMcpList(reqId, params);
        return;
    }
    if (op == QStringLiteral("mcp.server_tools")) {
        opMcpServerTools(reqId, params);
        return;
    }
    if (op == QStringLiteral("skill.list")) {
        opSkillList(reqId, params);
        return;
    }
    if (op == QStringLiteral("skill.get")) {
        opSkillGet(reqId, params);
        return;
    }

    if (op == QStringLiteral("poll.list")) {
        opPollList(reqId, params);
        return;
    }
    if (op == QStringLiteral("poll.results")) {
        opPollResults(reqId, params);
        return;
    }
    if (op == QStringLiteral("poll.start")) {
        opPollStart(reqId, params);
        return;
    }
    if (op == QStringLiteral("poll.vote")) {
        opPollVote(reqId, params);
        return;
    }
    if (op == QStringLiteral("poll.close")) {
        opPollClose(reqId, params);
        return;
    }

    if (op == QStringLiteral("activity.for_project")) {
        opActivityForProject(reqId, params);
        return;
    }
    if (op == QStringLiteral("activity.for_conversation")) {
        opActivityForConversation(reqId, params);
        return;
    }
    if (op == QStringLiteral("activity.by_turn")) {
        opActivityByTurn(reqId, params);
        return;
    }

    if (op == QStringLiteral("project_template.list")) {
        opProjectTemplateList(reqId, params);
        return;
    }
    if (op == QStringLiteral("project_template.list_landing")) {
        opProjectTemplateListLanding(reqId, params);
        return;
    }
    if (op == QStringLiteral("project_template.get")) {
        opProjectTemplateGet(reqId, params);
        return;
    }
    if (op == QStringLiteral("project_template.roster")) {
        opProjectTemplateRoster(reqId, params);
        return;
    }
    if (op == QStringLiteral("project_template.create_project")) {
        opProjectTemplateCreateProject(reqId, params);
        return;
    }
    if (op == QStringLiteral("project_template.pin_user")) {
        opProjectTemplatePinUser(reqId, params);
        return;
    }
    if (op == QStringLiteral("project_template.save_as_new")) {
        opProjectTemplateSaveAsNew(reqId, params);
        return;
    }
    if (op == QStringLiteral("project_template.delete_user")) {
        opProjectTemplateDeleteUser(reqId, params);
        return;
    }

    // Workspace mount registry — 5 ops mirror the FolderMountRegistry
    // Q_INVOKABLE surface. Bytes never leave the client; this surface
    // manages mount metadata only.
    if (op == QStringLiteral("workspace.mount.register")) {
        opWorkspaceMountRegister(reqId, params);
        return;
    }
    if (op == QStringLiteral("workspace.mount.unregister")) {
        opWorkspaceMountUnregister(reqId, params);
        return;
    }
    if (op == QStringLiteral("workspace.mount.update_tree")) {
        opWorkspaceMountUpdateTree(reqId, params);
        return;
    }
    if (op == QStringLiteral("workspace.mount.update_tier")) {
        opWorkspaceMountUpdateTier(reqId, params);
        return;
    }
    if (op == QStringLiteral("workspace.mount.for_folder")) {
        opWorkspaceMountForFolder(reqId, params);
        return;
    }

    if (op == QStringLiteral("folder.members")) {
        opFolderMembers(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.members.set")) {
        opFolderMembersSet(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.member.add")) {
        opFolderMemberAdd(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.member.override.set")) {
        opFolderMemberOverrideSet(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.member.remove")) {
        opFolderMemberRemove(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.members")) {
        opConvMembers(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.members.set")) {
        opConvMembersSet(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.member.add")) {
        opConvMemberAdd(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.member.remove")) {
        opConvMemberRemove(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.coordinator.set")) {
        opConvCoordinatorSet(reqId, params);
        return;
    }

    if (op == QStringLiteral("folder.documents")) {
        opFolderDocuments(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.documents.upload")) {
        opFolderDocumentUpload(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.documents.remove")) {
        opFolderDocumentRemove(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.kickoff.individual")) {
        opFolderKickoffIndividual(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.kickoff.group")) {
        opFolderKickoffGroup(reqId, params);
        return;
    }
    if (op == QStringLiteral("folder.member.chat.open")) {
        opFolderMemberChatOpen(reqId, params);
        return;
    }

    if (op == QStringLiteral("skill.preferred.list")) {
        opSkillPreferredList(reqId, params);
        return;
    }
    if (op == QStringLiteral("skill.preferred.set")) {
        opSkillPreferredSet(reqId, params);
        return;
    }
    if (op == QStringLiteral("skill.expose_only_preferred")) {
        opSkillExposeOnly(reqId, params);
        return;
    }
    if (op == QStringLiteral("skill.set_expose_only_preferred")) {
        opSkillSetExposeOnly(reqId, params);
        return;
    }
    if (op == QStringLiteral("skill.override_parent_folder")) {
        opSkillOverrideParent(reqId, params);
        return;
    }
    if (op == QStringLiteral("skill.set_override_parent_folder")) {
        opSkillSetOverrideParent(reqId, params);
        return;
    }

    if (op == QStringLiteral("heartbeat.configs_for_folder")) {
        opHbConfigsForFolder(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.configs_for_conversation")) {
        opHbConfigsForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.config_by_id")) {
        opHbConfigById(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.upsert")) {
        opHbUpsert(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.remove")) {
        opHbRemove(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.run_now")) {
        opHbRunNow(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.cancel_run")) {
        opHbCancelRun(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.recent_runs")) {
        opHbRecentRuns(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.next_fires_preview")) {
        opHbNextFires(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.recent_config_changes")) {
        opHbConfigChanges(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.config_status")) {
        opHbConfigStatus(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.manual_post")) {
        opHbManualPost(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.manual_dismiss")) {
        opHbManualDismiss(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.suggested_cap_for_conv")) {
        opHbCapForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("heartbeat.suggested_cap_for_folder")) {
        opHbCapForFolder(reqId, params);
        return;
    }

    if (op == QStringLiteral("conv.settings.get")) {
        opConvSettingsGet(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.settings.save")) {
        opConvSettingsSave(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.primary_agent")) {
        opConvPrimaryAgent(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.primary_agent.set")) {
        opConvPrimaryAgentSet(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.heartbeat.gate")) {
        opConvHbGate(reqId, params);
        return;
    }
    if (op == QStringLiteral("conv.heartbeat.gate.set")) {
        opConvHbGateSet(reqId, params);
        return;
    }
    if (op == QStringLiteral("agent.pattern")) {
        opAgentPattern(reqId, params);
        return;
    }
    if (op == QStringLiteral("agent.pattern.set")) {
        opAgentPatternSet(reqId, params);
        return;
    }
    if (op == QStringLiteral("agent.require_confirmation")) {
        opAgentRequireConfirm(reqId, params);
        return;
    }
    if (op == QStringLiteral("agent.require_confirmation.set")) {
        opAgentRequireConfirmSet(reqId, params);
        return;
    }
    if (op == QStringLiteral("tools.enabled")) {
        opToolsEnabled(reqId, params);
        return;
    }
    if (op == QStringLiteral("tools.enabled.set")) {
        opToolsEnabledSet(reqId, params);
        return;
    }
    // rag.enabled / rag.enabled.set removed: RAG enable is per-conversation
    // (llm_config.rag_enabled), carried by conv.settings.get/.set.

    if (op == QStringLiteral("plan.list_for_conv")) {
        opPlanListForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("plan.get")) {
        opPlanGet(reqId, params);
        return;
    }
    if (op == QStringLiteral("task.start")) {
        opTaskStart(reqId, params);
        return;
    }
    if (op == QStringLiteral("plan.stop")) {
        opPlanStop(reqId, params);
        return;
    }
    if (op == QStringLiteral("plan.stop_all_in_conv")) {
        opPlanStopAllInConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("step.retry")) {
        opStepRetry(reqId, params);
        return;
    }
    if (op == QStringLiteral("step.override_done")) {
        opStepOverrideDone(reqId, params);
        return;
    }
    if (op == QStringLiteral("step.skip")) {
        opStepSkip(reqId, params);
        return;
    }

    if (op == QStringLiteral("tool_call.list_for_conv")) {
        opToolCallListForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("tool_call.list_for_message")) {
        opToolCallListForMessage(reqId, params);
        return;
    }
    if (op == QStringLiteral("tool_call.list_for_turn")) {
        opToolCallListForTurn(reqId, params);
        return;
    }
    if (op == QStringLiteral("tool_call.get")) {
        opToolCallGet(reqId, params);
        return;
    }
    if (op == QStringLiteral("tool_call.approve")) {
        opToolCallApprove(reqId, params);
        return;
    }
    if (op == QStringLiteral("tool_call.deny")) {
        opToolCallDeny(reqId, params);
        return;
    }

    if (op == QStringLiteral("attachment.list_for_message")) {
        opAttachmentListForMsg(reqId, params);
        return;
    }
    if (op == QStringLiteral("attachment.get")) {
        opAttachmentGet(reqId, params);
        return;
    }
    if (op == QStringLiteral("artifact.list_for_conv")) {
        opArtifactListForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("artifact.download")) {
        opArtifactDownload(reqId, params);
        return;
    }
    if (op == QStringLiteral("image.list_for_conv")) {
        opImageListForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("image.download")) {
        opImageDownload(reqId, params);
        return;
    }
    if (op == QStringLiteral("audio.list_for_conv")) {
        opAudioListForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("audio.download")) {
        opAudioDownload(reqId, params);
        return;
    }
    if (op == QStringLiteral("msg.send_with_attachments")) {
        opMsgSendWithAttachments(reqId, params);
        return;
    }

    if (op == QStringLiteral("canvas.active_for_conv")) {
        opCanvasActiveForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.history_for_conv")) {
        opCanvasHistoryForConv(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.disk_mirror_path")) {
        opCanvasDiskMirrorPath(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.suggested_export_name")) {
        opCanvasSuggestedExportName(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.read_slice")) {
        opCanvasReadSlice(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.open")) {
        opCanvasOpen(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.edit")) {
        opCanvasEdit(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.close")) {
        opCanvasClose(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.switch_to")) {
        opCanvasSwitchTo(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.tools.list")) {
        opCanvasToolsList(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.tools.run")) {
        opCanvasToolsRun(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.ai_actions.list")) {
        opCanvasAiActionsList(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.ai_actions.trigger")) {
        opCanvasAiActionsTrigger(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.start")) {
        opCanvasRunStart(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.cancel")) {
        opCanvasRunCancel(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.send_input")) {
        opCanvasRunSendInput(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.eof")) {
        opCanvasRunEof(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.send_to_ide")) {
        opCanvasRunSendToIde(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.sandbox_state")) {
        opCanvasRunSandboxState(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.is_running")) {
        opCanvasRunIsRunning(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.supported_languages")) {
        opCanvasRunSupportedLangs(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.run.supported_ide_languages")) {
        opCanvasRunSupportedIdeLangs(reqId, params);
        return;
    }
    if (op == QStringLiteral("canvas.console.clear")) {
        opCanvasConsoleClear(reqId, params);
        return;
    }

    sendErr(reqId, QStringLiteral("unknown_op"), QStringLiteral("unknown op: %1").arg(op));
}

// ===========================================================================
// Auth ops
// ===========================================================================

void WireSession::opPing(const QString& reqId, const QJsonObject&) {
    sendOk(
        reqId,
        QJsonObject{
            {QStringLiteral("time_ms"), static_cast<qint64>(QDateTime::currentMSecsSinceEpoch())},
            {QStringLiteral("host_alive"), m_host && m_host->isConnected()},
        });
}

void WireSession::opAuthPair(const QString& reqId, const QJsonObject& params) {
    if (!m_auth) {
        sendErr(reqId, QStringLiteral("server_error"), QStringLiteral("auth unavailable"));
        return;
    }
    const QString code = params.value(QStringLiteral("code")).toString();
    const QString name = params.value(QStringLiteral("client_name")).toString();
    if (!V::nonEmptyBounded(code) || !V::nonEmptyBounded(name)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'code' and 'client_name' required"));
        return;
    }
    if (!m_auth->consumePairingCode(code)) {
        sendErr(reqId,
                QStringLiteral("auth_failed"),
                QStringLiteral("invalid or expired pairing code"));
        return;
    }
    auto issued = m_auth->issueToken(name);
    if (!issued) {
        sendErr(reqId, QStringLiteral("server_error"), QStringLiteral("token issuance failed"));
        return;
    }
    m_clientId = issued->clientId;
    m_clientName = name;
    // Record any capabilities the client advertised so host-initiated
    // routing can gate on them (see hasCapability()).
    m_capabilities = parseCapabilities(params);
    emit clientIdRebound(m_clientId, this);
    if (qEnvironmentVariableIntValue("VERZETA_MOUNT_TRACE") > 0) {
        qCInfo(wireSess).noquote() << "MOUNT-TRACE: opAuthPair bound clientId=" << m_clientId
                                   << "session=" << static_cast<void*>(this);
    }
    sendOk(reqId,
           QJsonObject{
               {QStringLiteral("token"), issued->token},
               {QStringLiteral("client_id"), issued->clientId},
               {QStringLiteral("name"), name},
           });
}

void WireSession::opAuthToken(const QString& reqId, const QJsonObject& params) {
    if (!m_auth) {
        sendErr(reqId, QStringLiteral("server_error"), QStringLiteral("auth unavailable"));
        return;
    }
    const QString token = params.value(QStringLiteral("token")).toString();
    if (!V::nonEmptyBounded(token)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'token' required"));
        return;
    }
    auto rec = m_auth->verifyToken(token);
    if (!rec) {
        sendErr(reqId, QStringLiteral("auth_failed"), QStringLiteral("invalid or revoked token"));
        return;
    }
    m_clientId = rec->id;
    m_clientName = rec->name;
    // Record any capabilities the client advertised so host-initiated
    // routing can gate on them (see hasCapability()).
    m_capabilities = parseCapabilities(params);
    emit clientIdRebound(m_clientId, this);
    if (qEnvironmentVariableIntValue("VERZETA_MOUNT_TRACE") > 0) {
        qCInfo(wireSess).noquote() << "MOUNT-TRACE: opAuthToken bound clientId=" << m_clientId
                                   << "session=" << static_cast<void*>(this);
    }
    sendOk(reqId,
           QJsonObject{
               {QStringLiteral("client_id"), rec->id},
               {QStringLiteral("name"), rec->name},
               {QStringLiteral("last_seen_at"),
                rec->lastSeenAt.isValid()
                    ? QJsonValue(static_cast<qint64>(rec->lastSeenAt.toMSecsSinceEpoch()))
                    : QJsonValue(QJsonValue::Null)},
           });
}

void WireSession::opAuthMe(const QString& reqId, const QJsonObject&) {
    qint64 lastSeen = 0;
    bool have = false;
    if (m_auth) {
        for (const auto& r : m_auth->listClients(true)) {
            if (r.id == m_clientId && r.lastSeenAt.isValid()) {
                lastSeen = r.lastSeenAt.toMSecsSinceEpoch();
                have = true;
                break;
            }
        }
    }
    sendOk(reqId,
           QJsonObject{
               {QStringLiteral("client_id"), m_clientId},
               {QStringLiteral("name"), m_clientName},
               {QStringLiteral("last_seen_at"),
                have ? QJsonValue(lastSeen) : QJsonValue(QJsonValue::Null)},
           });
}

void WireSession::opAuthRevokeSelf(const QString& reqId, const QJsonObject&) {
    if (m_auth)
        m_auth->revokeClient(m_clientId);
    sendOk(reqId, QJsonValue{});
    if (m_socket)
        m_socket->close();
}

void WireSession::opClientsList(const QString& reqId, const QJsonObject&) {
    if (!m_auth) {
        sendErr(reqId, QStringLiteral("server_error"), QStringLiteral("auth unavailable"));
        return;
    }
    QJsonArray arr;
    for (const auto& r : m_auth->listClients(true)) {
        if (r.id == m_clientId)
            continue;
        arr.append(QJsonObject{
            {QStringLiteral("id"), r.id},
            {QStringLiteral("name"), r.name},
            {QStringLiteral("created_at"),
             r.createdAt.isValid()
                 ? QJsonValue(static_cast<qint64>(r.createdAt.toMSecsSinceEpoch()))
                 : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("last_seen_at"),
             r.lastSeenAt.isValid()
                 ? QJsonValue(static_cast<qint64>(r.lastSeenAt.toMSecsSinceEpoch()))
                 : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("revoked"), !r.isActive()},
        });
    }
    sendOk(reqId, arr);
}

void WireSession::opClientsRevoke(const QString& reqId, const QJsonObject& params) {
    if (!m_auth) {
        sendErr(reqId, QStringLiteral("server_error"), QStringLiteral("auth unavailable"));
        return;
    }
    const QString id = params.value(QStringLiteral("client_id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'client_id' must be a valid UUID"));
        return;
    }
    if (id == m_clientId) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("use auth.revoke_self for the current client"));
        return;
    }
    sendOk(reqId,
           QJsonObject{
               {QStringLiteral("revoked"), m_auth->revokeClient(id)},
           });
}

// ===========================================================================
// Subscriptions
// ===========================================================================

void WireSession::opMsgSubscribe(const QString& reqId, const QJsonObject& params) {
    const QString convId = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(convId)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'conv_id' must be a valid UUID"));
        return;
    }
    if (!m_subscribedConvIds.contains(convId) && m_subscribedConvIds.size() >= kMaxSubscriptions) {
        sendErr(reqId,
                QStringLiteral("rate_limited"),
                QStringLiteral("subscription cap reached (%1)").arg(kMaxSubscriptions));
        return;
    }
    m_subscribedConvIds.insert(convId);
    sendOk(reqId, QJsonValue{});
}

void WireSession::opMsgUnsubscribe(const QString& reqId, const QJsonObject& params) {
    const QString convId = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(convId)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'conv_id' must be a valid UUID"));
        return;
    }
    m_subscribedConvIds.remove(convId);
    sendOk(reqId, QJsonValue{});
}

// ===========================================================================
// Conversations + messages — reads route to host via async invoke; writes
// route via fire-and-forget. Args are typed JSON {type, value} pairs.
// ===========================================================================

void WireSession::opConvList(const QString& reqId, const QJsonObject&) {
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->listConversations());
}

void WireSession::opConvGet(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be a valid UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    const QJsonObject row = m_db->conversationById(id);
    if (row.isEmpty()) {
        sendErr(reqId, QStringLiteral("not_found"), QStringLiteral("no such conversation"));
        return;
    }
    sendOk(reqId, row);
}

void WireSession::opMsgList(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(cid)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'conv_id' must be a valid UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->messagesForConversation(cid));
}

void WireSession::opMsgSend(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    const QString text = params.value(QStringLiteral("text")).toString();
    if (!V::isUuidShape(cid)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'conv_id' must be a valid UUID"));
        return;
    }
    if (!V::nonEmptyBounded(text)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'text' required"));
        return;
    }
    // Honest ack: a silently-dropped chat message is the worst
    // possible lie to a client — see requireHostOnline.
    if (!requireHostOnline(reqId))
        return;
    {
        invokeFire(Targets::ChatController, QStringLiteral("switchConversation"), {argStr(cid)});
        invokeFire(Targets::ChatController,
                   QStringLiteral("sendMessage"),
                   {argStr(text), argList(QJsonArray{})});
    }
    sendOk(reqId,
           QJsonObject{
               {QStringLiteral("status"), QStringLiteral("queued")},
               {QStringLiteral("conv_id"), cid},
           });
}

void WireSession::opMsgStop(const QString& reqId, const QJsonObject&) {
    if (!requireHostOnline(reqId))
        return;
    invokeFire(Targets::ChatController, QStringLiteral("stopGeneration"), {});
    sendOk(reqId, QJsonValue{});
}

void WireSession::opConvCreate(const QString& reqId, const QJsonObject& params) {
    const QString title =
        params.value(QStringLiteral("title")).toString(QStringLiteral("New Chat"));
    if (!V::optionalBounded(title)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'title' too long"));
        return;
    }
    asyncInvokeWrapId(
        reqId, Targets::Conversations, QStringLiteral("newConversation"), {argStr(title)});
}

void WireSession::opConvCreateWithAgent(const QString& reqId, const QJsonObject& params) {
    const QString aid = params.value(QStringLiteral("agent_id")).toString();
    const QString title = params.value(QStringLiteral("title")).toString();
    if (!V::isUuidShape(aid)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'agent_id' must be UUID"));
        return;
    }
    if (!V::optionalBounded(title)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'title' too long"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::Conversations,
                      QStringLiteral("newConversationWithAgent"),
                      {argStr(aid), argStr(title)});
}

void WireSession::opConvRename(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    const QString t = params.value(QStringLiteral("title")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    if (!V::nonEmptyBounded(t)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'title' required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("renameConversation"),
                {argStr(id), argStr(t)},
                QStringLiteral("bool"));
}

void WireSession::opConvSetPinned(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    const bool pinned = params.value(QStringLiteral("pinned")).toBool();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::ConversationService,
                QStringLiteral("setConversationPinned"),
                {argStr(id), argBool(pinned)},
                QStringLiteral("bool"));
}

void WireSession::opConvDelete(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    // Honest ack: a dropped delete "comes back" on the next list —
    // see requireHostOnline.
    if (!requireHostOnline(reqId))
        return;
    invokeFire(Targets::Conversations, QStringLiteral("deleteConversation"), {argStr(id)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}

void WireSession::opConvMoveToFolder(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    if (!V::isUuidShape(cid)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!fid.isEmpty() && !V::isUuidShape(fid)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'folder_id' must be UUID or empty"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("moveToFolder"),
                {argStr(cid), argStr(fid)},
                QStringLiteral("bool"));
}

void WireSession::opConvIsGroup(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    invokeAsyncSession(Targets::Conversations,
                       QStringLiteral("isGroup"),
                       {argStr(id)},
                       QStringLiteral("bool"),
                       5000,
                       [self, rid](bool ok, const QJsonValue& v, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid, QJsonObject{{QStringLiteral("is_group"), v.toBool()}});
                       });
}

void WireSession::opConvGroupMembers(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("groupMembers"),
                {argStr(id)},
                QStringLiteral("QVariantList"));
}

void WireSession::opConvFolderId(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::Conversations,
                      QStringLiteral("folderIdOf"),
                      {argStr(id)},
                      QStringLiteral("folder_id"));
}

void WireSession::opConvOpen(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    if (m_host) {
        invokeFire(Targets::ChatController, QStringLiteral("switchConversation"), {argStr(id)});
    }
    if (!m_db || !m_db->isOpen()) {
        sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
        return;
    }
    const QJsonObject settings = m_db->conversationSettings(id);
    sendOk(reqId,
           QJsonObject{
               {QStringLiteral("conv_id"), id},
               {QStringLiteral("provider"), settings.value(QStringLiteral("providerId"))},
               {QStringLiteral("model"), settings.value(QStringLiteral("modelName"))},
           });
}

// Folders --------------------------------------------------------------------

void WireSession::opFolderListProjects(const QString& reqId, const QJsonObject&) {
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("listProjectFolders"),
                {},
                QStringLiteral("QVariantList"));
}
void WireSession::opFolderListAll(const QString& reqId, const QJsonObject&) {
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->listAllFolders());
}
void WireSession::opFolderConversations(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    if (!fid.isEmpty() && !V::isUuidShape(fid)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'folder_id' must be UUID or empty"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("conversationsInFolder"),
                {argStr(fid)},
                QStringLiteral("QVariantList"));
}
void WireSession::opFolderCreate(const QString& reqId, const QJsonObject& params) {
    const QString name = params.value(QStringLiteral("name")).toString();
    if (!V::nonEmptyBounded(name)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'name' required"));
        return;
    }
    // Wrap the QString id in {id, type} so clients' folder-create
    // parsers get both fields. ConversationService::createFolder inserts
    // a folder with NO folder_type, so the row defaults to 'regular' —
    // report that truthfully rather than claiming 'project'. A wrong
    // type here made paired clients optimistically render the new folder
    // as a Project Room (team-member machinery) until the corrective
    // folder.added event arrived; worse, a conversation moved into it
    // could appear misplaced or vanish on clients that only sectioned
    // project/organization folders.
    QPointer<WireSession> self(this);
    QString rid = reqId;
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    invokeAsyncSession(Targets::Conversations,
                       QStringLiteral("createFolder"),
                       {argStr(name)},
                       QStringLiteral("QString"),
                       5000,
                       [self, rid](bool ok, const QJsonValue& data, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid,
                                        QJsonObject{
                                            {QStringLiteral("id"), data.toString()},
                                            {QStringLiteral("type"), QStringLiteral("regular")},
                                        });
                       });
}
void WireSession::opFolderRename(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    const QString n = params.value(QStringLiteral("name")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    if (!V::nonEmptyBounded(n)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'name' required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("renameFolder"),
                {argStr(id), argStr(n)},
                QStringLiteral("bool"));
}
void WireSession::opFolderDelete(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("deleteFolder"),
                {argStr(id)},
                QStringLiteral("bool"));
}
void WireSession::opFolderInfo(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("folderInfo"),
                {argStr(id)},
                QStringLiteral("QVariantMap"));
}
void WireSession::opFolderUpdateMetadata(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    const QString ft = params.value(QStringLiteral("folder_type")).toString();
    const QString g = params.value(QStringLiteral("goal")).toString();
    const QString d = params.value(QStringLiteral("description")).toString();
    QStringList aids;
    for (const QJsonValue& v : params.value(QStringLiteral("agent_ids")).toArray())
        aids.append(v.toString());
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    if (!V::isValidFolderType(ft)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad 'folder_type'"));
        return;
    }
    if (!V::optionalBounded(g) || !V::optionalBounded(d)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'goal' or 'description' too long"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("updateFolderMetadata"),
                {argStr(id), argStr(ft), argStr(g), argStr(d), argStringList(aids)},
                QStringLiteral("bool"));
}
void WireSession::opGroupCreate(const QString& reqId, const QJsonObject& params) {
    const QString title = params.value(QStringLiteral("title")).toString();
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    const QJsonArray membersArr = params.value(QStringLiteral("members")).toArray();
    if (!V::nonEmptyBounded(title)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'title' required"));
        return;
    }
    if (!fid.isEmpty() && !V::isUuidShape(fid)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'folder_id' must be UUID or empty"));
        return;
    }
    if (membersArr.isEmpty()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'members' required"));
        return;
    }
    QJsonArray mlist;
    for (const QJsonValue& v : membersArr) {
        const QJsonObject m = v.toObject();
        const QString aid = m.contains(QStringLiteral("agent_id"))
                                ? m.value(QStringLiteral("agent_id")).toString()
                                : m.value(QStringLiteral("agentId")).toString();
        const QString al = m.value(QStringLiteral("alias")).toString();
        const bool ic = m.contains(QStringLiteral("is_coordinator"))
                            ? m.value(QStringLiteral("is_coordinator")).toBool()
                            : m.value(QStringLiteral("isCoordinator")).toBool();
        if (!V::isUuidShape(aid)) {
            sendErr(reqId,
                    QStringLiteral("invalid_params"),
                    QStringLiteral("member 'agent_id' must be UUID"));
            return;
        }
        mlist.append(QJsonObject{
            {QStringLiteral("agentId"), aid},
            {QStringLiteral("alias"), al},
            {QStringLiteral("isCoordinator"), ic},
        });
    }
    asyncInvokeWrapId(reqId,
                      Targets::Conversations,
                      QStringLiteral("newGroupConversation"),
                      {argStr(title), argList(mlist), argStr(fid)});
}

// Models ---------------------------------------------------------------------

// models.catalog aggregates EVERYTHING the picker needs in one round-trip:
// the list of registered providers (filtered to ones with models — the
// host-side QML does the same), each provider's available models, and
// the host's active provider/model selection. The fan-out is done in the
// wire layer via chained async invokes; the response is sent only after
// the last per-provider modelsForProvider() returns.
void WireSession::opModelsCatalog(const QString& reqId, const QJsonObject&) {
    QPointer<WireSession> self(this);
    QString rid = reqId;
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }

    // Shared state for the chained fan-out — captured by reference into
    // each callback via shared_ptr so the lambdas can mutate the same
    // accumulator without race conditions (callbacks run sequentially
    // on the wire thread anyway, but shared_ptr keeps lifetime safe).
    /**
     * @brief Per-call accumulator for the chained models / active-provider
     *        fan-out. Held via shared_ptr so the IPC callbacks can mutate
     *        the same object safely.
     */
    struct State {
        QJsonArray providers;  // accumulator with {provider_id, display_name, models[]}
        QString activeProvider;
        QString activeModel;
        int pendingModelsCalls = 0;
        bool activeProviderRead = false;
        bool activeModelRead = false;
    };
    auto state = std::make_shared<State>();

    auto maybeFinish = [self, rid, state]() {
        if (!self)
            return;
        if (state->pendingModelsCalls != 0)
            return;
        if (!state->activeProviderRead)
            return;
        if (!state->activeModelRead)
            return;
        // Filter out providers that returned 0 models — they're either
        // un-configured (no API key) or temporarily unreachable. Showing
        // them in the picker just produces empty groups the user can't
        // pick from, which is what the user complained about.
        QJsonArray nonEmpty;
        for (const QJsonValue& v : state->providers) {
            const QJsonObject p = v.toObject();
            if (p.value(QStringLiteral("models")).toArray().size() > 0) {
                nonEmpty.append(p);
            }
        }
        self->sendOk(rid,
                     QJsonObject{
                         {QStringLiteral("providers"), nonEmpty},
                         {QStringLiteral("active_provider"), state->activeProvider},
                         {QStringLiteral("active_model"), state->activeModel},
                     });
    };

    // Step 1: read availableProviders Q_PROPERTY (QVariantList of
    // {providerId, displayName} camelCase maps).
    invokePropertyGetSession(
        Targets::AgentSettings,
        QStringLiteral("availableProviders"),
        5000,
        [self, rid, state, maybeFinish](bool ok, const QJsonValue& providers, const QString& err) {
            if (!self)
                return;
            if (!ok) {
                self->sendErr(rid, QStringLiteral("server_error"), err);
                return;
            }
            const QJsonArray providerArr = providers.toArray();
            if (providerArr.isEmpty()) {
                // No providers — short-circuit to empty catalog.
                self->sendOk(rid,
                             QJsonObject{
                                 {QStringLiteral("providers"), QJsonArray()},
                                 {QStringLiteral("active_provider"), QString()},
                                 {QStringLiteral("active_model"), QString()},
                             });
                return;
            }
            // Seed accumulator + fan out per-provider modelsForProvider calls.
            // The capability flags (supports_streaming / supports_tool_calling /
            // supports_vision) ride through from AgentSettings
            // so paired wire clients can render per-provider capability
            // glyphs without a separate round-trip.  Older clients that
            // do not look at these fields ignore them — additive shape.
            for (const QJsonValue& v : providerArr) {
                const QJsonObject pobj = v.toObject();
                const QString pid = pobj.value(QStringLiteral("providerId")).toString();
                const QString pdn = pobj.value(QStringLiteral("displayName")).toString();
                const bool strm = pobj.value(QStringLiteral("supportsStreaming")).toBool();
                const bool tool = pobj.value(QStringLiteral("supportsToolCalling")).toBool();
                const bool visn = pobj.value(QStringLiteral("supportsVision")).toBool();
                state->providers.append(QJsonObject{
                    {QStringLiteral("provider_id"), pid},
                    {QStringLiteral("display_name"), pdn},
                    {QStringLiteral("supports_streaming"), strm},
                    {QStringLiteral("supports_tool_calling"), tool},
                    {QStringLiteral("supports_vision"), visn},
                    {QStringLiteral("models"), QJsonArray()},
                });
            }
            state->pendingModelsCalls = state->providers.size();
            for (int i = 0; i < state->providers.size(); ++i) {
                const QJsonObject pobj = state->providers[i].toObject();
                const QString pid = pobj.value(QStringLiteral("provider_id")).toString();
                self->invokeAsyncSession(Targets::AgentSettings,
                                         QStringLiteral("modelsForProvider"),
                                         {argStr(pid)},
                                         QStringLiteral("QStringList"),
                                         5000,
                                         [self, state, maybeFinish, i](
                                             bool ok, const QJsonValue& models, const QString&) {
                                             if (!self)
                                                 return;
                                             QJsonObject p = state->providers[i].toObject();
                                             if (ok && models.isArray()) {
                                                 p.insert(QStringLiteral("models"),
                                                          models.toArray());
                                             }
                                             state->providers.replace(i, p);
                                             --state->pendingModelsCalls;
                                             maybeFinish();
                                         });
            }
            // In parallel: read activeProvider + activeModel.
            self->invokePropertyGetSession(
                Targets::AgentSettings,
                QStringLiteral("activeProvider"),
                5000,
                [self, state, maybeFinish](bool ok, const QJsonValue& v, const QString&) {
                    if (!self)
                        return;
                    state->activeProvider = ok ? v.toString() : QString();
                    state->activeProviderRead = true;
                    maybeFinish();
                });
            self->invokePropertyGetSession(
                Targets::AgentSettings,
                QStringLiteral("activeModel"),
                5000,
                [self, state, maybeFinish](bool ok, const QJsonValue& v, const QString&) {
                    if (!self)
                        return;
                    state->activeModel = ok ? v.toString() : QString();
                    state->activeModelRead = true;
                    maybeFinish();
                });
        });
}

void WireSession::opModelsActive(const QString& reqId, const QJsonObject&) {
    QPointer<WireSession> self(this);
    QString rid = reqId;
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    invokePropertyGetSession(
        Targets::AgentSettings,
        QStringLiteral("activeProvider"),
        5000,
        [self, rid](bool ok, const QJsonValue& ap, const QString& err) {
            if (!self)
                return;
            if (!ok) {
                self->sendErr(rid, QStringLiteral("server_error"), err);
                return;
            }
            self->invokePropertyGetSession(
                Targets::AgentSettings,
                QStringLiteral("activeModel"),
                5000,
                [self, rid, ap](bool ok2, const QJsonValue& am, const QString& e2) {
                    if (!self)
                        return;
                    if (!ok2) {
                        self->sendErr(rid, QStringLiteral("server_error"), e2);
                        return;
                    }
                    self->sendOk(rid,
                                 QJsonObject{
                                     {QStringLiteral("provider"), ap},
                                     {QStringLiteral("model"), am},
                                 });
                });
        });
}

void WireSession::opModelsForProvider(const QString& reqId, const QJsonObject& params) {
    const QString p = params.value(QStringLiteral("provider_id")).toString();
    if (!V::nonEmptyBounded(p)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'provider_id' required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::AgentSettings,
                QStringLiteral("modelsForProvider"),
                {argStr(p)},
                QStringLiteral("QStringList"));
}

void WireSession::opModelsSetActive(const QString& reqId, const QJsonObject& params) {
    const QString p = params.value(QStringLiteral("provider_id")).toString();
    const QString m = params.value(QStringLiteral("model_name")).toString();
    if (!V::nonEmptyBounded(p) || !V::nonEmptyBounded(m)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'provider_id' and 'model_name' required"));
        return;
    }
    if (m_host)
        invokeFire(Targets::AgentSettings, QStringLiteral("setModel"), {argStr(p), argStr(m)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}


void WireSession::opSearchProviders(const QString& reqId, const QJsonObject&) {
    // Registry::providers() → [{id,displayName,requiresApiKey,hasKey,
    // active,baseUrl}]. hasKey is a boolean status only — never the key.
    asyncInvoke(reqId,
                Targets::WebSearchProviders,
                QStringLiteral("providers"),
                {},
                QStringLiteral("QVariantList"));
}

void WireSession::opSearchActive(const QString& reqId, const QJsonObject&) {
    asyncInvokeWrapId(reqId,
                      Targets::WebSearchProviders,
                      QStringLiteral("activeProviderId"),
                      {},
                      QStringLiteral("provider"));
}

void WireSession::opSearchSetActive(const QString& reqId, const QJsonObject& params) {
    const QString p = params.value(QStringLiteral("provider_id")).toString();
    if (!V::nonEmptyBounded(p)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'provider_id' required"));
        return;
    }
    if (m_host)
        invokeFire(Targets::WebSearchProviders, QStringLiteral("setActiveProvider"), {argStr(p)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}

// Catalogs (read-only) -------------------------------------------------------

void WireSession::opAgentList(const QString& reqId, const QJsonObject&) {
    asyncInvoke(reqId,
                Targets::AgentRegistry,
                QStringLiteral("agentList"),
                {},
                QStringLiteral("QVariantList"));
}
void WireSession::opAgentGet(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::AgentRegistry,
                QStringLiteral("agentInfo"),
                {argStr(id)},
                QStringLiteral("QVariantMap"));
}
void WireSession::opToolList(const QString& reqId, const QJsonObject&) {
    asyncInvoke(reqId,
                Targets::ToolService,
                QStringLiteral("registeredToolsList"),
                {},
                QStringLiteral("QVariantList"));
}
void WireSession::opMcpList(const QString& reqId, const QJsonObject&) {
    asyncInvoke(reqId,
                Targets::McpService,
                QStringLiteral("serverList"),
                {},
                QStringLiteral("QVariantList"));
}
void WireSession::opMcpServerTools(const QString& reqId, const QJsonObject& params) {
    const QString s = params.value(QStringLiteral("server_name")).toString();
    if (!V::nonEmptyBounded(s)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'server_name' required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::McpService,
                QStringLiteral("serverTools"),
                {argStr(s)},
                QStringLiteral("QVariantList"));
}
void WireSession::opSkillList(const QString& reqId, const QJsonObject&) {
    asyncInvoke(reqId,
                Targets::SkillService,
                QStringLiteral("installedSkills"),
                {},
                QStringLiteral("QVariantList"));
}
void WireSession::opSkillGet(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::nonEmptyBounded(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::SkillService,
                QStringLiteral("skillDetails"),
                {argStr(id)},
                QStringLiteral("QVariantMap"));
}

// Membership -----------------------------------------------------------------

void WireSession::opFolderMembers(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    if (!V::isUuidShape(fid)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("projectMembersList"),
                {argStr(fid)},
                QStringLiteral("QVariantList"));
}
void WireSession::opFolderMembersSet(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    const QJsonArray arr = params.value(QStringLiteral("members")).toArray();
    if (!V::isUuidShape(fid)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    QJsonArray ml;
    for (const QJsonValue& v : arr) {
        const QJsonObject m = v.toObject();
        const QString aid = m.contains(QStringLiteral("agent_id"))
                                ? m.value(QStringLiteral("agent_id")).toString()
                                : m.value(QStringLiteral("agentId")).toString();
        if (!V::isUuidShape(aid)) {
            sendErr(reqId,
                    QStringLiteral("invalid_params"),
                    QStringLiteral("member 'agent_id' must be UUID"));
            return;
        }
        QJsonObject row{
            {QStringLiteral("agentId"), aid},
            {QStringLiteral("alias"), m.value(QStringLiteral("alias")).toString()},
            {QStringLiteral("isCoordinator"),
             m.contains(QStringLiteral("is_coordinator"))
                 ? m.value(QStringLiteral("is_coordinator")).toBool()
                 : m.value(QStringLiteral("isCoordinator")).toBool()},
        };
        // setProjectMembersFromList replaces every row, so a per-member
        // override or provenance field a client does not send is lost.
        // Forward the ones the client does send; a client that sends none
        // gets the same result as before.
        if (!copyMemberOverrideFields(m, row)) {
            sendErr(reqId,
                    QStringLiteral("invalid_params"),
                    QStringLiteral("member override fields are malformed"));
            return;
        }
        ml.append(row);
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("setProjectMembersFromList"),
                {argStr(fid), argList(ml)},
                QStringLiteral("bool"));
}
void WireSession::opFolderMemberAdd(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    const QString aid = params.contains(QStringLiteral("agent_id"))
                            ? params.value(QStringLiteral("agent_id")).toString()
                            : params.value(QStringLiteral("agentId")).toString();
    const QString al = params.value(QStringLiteral("alias")).toString();
    const bool ic = params.contains(QStringLiteral("is_coordinator"))
                        ? params.value(QStringLiteral("is_coordinator")).toBool()
                        : params.value(QStringLiteral("isCoordinator")).toBool();
    if (!V::isUuidShape(fid) || !V::isUuidShape(aid) || !V::nonEmptyBounded(al)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("addProjectMemberMap"),
                {argMap(QJsonObject{
                    {QStringLiteral("folderId"), fid},
                    {QStringLiteral("agentId"), aid},
                    {QStringLiteral("alias"), al},
                    {QStringLiteral("isCoordinator"), ic},
                })},
                QStringLiteral("bool"));
}
/**
 * @brief Sets one project member's provider, model and tool allowlist,
 *        keyed by folder and alias, without replacing the rest of the
 *        roster.
 * @param reqId  Client request id.
 * @param params `folder_id` (UUID), `alias`, and optional
 *               `model_provider`, `model_name`, `allowed_tools`
 *               (camelCase also accepted). An absent or empty field
 *               clears that override.
 * Replies with a bool: true when a member with that alias was updated.
 */
void WireSession::opFolderMemberOverrideSet(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    const QString al = params.value(QStringLiteral("alias")).toString();
    if (!V::isUuidShape(fid) || !V::nonEmptyBounded(al)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'folder_id' must be UUID and 'alias' is required"));
        return;
    }
    QJsonObject fields;
    if (!copyMemberOverrideFields(params, fields)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("member override fields are malformed"));
        return;
    }
    QStringList tools;
    for (const QJsonValue& t : fields.value(QStringLiteral("allowedTools")).toArray())
        tools.append(t.toString());
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("updateProjectMemberModelOverride"),
                {argStr(fid),
                 argStr(al),
                 argStr(fields.value(QStringLiteral("modelProvider")).toString()),
                 argStr(fields.value(QStringLiteral("modelName")).toString()),
                 argStringList(tools)},
                QStringLiteral("bool"));
}
void WireSession::opFolderMemberRemove(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    const QString al = params.value(QStringLiteral("alias")).toString();
    if (!V::isUuidShape(fid) || !V::nonEmptyBounded(al)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("removeProjectMemberAlias"),
                {argStr(fid), argStr(al)},
                QStringLiteral("bool"));
}
void WireSession::opConvMembers(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(cid)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("conversationMembersList"),
                {argStr(cid)},
                QStringLiteral("QVariantList"));
}
void WireSession::opConvMembersSet(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    const QJsonArray arr = params.value(QStringLiteral("members")).toArray();
    if (!V::isUuidShape(cid)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    QJsonArray ml;
    for (const QJsonValue& v : arr) {
        const QJsonObject m = v.toObject();
        const QString aid = m.contains(QStringLiteral("agent_id"))
                                ? m.value(QStringLiteral("agent_id")).toString()
                                : m.value(QStringLiteral("agentId")).toString();
        if (!V::isUuidShape(aid)) {
            sendErr(reqId,
                    QStringLiteral("invalid_params"),
                    QStringLiteral("member 'agent_id' must be UUID"));
            return;
        }
        ml.append(QJsonObject{
            {QStringLiteral("agentId"), aid},
            {QStringLiteral("alias"), m.value(QStringLiteral("alias")).toString()},
            {QStringLiteral("isCoordinator"),
             m.contains(QStringLiteral("is_coordinator"))
                 ? m.value(QStringLiteral("is_coordinator")).toBool()
                 : m.value(QStringLiteral("isCoordinator")).toBool()},
        });
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("setConversationMembersFromList"),
                {argStr(cid), argList(ml)},
                QStringLiteral("bool"));
}
void WireSession::opConvMemberAdd(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    const QString aid = params.contains(QStringLiteral("agent_id"))
                            ? params.value(QStringLiteral("agent_id")).toString()
                            : params.value(QStringLiteral("agentId")).toString();
    const QString al = params.value(QStringLiteral("alias")).toString();
    const bool ic = params.contains(QStringLiteral("is_coordinator"))
                        ? params.value(QStringLiteral("is_coordinator")).toBool()
                        : params.value(QStringLiteral("isCoordinator")).toBool();
    if (!V::isUuidShape(cid) || !V::isUuidShape(aid) || !V::nonEmptyBounded(al)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("addConversationMemberMap"),
                {argMap(QJsonObject{
                    {QStringLiteral("conversationId"), cid},
                    {QStringLiteral("agentId"), aid},
                    {QStringLiteral("alias"), al},
                    {QStringLiteral("isCoordinator"), ic},
                })},
                QStringLiteral("bool"));
}
void WireSession::opConvMemberRemove(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    const QString al = params.value(QStringLiteral("alias")).toString();
    if (!V::isUuidShape(cid) || !V::nonEmptyBounded(al)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("removeConversationMemberAlias"),
                {argStr(cid), argStr(al)},
                QStringLiteral("bool"));
}
void WireSession::opConvCoordinatorSet(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    const QString al = params.value(QStringLiteral("alias")).toString();
    if (!V::isUuidShape(cid)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::MembershipService,
                QStringLiteral("setConversationCoordinatorAlias"),
                {argStr(cid), argStr(al)},
                QStringLiteral("bool"));
}

// Project documents + kickoff -----------------------------------------------

void WireSession::opFolderDocuments(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    if (!V::isUuidShape(fid)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("projectDocuments"),
                {argStr(fid)},
                QStringLiteral("QVariantList"));
}
void WireSession::opFolderDocumentUpload(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    const QString fn = params.value(QStringLiteral("file_name")).toString();
    const QString b64 = params.value(QStringLiteral("content_base64")).toString();
    if (!V::isUuidShape(fid)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    if (!V::isSafeFileName(fn)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad 'file_name'"));
        return;
    }
    if (b64.isEmpty()) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'content_base64' required"));
        return;
    }
    // Stage bytes to a wire-process temp file, then call existing host
    // Q_INVOKABLE addProjectDocument(folderId, sourcePath). The host
    // copies the file into its own location.
    const QString staged = stageBase64Upload(fn, b64);
    if (staged.isEmpty()) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral(
                    "the file could not be saved (invalid data, too large, or a write error)"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::Conversations,
                      QStringLiteral("addProjectDocument"),
                      {argStr(fid), argStr(staged)},
                      QStringLiteral("file_name"));
}
void WireSession::opFolderDocumentRemove(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    const QString fn = params.value(QStringLiteral("file_name")).toString();
    if (!V::isUuidShape(fid) || !V::nonEmptyBounded(fn)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("removeProjectDocument"),
                {argStr(fid), argStr(fn)},
                QStringLiteral("bool"));
}
void WireSession::opFolderKickoffIndividual(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    if (!V::isUuidShape(fid)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    // createIndividualChatsForProject returns count of chats created;
    // Android expects {created_count} per parseFolderConversations callsite.
    QPointer<WireSession> self(this);
    QString rid = reqId;
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    invokeAsyncSession(
        Targets::Conversations,
        QStringLiteral("createIndividualChatsForProject"),
        {argStr(fid)},
        QStringLiteral("int"),
        15000,
        [self, rid](bool ok, const QJsonValue& v, const QString& err) {
            if (!self)
                return;
            if (!ok) {
                self->sendErr(rid, QStringLiteral("server_error"), err);
                return;
            }
            self->sendOk(rid, QJsonObject{{QStringLiteral("created_count"), v.toInt()}});
        });
}
void WireSession::opFolderKickoffGroup(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    if (!V::isUuidShape(fid)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    asyncInvokeWrapId(
        reqId, Targets::Conversations, QStringLiteral("createGroupChatForProject"), {argStr(fid)});
}
void WireSession::opFolderMemberChatOpen(const QString& reqId, const QJsonObject& params) {
    const QString fid = params.value(QStringLiteral("folder_id")).toString();
    const QString aid = params.value(QStringLiteral("agent_id")).toString();
    const QString al = params.value(QStringLiteral("alias")).toString();
    if (!V::isUuidShape(fid) || !V::isUuidShape(aid) || !V::nonEmptyBounded(al)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::Conversations,
                      QStringLiteral("openDirectChatWithMember"),
                      {argStr(fid), argStr(aid), argStr(al)});
}

// Preferred skills -----------------------------------------------------------

void WireSession::opSkillPreferredList(const QString& reqId, const QJsonObject& params) {
    const QString st = params.value(QStringLiteral("scope_type")).toString();
    const QString si = params.value(QStringLiteral("scope_id")).toString();
    asyncInvoke(reqId,
                Targets::SkillService,
                QStringLiteral("preferredSkillsFor"),
                {argStr(st), argStr(si)},
                QStringLiteral("QStringList"));
}
void WireSession::opSkillPreferredSet(const QString& reqId, const QJsonObject& params) {
    const QString st = params.value(QStringLiteral("scope_type")).toString();
    const QString si = params.value(QStringLiteral("scope_id")).toString();
    QStringList ids;
    for (const QJsonValue& v : params.value(QStringLiteral("skill_ids")).toArray())
        ids.append(v.toString());
    asyncInvoke(reqId,
                Targets::SkillService,
                QStringLiteral("setPreferredSkills"),
                {argStr(st), argStr(si), argStringList(ids)},
                QStringLiteral("bool"));
}
void WireSession::opSkillExposeOnly(const QString& reqId, const QJsonObject& params) {
    const QString st = params.value(QStringLiteral("scope_type")).toString();
    const QString si = params.value(QStringLiteral("scope_id")).toString();
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    invokeAsyncSession(
        Targets::SkillService,
        QStringLiteral("exposeOnlyPreferred"),
        {argStr(st), argStr(si)},
        QStringLiteral("bool"),
        5000,
        [self, rid](bool ok, const QJsonValue& v, const QString& err) {
            if (!self)
                return;
            if (!ok) {
                self->sendErr(rid, QStringLiteral("server_error"), err);
                return;
            }
            self->sendOk(rid, QJsonObject{{QStringLiteral("expose_only"), v.toBool()}});
        });
}
void WireSession::opSkillSetExposeOnly(const QString& reqId, const QJsonObject& params) {
    const QString st = params.value(QStringLiteral("scope_type")).toString();
    const QString si = params.value(QStringLiteral("scope_id")).toString();
    const bool e = params.value(QStringLiteral("expose_only")).toBool();
    asyncInvoke(reqId,
                Targets::SkillService,
                QStringLiteral("setExposeOnlyPreferred"),
                {argStr(st), argStr(si), argBool(e)},
                QStringLiteral("bool"));
}
void WireSession::opSkillOverrideParent(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(cid)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    invokeAsyncSession(Targets::SkillService,
                       QStringLiteral("overrideParentFolder"),
                       {argStr(cid)},
                       QStringLiteral("bool"),
                       5000,
                       [self, rid](bool ok, const QJsonValue& v, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid, QJsonObject{{QStringLiteral("override"), v.toBool()}});
                       });
}
void WireSession::opSkillSetOverrideParent(const QString& reqId, const QJsonObject& params) {
    const QString cid = params.value(QStringLiteral("conv_id")).toString();
    const bool o = params.value(QStringLiteral("override")).toBool();
    if (!V::isUuidShape(cid)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::SkillService,
                QStringLiteral("setOverrideParentFolder"),
                {argStr(cid), argBool(o)},
                QStringLiteral("bool"));
}

// Heartbeats -----------------------------------------------------------------

void WireSession::opHbConfigsForFolder(const QString& reqId, const QJsonObject& params) {
    const QString f = params.value(QStringLiteral("folder_id")).toString();
    if (!V::isUuidShape(f)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::HeartbeatConfigService,
                QStringLiteral("configsForFolder"),
                {argStr(f)},
                QStringLiteral("QVariantList"));
}
void WireSession::opHbConfigsForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::HeartbeatConfigService,
                QStringLiteral("configsForConversation"),
                {argStr(c)},
                QStringLiteral("QVariantList"));
}
void WireSession::opHbConfigById(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::HeartbeatConfigService,
                QStringLiteral("configByIdMap"),
                {argStr(id)},
                QStringLiteral("QVariantMap"));
}
void WireSession::opHbUpsert(const QString& reqId, const QJsonObject& params) {
    QJsonObject f;
    if (params.contains(QStringLiteral("id")))
        f.insert(QStringLiteral("id"), params.value(QStringLiteral("id")));
    f.insert(QStringLiteral("agentId"),
             params.contains(QStringLiteral("agent_id")) ? params.value(QStringLiteral("agent_id"))
                                                         : params.value(QStringLiteral("agentId")));
    f.insert(QStringLiteral("scopeType"),
             params.contains(QStringLiteral("scope_type"))
                 ? params.value(QStringLiteral("scope_type"))
                 : params.value(QStringLiteral("scopeType")));
    f.insert(QStringLiteral("scopeId"),
             params.contains(QStringLiteral("scope_id")) ? params.value(QStringLiteral("scope_id"))
                                                         : params.value(QStringLiteral("scopeId")));
    f.insert(QStringLiteral("alias"), params.value(QStringLiteral("alias")));
    f.insert(QStringLiteral("enabled"), params.value(QStringLiteral("enabled")).toBool(true));
    f.insert(QStringLiteral("schedule"), params.value(QStringLiteral("schedule")));
    f.insert(QStringLiteral("goal"), params.value(QStringLiteral("goal")));
    f.insert(QStringLiteral("surfaceCriteria"),
             params.contains(QStringLiteral("surface_criteria"))
                 ? params.value(QStringLiteral("surface_criteria"))
                 : params.value(QStringLiteral("surfaceCriteria")));
    f.insert(QStringLiteral("maxRunsPerDay"),
             params.contains(QStringLiteral("max_runs_per_day"))
                 ? params.value(QStringLiteral("max_runs_per_day"))
                 : params.value(QStringLiteral("maxRunsPerDay")));
    f.insert(QStringLiteral("autoSurfaceTargetConversationId"),
             params.contains(QStringLiteral("auto_surface_target_conversation_id"))
                 ? params.value(QStringLiteral("auto_surface_target_conversation_id"))
                 : params.value(QStringLiteral("autoSurfaceTargetConversationId")));
    f.insert(QStringLiteral("selfConfigAllowed"),
             params.contains(QStringLiteral("self_config_allowed"))
                 ? params.value(QStringLiteral("self_config_allowed"))
                 : params.value(QStringLiteral("selfConfigAllowed")));
    asyncInvoke(reqId,
                Targets::HeartbeatConfigService,
                QStringLiteral("upsertConfigMap"),
                {argMap(f)},
                QStringLiteral("QString"));
}
void WireSession::opHbRemove(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::HeartbeatConfigService,
                QStringLiteral("removeConfig"),
                {argStr(id)},
                QStringLiteral("bool"));
}
void WireSession::opHbRunNow(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::HeartbeatSubagentService,
                QStringLiteral("runNow"),
                {argStr(id)},
                QStringLiteral("QString"));
}
void WireSession::opHbCancelRun(const QString& reqId, const QJsonObject& params) {
    const QString r = params.value(QStringLiteral("run_id")).toString();
    if (!V::nonEmptyBounded(r)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'run_id' required"));
        return;
    }
    if (m_host)
        invokeFire(Targets::HeartbeatSubagentService, QStringLiteral("cancelRun"), {argStr(r)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opHbRecentRuns(const QString& reqId, const QJsonObject& params) {
    int limit = params.value(QStringLiteral("limit")).toInt(50);
    limit = qBound(1, limit, 500);
    asyncInvoke(reqId,
                Targets::HeartbeatSubagentService,
                QStringLiteral("recentRunsList"),
                {argInt(limit)},
                QStringLiteral("QVariantList"));
}
void WireSession::opHbNextFires(const QString& reqId, const QJsonObject& params) {
    int limit = params.value(QStringLiteral("limit")).toInt(10);
    limit = qBound(1, limit, 100);
    asyncInvoke(reqId,
                Targets::HeartbeatSubagentService,
                QStringLiteral("nextFiresPreview"),
                {argInt(limit)},
                QStringLiteral("QVariantList"));
}
void WireSession::opHbConfigChanges(const QString& reqId, const QJsonObject& params) {
    int limit = params.value(QStringLiteral("limit")).toInt(100);
    limit = qBound(1, limit, 500);
    asyncInvoke(reqId,
                Targets::HeartbeatConfigService,
                QStringLiteral("recentConfigChanges"),
                {argInt(limit)},
                QStringLiteral("QVariantList"));
}
void WireSession::opHbConfigStatus(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::HeartbeatSubagentService,
                QStringLiteral("configStatus"),
                {argStr(id)},
                QStringLiteral("QVariantMap"));
}
void WireSession::opHbManualPost(const QString& reqId, const QJsonObject& params) {
    const QString r = params.value(QStringLiteral("report_id")).toString();
    const QString c = params.value(QStringLiteral("target_conv_id")).toString();
    if (!V::nonEmptyBounded(r) || !V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvoke(reqId,
                Targets::HeartbeatSubagentService,
                QStringLiteral("manualPost"),
                {argStr(r), argStr(c)},
                QStringLiteral("bool"));
}
void WireSession::opHbManualDismiss(const QString& reqId, const QJsonObject& params) {
    const QString r = params.value(QStringLiteral("report_id")).toString();
    if (!V::nonEmptyBounded(r)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'report_id' required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::HeartbeatSubagentService,
                QStringLiteral("manualDismiss"),
                {argStr(r)},
                QStringLiteral("bool"));
}
void WireSession::opHbCapForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    invokeAsyncSession(Targets::HeartbeatConfigService,
                       QStringLiteral("suggestedAutoSurfaceCap"),
                       {argStr(c)},
                       QStringLiteral("int"),
                       5000,
                       [self, rid](bool ok, const QJsonValue& v, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid, QJsonObject{{QStringLiteral("cap"), v.toInt()}});
                       });
}
void WireSession::opHbCapForFolder(const QString& reqId, const QJsonObject& params) {
    const QString f = params.value(QStringLiteral("folder_id")).toString();
    if (!V::isUuidShape(f)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    invokeAsyncSession(Targets::HeartbeatConfigService,
                       QStringLiteral("suggestedAutoSurfaceCapForFolder"),
                       {argStr(f)},
                       QStringLiteral("int"),
                       5000,
                       [self, rid](bool ok, const QJsonValue& v, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid, QJsonObject{{QStringLiteral("cap"), v.toInt()}});
                       });
}

// Per-conv settings ----------------------------------------------------------

// conv.settings.get aggregates per-conv DB fields with global host
// settings (agent pattern, require_confirmation, tools.enabled,
// rag.enabled) + per-conv heartbeat gate. Each non-DB field comes
// from the host via a property/Q_INVOKABLE chain so the Android
// SettingsScreen / ConvSettingsUi gets the full picture in one
// round-trip.
void WireSession::opConvSettingsGet(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    QJsonObject base = m_db->conversationSettings(c);

    if (!m_host || !m_host->isConnected()) {
        // Host bridge offline: return what we have from the DB. The
        // heartbeat fields are not in the DB read; we leave them out and
        // clients default them.
        sendOk(reqId, base);
        return;
    }

    QPointer<WireSession> self(this);
    QString rid = reqId;
    QString cid = c;
    // Chain: heartbeatAutoSurface → heartbeatAutoSurfaceMaxPerDay →
    // respond. toolsEnabled and ragEnabled are per conversation and live
    // in llm_config, so the DB read above already returns them.
    invokeAsyncSession(
        Targets::Conversations,
        QStringLiteral("heartbeatAutoSurface"),
        {argStr(c)},
        QStringLiteral("bool"),
        5000,
        [self, rid, cid, base](bool ok1, const QJsonValue& v1, const QString&) mutable {
            if (!self)
                return;
            base.insert(QStringLiteral("heartbeatAutoSurface"), ok1 ? v1.toBool() : false);
            self->invokeAsyncSession(
                Targets::Conversations,
                QStringLiteral("heartbeatAutoSurfaceMaxPerDay"),
                {argStr(cid)},
                QStringLiteral("int"),
                5000,
                [self, rid, base](bool ok2, const QJsonValue& v2, const QString&) mutable {
                    if (!self)
                        return;
                    base.insert(QStringLiteral("autoSurfaceMaxPerDay"), ok2 ? v2.toInt() : 1);
                    self->sendOk(rid, base);
                });
        });
}
void WireSession::opConvSettingsSave(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    QJsonObject cfg;
    if (params.contains(QStringLiteral("system_prompt")))
        cfg.insert(QStringLiteral("systemPrompt"), params.value(QStringLiteral("system_prompt")));
    if (params.contains(QStringLiteral("temperature")))
        cfg.insert(QStringLiteral("temperature"), params.value(QStringLiteral("temperature")));
    if (params.contains(QStringLiteral("max_tokens")))
        cfg.insert(QStringLiteral("maxTokens"), params.value(QStringLiteral("max_tokens")));
    if (params.contains(QStringLiteral("context_window")))
        cfg.insert(QStringLiteral("contextWindow"), params.value(QStringLiteral("context_window")));
    if (params.contains(QStringLiteral("streaming")))
        cfg.insert(QStringLiteral("streaming"), params.value(QStringLiteral("streaming")));
    if (params.contains(QStringLiteral("thinking")))
        cfg.insert(QStringLiteral("thinking"), params.value(QStringLiteral("thinking")));
    if (params.contains(QStringLiteral("top_k")))
        cfg.insert(QStringLiteral("topK"), params.value(QStringLiteral("top_k")));
    if (params.contains(QStringLiteral("top_p")))
        cfg.insert(QStringLiteral("topP"), params.value(QStringLiteral("top_p")));
    if (params.contains(QStringLiteral("repeat_penalty")))
        cfg.insert(QStringLiteral("repeatPenalty"), params.value(QStringLiteral("repeat_penalty")));
    if (params.contains(QStringLiteral("presence_penalty")))
        cfg.insert(QStringLiteral("presencePenalty"),
                   params.value(QStringLiteral("presence_penalty")));
    if (params.contains(QStringLiteral("frequency_penalty")))
        cfg.insert(QStringLiteral("frequencyPenalty"),
                   params.value(QStringLiteral("frequency_penalty")));
    if (params.contains(QStringLiteral("force_app_sampling")))
        cfg.insert(QStringLiteral("forceAppSampling"),
                   params.value(QStringLiteral("force_app_sampling")));
    if (params.contains(QStringLiteral("tools_in_system_prompt")))
        cfg.insert(QStringLiteral("toolsInSystemPrompt"),
                   params.value(QStringLiteral("tools_in_system_prompt")));
    if (params.contains(QStringLiteral("dynamic_compact_enabled")))
        cfg.insert(QStringLiteral("dynamicCompactEnabled"),
                   params.value(QStringLiteral("dynamic_compact_enabled")));
    if (params.contains(QStringLiteral("implicit_task_completion")))
        cfg.insert(QStringLiteral("implicitTaskCompletion"),
                   params.value(QStringLiteral("implicit_task_completion")));
    if (params.contains(QStringLiteral("compact_every_turns")))
        cfg.insert(QStringLiteral("compactEveryTurns"),
                   params.value(QStringLiteral("compact_every_turns")));
    if (params.contains(QStringLiteral("rag_enabled")))
        cfg.insert(QStringLiteral("ragEnabled"), params.value(QStringLiteral("rag_enabled")));
    if (params.contains(QStringLiteral("aim_enabled")))
        cfg.insert(QStringLiteral("aimEnabled"), params.value(QStringLiteral("aim_enabled")));
    if (params.contains(QStringLiteral("acn_enabled")))
        cfg.insert(QStringLiteral("acnEnabled"), params.value(QStringLiteral("acn_enabled")));
    if (params.contains(QStringLiteral("max_auto_rounds")))
        cfg.insert(QStringLiteral("maxAutoRounds"),
                   params.value(QStringLiteral("max_auto_rounds")));
    // Honest ack: a dropped settings save silently reverts on next
    // read — see requireHostOnline.
    if (!requireHostOnline(reqId))
        return;
    {
        invokeFire(Targets::ChatController, QStringLiteral("switchConversation"), {argStr(c)});
        invokeFire(Targets::AgentSettings, QStringLiteral("saveConversationConfig"), {argMap(cfg)});
    }
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opConvPrimaryAgent(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::TaskController,
                      QStringLiteral("primaryAgentId"),
                      {argStr(c)},
                      QStringLiteral("agent_id"));
}
void WireSession::opConvPrimaryAgentSet(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString a = params.value(QStringLiteral("agent_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (m_host)
        invokeFire(
            Targets::TaskController, QStringLiteral("setPrimaryAgent"), {argStr(c), argStr(a)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opConvHbGate(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    // Two reads in sequence — return both.
    QPointer<WireSession> self(this);
    QString rid = reqId;
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    invokeAsyncSession(
        Targets::Conversations,
        QStringLiteral("heartbeatAutoSurface"),
        {argStr(c)},
        QStringLiteral("bool"),
        5000,
        [self, rid, c](bool ok1, const QJsonValue& v1, const QString& e1) {
            if (!self)
                return;
            if (!ok1) {
                self->sendErr(rid, QStringLiteral("server_error"), e1);
                return;
            }
            const bool allow = v1.toBool();
            self->invokeAsyncSession(
                Targets::Conversations,
                QStringLiteral("heartbeatAutoSurfaceMaxPerDay"),
                {argStr(c)},
                QStringLiteral("int"),
                5000,
                [self, rid, allow](bool ok2, const QJsonValue& v2, const QString& e2) {
                    if (!self)
                        return;
                    if (!ok2) {
                        self->sendErr(rid, QStringLiteral("server_error"), e2);
                        return;
                    }
                    self->sendOk(rid,
                                 QJsonObject{
                                     {QStringLiteral("allow"), allow},
                                     {QStringLiteral("max_per_day"), v2.toInt()},
                                 });
                });
        });
}
void WireSession::opConvHbGateSet(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const bool a = params.value(QStringLiteral("allow")).toBool();
    int mpd = params.value(QStringLiteral("max_per_day")).toInt(1);
    mpd = qBound(1, mpd, 24);
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Conversations,
                QStringLiteral("setHeartbeatAutoSurface"),
                {argStr(c), argBool(a), argInt(mpd)},
                QStringLiteral("bool"));
}

void WireSession::opAgentPattern(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    const QJsonObject settings = m_db->conversationSettings(c);
    QString pattern = settings.value(QStringLiteral("agentPattern")).toString();
    if (pattern.isEmpty())
        pattern = QStringLiteral("direct");  // default for legacy rows missing the column
    sendOk(reqId, QJsonObject{{QStringLiteral("pattern"), pattern}});
}
void WireSession::opAgentPatternSet(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString p = params.value(QStringLiteral("pattern")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!V::nonEmptyBounded(p)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'pattern' required"));
        return;
    }
    if (m_host) {
        // Pre-flight switch on per-client ChatController so per-client
        // AgentSettings's m_activeConvId follows; idempotent if already
        // active (switchConversation early-returns on equal id).
        invokeFire(Targets::ChatController, QStringLiteral("switchConversation"), {argStr(c)});
        invokeFire(Targets::AgentSettings, QStringLiteral("setAgentPattern"), {argStr(p)});
    }
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opAgentRequireConfirm(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    const QJsonObject settings = m_db->conversationSettings(c);
    // requireConfirmation defaults to false on legacy rows missing the key.
    const bool require = settings.value(QStringLiteral("requireConfirmation")).toBool(false);
    sendOk(reqId, QJsonObject{{QStringLiteral("require"), require}});
}
void WireSession::opAgentRequireConfirmSet(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const bool r = params.value(QStringLiteral("require")).toBool();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (m_host) {
        invokeFire(Targets::ChatController, QStringLiteral("switchConversation"), {argStr(c)});
        invokeFire(Targets::AgentSettings, QStringLiteral("setRequireConfirmation"), {argBool(r)});
    }
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opToolsEnabled(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    const QJsonObject settings = m_db->conversationSettings(c);
    // toolsEnabled defaults to true on legacy rows (matches LlmConfig
    // struct default and the former global default).
    const bool enabled = settings.value(QStringLiteral("toolsEnabled")).toBool(true);
    sendOk(reqId, QJsonObject{{QStringLiteral("enabled"), enabled}});
}
void WireSession::opToolsEnabledSet(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const bool e = params.value(QStringLiteral("enabled")).toBool();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (m_host) {
        invokeFire(Targets::ChatController, QStringLiteral("switchConversation"), {argStr(c)});
        // toolsEnabled is a Q_PROPERTY on AgentSettings; the per-client
        // routing for property_set frames hits the per-client AS too.
        invokePropertySetSession(Targets::AgentSettings,
                                 QStringLiteral("toolsEnabled"),
                                 QStringLiteral("bool"),
                                 QJsonValue(e),
                                 5000,
                                 nullptr);
    }
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
// RAG enable is per-conversation (llm_config.rag_enabled): read it via
// conv.settings.get and write it via conv.settings.set (rag_enabled). The
// former global rag.enabled / rag.enabled.set ops were removed with the global
// RagService.enabled flag.

// Plans ----------------------------------------------------------------------

void WireSession::opPlanListForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->plansForConversation(c));
}
void WireSession::opPlanGet(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    const QJsonObject row = m_db->planById(id);
    if (row.isEmpty()) {
        sendErr(reqId, QStringLiteral("not_found"), QStringLiteral("no such plan"));
        return;
    }
    sendOk(reqId, row);
}
void WireSession::opTaskStart(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString g = params.value(QStringLiteral("goal")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!V::nonEmptyBounded(g)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'goal' required"));
        return;
    }
    if (m_host)
        invokeFire(Targets::TaskController,
                   QStringLiteral("userInitiatedStartTask"),
                   {argStr(c), argStr(g)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opPlanStop(const QString& reqId, const QJsonObject& params) {
    const QString p = params.value(QStringLiteral("plan_id")).toString();
    const QString r =
        params.value(QStringLiteral("reason")).toString(QStringLiteral("Stopped by user"));
    if (!V::isUuidShape(p)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'plan_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::TaskController,
                QStringLiteral("stopPlan"),
                {argStr(p), argStr(r)},
                QStringLiteral("bool"));
}
void WireSession::opPlanStopAllInConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    invokeAsyncSession(
        Targets::TaskController,
        QStringLiteral("stopAllActivePlansInConversation"),
        {argStr(c)},
        QStringLiteral("int"),
        5000,
        [self, rid](bool ok, const QJsonValue& v, const QString& err) {
            if (!self)
                return;
            if (!ok) {
                self->sendErr(rid, QStringLiteral("server_error"), err);
                return;
            }
            self->sendOk(rid, QJsonObject{{QStringLiteral("stopped_count"), v.toInt()}});
        });
}
void WireSession::opStepRetry(const QString& reqId, const QJsonObject& params) {
    const QString s = params.value(QStringLiteral("step_id")).toString();
    const QString n = params.value(QStringLiteral("notes")).toString();
    if (!V::isUuidShape(s)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'step_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::TaskController,
                QStringLiteral("retryStep"),
                {argStr(s), argStr(n)},
                QStringLiteral("bool"));
}
void WireSession::opStepOverrideDone(const QString& reqId, const QJsonObject& params) {
    const QString s = params.value(QStringLiteral("step_id")).toString();
    if (!V::isUuidShape(s)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'step_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::TaskController,
                QStringLiteral("overrideStepAsDone"),
                {argStr(s)},
                QStringLiteral("bool"));
}
void WireSession::opStepSkip(const QString& reqId, const QJsonObject& params) {
    const QString s = params.value(QStringLiteral("step_id")).toString();
    const QString r =
        params.value(QStringLiteral("reason")).toString(QStringLiteral("Skipped by user"));
    if (!V::isUuidShape(s)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'step_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::TaskController,
                QStringLiteral("skipStep"),
                {argStr(s), argStr(r)},
                QStringLiteral("bool"));
}

// Tool-call activity + agent confirmation -----------------------------------

void WireSession::opToolCallListForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->toolCallsForConversation(c));
}
void WireSession::opToolCallListForMessage(const QString& reqId, const QJsonObject& params) {
    const QString m = params.value(QStringLiteral("message_id")).toString();
    if (!V::isUuidShape(m)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'message_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->toolCallsForMessage(m));
}
void WireSession::opToolCallGet(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    const QJsonObject row = m_db->toolCallById(id);
    if (row.isEmpty()) {
        sendErr(reqId, QStringLiteral("not_found"), QStringLiteral("no such tool call"));
        return;
    }
    sendOk(reqId, row);
}
void WireSession::opToolCallListForTurn(const QString& reqId, const QJsonObject& params) {
    const QString turnId = params.value(QStringLiteral("message_id")).toString();
    const QString agentId = params.value(QStringLiteral("agent_id")).toString();
    if (!V::isUuidShape(turnId)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'message_id' must be UUID"));
        return;
    }
    if (!agentId.isEmpty() && !V::isUuidShape(agentId)) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'agent_id' must be UUID or empty"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->toolCallsForTurn(turnId, agentId));
}
void WireSession::opToolCallApprove(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("call_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'call_id' must be UUID"));
        return;
    }
    if (m_host)
        invokeFire(Targets::AgentService, QStringLiteral("approveToolCall"), {argStr(c)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opToolCallDeny(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("call_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'call_id' must be UUID"));
        return;
    }
    if (m_host)
        invokeFire(Targets::AgentService, QStringLiteral("denyToolCall"), {argStr(c)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}

// Attachments / artifacts / images / audio.
// Reads use the wire process's read-only DB connection (or direct file read).
// Writes use the existing host Q_INVOKABLE methods after staging bytes
// to a wire-process-owned temp file.

void WireSession::opAttachmentListForMsg(const QString& reqId, const QJsonObject& params) {
    const QString m = params.value(QStringLiteral("message_id")).toString();
    if (!V::isUuidShape(m)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'message_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->attachmentsForMessage(m));
}
void WireSession::opAttachmentGet(const QString& reqId, const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("id")).toString();
    if (!V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    const QJsonObject row = m_db->attachmentBytes(id);
    if (row.isEmpty()) {
        sendErr(
            reqId, QStringLiteral("not_found"), QStringLiteral("attachment missing or oversized"));
        return;
    }
    sendOk(reqId, row);
}
void WireSession::opArtifactListForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    sendOk(reqId, m_db->artifactsForConversation(c));
}
void WireSession::opArtifactDownload(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString fn = params.value(QStringLiteral("file_name")).toString();
    if (!V::isUuidShape(c) || !V::isSafeFileName(fn)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    if (!m_db || !m_db->isOpen()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral(
                    "The remote server could not open the Verzeta Studio database on the host."));
        return;
    }
    const QJsonObject row = m_db->artifactBytes(c, fn);
    if (row.isEmpty()) {
        sendErr(reqId, QStringLiteral("not_found"), QStringLiteral("no such artifact"));
        return;
    }
    sendOk(reqId, row);
}
// Image/audio list ops call host Q_INVOKABLEs to get the path list, then
// transform to the {file_name, total_bytes, exists} shape Android expects.

namespace {

QJsonArray pathsToFiles(const QStringList& paths) {
    QJsonArray out;
    for (const QString& p : paths) {
        QFileInfo fi(p);
        out.append(QJsonObject{
            {QStringLiteral("file_name"), fi.fileName()},
            {QStringLiteral("total_bytes"), fi.exists() ? fi.size() : qint64(-1)},
            {QStringLiteral("exists"), fi.exists()},
        });
    }
    return out;
}

QStringList jsonArrayToStringList(const QJsonValue& v) {
    QStringList out;
    if (!v.isArray())
        return out;
    const QJsonArray a = v.toArray();
    out.reserve(a.size());
    for (const QJsonValue& e : a)
        out.append(e.toString());
    return out;
}

}  // namespace

void WireSession::opImageListForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    invokeAsyncSession(Targets::ImageService,
                       QStringLiteral("conversationImages"),
                       {argStr(c)},
                       QStringLiteral("QStringList"),
                       5000,
                       [self, rid](bool ok, const QJsonValue& v, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid, pathsToFiles(jsonArrayToStringList(v)));
                       });
}
void WireSession::opAudioListForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    invokeAsyncSession(Targets::AudioService,
                       QStringLiteral("conversationAudio"),
                       {argStr(c)},
                       QStringLiteral("QStringList"),
                       5000,
                       [self, rid](bool ok, const QJsonValue& v, const QString& err) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->sendErr(rid, QStringLiteral("server_error"), err);
                               return;
                           }
                           self->sendOk(rid, pathsToFiles(jsonArrayToStringList(v)));
                       });
}

// Image/audio download takes (conv_id, file_name) — the wire-side fetches
// the absolute-path list from the existing host Q_INVOKABLE, finds the
// matching basename, then reads the file directly from disk.

// Image/audio download takes (conv_id, file_name) — fetch the path list
// from the existing host Q_INVOKABLE, find the matching basename, read
// the file directly from disk in the wire process.
void WireSession::opImageDownload(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString fn = params.value(QStringLiteral("file_name")).toString();
    if (!V::isUuidShape(c) || !V::isSafeFileName(fn)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    QString wanted = fn;
    invokeAsyncSession(
        Targets::ImageService,
        QStringLiteral("conversationImages"),
        {argStr(c)},
        QStringLiteral("QStringList"),
        5000,
        [self, rid, wanted](bool ok, const QJsonValue& v, const QString& err) {
            if (!self)
                return;
            if (!ok) {
                self->sendErr(rid, QStringLiteral("server_error"), err);
                return;
            }
            const QStringList paths = jsonArrayToStringList(v);
            for (const QString& p : paths) {
                QFileInfo fi(p);
                if (fi.fileName() != wanted)
                    continue;
                const QJsonObject row = readMediaFile(p);
                if (row.isEmpty()) {
                    self->sendErr(
                        rid,
                        QStringLiteral("not_found"),
                        QStringLiteral(
                            "the file is missing, too large, or outside the app data folder"));
                    return;
                }
                QJsonObject augmented = row;
                augmented.insert(QStringLiteral("total_bytes"),
                                 augmented.value(QStringLiteral("size_bytes")));
                augmented.insert(QStringLiteral("mime_type"), QString());
                augmented.insert(QStringLiteral("truncated"), false);
                self->sendOk(rid, augmented);
                return;
            }
            self->sendErr(rid,
                          QStringLiteral("not_found"),
                          QStringLiteral("no such file in conversation media"));
        });
}
void WireSession::opAudioDownload(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString fn = params.value(QStringLiteral("file_name")).toString();
    if (!V::isUuidShape(c) || !V::isSafeFileName(fn)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    if (!m_host || !m_host->isConnected()) {
        sendErr(reqId,
                QStringLiteral("server_error"),
                QStringLiteral("Verzeta Studio is not connected to the remote server. Check that "
                               "it is running on the host."));
        return;
    }
    QPointer<WireSession> self(this);
    QString rid = reqId;
    QString wanted = fn;
    invokeAsyncSession(
        Targets::AudioService,
        QStringLiteral("conversationAudio"),
        {argStr(c)},
        QStringLiteral("QStringList"),
        5000,
        [self, rid, wanted](bool ok, const QJsonValue& v, const QString& err) {
            if (!self)
                return;
            if (!ok) {
                self->sendErr(rid, QStringLiteral("server_error"), err);
                return;
            }
            const QStringList paths = jsonArrayToStringList(v);
            for (const QString& p : paths) {
                QFileInfo fi(p);
                if (fi.fileName() != wanted)
                    continue;
                const QJsonObject row = readMediaFile(p);
                if (row.isEmpty()) {
                    self->sendErr(
                        rid,
                        QStringLiteral("not_found"),
                        QStringLiteral(
                            "the file is missing, too large, or outside the app data folder"));
                    return;
                }
                QJsonObject augmented = row;
                augmented.insert(QStringLiteral("total_bytes"),
                                 augmented.value(QStringLiteral("size_bytes")));
                augmented.insert(QStringLiteral("mime_type"), QString());
                augmented.insert(QStringLiteral("truncated"), false);
                self->sendOk(rid, augmented);
                return;
            }
            self->sendErr(rid,
                          QStringLiteral("not_found"),
                          QStringLiteral("no such file in conversation media"));
        });
}

void WireSession::opMsgSendWithAttachments(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString text = params.value(QStringLiteral("text")).toString();
    const QJsonArray atts = params.value(QStringLiteral("attachments")).toArray();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (atts.isEmpty()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'attachments' required"));
        return;
    }

    QStringList paths;
    paths.reserve(atts.size());
    for (const QJsonValue& v : atts) {
        const QJsonObject o = v.toObject();
        const QString fn = o.value(QStringLiteral("filename")).toString();
        const QString b64 = o.value(QStringLiteral("content_base64")).toString();
        const QString staged = stageBase64Upload(fn, b64);
        if (staged.isEmpty()) {
            sendErr(reqId,
                    QStringLiteral("invalid_params"),
                    QStringLiteral("attachment '%1' could not be saved (invalid data, too large, "
                                   "or a write error)")
                        .arg(fn));
            return;
        }
        paths.append(staged);
    }

    if (m_host) {
        invokeFire(Targets::ChatController, QStringLiteral("switchConversation"), {argStr(c)});
        invokeFire(Targets::ChatController,
                   QStringLiteral("sendMessageWithAttachments"),
                   {argStr(text), argStringList(paths)});
    }
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}

// Canvas --------------------------------------------------------------------

void WireSession::opCanvasActiveForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Canvas,
                QStringLiteral("activeCanvasFor"),
                {argStr(c)},
                QStringLiteral("QVariantMap"));
}
void WireSession::opCanvasHistoryForConv(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Canvas,
                QStringLiteral("historyForConversation"),
                {argStr(c)},
                QStringLiteral("QVariantList"));
}
void WireSession::opCanvasDiskMirrorPath(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::Canvas,
                      QStringLiteral("diskMirrorPath"),
                      {argStr(c)},
                      QStringLiteral("path"));
}
void WireSession::opCanvasSuggestedExportName(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::Canvas,
                      QStringLiteral("suggestedExportName"),
                      {argStr(c)},
                      QStringLiteral("file_name"));
}
void WireSession::opCanvasReadSlice(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const int s = params.value(QStringLiteral("start_line")).toInt(1);
    const int e = params.value(QStringLiteral("end_line")).toInt(-1);
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (s <= 0) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'start_line' must be positive"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::Canvas,
                      QStringLiteral("readCanvasSlice"),
                      {argStr(c), argInt(s), argInt(e)},
                      QStringLiteral("content"));
}
void WireSession::opCanvasOpen(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString f = params.value(QStringLiteral("filename")).toString();
    const QString lang = params.value(QStringLiteral("language")).toString();
    const QString content = params.value(QStringLiteral("content")).toString();
    const QString src = params.value(QStringLiteral("source_msg_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (!V::isSafeFileName(f)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'filename' invalid"));
        return;
    }
    if (content.size() > kMaxContentBytes) {
        sendErr(reqId,
                QStringLiteral("payload_too_large"),
                QStringLiteral("content exceeds size limit"));
        return;
    }
    asyncInvokeWrapId(reqId,
                      Targets::Canvas,
                      QStringLiteral("openCanvas"),
                      {argStr(c), argStr(f), argStr(lang), argStr(content), argStr(src)},
                      QStringLiteral("canvas_id"));
}
void WireSession::opCanvasEdit(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString content = params.value(QStringLiteral("content")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (content.size() > kMaxContentBytes) {
        sendErr(reqId,
                QStringLiteral("payload_too_large"),
                QStringLiteral("content exceeds size limit"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Canvas,
                QStringLiteral("editCanvas"),
                {argStr(c), argStr(content)},
                QStringLiteral("bool"));
}
void WireSession::opCanvasClose(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(
        reqId, Targets::Canvas, QStringLiteral("closeCanvas"), {argStr(c)}, QStringLiteral("bool"));
}
void WireSession::opCanvasSwitchTo(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString id = params.value(QStringLiteral("canvas_id")).toString();
    if (!V::isUuidShape(c) || !V::isUuidShape(id)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Canvas,
                QStringLiteral("switchToCanvas"),
                {argStr(c), argStr(id)},
                QStringLiteral("bool"));
}
void WireSession::opCanvasToolsList(const QString& reqId, const QJsonObject& params) {
    const QString lang = params.value(QStringLiteral("language")).toString();
    asyncInvoke(reqId,
                Targets::Canvas,
                QStringLiteral("availableActionsForLanguage"),
                {argStr(lang)},
                QStringLiteral("QVariantList"));
}
void WireSession::opCanvasToolsRun(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const QString a = params.value(QStringLiteral("action_id")).toString();
    if (!V::isUuidShape(c) || !V::nonEmptyBounded(a)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("bad params"));
        return;
    }
    asyncInvoke(reqId,
                Targets::Canvas,
                QStringLiteral("performAction"),
                {argStr(c), argStr(a)},
                QStringLiteral("QVariantMap"));
}
void WireSession::opCanvasAiActionsList(const QString& reqId, const QJsonObject& params) {
    const QString lang = params.value(QStringLiteral("language")).toString();
    asyncInvoke(reqId,
                Targets::CanvasAiActions,
                QStringLiteral("availableForLanguage"),
                {argStr(lang)},
                QStringLiteral("QVariantList"));
}
void WireSession::opCanvasAiActionsTrigger(const QString& reqId, const QJsonObject& params) {
    const QString a = params.value(QStringLiteral("action_id")).toString();
    const QString s = params.value(QStringLiteral("submenu_choice")).toString();
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::nonEmptyBounded(a)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'action_id' required"));
        return;
    }
    if (!c.isEmpty() && !V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    // The action runs on this client's own CanvasAiActions, bound to its
    // wire ChatController. With a conv_id, point that ChatController at the
    // conversation first, the same way msg.send does, so the action reads
    // that conversation's canvas and posts into it.
    if (!c.isEmpty() && m_host)
        invokeFire(Targets::ChatController, QStringLiteral("switchConversation"), {argStr(c)});
    asyncInvoke(reqId,
                Targets::CanvasAiActions,
                QStringLiteral("trigger"),
                {argStr(a), argStr(s)},
                QStringLiteral("bool"));
}
void WireSession::opCanvasRunStart(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::CanvasRunner,
                QStringLiteral("runActiveCanvas"),
                {argStr(c)},
                QStringLiteral("bool"));
}
void WireSession::opCanvasRunCancel(const QString& reqId, const QJsonObject&) {
    if (m_host)
        invokeFire(Targets::CanvasRunner, QStringLiteral("cancel"), {});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opCanvasRunSendInput(const QString& reqId, const QJsonObject& params) {
    // A wire client answering an input() prompt. The prompt itself already
    // reaches the client: CanvasRunner runs Python with -u and flushes a
    // newline-less partial line after a short debounce, and the host forwards
    // each console row as a canvas.run.line event. Without this op the client
    // can see the question but never answer it.
    const QJsonValue tv = params.value(QStringLiteral("text"));
    if (!tv.isString()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'text' must be a string"));
        return;
    }
    const QString text = tv.toString();
    // Bound the write with the interactive cap, not the 16 MiB bulk-content
    // cap: this is a typed answer, and the far end is a sandboxed child's
    // stdin pipe. Measure the encoded size, since QString::size() counts
    // UTF-16 code units, not bytes on the wire.
    if (text.toUtf8().size() > kMaxStdinBytes) {
        sendErr(reqId,
                QStringLiteral("payload_too_large"),
                QStringLiteral("stdin write exceeds size limit"));
        return;
    }
    if (!requireHostOnline(reqId))
        return;
    invokeFire(Targets::CanvasRunner, QStringLiteral("sendInput"), {argStr(text)});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opCanvasRunEof(const QString& reqId, const QJsonObject&) {
    // Idempotent on the host (CanvasRunner::sendEof is a no-op when nothing
    // is running), so no is-running gate is needed here.
    if (!requireHostOnline(reqId))
        return;
    invokeFire(Targets::CanvasRunner, QStringLiteral("sendEof"), {});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}
void WireSession::opCanvasRunSendToIde(const QString& reqId, const QJsonObject& params) {
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::CanvasRunner,
                QStringLiteral("sendToIde"),
                {argStr(c)},
                QStringLiteral("bool"));
}
void WireSession::opCanvasRunIsRunning(const QString& reqId, const QJsonObject&) {
    asyncInvoke(
        reqId, Targets::CanvasRunner, QStringLiteral("isRunning"), {}, QStringLiteral("bool"));
}

void WireSession::opCanvasRunSandboxState(const QString& reqId, const QJsonObject&) {
    // CanvasRunner doesn't expose a sandboxAvailable Q_INVOKABLE — the
    // sandbox-availability check is internal to runActiveCanvas which
    // surfaces failures via canvas.error events. Wire reports
    // available=true unconditionally; clients that care about real
    // sandbox state should subscribe to canvas.error and observe the
    // outcome of canvas.run.start.
    sendOk(reqId,
           QJsonObject{
               {QStringLiteral("available"), true},
               {QStringLiteral("blocker_reason"), QString()},
           });
}
void WireSession::opCanvasRunSupportedLangs(const QString& reqId, const QJsonObject&) {
    asyncInvoke(reqId,
                Targets::CanvasRunner,
                QStringLiteral("supportedRunLanguages"),
                {},
                QStringLiteral("QStringList"));
}
void WireSession::opCanvasRunSupportedIdeLangs(const QString& reqId, const QJsonObject&) {
    asyncInvoke(reqId,
                Targets::CanvasRunner,
                QStringLiteral("supportedIdeLanguages"),
                {},
                QStringLiteral("QStringList"));
}
void WireSession::opCanvasConsoleClear(const QString& reqId, const QJsonObject&) {
    if (m_host)
        invokeFire(Targets::CanvasConsoleModel, QStringLiteral("clear"), {});
    sendOk(reqId, QJsonObject{{QStringLiteral("queued"), true}});
}

// ===========================================================================
// Host signal forwarding. The bridge forwards EVERY host signal it has wired
// (it doesn't filter per-session). This session selects what to send to its
// WS client based on per-session subscriptions + auth state.
// ===========================================================================

void WireSession::onHostSignal(const QString& name, const QJsonObject& args) {
    if (!isAuthenticated())
        return;

    auto withConvFilter = [&](const QString& evt, const QString& convKey) {
        const QString cid = args.value(convKey).toString();
        if (!m_subscribedConvIds.contains(cid))
            return;
        sendEvent(evt, args);
    };

    auto withMessageEnrich = [&](const QString& evt) {
        const QString cid = args.value(QStringLiteral("conv_id")).toString();
        if (!m_subscribedConvIds.contains(cid))
            return;
        QJsonObject enriched = args;
        if (m_db && m_db->isOpen()) {
            const QString mid = args.value(QStringLiteral("msg_id")).toString();
            const QJsonObject snapshot = m_db->messageById(mid);
            for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
                enriched.insert(it.key(), it.value());
            }
        }
        sendEvent(evt, enriched);
        // Artifact derivation: re-read the conv's artifact list and
        // emit artifact.added for the newest matching row that didn't
        // exist before (matches the desktop's onMessageInserted hook).
        if (m_db && m_db->isOpen()) {
            const QString role = enriched.value(QStringLiteral("role")).toString();
            const QString finish = enriched.value(QStringLiteral("finish_reason")).toString();
            const bool maybeArtifact =
                (role == QStringLiteral("tool")) ||
                (role == QStringLiteral("assistant") && finish == QStringLiteral("artifact"));
            if (maybeArtifact) {
                const QString mid = enriched.value(QStringLiteral("msg_id")).toString();
                for (const QJsonValue& v : m_db->artifactsForConversation(cid)) {
                    const QJsonObject row = v.toObject();
                    if (row.value(QStringLiteral("source_msg_id")).toString() == mid) {
                        QJsonObject ev = row;
                        ev.insert(QStringLiteral("conv_id"), cid);
                        sendEvent(QStringLiteral("artifact.added"), ev);
                        break;
                    }
                }
            }
        }
    };

    auto withToolCallEnrich = [&](const QString& evt) {
        const QString cid = args.value(QStringLiteral("conv_id")).toString();
        if (!m_subscribedConvIds.contains(cid))
            return;
        QJsonObject enriched = args;
        if (m_db && m_db->isOpen()) {
            const QString id = args.value(QStringLiteral("call_id")).toString();
            const QJsonObject snapshot = m_db->toolCallById(id);
            for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
                enriched.insert(it.key(), it.value());
            }
        }
        sendEvent(evt, enriched);
    };

    if (name == QString::fromLatin1(Signals::MessageAdded)) {
        withMessageEnrich(QStringLiteral("message.added"));
        return;
    }
    if (name == QString::fromLatin1(Signals::MessageUpdated)) {
        withMessageEnrich(QStringLiteral("message.updated"));
        return;
    }
    if (name == QString::fromLatin1(Signals::MessageDeleted)) {
        withConvFilter(QStringLiteral("message.deleted"), QStringLiteral("conv_id"));
        return;
    }
    if (name == QString::fromLatin1(Signals::MessageStreamingStarted)) {
        withConvFilter(QStringLiteral("message.streaming.started"), QStringLiteral("conv_id"));
        return;
    }
    if (name == QString::fromLatin1(Signals::MessageContentStreamed)) {
        withConvFilter(QStringLiteral("message.streaming.delta"), QStringLiteral("conv_id"));
        return;
    }
    if (name == QString::fromLatin1(Signals::MessageStreamingAborted)) {
        withConvFilter(QStringLiteral("message.streaming.aborted"), QStringLiteral("conv_id"));
        return;
    }
    if (name == QString::fromLatin1(Signals::ContextFillMeasured)) {
        withConvFilter(QStringLiteral("chat.context_fill.changed"), QStringLiteral("conv_id"));
        return;
    }
    if (name == QString::fromLatin1(Signals::UserMentioned)) {
        sendEvent(QStringLiteral("chat.user_mentioned"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::MessageContentRewritten)) {
        withMessageEnrich(QStringLiteral("message.updated"));
        return;
    }
    if (name == QString::fromLatin1(Signals::ToolCallAdded)) {
        withToolCallEnrich(QStringLiteral("tool_call.added"));
        return;
    }
    if (name == QString::fromLatin1(Signals::ToolCallUpdated)) {
        withToolCallEnrich(QStringLiteral("tool_call.updated"));
        return;
    }

    // List-level events (no per-conv filter — all auth'd clients see them).
    // Enrich conv/folder snapshot events with the full row from DB so
    // Android's parseConversationFromEvent / parseFolderFromObject have
    // every field they need.
    auto enrichConvEvent = [&](const QString& evt) {
        QJsonObject enriched = args;
        const QString cid = args.value(QStringLiteral("conv_id")).toString();
        if (m_db && m_db->isOpen() && !cid.isEmpty()) {
            const QJsonObject row = m_db->conversationById(cid);
            for (auto it = row.begin(); it != row.end(); ++it) {
                enriched.insert(it.key(), it.value());
            }
        }
        sendEvent(evt, enriched);
    };
    auto enrichFolderEvent = [&](const QString& evt) {
        QJsonObject enriched = args;
        const QString fid = args.value(QStringLiteral("folder_id")).toString();
        if (m_db && m_db->isOpen() && !fid.isEmpty()) {
            // listAllFolders returns all rows; pick the one matching fid.
            for (const QJsonValue& v : m_db->listAllFolders()) {
                const QJsonObject row = v.toObject();
                if (row.value(QStringLiteral("id")).toString() == fid) {
                    for (auto it = row.begin(); it != row.end(); ++it) {
                        enriched.insert(it.key(), it.value());
                    }
                    break;
                }
            }
        }
        sendEvent(evt, enriched);
    };

    if (name == QString::fromLatin1(Signals::ConversationCreated)) {
        enrichConvEvent(QStringLiteral("conv.added"));
        return;
    }
    if (name == QString::fromLatin1(Signals::ConversationUpdated)) {
        enrichConvEvent(QStringLiteral("conv.updated"));
        return;
    }
    if (name == QString::fromLatin1(Signals::ConversationDeleted)) {
        sendEvent(QStringLiteral("conv.deleted"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::ConversationRenamed)) {
        enrichConvEvent(QStringLiteral("conv.renamed"));
        return;
    }
    if (name == QString::fromLatin1(Signals::FolderCreated)) {
        enrichFolderEvent(QStringLiteral("folder.added"));
        return;
    }
    if (name == QString::fromLatin1(Signals::FolderUpdated)) {
        enrichFolderEvent(QStringLiteral("folder.updated"));
        return;
    }
    if (name == QString::fromLatin1(Signals::FolderDeleted)) {
        sendEvent(QStringLiteral("folder.deleted"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::ProjectMembersChanged)) {
        sendEvent(QStringLiteral("folder.members.changed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::ConversationMembersChanged)) {
        sendEvent(QStringLiteral("conv.members.changed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::HeartbeatConfigChanged)) {
        QJsonObject enriched = args;
        if (m_db && m_db->isOpen()) {
            const QString cid = args.value(QStringLiteral("config_id")).toString();
            const QJsonObject row = m_db->heartbeatConfigById(cid);
            for (auto it = row.begin(); it != row.end(); ++it) {
                enriched.insert(it.key(), it.value());
            }
        }
        sendEvent(QStringLiteral("heartbeat.config.changed"), enriched);
        return;
    }
    if (name == QString::fromLatin1(Signals::HeartbeatConfigRemoved)) {
        // Android reads `id` not `config_id` on this event.
        QJsonObject enriched = args;
        enriched.insert(QStringLiteral("id"), args.value(QStringLiteral("config_id")));
        sendEvent(QStringLiteral("heartbeat.config.removed"), enriched);
        return;
    }
    if (name == QString::fromLatin1(Signals::PlanCreated) ||
        name == QString::fromLatin1(Signals::PlanUpdated)) {
        QJsonObject enriched = args;
        if (m_db && m_db->isOpen()) {
            const QString pid = args.value(QStringLiteral("plan_id")).toString();
            const QJsonObject row = m_db->planById(pid);
            for (auto it = row.begin(); it != row.end(); ++it) {
                enriched.insert(it.key(), it.value());
            }
        }
        const QString evt = (name == QString::fromLatin1(Signals::PlanCreated))
                                ? QStringLiteral("plan.created")
                                : QStringLiteral("plan.updated");
        sendEvent(evt, enriched);
        return;
    }
    if (name == QString::fromLatin1(Signals::PlanDeleted)) {
        // Android reads `id` not `plan_id`.
        QJsonObject enriched = args;
        enriched.insert(QStringLiteral("id"), args.value(QStringLiteral("plan_id")));
        sendEvent(QStringLiteral("plan.deleted"), enriched);
        return;
    }
    if (name == QString::fromLatin1(Signals::StepUpdated)) {
        // Android needs plan_id to follow up with plan.get; the host
        // signal only carries step_id, so we resolve it here.
        QJsonObject enriched = args;
        if (m_db && m_db->isOpen()) {
            const QString sid = args.value(QStringLiteral("step_id")).toString();
            const QString pid = m_db->planIdForStep(sid);
            if (!pid.isEmpty())
                enriched.insert(QStringLiteral("plan_id"), pid);
        }
        sendEvent(QStringLiteral("step.updated"), enriched);
        return;
    }
    if (name == QString::fromLatin1(Signals::AgentStepStarted)) {
        sendEvent(QStringLiteral("agent.step.started"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::AgentStepCompleted)) {
        sendEvent(QStringLiteral("agent.step.completed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::AgentIsRunningChanged) ||
        name == QString::fromLatin1(Signals::AgentIterationChanged)) {
        sendEvent(QStringLiteral("agent.run.state"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::AgentToolCallRequested)) {
        sendEvent(QStringLiteral("tool_call.requested"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::AgentToolCallCompleted)) {
        sendEvent(QStringLiteral("tool_call.completed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::ImageGenerated)) {
        withConvFilter(QStringLiteral("image.generated"), QStringLiteral("conv_id"));
        return;
    }
    if (name == QString::fromLatin1(Signals::AudioGenerated)) {
        withConvFilter(QStringLiteral("audio.generated"), QStringLiteral("conv_id"));
        return;
    }
    if (name == QString::fromLatin1(Signals::CanvasOpened) ||
        name == QString::fromLatin1(Signals::CanvasUpdated)) {
        const QString evtName = (name == QString::fromLatin1(Signals::CanvasOpened))
                                    ? QStringLiteral("canvas.opened")
                                    : QStringLiteral("canvas.updated");
        const QString cid = args.value(QStringLiteral("conv_id")).toString();
        if (!m_subscribedConvIds.contains(cid))
            return;

        QJsonObject enriched = args;
        if (m_db && m_db->isOpen()) {
            const QString canvasId = args.value(QStringLiteral("canvas_id")).toString();
            const QJsonObject row = m_db->canvasArtifactById(canvasId);
            for (auto it = row.begin(); it != row.end(); ++it) {
                enriched.insert(it.key(), it.value());
            }
        }
        sendEvent(evtName, enriched);

        // The canvas-artifact row IS an artifact — also emit
        // `artifact.added` (canvas.opened only) with the ArtifactRowUi
        // shape so the Android ArtifactsScreen merges in the new row
        // in real time.
        if (name == QString::fromLatin1(Signals::CanvasOpened) && m_db && m_db->isOpen()) {
            for (const QJsonValue& v : m_db->artifactsForConversation(cid)) {
                const QJsonObject row = v.toObject();
                if (row.value(QStringLiteral("id")).toString() ==
                    args.value(QStringLiteral("canvas_id")).toString()) {
                    QJsonObject evt = row;
                    evt.insert(QStringLiteral("conv_id"), cid);
                    sendEvent(QStringLiteral("artifact.added"), evt);
                    break;
                }
            }
        }
        return;
    }
    if (name == QString::fromLatin1(Signals::CanvasClosed)) {
        withConvFilter(QStringLiteral("canvas.closed"), QStringLiteral("conv_id"));
        return;
    }
    if (name == QString::fromLatin1(Signals::CanvasErrorOccurred)) {
        sendEvent(QStringLiteral("canvas.error"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::CanvasConsoleLineAppended)) {
        sendEvent(QStringLiteral("canvas.run.line"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::CanvasRunningChanged)) {
        sendEvent(QStringLiteral("canvas.run.state"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::AgentPatternChanged)) {
        sendEvent(QStringLiteral("agent.pattern.changed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::RequireConfirmationChanged)) {
        sendEvent(QStringLiteral("agent.require_confirmation.changed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::ToolsEnabledChanged)) {
        sendEvent(QStringLiteral("tools.enabled.changed"), args);
        return;
    }
    // rag.enabled.changed removed: RAG enable is per-conversation, surfaced via
    // the active.conversation.settings.changed event (llm_config.rag_enabled).
    if (name == QString::fromLatin1(Signals::ActiveProviderChanged)) {
        sendEvent(QStringLiteral("models.active_changed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::ModelsRefreshed)) {
        sendEvent(QStringLiteral("models.refreshed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::ActiveConversationSettingsChanged)) {
        sendEvent(QStringLiteral("conv.settings.changed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::UserMessageQueued)) {
        const QString cid = args.value(QStringLiteral("client_id")).toString();
        if (cid == m_clientId) {
            sendEvent(QStringLiteral("user.message.queued"),
                      QJsonObject{
                          {QStringLiteral("text"), args.value(QStringLiteral("text"))},
                      });
        }
        return;
    }

    if (name == QString::fromLatin1(Signals::PollCreated)) {
        sendEvent(QStringLiteral("poll.created"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::PollUpdated)) {
        sendEvent(QStringLiteral("poll.updated"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::PollClosed)) {
        sendEvent(QStringLiteral("poll.closed"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::VoteCast)) {
        sendEvent(QStringLiteral("poll.vote_cast"), args);
        return;
    }

    if (name == QString::fromLatin1(Signals::ProjectTemplateProjectCreated)) {
        sendEvent(QStringLiteral("project_template.project_created"), args);
        return;
    }
    if (name == QString::fromLatin1(Signals::ProjectTemplateCatalogChanged)) {
        sendEvent(QStringLiteral("project_template.catalog_changed"), args);
        return;
    }
}


void WireSession::opPollList(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString c = params.value(QStringLiteral("conv_id")).toString();
    const bool openOnly = params.value(QStringLiteral("open_only")).toBool(true);
    if (!V::isUuidShape(c)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::PollService,
                QStringLiteral("pollsForConversation"),
                {argStr(c), argBool(openOnly)},
                QStringLiteral("QVariantList"));
}

void WireSession::opPollResults(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString p = params.value(QStringLiteral("poll_id")).toString();
    if (!V::isUuidShape(p)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'poll_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::PollService,
                QStringLiteral("pollResults"),
                {argStr(p)},
                QStringLiteral("QVariantMap"));
}

void WireSession::opPollStart(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString convId = params.value(QStringLiteral("conv_id")).toString();
    const QString question = params.value(QStringLiteral("question")).toString();
    const QJsonArray optsArr = params.value(QStringLiteral("options")).toArray();
    const QString mode = params.value(QStringLiteral("mode")).toString(QStringLiteral("single"));
    const int closesInMinutes = params.value(QStringLiteral("closes_in_minutes")).toInt(0);
    if (!V::isUuidShape(convId)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    if (question.trimmed().isEmpty()) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'question' must be non-empty"));
        return;
    }
    if (optsArr.size() < 2 || optsArr.size() > 10) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("'options' must be 2..10 strings"));
        return;
    }
    QStringList opts;
    for (const QJsonValue& v : optsArr)
        opts.append(v.toString());

    // User-initiated wire calls use "user" / "user" identity (the
    // Android client never poses as an agent — agent-initiated polls
    // come from the start_poll tool path through ChatController).
    asyncInvokeWrapId(reqId,
                      Targets::PollService,
                      QStringLiteral("createPoll"),
                      {argStr(convId),
                       argStr(QStringLiteral("user")),
                       argStr(QStringLiteral("user")),
                       argStr(question),
                       argStringList(opts),
                       argStr(mode),
                       argInt(closesInMinutes)},
                      QStringLiteral("poll_id"));
}

void WireSession::opPollVote(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString pollId = params.value(QStringLiteral("poll_id")).toString();
    const QString optionId = params.value(QStringLiteral("option_id")).toString();
    const QString optionText = params.value(QStringLiteral("option_text")).toString();
    if (!V::isUuidShape(pollId)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'poll_id' must be UUID"));
        return;
    }
    if (optionId.isEmpty() && optionText.isEmpty()) {
        sendErr(reqId,
                QStringLiteral("invalid_params"),
                QStringLiteral("either 'option_id' or 'option_text' required"));
        return;
    }
    // Dispatch to the right Q_INVOKABLE wrapper. PollService::castVote
    // (the 5-arg form) is not Q_INVOKABLE; the two thin wrappers are.
    // Identity is hardcoded to "user" / "user" — agent-initiated votes
    // come from the cast_vote tool, not the wire surface.
    if (!optionId.isEmpty()) {
        asyncInvoke(reqId,
                    Targets::PollService,
                    QStringLiteral("castVoteByOptionId"),
                    {argStr(pollId),
                     argStr(QStringLiteral("user")),
                     argStr(QStringLiteral("user")),
                     argStr(optionId)},
                    QStringLiteral("bool"));
    } else {
        asyncInvoke(reqId,
                    Targets::PollService,
                    QStringLiteral("castVoteByOptionText"),
                    {argStr(pollId),
                     argStr(QStringLiteral("user")),
                     argStr(QStringLiteral("user")),
                     argStr(optionText)},
                    QStringLiteral("bool"));
    }
}

void WireSession::opPollClose(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString p = params.value(QStringLiteral("poll_id")).toString();
    if (!V::isUuidShape(p)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'poll_id' must be UUID"));
        return;
    }
    asyncInvoke(reqId,
                Targets::PollService,
                QStringLiteral("closePoll"),
                {argStr(p)},
                QStringLiteral("bool"));
}


void WireSession::opActivityForProject(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString folderId = params.value(QStringLiteral("folder_id")).toString();
    if (!V::isUuidShape(folderId)) {
        sendErr(
            reqId, QStringLiteral("invalid_params"), QStringLiteral("'folder_id' must be UUID"));
        return;
    }
    const int limit = params.value(QStringLiteral("limit")).toInt(100);
    asyncInvoke(reqId,
                Targets::AuditService,
                QStringLiteral("recentActivityForProject"),
                {argStr(folderId), argInt(limit)},
                QStringLiteral("QVariantList"));
}

void WireSession::opActivityForConversation(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString convId = params.value(QStringLiteral("conv_id")).toString();
    if (!V::isUuidShape(convId)) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'conv_id' must be UUID"));
        return;
    }
    const int limit = params.value(QStringLiteral("limit")).toInt(100);
    asyncInvoke(reqId,
                Targets::AuditService,
                QStringLiteral("recentActivityForConversation"),
                {argStr(convId), argInt(limit)},
                QStringLiteral("QVariantList"));
}

void WireSession::opActivityByTurn(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString turnId = params.value(QStringLiteral("turn_id")).toString();
    if (turnId.isEmpty()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'turn_id' is required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::AuditService,
                QStringLiteral("recentActivityByTurn"),
                {argStr(turnId)},
                QStringLiteral("QVariantList"));
}


void WireSession::opProjectTemplateList(const QString& reqId, const QJsonObject&) {
    if (!requireAuth(reqId))
        return;
    asyncInvoke(reqId,
                Targets::ProjectTemplateService,
                QStringLiteral("templates"),
                {},
                QStringLiteral("QVariantList"));
}

void WireSession::opProjectTemplateListLanding(const QString& reqId, const QJsonObject&) {
    if (!requireAuth(reqId))
        return;
    asyncInvoke(reqId,
                Targets::ProjectTemplateService,
                QStringLiteral("landingTemplates"),
                {},
                QStringLiteral("QVariantList"));
}

void WireSession::opProjectTemplateGet(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString id = params.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' is required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::ProjectTemplateService,
                QStringLiteral("templateById"),
                {argStr(id)},
                QStringLiteral("QVariantMap"));
}

void WireSession::opProjectTemplateRoster(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString id = params.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' is required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::ProjectTemplateService,
                QStringLiteral("templateRoster"),
                {argStr(id)},
                QStringLiteral("QVariantList"));
}

void WireSession::opProjectTemplateCreateProject(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString id = params.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' is required"));
        return;
    }
    const QJsonObject customisations = params.value(QStringLiteral("customisations")).toObject();
    // Wrap the returned folderId so Android reads `data.folder_id` (matches
    // the conv.create / group.create response shape it already parses).
    asyncInvokeWrapId(reqId,
                      Targets::ProjectTemplateService,
                      QStringLiteral("createProjectFromTemplate"),
                      {argStr(id), argMap(customisations)},
                      QStringLiteral("folder_id"));
}

void WireSession::opProjectTemplatePinUser(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString id = params.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' is required"));
        return;
    }
    const bool pinned = params.value(QStringLiteral("pinned")).toBool(true);
    asyncInvoke(reqId,
                Targets::ProjectTemplateService,
                QStringLiteral("setTemplatePinned"),
                {argStr(id), argBool(pinned)},
                QStringLiteral("bool"));
}

void WireSession::opProjectTemplateSaveAsNew(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    // sourceTemplateId is OPTIONAL — empty means "start from neutral
    // defaults". ProjectTemplateService::saveAsNewTemplate accepts empty.
    const QString sourceId = params.value(QStringLiteral("source_template_id")).toString();
    const QJsonObject edits = params.value(QStringLiteral("edits")).toObject();
    // Wrap as `template_id` so the Android response shape matches
    // create_project's `folder_id` envelope.
    asyncInvokeWrapId(reqId,
                      Targets::ProjectTemplateService,
                      QStringLiteral("saveAsNewTemplate"),
                      {argStr(sourceId), argMap(edits)},
                      QStringLiteral("template_id"));
}

void WireSession::opProjectTemplateDeleteUser(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString id = params.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        sendErr(reqId, QStringLiteral("invalid_params"), QStringLiteral("'id' is required"));
        return;
    }
    asyncInvoke(reqId,
                Targets::ProjectTemplateService,
                QStringLiteral("deleteUserTemplate"),
                {argStr(id)},
                QStringLiteral("bool"));
}

// ===========================================================================
// Workspace mount registry — 5 ops mirror FolderMountRegistry's Q_INVOKABLE
// surface. Each op forwards its params to the process-wide registry through
// the host bridge. The registry returns a QVariantMap of shape
// `{ok:bool, error?:string}` (read ops return the full row map on hit, empty
// map on miss). The wire layer wraps it as `data` via the standard sendOk
// path; the client inspects `data.ok` to differentiate operation success
// from operation failure.
//
// Parameter validation (UUID shape, tier enum) lives in the registry so the
// definition of "what counts as a parameter error" stays in one place. The
// wire ops here are intentionally thin glue.
// ===========================================================================

void WireSession::opWorkspaceMountRegister(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString folderId = params.value(QStringLiteral("folder_id")).toString();
    const QString mountId = params.value(QStringLiteral("mount_id")).toString();
    const QString ownerLabel = params.value(QStringLiteral("owner_label")).toString();
    const QString treeJson = params.value(QStringLiteral("tree_json")).toString();
    const QString blocklistJson =
        params.value(QStringLiteral("blocklist_json")).toString(QStringLiteral("[]"));
    const QString allowlistJson =
        params.value(QStringLiteral("allowlist_json")).toString(QStringLiteral("[]"));
    const QString permissionTier =
        params.value(QStringLiteral("permission_tier")).toString(QStringLiteral("ask"));
    const QString optionsJson =
        params.value(QStringLiteral("options_json")).toString(QStringLiteral("{}"));
    // client_id is forced to the session's authenticated identity —
    // foreign-clientId smuggling at the parameter level is rejected
    // outright at the registry layer, but pinning it here means the
    // bridge dispatch carries the right tag for the per-client audit
    // trail without trusting the wire payload.
    const QString clientId = m_clientId;
    asyncInvoke(reqId,
                Targets::FolderMountRegistry,
                QStringLiteral("registerMount"),
                {argStr(folderId),
                 argStr(mountId),
                 argStr(clientId),
                 argStr(ownerLabel),
                 argStr(treeJson),
                 argStr(blocklistJson),
                 argStr(allowlistJson),
                 argStr(permissionTier),
                 argStr(optionsJson)},
                QStringLiteral("QVariantMap"));
}

void WireSession::opWorkspaceMountUnregister(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString folderId = params.value(QStringLiteral("folder_id")).toString();
    asyncInvoke(reqId,
                Targets::FolderMountRegistry,
                QStringLiteral("unregisterMount"),
                {argStr(folderId), argStr(m_clientId)},
                QStringLiteral("QVariantMap"));
}

void WireSession::opWorkspaceMountUpdateTree(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString folderId = params.value(QStringLiteral("folder_id")).toString();
    const QString treeJson = params.value(QStringLiteral("tree_json")).toString();
    asyncInvoke(reqId,
                Targets::FolderMountRegistry,
                QStringLiteral("updateTree"),
                {argStr(folderId), argStr(m_clientId), argStr(treeJson)},
                QStringLiteral("QVariantMap"));
}

void WireSession::opWorkspaceMountUpdateTier(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString folderId = params.value(QStringLiteral("folder_id")).toString();
    const QString newTier = params.value(QStringLiteral("permission_tier")).toString();
    asyncInvoke(reqId,
                Targets::FolderMountRegistry,
                QStringLiteral("updateTier"),
                {argStr(folderId), argStr(m_clientId), argStr(newTier)},
                QStringLiteral("QVariantMap"));
}

void WireSession::opWorkspaceMountForFolder(const QString& reqId, const QJsonObject& params) {
    if (!requireAuth(reqId))
        return;
    const QString folderId = params.value(QStringLiteral("folder_id")).toString();
    asyncInvoke(reqId,
                Targets::FolderMountRegistry,
                QStringLiteral("mountFor"),
                {argStr(folderId)},
                QStringLiteral("QVariantMap"));
}

}  // namespace Verzeta::Remote

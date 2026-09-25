// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-host-bridge.cpp
 * @brief Implementation of the Verzeta Remote host-side bridge.
 *
 *        The only piece of remote-related code that runs inside the
 *        verzeta-studio binary. Lives on its own QThread, talks to
 *        the separate verzeta-remote binary over QLocalSocket, and
 *        uses ONLY existing host signals + existing host
 *        Q_INVOKABLEs via Qt's standard cross-thread signal/slot
 *        machinery.
 * @layer Service (presentation; remote-access bridge).
 * @dependencies Qt6::Core, Qt6::Network (QLocalServer / QLocalSocket).
 */

#include "wire-host-bridge.h"

#if defined(Q_OS_LINUX)
#include <signal.h>
#include <sys/prctl.h>
#endif

#include "../api/llm-interface.h"
#include "../models/canvas-console-model.h"
#include "../models/conversation.h"
#include "../models/message.h"
#include "../models/tool-call.h"
#include "../services/agent-registry.h"
#include "../services/agent-service.h"
#include "../services/agent-settings-controller.h"
#include "../services/audio-service.h"
#include "../services/audit-service.h"
#include "../services/canvas-ai-actions.h"
#include "../services/canvas-runner.h"
#include "../services/canvas-service.h"
#include "../services/chat-controller.h"
#include "../services/conversation-controller.h"
#include "../services/conversation-service.h"
#include "../services/file-service.h"
#include "../services/folder-mount-registry.h"
#include "../services/heartbeat-config-service.h"
#include "../services/heartbeat-subagent-service.h"
#include "../services/image-service.h"
#include "../services/mcp-service.h"
#include "../services/membership-service.h"
#include "../services/message-service.h"
#include "../services/plan-service.h"
#include "../services/poll-service.h"
#include "../services/project-template-service.h"
#include "../services/rag-service.h"
#include "../services/search/web-search-provider-registry.h"
#include "../services/session/session-router.h"
#include "../services/settings-service.h"
#include "../services/skill-service.h"
#include "../services/task-controller.h"
#include "../services/tool-service.h"
#include "server/wire-auth.h"
#include "wire-protocol.h"

#include <QThread>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMetaType>
#include <QNetworkInterface>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QSysInfo>
#include <QVariantList>
#include <QVariantMap>

Q_LOGGING_CATEGORY(verzetaRemote, "verzeta.remote", QtInfoMsg)

namespace Verzeta::Remote {

namespace {

/**
 * @brief Splits a length-prefixed JSON stream into discrete frames.
 *
 * The stream format is `(4-byte big-endian length)(JSON payload)`
 * repeated. Partial frames remain buffered until enough bytes arrive.
 */
class FrameReader {
  public:
    /**
     * @brief Appends bytes from the socket and drains every complete
     *        frame.
     * @param chunk      Newly-read bytes to append.
     * @param emitFrame  Callable invoked once per complete frame with
     *                   the decoded `QJsonObject`.
     * @returns True to keep the connection alive; false on oversized
     *          frames (the caller drops the connection).
     */
    template <typename Emit> bool feed(const QByteArray& chunk, Emit emitFrame) {
        m_buf.append(chunk);
        while (true) {
            if (m_buf.size() < 4)
                return true;
            const quint32 len =
                (static_cast<quint8>(m_buf[0]) << 24) | (static_cast<quint8>(m_buf[1]) << 16) |
                (static_cast<quint8>(m_buf[2]) << 8) | static_cast<quint8>(m_buf[3]);
            if (len > kMaxWireFrameBytes) {
                qCWarning(verzetaRemote)
                    << "FrameReader: oversized frame" << len << "exceeds cap" << kMaxWireFrameBytes;
                return false;
            }
            if (m_buf.size() < static_cast<int>(4 + len))
                return true;
            const QByteArray payload = m_buf.mid(4, len);
            m_buf.remove(0, 4 + len);
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                qCWarning(verzetaRemote)
                    << "FrameReader: malformed JSON frame:" << err.errorString();
                continue;  // skip this frame; keep the connection
            }
            emitFrame(doc.object());
        }
    }

    /**
     * @brief Drops any buffered partial frame.
     */
    void clear() { m_buf.clear(); }

  private:
    QByteArray m_buf;
};

/** Encode a JSON object to the length-prefixed wire format. */
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

// ---------------------------------------------------------------------------
// HostBridgePeer — the QObject that lives on the bridge thread. Owns the
// QLocalServer + the QLocalSocket. Subscribes to host signals.
// Forwards them. Receives invoke commands and dispatches via existing
// host Q_INVOKABLEs.
// ---------------------------------------------------------------------------

/**
 * @brief QObject that lives on the dedicated bridge thread. Owns the
 *        QLocalServer + the QLocalSocket, subscribes to host signals,
 *        forwards every frame, and dispatches inbound invoke commands
 *        to existing host Q_INVOKABLEs.
 */
class HostBridgePeer : public QObject {
    Q_OBJECT
    friend class WireHostBridge;

  public:
    /**
     * @brief Constructs the peer with the bundle of non-owning service
     *        pointers shared with the bridge.
     * @param services  Bundle of host service references.
     * @param parent    Optional Qt parent.
     */
    explicit HostBridgePeer(WireHostBridge::Services services, QObject* parent = nullptr)
        : QObject(parent), m_services(std::move(services)) {}

    /**
     * @brief Returns the QLocalSocket path the server is bound to.
     * @returns Absolute socket path string.
     */
    QString listenSocketName() const { return m_socketName; }

  signals:
    /**
     * @brief Emitted on the bridge thread once the QLocalServer is
     *        actively listening and the host-signal subscriptions are
     *        wired.
     *
     * The WireHostBridge ctor connects this to its auto-start fire slot
     * via Qt::QueuedConnection so the spawned verzeta-remote child
     * process can never dial in before the QLocalServer is ready to
     * accept.
     */
    void bridgeReady();

  public slots:
    /**
     * @brief Sets up the QLocalServer and signal subscriptions on the
     *        bridge thread.
     *
     * Called via QueuedConnection from the WireHostBridge ctor.
     */
    void onStart() {
        m_socketName = hostBridgeSocketName();
        // Best-effort cleanup of stale socket from a prior crashed run.
        QLocalServer::removeServer(m_socketName);

        m_server = new QLocalServer(this);
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        if (!m_server->listen(m_socketName)) {
            qCCritical(verzetaRemote) << "HostBridgePeer: cannot listen on" << m_socketName << ":"
                                      << m_server->errorString();
            return;
        }
        qCInfo(verzetaRemote) << "HostBridgePeer: listening at" << m_server->fullServerName();

        QObject::connect(
            m_server, &QLocalServer::newConnection, this, &HostBridgePeer::onNewConnection);

        m_clientRpcSweepTimer = new QTimer(this);
        m_clientRpcSweepTimer->setInterval(WireHostBridge::kBridgeRpcSweepIntervalMs);
        QObject::connect(
            m_clientRpcSweepTimer, &QTimer::timeout, this, &HostBridgePeer::sweepClientRpcTimeouts);
        m_clientRpcSweepTimer->start();

        wireHostSignals();

        // Notify WireHostBridge that the bridge is fully ready
        // (listening + signal subscriptions wired). Auto-start can
        // now safely spawn verzeta-remote.
        // Qt signal emission, not a function call.
        emit bridgeReady();
    }

    /**
     * @brief Tears down the QLocalServer and socket on the bridge
     *        thread.
     *
     * Called via QueuedConnection from the WireHostBridge dtor.
     */
    void onStop() {
        drainPendingClientRpc(QStringLiteral("bridge_destroyed"),
                              QStringLiteral("bridge shutting down"));
        if (m_clientRpcSweepTimer)
            m_clientRpcSweepTimer->stop();

        if (m_socket) {
            // Detach our slot subscriptions and null m_socket BEFORE
            // disconnectFromServer(). On a still-connected socket
            // disconnectFromServer() can emit `disconnected`
            // SYNCHRONOUSLY (sender + receiver both on this thread →
            // Qt::DirectConnection), re-entering onSocketDisconnected,
            // which deletes m_socket and nulls it; control then returns
            // here and `m_socket->deleteLater()` dereferences a null
            // pointer (segfault). This mirrors the identical detach +
            // null-first guard in onNewConnection's supersede path. It
            // is why the clean-exit teardown was crashing once the
            // bridge thread was stopped before the child process.
            QLocalSocket* sock = m_socket;
            m_socket = nullptr;
            m_reader.clear();
            QObject::disconnect(sock, nullptr, this, nullptr);
            sock->disconnectFromServer();
            sock->deleteLater();
        }
        if (m_server) {
            m_server->close();
            m_server->deleteLater();
            m_server = nullptr;
        }
    }

  private slots:
    /**
     * @brief QLocalServer::newConnection handler that accepts an incoming
     *        verzeta-remote daemon and wires its socket.
     */
    void onNewConnection() {
        while (m_server->hasPendingConnections()) {
            QLocalSocket* sock = m_server->nextPendingConnection();
            if (!sock)
                break;
            if (m_socket) {
                // Only one remote process at a time. New connection wins.

                drainPendingClientRpc(QStringLiteral("bridge_disconnected"),
                                      QStringLiteral("IPC peer superseded by reconnect"));

                // Detach our slot subscriptions from the old socket and
                // null m_socket BEFORE calling disconnectFromServer.
                // QLocalSocket::disconnectFromServer can emit the
                // `disconnected` signal synchronously (sender + receiver
                // both on the bridge thread → Qt::DirectConnection);
                // without the detach + null-first, onSocketDisconnected
                // re-enters during disconnectFromServer, clears m_socket,
                // and the deleteLater() below this block then dereferences
                // a null this-pointer. Documented as a hard regression in
                // the project notes.
                QLocalSocket* old = m_socket;
                m_socket = nullptr;
                m_reader.clear();
                QObject::disconnect(old, nullptr, this, nullptr);
                old->disconnectFromServer();
                old->deleteLater();
                // Tear down per-client wire ChatController instances —
                // mirrors what onSocketDisconnected would have done had
                // we left it wired. SessionRouter lives on main thread;
                // marshal via BlockingQueuedConnection (one-way worker
                // → main, no deadlock).
                if (m_services.sessionRouter) {
                    auto* router = m_services.sessionRouter;
                    QMetaObject::invokeMethod(
                        router,
                        [router]() { router->destroyAllWireSessions(); },
                        Qt::BlockingQueuedConnection);
                }
                qCInfo(verzetaRemote) << "HostBridgePeer: superseding existing remote connection";
            }
            m_socket = sock;
            m_reader.clear();
            QObject::connect(
                sock, &QLocalSocket::readyRead, this, &HostBridgePeer::onSocketReadyRead);
            QObject::connect(
                sock, &QLocalSocket::disconnected, this, &HostBridgePeer::onSocketDisconnected);
            // Send hello so the remote can confirm the bridge is alive.
            sendFrame(QJsonObject{
                {QStringLiteral("type"), QString::fromLatin1(IpcType::Hello)},
                {QStringLiteral("host"), QStringLiteral("Verzeta Studio")},
                {QStringLiteral("protocol"), QStringLiteral("verzeta-bridge-1")},
            });
            qCInfo(verzetaRemote) << "HostBridgePeer: remote connected";
        }
    }

    /**
     * @brief Socket-disconnect handler that tears down per-client wire
     *        ChatController instances so they will be re-created on
     *        the daemon's next reconnect.
     */
    void onSocketDisconnected() {
        qCInfo(verzetaRemote) << "HostBridgePeer: remote disconnected";

        drainPendingClientRpc(QStringLiteral("bridge_disconnected"),
                              QStringLiteral("IPC peer disconnected"));

        if (m_socket) {
            m_socket->deleteLater();
            m_socket = nullptr;
        }
        m_reader.clear();
        if (m_services.sessionRouter) {
            auto* router = m_services.sessionRouter;
            QMetaObject::invokeMethod(
                router,
                [router]() { router->destroyAllWireSessions(); },
                Qt::BlockingQueuedConnection);
        }
    }

    /**
     * @brief Reads available bytes from the socket, hands them to the
     *        FrameReader, and dispatches one handleFrame() per decoded
     *        frame.
     */
    void onSocketReadyRead() {
        if (!m_socket)
            return;
        const QByteArray chunk = m_socket->readAll();
        const bool ok = m_reader.feed(chunk, [this](const QJsonObject& obj) { handleFrame(obj); });
        if (!ok) {
            // Oversized / fatal protocol failure — drop connection.
            m_socket->disconnectFromServer();
        }
    }

    // -----------------------------------------------------------------
    // Host signal forwarders. Each connects via Qt::QueuedConnection so
    // the slot runs on this peer's thread (the bridge worker), not on
    // main. Slot serialises and writes one IPC frame.
    // -----------------------------------------------------------------

    /**
     * @brief Forwards MessageService::messageAdded to the daemon.
     * @param convId  Conversation UUID.
     * @param msgId   Message UUID.
     */
    void onMessageAdded(const QString& convId, const QString& msgId) {
        sendSignal(Signals::MessageAdded,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("msg_id"), msgId},
                   });
    }
    /**
     * @brief Forwards MessageService::contextFillMeasured to the
     *        daemon (wire clients render the context gauge from it).
     * @param convId  Conversation UUID.
     * @param percent Estimated context fill (0-100).
     */
    void onContextFillMeasured(const QString& convId, int percent) {
        sendSignal(Signals::ContextFillMeasured,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("percent"), percent},
                   });
    }

    /**
     * @brief Forwards MessageService::userMentioned to the daemon
     *        (clients raise platform notifications from it).
     * @param convId Conversation UUID.
     * @param alias  Alias of the mentioning agent.
     * @param text   The mentioning message's content.
     */
    void onUserMentioned(const QString& convId, const QString& alias, const QString& text) {
        sendSignal(Signals::UserMentioned,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("alias"), alias},
                       {QStringLiteral("text"), text},
                   });
    }

    /**
     * @brief Forwards MessageService::messageUpdated to the daemon.
     * @param convId  Conversation UUID.
     * @param msgId   Message UUID.
     */
    void onMessageUpdated(const QString& convId, const QString& msgId) {
        sendSignal(Signals::MessageUpdated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("msg_id"), msgId},
                   });
    }

    /**
     * @brief Forwards MessageService::messageDeleted to the daemon.
     * @param convId  Conversation UUID.
     * @param msgId   Message UUID.
     */
    void onMessageDeleted(const QString& convId, const QString& msgId) {
        sendSignal(Signals::MessageDeleted,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("msg_id"), msgId},
                   });
    }

    /**
     * @brief Forwards MessageService::messageStreamingStarted.
     * @param convId       Conversation UUID.
     * @param msgId        Message UUID whose stream begins.
     * @param role         Message role ("assistant", "system", …).
     * @param agentId      Agent UUID producing the stream, or empty.
     * @param memberAlias  Group-chat member alias, or empty.
     */
    void onMessageStreamingStarted(const QString& convId,
                                   const QString& msgId,
                                   const QString& role,
                                   const QString& agentId,
                                   const QString& memberAlias) {
        sendSignal(Signals::MessageStreamingStarted,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("msg_id"), msgId},
                       {QStringLiteral("role"), role},
                       {QStringLiteral("agent_id"), agentId},
                       {QStringLiteral("member_alias"), memberAlias},
                   });
    }
    /**
     * @brief Forwards MessageService::messageContentStreamed to the daemon.
     * @param convId  Conversation UUID.
     * @param msgId   Message UUID being streamed.
     * @param delta   New text fragment.
     */
    void
    onMessageContentStreamed(const QString& convId, const QString& msgId, const QString& delta) {
        sendSignal(Signals::MessageContentStreamed,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("msg_id"), msgId},
                       {QStringLiteral("delta"), delta},
                   });
    }
    /**
     * @brief Forwards MessageService::messageContentRewritten to the daemon.
     * @param convId      Conversation UUID.
     * @param msgId       Message UUID being rewritten.
     * @param newContent  Replacement content.
     */
    void onMessageContentRewritten(const QString& convId,
                                   const QString& msgId,
                                   const QString& newContent) {
        sendSignal(Signals::MessageContentRewritten,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("msg_id"), msgId},
                       {QStringLiteral("new_content"), newContent},
                   });
    }
    /**
     * @brief Forwards MessageService::messageStreamingAborted to the daemon.
     * @param convId  Conversation UUID.
     * @param msgId   Message UUID whose stream aborted.
     */
    void onMessageStreamingAborted(const QString& convId, const QString& msgId) {
        sendSignal(Signals::MessageStreamingAborted,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("msg_id"), msgId},
                   });
    }
    /**
     * @brief Forwards MessageService::toolCallAdded to the daemon.
     * @param convId  Conversation UUID.
     * @param callId  Tool-call UUID.
     */
    void onToolCallAdded(const QString& convId, const QString& callId) {
        sendSignal(Signals::ToolCallAdded,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("call_id"), callId},
                   });
    }

    /**
     * @brief Forwards MessageService::toolCallUpdated to the daemon.
     * @param convId  Conversation UUID.
     * @param callId  Tool-call UUID.
     */
    void onToolCallUpdated(const QString& convId, const QString& callId) {
        sendSignal(Signals::ToolCallUpdated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("call_id"), callId},
                   });
    }

    /**
     * @brief Forwards ConversationService::conversationCreated.
     * @param convId  New conversation UUID.
     */
    void onConversationCreated(const QString& convId) {
        sendSignal(Signals::ConversationCreated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                   });
    }
    void onConversationUpdated(const QString& convId) {
        sendSignal(Signals::ConversationUpdated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                   });
    }
    void onConversationDeleted(const QString& convId) {
        sendSignal(Signals::ConversationDeleted,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                   });
    }
    void onConversationRenamed(const QString& convId, const QString& title) {
        sendSignal(Signals::ConversationRenamed,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("title"), title},
                   });
    }
    /**
     * @brief Forwards ConversationService::folderCreated.
     * @param folderId  New folder UUID.
     */
    void onFolderCreated(const QString& folderId) {
        sendSignal(Signals::FolderCreated,
                   QJsonObject{
                       {QStringLiteral("folder_id"), folderId},
                   });
    }
    void onFolderUpdated(const QString& folderId) {
        sendSignal(Signals::FolderUpdated,
                   QJsonObject{
                       {QStringLiteral("folder_id"), folderId},
                   });
    }
    void onFolderDeleted(const QString& folderId) {
        sendSignal(Signals::FolderDeleted,
                   QJsonObject{
                       {QStringLiteral("folder_id"), folderId},
                   });
    }
    void onActiveProviderChanged() {
        // Enrich with the current active_provider / active_model so
        // Android's models.active_changed handler can apply the snapshot
        // directly without a follow-up models.active round-trip. Read
        // happens on main via BlockingQueuedConnection (worker → main
        // ONE-WAY — no deadlock, microsecond latency).
        QJsonObject args;
        if (auto* a = m_services.agentSettings.data()) {
            QPointer<AgentSettingsController> tgt = a;
            QString ap, am;
            QMetaObject::invokeMethod(
                a,
                [tgt, &ap, &am]() {
                    if (!tgt)
                        return;
                    ap = tgt->property("activeProvider").toString();
                    am = tgt->property("activeModel").toString();
                },
                Qt::BlockingQueuedConnection);
            args.insert(QStringLiteral("active_provider"), ap);
            args.insert(QStringLiteral("active_model"), am);
        }
        sendSignal(Signals::ActiveProviderChanged, args);
    }
    void onAgentPatternChanged(const QString& pattern) {
        sendSignal(Signals::AgentPatternChanged,
                   QJsonObject{
                       {QStringLiteral("pattern"), pattern},
                   });
    }
    void onRequireConfirmationChanged(bool require) {
        sendSignal(Signals::RequireConfirmationChanged,
                   QJsonObject{
                       {QStringLiteral("require"), require},
                   });
    }
    void onToolsEnabledChanged() { sendSignal(Signals::ToolsEnabledChanged, QJsonObject{}); }
    void onActiveConversationSettingsChanged() {
        sendSignal(Signals::ActiveConversationSettingsChanged, QJsonObject{});
    }
    void onModelsRefreshed(const QString& providerId, const QStringList& models) {
        sendSignal(Signals::ModelsRefreshed,
                   QJsonObject{
                       {QStringLiteral("provider_id"), providerId},
                       {QStringLiteral("models"), QJsonArray::fromStringList(models)},
                   });
    }
    /**
     * @brief Forwards MembershipService::projectMembersChanged.
     * @param folderId  Project folder UUID whose membership mutated.
     */
    void onProjectMembersChanged(const QString& folderId) {
        sendSignal(Signals::ProjectMembersChanged,
                   QJsonObject{
                       {QStringLiteral("folder_id"), folderId},
                   });
    }
    void onConversationMembersChanged(const QString& convId) {
        sendSignal(Signals::ConversationMembersChanged,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                   });
    }
    void onHeartbeatConfigChanged(const QString& configId) {
        sendSignal(Signals::HeartbeatConfigChanged,
                   QJsonObject{
                       {QStringLiteral("config_id"), configId},
                   });
    }
    void onHeartbeatConfigRemoved(const QString& configId) {
        sendSignal(Signals::HeartbeatConfigRemoved,
                   QJsonObject{
                       {QStringLiteral("config_id"), configId},
                   });
    }
    void onPollCreated(const QString& convId, const QString& pollId) {
        sendSignal(Signals::PollCreated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("poll_id"), pollId},
                   });
    }
    /**
     * @brief Forwards PollService::pollUpdated.
     * @param convId  Conversation UUID.
     * @param pollId  Poll UUID.
     */
    void onPollUpdated(const QString& convId, const QString& pollId) {
        sendSignal(Signals::PollUpdated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("poll_id"), pollId},
                   });
    }

    /**
     * @brief Forwards PollService::pollClosed.
     * @param convId  Conversation UUID.
     * @param pollId  Poll UUID.
     */
    void onPollClosed(const QString& convId, const QString& pollId) {
        sendSignal(Signals::PollClosed,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("poll_id"), pollId},
                   });
    }

    /**
     * @brief Forwards PollService::voteCast.
     * @param convId      Conversation UUID.
     * @param pollId      Poll UUID.
     * @param voterAlias  Alias of the member casting the vote.
     * @param optionId    Identifier of the selected option.
     */
    void onVoteCast(const QString& convId,
                    const QString& pollId,
                    const QString& voterAlias,
                    const QString& optionId) {
        sendSignal(Signals::VoteCast,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("poll_id"), pollId},
                       {QStringLiteral("voter_alias"), voterAlias},
                       {QStringLiteral("option_id"), optionId},
                   });
    }

    /**
     * @brief Forwards an activity_log row insertion to the daemon.
     * @param projectFolderId  Project / org folder UUID, or empty for
     *                         app-level events that reach every session.
     * @param conversationId   Conversation UUID, or empty.
     *
     * Daemon-side WireSession filters per-client (e.g. only forwards
     * when the payload's scope matches what the session is currently
     * viewing).
     */
    void onActivityLogged(const QString& projectFolderId, const QString& conversationId) {
        sendSignal(Signals::ActivityLogged,
                   QJsonObject{
                       {QStringLiteral("project_folder_id"), projectFolderId},
                       {QStringLiteral("conversation_id"), conversationId},
                   });
    }

    // Project Rooms signal forwarders.

    /**
     * @brief Forwards `ProjectTemplateService::projectCreatedFromTemplate`
     *        to the daemon.
     * @param folderId     New project folder UUID.
     * @param folderName   Display name for the new folder.
     * @param memberCount  Number of members the template seeded.
     *
     * Paired clients that initiated the create (Android Quick Start)
     * use this to bridge into the existing kickoff sheet; sibling
     * clients see the new folder land in their sidebar via the
     * standard `folder.added` event and do not need to react to
     * this one.
     */
    void onProjectTemplateProjectCreated(const QString& folderId,
                                         const QString& folderName,
                                         int memberCount) {
        sendSignal(Signals::ProjectTemplateProjectCreated,
                   QJsonObject{
                       {QStringLiteral("folder_id"), folderId},
                       {QStringLiteral("folder_name"), folderName},
                       {QStringLiteral("member_count"), memberCount},
                   });
    }

    /**
     * @brief Forwards `ProjectTemplateService::catalogChanged` to the
     *        daemon so every paired client re-fetches its catalog via
     *        `project_template.list` / `.list_landing`.
     */
    void onProjectTemplateCatalogChanged() {
        sendSignal(Signals::ProjectTemplateCatalogChanged, QJsonObject{});
    }
    void onPlanCreated(const QString& planId) {
        sendSignal(Signals::PlanCreated,
                   QJsonObject{
                       {QStringLiteral("plan_id"), planId},
                   });
    }
    void onPlanUpdated(const QString& planId) {
        sendSignal(Signals::PlanUpdated,
                   QJsonObject{
                       {QStringLiteral("plan_id"), planId},
                   });
    }
    void onPlanDeleted(const QString& planId) {
        sendSignal(Signals::PlanDeleted,
                   QJsonObject{
                       {QStringLiteral("plan_id"), planId},
                   });
    }
    void onStepUpdated(const QString& stepId) {
        sendSignal(Signals::StepUpdated,
                   QJsonObject{
                       {QStringLiteral("step_id"), stepId},
                   });
    }
    void onAgentToolCallRequested(const ToolCall& call) {
        QJsonObject obj = call.toJson();
        // Android reads call_id (not id) on tool_call.requested events.
        obj.insert(QStringLiteral("call_id"), call.id);
        sendSignal(Signals::AgentToolCallRequested, obj);
    }
    void onAgentToolCallCompleted(const ToolCall& call) {
        QJsonObject obj = call.toJson();
        obj.insert(QStringLiteral("call_id"), call.id);
        sendSignal(Signals::AgentToolCallCompleted, obj);
    }
    void onAgentStepStarted(int iteration, const QString& description) {
        sendSignal(Signals::AgentStepStarted,
                   QJsonObject{
                       {QStringLiteral("iteration"), iteration},
                       {QStringLiteral("description"), description},
                   });
    }
    /**
     * @brief Forwards AgentService::agentStepCompleted.
     * @param iteration    Step iteration index.
     * @param description  Human-readable step description.
     * @param success      Whether the step succeeded.
     */
    void onAgentStepCompleted(int iteration, const QString& description, bool success) {
        sendSignal(Signals::AgentStepCompleted,
                   QJsonObject{
                       {QStringLiteral("iteration"), iteration},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("success"), success},
                   });
    }
    /** @brief Forwards AgentService::isRunningChanged to the daemon. */
    void onAgentIsRunningChanged() { sendSignal(Signals::AgentIsRunningChanged, QJsonObject{}); }
    void onAgentIterationChanged() { sendSignal(Signals::AgentIterationChanged, QJsonObject{}); }
    void onImageGenerated(const QString& convId, const QString& imagePath) {
        sendSignal(Signals::ImageGenerated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("path"), imagePath},
                   });
    }
    /**
     * @brief Forwards AudioService::audioGenerated.
     * @param convId     Conversation UUID.
     * @param audioPath  Absolute path to the produced audio file.
     */
    void onAudioGenerated(const QString& convId, const QString& audioPath) {
        sendSignal(Signals::AudioGenerated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("path"), audioPath},
                   });
    }

    /**
     * @brief Forwards CanvasService::canvasOpened.
     * @param convId    Conversation UUID.
     * @param canvasId  Canvas artifact UUID.
     */
    void onCanvasOpened(const QString& convId, const QString& canvasId) {
        sendSignal(Signals::CanvasOpened,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("canvas_id"), canvasId},
                   });
    }

    /**
     * @brief Forwards CanvasService::canvasUpdated.
     * @param convId    Conversation UUID.
     * @param canvasId  Canvas artifact UUID.
     * @param revision  Monotonic revision number after the update.
     */
    void onCanvasUpdated(const QString& convId, const QString& canvasId, int revision) {
        sendSignal(Signals::CanvasUpdated,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("canvas_id"), canvasId},
                       {QStringLiteral("revision"), revision},
                   });
    }
    /**
     * @brief Forwards CanvasService::canvasClosed to the daemon.
     * @param convId    Conversation UUID.
     * @param canvasId  Canvas artifact UUID being closed.
     */
    void onCanvasClosed(const QString& convId, const QString& canvasId) {
        sendSignal(Signals::CanvasClosed,
                   QJsonObject{
                       {QStringLiteral("conv_id"), convId},
                       {QStringLiteral("canvas_id"), canvasId},
                   });
    }
    /**
     * @brief Forwards CanvasService::errorOccurred.
     * @param message  Human-readable error description.
     */
    void onCanvasError(const QString& message) {
        sendSignal(Signals::CanvasErrorOccurred,
                   QJsonObject{
                       {QStringLiteral("message"), message},
                   });
    }
    void onCanvasRunningChanged() { sendSignal(Signals::CanvasRunningChanged, QJsonObject{}); }
    void onCanvasConsoleLineAppended(const QString& kind, const QString& text) {
        sendSignal(Signals::CanvasConsoleLineAppended,
                   QJsonObject{
                       {QStringLiteral("kind"), kind},
                       {QStringLiteral("text"), text},
                   });
    }

  private:
    void wireHostSignals() {
        // EVERY connection here is Qt::QueuedConnection — Qt's standard
        // cross-thread delivery. The signal fires on the host main thread;
        // Qt marshals the args + posts to this peer's event queue; the
        // slot runs on the bridge thread and writes a JSON frame to the
        // QLocalSocket. NEVER blocks main. NEVER calls back into host
        // code synchronously.

        if (auto* p = m_services.msg.data()) {
            connect(p,
                    &MessageService::messageAdded,
                    this,
                    &HostBridgePeer::onMessageAdded,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::messageUpdated,
                    this,
                    &HostBridgePeer::onMessageUpdated,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::messageDeleted,
                    this,
                    &HostBridgePeer::onMessageDeleted,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::messageStreamingStarted,
                    this,
                    &HostBridgePeer::onMessageStreamingStarted,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::messageContentStreamed,
                    this,
                    &HostBridgePeer::onMessageContentStreamed,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::messageContentRewritten,
                    this,
                    &HostBridgePeer::onMessageContentRewritten,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::messageStreamingAborted,
                    this,
                    &HostBridgePeer::onMessageStreamingAborted,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::toolCallAdded,
                    this,
                    &HostBridgePeer::onToolCallAdded,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::toolCallUpdated,
                    this,
                    &HostBridgePeer::onToolCallUpdated,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::contextFillMeasured,
                    this,
                    &HostBridgePeer::onContextFillMeasured,
                    Qt::QueuedConnection);
            connect(p,
                    &MessageService::userMentioned,
                    this,
                    &HostBridgePeer::onUserMentioned,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.convSvc.data()) {
            connect(p,
                    &ConversationService::conversationCreated,
                    this,
                    &HostBridgePeer::onConversationCreated,
                    Qt::QueuedConnection);
            connect(p,
                    &ConversationService::conversationUpdated,
                    this,
                    &HostBridgePeer::onConversationUpdated,
                    Qt::QueuedConnection);
            connect(p,
                    &ConversationService::conversationDeleted,
                    this,
                    &HostBridgePeer::onConversationDeleted,
                    Qt::QueuedConnection);
            connect(p,
                    &ConversationService::folderCreated,
                    this,
                    &HostBridgePeer::onFolderCreated,
                    Qt::QueuedConnection);
            connect(p,
                    &ConversationService::folderUpdated,
                    this,
                    &HostBridgePeer::onFolderUpdated,
                    Qt::QueuedConnection);
            connect(p,
                    &ConversationService::folderDeleted,
                    this,
                    &HostBridgePeer::onFolderDeleted,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.convCtrl.data()) {
            connect(p,
                    &ConversationController::conversationRenamed,
                    this,
                    &HostBridgePeer::onConversationRenamed,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.agentSettings.data()) {
            connect(p,
                    &AgentSettingsController::activeProviderChanged,
                    this,
                    &HostBridgePeer::onActiveProviderChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentSettingsController::agentPatternChanged,
                    this,
                    &HostBridgePeer::onAgentPatternChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentSettingsController::requireConfirmationChanged,
                    this,
                    &HostBridgePeer::onRequireConfirmationChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentSettingsController::toolsEnabledChanged,
                    this,
                    &HostBridgePeer::onToolsEnabledChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentSettingsController::activeConversationSettingsChanged,
                    this,
                    &HostBridgePeer::onActiveConversationSettingsChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentSettingsController::modelsRefreshed,
                    this,
                    &HostBridgePeer::onModelsRefreshed,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.membershipService.data()) {
            connect(p,
                    &MembershipService::projectMembersChanged,
                    this,
                    &HostBridgePeer::onProjectMembersChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &MembershipService::conversationMembersChanged,
                    this,
                    &HostBridgePeer::onConversationMembersChanged,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.pollService.data()) {
            connect(p,
                    &PollService::pollCreated,
                    this,
                    &HostBridgePeer::onPollCreated,
                    Qt::QueuedConnection);
            connect(p,
                    &PollService::pollUpdated,
                    this,
                    &HostBridgePeer::onPollUpdated,
                    Qt::QueuedConnection);
            connect(p,
                    &PollService::pollClosed,
                    this,
                    &HostBridgePeer::onPollClosed,
                    Qt::QueuedConnection);
            connect(
                p, &PollService::voteCast, this, &HostBridgePeer::onVoteCast, Qt::QueuedConnection);
        }
        if (auto* p = m_services.heartbeatConfig.data()) {
            connect(p,
                    &HeartbeatConfigService::configChanged,
                    this,
                    &HostBridgePeer::onHeartbeatConfigChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &HeartbeatConfigService::configRemoved,
                    this,
                    &HostBridgePeer::onHeartbeatConfigRemoved,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.auditService.data()) {
            connect(p,
                    &AuditService::activityLogged,
                    this,
                    &HostBridgePeer::onActivityLogged,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.projectTemplateService.data()) {
            connect(p,
                    &ProjectTemplateService::projectCreatedFromTemplate,
                    this,
                    &HostBridgePeer::onProjectTemplateProjectCreated,
                    Qt::QueuedConnection);
            connect(p,
                    &ProjectTemplateService::catalogChanged,
                    this,
                    &HostBridgePeer::onProjectTemplateCatalogChanged,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.planService.data()) {
            connect(p,
                    &PlanService::planCreated,
                    this,
                    &HostBridgePeer::onPlanCreated,
                    Qt::QueuedConnection);
            connect(p,
                    &PlanService::planUpdated,
                    this,
                    &HostBridgePeer::onPlanUpdated,
                    Qt::QueuedConnection);
            connect(p,
                    &PlanService::planDeleted,
                    this,
                    &HostBridgePeer::onPlanDeleted,
                    Qt::QueuedConnection);
            connect(p,
                    &PlanService::stepUpdated,
                    this,
                    &HostBridgePeer::onStepUpdated,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.agentService.data()) {
            connect(p,
                    &AgentService::agentStepStarted,
                    this,
                    &HostBridgePeer::onAgentStepStarted,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentService::agentStepCompleted,
                    this,
                    &HostBridgePeer::onAgentStepCompleted,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentService::isRunningChanged,
                    this,
                    &HostBridgePeer::onAgentIsRunningChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentService::iterationChanged,
                    this,
                    &HostBridgePeer::onAgentIterationChanged,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentService::toolCallRequested,
                    this,
                    &HostBridgePeer::onAgentToolCallRequested,
                    Qt::QueuedConnection);
            connect(p,
                    &AgentService::toolCallCompleted,
                    this,
                    &HostBridgePeer::onAgentToolCallCompleted,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.imageService.data()) {
            connect(p,
                    &ImageService::imageGenerated,
                    this,
                    &HostBridgePeer::onImageGenerated,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.audioService.data()) {
            connect(p,
                    &AudioService::audioGenerated,
                    this,
                    &HostBridgePeer::onAudioGenerated,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.canvasService.data()) {
            connect(p,
                    &CanvasService::canvasOpened,
                    this,
                    &HostBridgePeer::onCanvasOpened,
                    Qt::QueuedConnection);
            connect(p,
                    &CanvasService::canvasUpdated,
                    this,
                    &HostBridgePeer::onCanvasUpdated,
                    Qt::QueuedConnection);
            connect(p,
                    &CanvasService::canvasClosed,
                    this,
                    &HostBridgePeer::onCanvasClosed,
                    Qt::QueuedConnection);
            connect(p,
                    &CanvasService::errorOccurred,
                    this,
                    &HostBridgePeer::onCanvasError,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.canvasRunner.data()) {
            connect(p,
                    &CanvasRunner::runningChanged,
                    this,
                    &HostBridgePeer::onCanvasRunningChanged,
                    Qt::QueuedConnection);
        }
        if (auto* p = m_services.canvasConsole.data()) {
            connect(p,
                    &CanvasConsoleModel::lineAppended,
                    this,
                    &HostBridgePeer::onCanvasConsoleLineAppended,
                    Qt::QueuedConnection);
        }
        if (auto* sr = m_services.sessionRouter) {
            connect(
                sr,
                &Verzeta::Session::SessionRouter::wireSessionUserMessageQueued,
                this,
                [this](const QString& sessionId, const QString& text) {
                    sendSignal(Signals::UserMessageQueued,
                               QJsonObject{
                                   {QStringLiteral("client_id"), sessionId},
                                   {QStringLiteral("text"), text},
                               });
                },
                Qt::QueuedConnection);
        }
    }

    void sendFrame(const QJsonObject& obj) {
        if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) {
            return;  // no peer connected; drop
        }
        const QByteArray bytes = frameOf(obj);
        m_socket->write(bytes);
    }

    void sendSignal(const char* name, const QJsonObject& args) {
        sendFrame(QJsonObject{
            {QStringLiteral("type"), QString::fromLatin1(IpcType::Signal)},
            {QStringLiteral("name"), QString::fromLatin1(name)},
            {QStringLiteral("args"), args},
        });
    }

    void sendInvokeResponse(const QString& reqId,
                            bool ok,
                            const QJsonValue& data,
                            const QString& error = {}) {
        QJsonObject f{
            {QStringLiteral("type"), QString::fromLatin1(IpcType::InvokeResponse)},
            {QStringLiteral("request_id"), reqId},
            {QStringLiteral("ok"), ok},
        };
        if (ok)
            f.insert(QStringLiteral("data"), data);
        else
            f.insert(QStringLiteral("error"), error);
        sendFrame(f);
    }

    // -----------------------------------------------------------------
    // Inbound frame handler. Dispatches `invoke` frames to host services.
    // EVERY dispatch goes through QMetaObject::invokeMethod with
    // Qt::QueuedConnection on EXISTING host Q_INVOKABLEs. NO new methods
    // are added to host classes. NO BlockingQueuedConnection from main →
    // worker (we're on the bridge worker thread; Qt queues the invoke on
    // main's event loop, fire-and-forget).
    //
    // For methods that have a return value (e.g. Conversations.newConversation
    // returns the new id), the invoke uses Qt::BlockingQueuedConnection
    // ONE-WAY (worker waits for main to run + return; main never waits
    // on worker, so deadlock is impossible). This is the standard Qt
    // RPC pattern.
    // -----------------------------------------------------------------

    void handleFrame(const QJsonObject& obj) {
        try {
            const QString type = obj.value(QStringLiteral("type")).toString();
            if (type == QString::fromLatin1(IpcType::Invoke)) {
                handleInvoke(obj);
                return;
            }
            if (type == QString::fromLatin1(IpcType::PropertyGet)) {
                handlePropertyGet(obj);
                return;
            }
            if (type == QString::fromLatin1(IpcType::PropertySet)) {
                handlePropertySet(obj);
                return;
            }
            if (type == QString::fromLatin1(IpcType::ClientRpcReply)) {
                handleClientRpcReply(obj);
                return;
            }
            // Other inbound frame types (subscribe/unsubscribe, future
            // additions) intentionally not handled — the bridge always
            // forwards every signal it has wired. The remote binary
            // filters per-client. Unknown types are dropped silently
            // for forward-compatibility.
        } catch (const std::exception& ex) {
            qCWarning(verzetaRemote) << "HostBridgePeer::handleFrame: std::exception:" << ex.what();
        } catch (...) {
            qCWarning(verzetaRemote) << "HostBridgePeer::handleFrame: unknown exception";
        }
    }

    /** Convert a typed QVariant returned by QObject::property() to a
     *  JSON value. Mirrors the converter in handleInvoke. */
    static QJsonValue variantToJson(const QVariant& v) {
        switch (v.typeId()) {
            case QMetaType::QString:
                return v.toString();
            case QMetaType::Bool:
                return v.toBool();
            case QMetaType::Int:
                return v.toInt();
            case QMetaType::UInt:
                return static_cast<int>(v.toUInt());
            case QMetaType::LongLong:
                return static_cast<double>(v.toLongLong());
            case QMetaType::ULongLong:
                return static_cast<double>(v.toULongLong());
            case QMetaType::Double:
                return v.toDouble();
            case QMetaType::QStringList:
                return QJsonArray::fromStringList(v.toStringList());
            case QMetaType::QVariantList:
                return QJsonArray::fromVariantList(v.toList());
            case QMetaType::QVariantMap:
                return QJsonObject::fromVariantMap(v.toMap());
            default: {
                if (v.canConvert<QString>())
                    return v.toString();
                return QJsonValue::Null;
            }
        }
    }

    void handlePropertyGet(const QJsonObject& obj) {
        const QString reqId = obj.value(QStringLiteral("request_id")).toString();
        const QString tgt = obj.value(QStringLiteral("target")).toString();
        const QString prop = obj.value(QStringLiteral("property")).toString();
        const QString clientId = obj.value(QStringLiteral("client_id")).toString();

        QObject* target = resolveTargetForClient(tgt, clientId);
        if (!target) {
            if (!reqId.isEmpty()) {
                sendInvokeResponse(reqId, false, {}, QStringLiteral("unknown target: %1").arg(tgt));
            }
            return;
        }
        if (prop.isEmpty()) {
            if (!reqId.isEmpty()) {
                sendInvokeResponse(reqId, false, {}, QStringLiteral("missing property"));
            }
            return;
        }

        // Run property() on target's thread. BlockingQueuedConnection is
        // ONE-WAY (worker waits for main; main never waits for worker).
        // No deadlock possible. Microsecond latency.
        QVariant value;
        const bool ok = QMetaObject::invokeMethod(
            target,
            [target, prop, &value]() { value = target->property(prop.toUtf8().constData()); },
            Qt::BlockingQueuedConnection);

        if (!ok || !value.isValid()) {
            sendInvokeResponse(
                reqId, false, {}, QStringLiteral("no such property: %1.%2").arg(tgt, prop));
            return;
        }
        sendInvokeResponse(reqId, true, variantToJson(value));
    }

    void handlePropertySet(const QJsonObject& obj) {
        const QString reqId = obj.value(QStringLiteral("request_id")).toString();
        const QString tgt = obj.value(QStringLiteral("target")).toString();
        const QString prop = obj.value(QStringLiteral("property")).toString();
        const QJsonObject vobj = obj.value(QStringLiteral("value")).toObject();
        const QString vType = vobj.value(QStringLiteral("type")).toString();
        const QJsonValue vVal = vobj.value(QStringLiteral("value"));
        const QString clientId = obj.value(QStringLiteral("client_id")).toString();

        QObject* target = resolveTargetForClient(tgt, clientId);
        if (!target) {
            if (!reqId.isEmpty()) {
                sendInvokeResponse(reqId, false, {}, QStringLiteral("unknown target: %1").arg(tgt));
            }
            return;
        }

        const QVariant value = jsonToTypedVariant(vType, vVal);
        if (!value.isValid() && vType != QStringLiteral("string")) {
            if (!reqId.isEmpty()) {
                sendInvokeResponse(
                    reqId, false, {}, QStringLiteral("bad value type: %1").arg(vType));
            }
            return;
        }

        // Fire-and-forget queued setProperty on the target's thread.
        QPointer<QObject> tg(target);
        const QByteArray propUtf8 = prop.toUtf8();
        const bool ok = QMetaObject::invokeMethod(
            target,
            [tg, propUtf8, value]() {
                if (tg)
                    tg->setProperty(propUtf8.constData(), value);
            },
            Qt::QueuedConnection);

        if (!reqId.isEmpty()) {
            sendInvokeResponse(reqId,
                               ok,
                               QJsonValue(),
                               ok ? QString() : QStringLiteral("setProperty dispatch failed"));
        }
    }

    /** Per-client target resolution.
     *
     *  When @p clientId is non-empty AND not the canonical "local"
     *  session id AND a SessionRouter is wired through Services, the
     *  ChatController target resolves to the per-client ChatController
     *  instance owned by SessionRouter. Lazy-create on first frame
     *  with a new clientId.
     *
     *  All other targets (MessageService, ConversationService, etc.)
     *  are shared across sessions and ignore @p clientId, using the same
     *  dispatch as the empty-clientId path.
     *
     *  THREADING: SessionRouter is owned by AppController (main thread)
     *  and asserts main-thread on its public methods. The bridge worker
     *  must NOT call SessionRouter methods directly. We marshal via
     *  Qt::BlockingQueuedConnection: worker waits for main, main never
     *  waits on worker (the contract). Microsecond cost; legitimate.
     */
    QObject* resolveTargetForClient(const QString& target, const QString& clientId) const {
        const bool wantsPerClient =
            !clientId.isEmpty() &&
            clientId != QString::fromUtf8(Verzeta::Session::kLocalSessionId) &&
            m_services.sessionRouter != nullptr;

        if (wantsPerClient && target == QString::fromLatin1(Targets::ChatController)) {
            ChatController* cc = nullptr;
            // Marshal to main thread for the lookup + lazy-create.
            // SessionRouter's mutating API (createWireSession) requires
            // main-thread per VERZETA_ASSERT_MAIN_THREAD inside its body;
            // the bridge runs on a worker QThread.
            auto* router = m_services.sessionRouter;
            const QString cid = clientId;
            QMetaObject::invokeMethod(
                router,
                [router, cid, &cc]() {
                    cc = router->sessionFor(cid);
                    if (!cc) {
                        cc = router->createWireSession(cid);
                    }
                },
                Qt::BlockingQueuedConnection);
            return cc;
        }
        if (wantsPerClient && target == QString::fromLatin1(Targets::AgentSettings)) {
            AgentSettingsController* as = nullptr;
            auto* router = m_services.sessionRouter;
            const QString cid = clientId;
            QMetaObject::invokeMethod(
                router,
                [router, cid, &as]() {
                    as = router->agentSettingsFor(cid);
                    if (!as) {
                        as = router->createWireAgentSettings(cid);
                    }
                },
                Qt::BlockingQueuedConnection);
            return as;
        }
        if (wantsPerClient && target == QString::fromLatin1(Targets::CanvasAiActions)) {
            // Canvas AI actions compose a prompt about the active canvas of
            // a conversation and send it through a ChatController. The
            // desktop's instance uses the desktop's ChatController, so a
            // wire client gets its own instance bound to its own wire
            // ChatController: the action then reads the canvas of the
            // conversation that client has open and posts into it, exactly
            // like msg.send. The instance is a child of the wire
            // ChatController, so it is destroyed with that session. The
            // desktop's instance is not touched.
            CanvasAiActions* actions = nullptr;
            auto* router = m_services.sessionRouter;
            CanvasService* canvas = m_services.canvasService.data();
            const QString cid = clientId;
            QMetaObject::invokeMethod(
                router,
                [router, cid, canvas, &actions]() {
                    ChatController* cc = router->sessionFor(cid);
                    if (!cc)
                        cc = router->createWireSession(cid);
                    if (!cc)
                        return;
                    actions =
                        cc->findChild<CanvasAiActions*>(QString(), Qt::FindDirectChildrenOnly);
                    if (!actions) {
                        actions = new CanvasAiActions(cc);
                        actions->setCanvasService(canvas);
                        actions->setChatController(cc);
                    }
                },
                Qt::BlockingQueuedConnection);
            return actions;
        }
        // Shared dispatch (legacy / explicit-local / unrouted targets).
        return resolveTarget(target);
    }

    QObject* resolveTarget(const QString& target) const {
        // Dispatch table: target name → QPointer<QObject>. All entries
        // are non-owning; AppController retains lifetime. Returns null
        // when the service isn't attached or has been destroyed.
        if (target == QString::fromLatin1(Targets::ChatController))
            return m_services.chatCtrl.data();
        if (target == QString::fromLatin1(Targets::Conversations))
            return m_services.convCtrl.data();
        if (target == QString::fromLatin1(Targets::ConversationService))
            return m_services.convSvc.data();
        if (target == QString::fromLatin1(Targets::MessageService))
            return m_services.msg.data();
        if (target == QString::fromLatin1(Targets::AgentSettings))
            return m_services.agentSettings.data();
        if (target == QString::fromLatin1(Targets::AgentRegistry))
            return m_services.agentRegistry.data();
        if (target == QString::fromLatin1(Targets::AgentService))
            return m_services.agentService.data();
        if (target == QString::fromLatin1(Targets::ToolService))
            return m_services.toolService.data();
        if (target == QString::fromLatin1(Targets::McpService))
            return m_services.mcpService.data();
        if (target == QString::fromLatin1(Targets::SkillService))
            return m_services.skillService.data();
        if (target == QString::fromLatin1(Targets::MembershipService))
            return m_services.membershipService.data();
        if (target == QString::fromLatin1(Targets::PollService))
            return m_services.pollService.data();
        if (target == QString::fromLatin1(Targets::AuditService))
            return m_services.auditService.data();
        if (target == QString::fromLatin1(Targets::ProjectTemplateService))
            return m_services.projectTemplateService.data();
        // Workspace mount registry — metadata-only substrate for
        // client-owned virtual workspace mounts (5 wire ops).
        if (target == QString::fromLatin1(Targets::FolderMountRegistry))
            return m_services.folderMountRegistry.data();
        if (target == QString::fromLatin1(Targets::HeartbeatConfigService))
            return m_services.heartbeatConfig.data();
        if (target == QString::fromLatin1(Targets::HeartbeatSubagentService))
            return m_services.heartbeatSubagent.data();
        if (target == QString::fromLatin1(Targets::TaskController))
            return m_services.taskController.data();
        if (target == QString::fromLatin1(Targets::PlanService))
            return m_services.planService.data();
        if (target == QString::fromLatin1(Targets::RagService))
            return m_services.ragService.data();
        if (target == QString::fromLatin1(Targets::FileService))
            return m_services.fileService.data();
        if (target == QString::fromLatin1(Targets::ImageService))
            return m_services.imageService.data();
        if (target == QString::fromLatin1(Targets::AudioService))
            return m_services.audioService.data();
        if (target == QString::fromLatin1(Targets::Canvas))
            return m_services.canvasService.data();
        if (target == QString::fromLatin1(Targets::CanvasRunner))
            return m_services.canvasRunner.data();
        if (target == QString::fromLatin1(Targets::CanvasAiActions))
            return m_services.canvasAiActions.data();
        if (target == QString::fromLatin1(Targets::CanvasConsoleModel))
            return m_services.canvasConsole.data();
        if (target == QString::fromLatin1(Targets::SettingsService))
            return m_services.settings.data();
        if (target == QString::fromLatin1(Targets::WebSearchProviders))
            return m_services.webSearchRegistry.data();
        return nullptr;
    }

    /** Convert a JSON value to a typed QVariant matching the type tag.
     *  Supported tags: string, bool, int, qint64, double, stringlist,
     *  variantmap, variantlist, bytearray-base64.
     *  Returns invalid QVariant on conversion failure. */
    static QVariant jsonToTypedVariant(const QString& type, const QJsonValue& v) {
        if (type == QStringLiteral("string"))
            return v.toString();
        if (type == QStringLiteral("bool"))
            return v.toBool();
        if (type == QStringLiteral("int"))
            return v.toInt();
        if (type == QStringLiteral("qint64"))
            return static_cast<qint64>(v.toDouble());
        if (type == QStringLiteral("double"))
            return v.toDouble();
        if (type == QStringLiteral("stringlist")) {
            const QJsonArray a = v.toArray();
            QStringList sl;
            sl.reserve(a.size());
            for (const QJsonValue& e : a)
                sl.append(e.toString());
            return sl;
        }
        if (type == QStringLiteral("variantmap"))
            return v.toObject().toVariantMap();
        if (type == QStringLiteral("variantlist"))
            return v.toArray().toVariantList();
        if (type == QStringLiteral("bytearray-base64")) {
            const QByteArray b64 = v.toString().toLatin1();
            return QByteArray::fromBase64(b64, QByteArray::AbortOnBase64DecodingErrors);
        }
        return {};
    }

    void handleInvoke(const QJsonObject& obj) {
        const QString reqId = obj.value(QStringLiteral("request_id")).toString();
        const QString tgt = obj.value(QStringLiteral("target")).toString();
        const QString method = obj.value(QStringLiteral("method")).toString();
        const QJsonArray rawArgs = obj.value(QStringLiteral("args")).toArray();
        const QString returnType = obj.value(QStringLiteral("return_type")).toString();
        const bool wantsResponse = !returnType.isEmpty();
        const QString clientId = obj.value(QStringLiteral("client_id")).toString();

        QObject* target = resolveTargetForClient(tgt, clientId);
        if (!target) {
            if (!reqId.isEmpty()) {
                sendInvokeResponse(reqId, false, {}, QStringLiteral("unknown target: %1").arg(tgt));
            }
            return;
        }
        if (method.isEmpty()) {
            if (!reqId.isEmpty()) {
                sendInvokeResponse(reqId, false, {}, QStringLiteral("missing method"));
            }
            return;
        }

        // Convert JSON args → typed QVariants. Each arg is
        // {type: "...", value: ...}.
        QVector<QVariant> args;
        args.reserve(rawArgs.size());
        for (const QJsonValue& a : rawArgs) {
            const QJsonObject ao = a.toObject();
            const QString aType = ao.value(QStringLiteral("type")).toString();
            const QJsonValue aVal = ao.value(QStringLiteral("value"));
            const QVariant qv = jsonToTypedVariant(aType, aVal);
            if (!qv.isValid() && aType != QStringLiteral("string")) {
                if (!reqId.isEmpty()) {
                    sendInvokeResponse(
                        reqId, false, {}, QStringLiteral("bad arg type: %1").arg(aType));
                }
                return;
            }
            args.append(qv);
        }

        // Build QGenericArgument array (Qt limit is 10).
        if (args.size() > 10) {
            if (!reqId.isEmpty()) {
                sendInvokeResponse(reqId, false, {}, QStringLiteral("too many args (max 10)"));
            }
            return;
        }
        QGenericArgument ga[10];
        for (int i = 0; i < args.size(); ++i) {
            ga[i] = QGenericArgument(args[i].typeName(), args[i].constData());
        }

        if (!wantsResponse) {
            // Fire-and-forget queued invocation. Method runs on the host's
            // main thread when its event loop next ticks. Never blocks
            // the bridge worker.
            const bool ok = QMetaObject::invokeMethod(target,
                                                      method.toUtf8().constData(),
                                                      Qt::QueuedConnection,
                                                      ga[0],
                                                      ga[1],
                                                      ga[2],
                                                      ga[3],
                                                      ga[4],
                                                      ga[5],
                                                      ga[6],
                                                      ga[7],
                                                      ga[8],
                                                      ga[9]);
            if (!reqId.isEmpty()) {
                sendInvokeResponse(reqId,
                                   ok,
                                   QJsonValue(),
                                   ok ? QString()
                                      : QStringLiteral("invokeMethod failed (no such method?)"));
            }
            return;
        }

        // BlockingQueuedConnection — ONE-WAY. Worker waits for main to
        // run + return. Main NEVER waits on worker (Qt's deadlock
        // detector confirms this). Microsecond cost. The bridge worker
        // briefly stalls; legitimate trade-off for sync RPC return.
        //
        // Normalize type aliases before constructing the QGenericReturnArgument
        // — Qt's metaobject stores the canonical type name (e.g.
        // "QVariantList" for `QVariantList`-returning methods), but some
        // Q_INVOKABLE signatures get encoded under aliases that mismatch
        // unless we normalize. Calling QMetaType::fromName + .name() round-
        // trips through the canonical form.
        const QByteArray rawTypeName = returnType.toUtf8();
        const QMetaType metaType = QMetaType::fromName(rawTypeName);
        const QByteArray canonicalTypeName =
            metaType.isValid() ? QByteArray(metaType.name()) : rawTypeName;
        QVariant retVal(metaType);
        QGenericReturnArgument retArg(canonicalTypeName.constData(), retVal.data());
        const bool ok = QMetaObject::invokeMethod(target,
                                                  method.toUtf8().constData(),
                                                  Qt::BlockingQueuedConnection,
                                                  retArg,
                                                  ga[0],
                                                  ga[1],
                                                  ga[2],
                                                  ga[3],
                                                  ga[4],
                                                  ga[5],
                                                  ga[6],
                                                  ga[7],
                                                  ga[8],
                                                  ga[9]);

        if (!ok) {
            sendInvokeResponse(reqId, false, {}, QStringLiteral("invokeMethod failed"));
            return;
        }
        // Convert return value to JSON.
        QJsonValue jsonReturn;
        if (returnType == QStringLiteral("QString")) {
            jsonReturn = retVal.toString();
        } else if (returnType == QStringLiteral("bool")) {
            jsonReturn = retVal.toBool();
        } else if (returnType == QStringLiteral("int")) {
            jsonReturn = retVal.toInt();
        } else if (returnType == QStringLiteral("qint64")) {
            jsonReturn = static_cast<double>(retVal.toLongLong());
        } else if (returnType == QStringLiteral("QStringList")) {
            jsonReturn = QJsonArray::fromStringList(retVal.toStringList());
        } else if (returnType == QStringLiteral("QVariantList")) {
            jsonReturn = QJsonArray::fromVariantList(retVal.toList());
        } else if (returnType == QStringLiteral("QVariantMap")) {
            jsonReturn = QJsonObject::fromVariantMap(retVal.toMap());
        } else {
            jsonReturn = QJsonValue::Null;
        }
        sendInvokeResponse(reqId, true, jsonReturn);
    }


    /**
     * @brief Marshals the dispatch from `WireHostBridge::dispatchClientRequest`
     *        onto the peer thread. Validates capacity + socket state,
     *        generates a request UUID, stores the pending entry, and
     *        writes an `IpcType::ClientRpcDispatch` IPC frame.
     */
    void dispatchClientRequestOnPeerThread(const QString& clientId,
                                           const QString& op,
                                           const QJsonObject& args,
                                           BridgeClientRpcCallback callback,
                                           int timeoutMs) {
        if (!callback)
            return;  // defence in depth; bridge already checked

        if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) {
            callback(BridgeClientRpcReply{false,
                                          QStringLiteral("bridge_offline"),
                                          QStringLiteral("IPC peer not connected"),
                                          QJsonValue()});
            return;
        }
        if (m_pendingClientRpc.size() >= WireHostBridge::kMaxPendingBridgeRpc) {
            callback(BridgeClientRpcReply{false,
                                          QStringLiteral("too_many_pending"),
                                          QStringLiteral("bridge pending-RPC map at capacity"),
                                          QJsonValue()});
            return;
        }

        const int clampedTimeoutMs = qMax(100, timeoutMs);
        const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qint64 expiresAtMs =
            QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(clampedTimeoutMs);

        PendingClientRpc entry;
        entry.op = op;
        entry.expiresAtMs = expiresAtMs;
        entry.callback = std::move(callback);
        m_pendingClientRpc.insert(requestId, std::move(entry));

        sendFrame(QJsonObject{
            {QStringLiteral("type"), QString::fromLatin1(IpcType::ClientRpcDispatch)},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("client_id"), clientId},
            {QStringLiteral("op"), op},
            {QStringLiteral("args"), args},
            {QStringLiteral("timeout_ms"), clampedTimeoutMs},
        });
    }

    /**
     * @brief Resolves a matching `IpcType::ClientRpcReply` frame against
     *        the per-peer pending-RPC map. Fires the callback exactly
     *        once and removes the entry. Unknown / stale request_ids
     *        are silently dropped (the matching callback already fired
     *        via the sweeper or the drain).
     */
    void handleClientRpcReply(const QJsonObject& obj) {
        const QString requestId = obj.value(QStringLiteral("request_id")).toString();
        if (requestId.isEmpty()) {
            qCWarning(verzetaRemote) << "HostBridgePeer: client_rpc_reply missing request_id";
            return;
        }
        auto it = m_pendingClientRpc.find(requestId);
        if (it == m_pendingClientRpc.end()) {
            // Stale — sweeper already fired or daemon replied late.
            return;
        }
        PendingClientRpc entry = std::move(it.value());
        m_pendingClientRpc.erase(it);

        BridgeClientRpcReply reply;
        const bool ok = obj.value(QStringLiteral("ok")).toBool(false);
        if (ok) {
            reply.ok = true;
            reply.data = obj.value(QStringLiteral("data"));
        } else {
            reply.ok = false;
            const QJsonObject err = obj.value(QStringLiteral("error")).toObject();
            reply.errorKind =
                err.value(QStringLiteral("kind")).toString(QStringLiteral("client_error"));
            reply.errorDetail = err.value(QStringLiteral("detail")).toString();
        }
        if (entry.callback)
            entry.callback(reply);
    }

    /**
     * @brief Periodic sweep that walks the pending-RPC map and fires
     *        timeout callbacks for every entry whose deadline has
     *        passed. Two-pass to avoid iterator invalidation if a
     *        callback re-enters and mutates the map.
     */
    void sweepClientRpcTimeouts() {
        if (m_pendingClientRpc.isEmpty())
            return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        QStringList expired;
        expired.reserve(m_pendingClientRpc.size());
        for (auto it = m_pendingClientRpc.constBegin(); it != m_pendingClientRpc.constEnd(); ++it) {
            if (it.value().expiresAtMs <= now) {
                expired.append(it.key());
            }
        }
        for (const QString& reqId : expired) {
            auto it = m_pendingClientRpc.find(reqId);
            if (it == m_pendingClientRpc.end())
                continue;
            PendingClientRpc entry = std::move(it.value());
            m_pendingClientRpc.erase(it);
            if (entry.callback) {
                entry.callback(BridgeClientRpcReply{
                    false,
                    QStringLiteral("timeout"),
                    QStringLiteral("client_rpc_reply not received within deadline"),
                    QJsonValue()});
            }
        }
    }

    /**
     * @brief Atomically drains every pending callback with the given
     *        error kind. Used on IPC peer disconnect, supersede, and
     *        peer destruction. After the drain the pending map is
     *        empty and the sweeper is harmless even if it fires once
     *        more before being stopped.
     *
     * Swap-out-then-fire so a callback that re-enters and issues a
     * fresh dispatch sees an empty pending map and a fresh capacity
     * budget. This mirrors WireSession's drain pattern.
     */
    void drainPendingClientRpc(const QString& kind, const QString& detail) {
        if (m_pendingClientRpc.isEmpty())
            return;
        QHash<QString, PendingClientRpc> drain;
        drain.swap(m_pendingClientRpc);
        for (auto it = drain.begin(); it != drain.end(); ++it) {
            if (it.value().callback) {
                it.value().callback(BridgeClientRpcReply{false, kind, detail, QJsonValue()});
            }
        }
    }

  private:
    /**
     * @brief One in-flight host-initiated client RPC.
     */
    struct PendingClientRpc {
        /** Wire op name (diagnostic-only on the bridge side; the
         *  daemon owns the wire dispatch). */
        QString op;
        /** Absolute deadline (epoch ms) at which the sweeper fires. */
        qint64 expiresAtMs = 0;
        /** Resolver invoked exactly once per entry. */
        BridgeClientRpcCallback callback;
    };

    WireHostBridge::Services m_services;
    QLocalServer* m_server = nullptr;
    QLocalSocket* m_socket = nullptr;
    QString m_socketName;
    FrameReader m_reader;

    /** Per-peer in-flight host-initiated client RPCs awaiting
     *  `client_rpc_reply`. Keyed on the bridge-generated request
     *  UUID. Bounded by `WireHostBridge::kMaxPendingBridgeRpc`. */
    QHash<QString, PendingClientRpc> m_pendingClientRpc;

    /** Periodic sweeper that walks `m_pendingClientRpc` and fires
     *  timeout callbacks. Constructed as a child of this peer inside
     *  `onStart` so the QTimer inherits the peer's bridge-worker
     *  thread affinity (the peer is moved to the worker thread before
     *  onStart runs; a value-member QTimer would have stayed on the
     *  main thread and Qt would refuse to start it from the wrong
     *  thread). Heap-allocated; RAII teardown via the QObject parent
     *  ownership. */
    QTimer* m_clientRpcSweepTimer = nullptr;
};

// ---------------------------------------------------------------------------
// WireHostBridge — the public-facing class. Simple wrapper that owns a
// QThread and the HostBridgePeer that lives on it.
// ---------------------------------------------------------------------------

WireHostBridge::WireHostBridge(Services services, QObject* parent)
    : QObject(parent), m_services(std::move(services)) {
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("verzeta-host-bridge"));

    m_peer = std::make_unique<HostBridgePeer>(m_services);
    m_peer->moveToThread(m_thread);

    QObject::connect(m_thread, &QThread::started, m_peer.get(), &HostBridgePeer::onStart);
    QObject::connect(
        m_thread, &QThread::finished, m_peer.get(), &HostBridgePeer::onStop, Qt::DirectConnection);

    QObject::connect(
        m_peer.get(),
        &HostBridgePeer::bridgeReady,
        this,
        [this]() {
            if (autoStartEnabled()) {
                qCInfo(verzetaRemote) << "WireHostBridge: bridge ready — firing auto-start";
                const bool ok = startRemoteServer(
                    preferredBindAddr(), preferredPort(), tlsEnabled(), certPath(), keyPath());
                if (!ok) {
                    qCWarning(verzetaRemote)
                        << "WireHostBridge: auto-start failed (see prior "
                           "startRemoteServer warning for cause). User will "
                           "need to start the server manually from the dialog.";
                }
            }
        },
        Qt::QueuedConnection);

    m_thread->start();
}

WireHostBridge::~WireHostBridge() {
    // ORDER MATTERS. This destructor runs on the main thread AFTER the
    // main event loop has exited, which invalidates the assumption
    // behind the peer's worker→main Qt::BlockingQueuedConnection
    // marshals (they need a live main loop to be serviced).
    //
    // 1) Stop the bridge worker FIRST — before touching the child
    //    process. Killing the child first drops the IPC socket, which
    //    fires onSocketDisconnected on the worker, whose blocking
    //    marshal to the dead main loop then parks the worker forever:
    //    quit() is never processed, the timed wait expires, and
    //    ~QObject destroys a still-running QThread child — a qFatal
    //    abort on every single exit.
    //
    // 2) quit() alone is not enough even in this order: a blocking
    //    marshal can already be in flight (client RPC dispatch, a
    //    daemon that died on its own at the same moment). So while
    //    waiting for the worker to finish, service posted metacalls —
    //    that releases any parked BlockingQueuedConnection, the worker
    //    drains, processes quit(), and the wait terminates. The loop is
    //    bounded by construction: the worker's code is event-driven
    //    (no waitFor*/sleeps on the bridge thread) and every blocking
    //    marshal it can enter targets the main thread, which this loop
    //    is servicing. SessionRouter and the services those marshals
    //    touch are declared BEFORE the bridge in AppController, so they
    //    are destroyed after us and are still alive here.
    if (m_thread) {
        m_thread->quit();
        QElapsedTimer drainTimer;
        drainTimer.start();
        bool warned = false;
        while (!m_thread->wait(50)) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            if (!warned && drainTimer.elapsed() > 10000) {
                warned = true;
                qCCritical(verzetaRemote)
                    << "WireHostBridge: bridge thread still draining after" << drainTimer.elapsed()
                    << "ms — a worker-side blocking"
                       " call is not being released; continuing to drain";
            }
        }
    }
    // 3) THEN make sure the verzeta-remote child process exits with us.
    //    The worker is stopped and its QLocalServer/socket were closed
    //    in onStop (thread-finished, direct connection), so nobody is
    //    left to react to the child's dropped IPC connection and a
    //    reconnect into a half-torn-down host is impossible (no
    //    listener exists any more).
    if (m_remoteServerProcess && m_remoteServerProcess->state() != QProcess::NotRunning) {
        m_remoteServerProcess->terminate();
        if (!m_remoteServerProcess->waitForFinished(2000)) {
            m_remoteServerProcess->kill();
            m_remoteServerProcess->waitForFinished(500);
        }
    }
    // m_peer is destroyed here on main thread but its event-loop slots
    // have already drained because the thread is done.
}

QString WireHostBridge::listenSocketName() const {
    return m_peer ? m_peer->listenSocketName() : QString();
}


void WireHostBridge::dispatchClientRequest(const QString& clientId,
                                           const QString& op,
                                           const QJsonObject& args,
                                           BridgeClientRpcCallback callback,
                                           int timeoutMs) {
    // Fail-closed: reject every call that cannot be honoured rather
    // than queue silently. Each rejection fires the callback exactly
    // once before returning so caller-side resources release.
    if (!callback)
        return;
    if (op.isEmpty()) {
        callback(BridgeClientRpcReply{false,
                                      QStringLiteral("invalid_argument"),
                                      QStringLiteral("op is empty"),
                                      QJsonValue()});
        return;
    }
    if (clientId.isEmpty()) {
        callback(BridgeClientRpcReply{false,
                                      QStringLiteral("invalid_argument"),
                                      QStringLiteral("client_id is empty"),
                                      QJsonValue()});
        return;
    }
    if (!m_peer) {
        callback(BridgeClientRpcReply{false,
                                      QStringLiteral("bridge_offline"),
                                      QStringLiteral("bridge has no peer"),
                                      QJsonValue()});
        return;
    }

    // Marshal to the peer's thread. The lambda owns the moved callback
    // until it runs; the queued invocation guarantees single-threaded
    // access to m_pendingClientRpc on the peer side.
    HostBridgePeer* peer = m_peer.get();
    QMetaObject::invokeMethod(
        peer,
        [peer, clientId, op, args, cb = std::move(callback), timeoutMs]() mutable {
            peer->dispatchClientRequestOnPeerThread(clientId, op, args, std::move(cb), timeoutMs);
        },
        Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Admin API — spawns the verzeta-remote binary as a child process.
// All methods run on the main thread; the daemon is the long-running
// child while the CLI invocations (--pair-code / --list-clients /
// --revoke) are short-lived synchronous calls capped at 5 seconds.
// ---------------------------------------------------------------------------

QString WireHostBridge::remoteBinaryPath() const {
    // Sit next to verzeta-studio in the install layout. On Linux dev
    // builds both binaries live in build-debug/backend/. AppImage and
    // Windows installers preserve the same relative arrangement.
    const QString dir = QCoreApplication::applicationDirPath();
    QString name = QStringLiteral("verzeta-remote");
    if (QSysInfo::productType() == QStringLiteral("windows")) {
        name += QStringLiteral(".exe");
    }
    const QString abs = QDir(dir).absoluteFilePath(name);
    return QFileInfo::exists(abs) ? abs : QString();
}

bool WireHostBridge::remoteServerRunning() const {
    return m_remoteServerProcess && m_remoteServerProcess->state() != QProcess::NotRunning;
}

QString WireHostBridge::remoteServerUrl() const {
    if (!remoteServerRunning())
        return QString();
    const QString host =
        (m_remoteServerBindAddr == QStringLiteral("0.0.0.0") || m_remoteServerBindAddr.isEmpty())
            ? QStringLiteral("0.0.0.0")
            : m_remoteServerBindAddr;
    return QStringLiteral("ws://%1:%2/ws").arg(host).arg(m_remoteServerPort);
}

bool WireHostBridge::startRemoteServer(const QString& bindAddr,
                                       int port,
                                       bool useTls,
                                       const QString& certPathArg,
                                       const QString& keyPathArg) {
    if (remoteServerRunning())
        return true;
    const QString bin = remoteBinaryPath();
    if (bin.isEmpty()) {
        qCWarning(verzetaRemote) << "startRemoteServer: verzeta-remote binary not found next to"
                                 << QCoreApplication::applicationDirPath();
        return false;
    }
    const QString addr = bindAddr.isEmpty() ? preferredBindAddr() : bindAddr;
    const int p = port <= 0 ? preferredPort() : port;

    if (!m_remoteServerProcess) {
        m_remoteServerProcess = new QProcess(this);
        m_remoteServerProcess->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_remoteServerProcess,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                &WireHostBridge::onRemoteServerProcessFinished);
#if defined(Q_OS_LINUX)
        m_remoteServerProcess->setChildProcessModifier(
            []() { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
#endif
#if defined(Q_OS_WIN)
        // verzeta-remote is a console-subsystem binary (it doubles as a
        // manually-run headless daemon). When the GUI app spawns it,
        // suppress the console window it would otherwise pop by setting
        // CREATE_NO_WINDOW. The literal is the ABI-stable Win32 flag
        // value, used here to avoid pulling <windows.h> into this file
        // for a single constant.
        m_remoteServerProcess->setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* cpa) {
                constexpr quint32 kCreateNoWindow = 0x08000000u;
                cpa->flags |= kCreateNoWindow;
            });
#endif
    }

    QStringList args{
        QStringLiteral("--bind"),
        addr,
        QStringLiteral("--port"),
        QString::number(p),
    };
    if (useTls) {
        const QString cert = certPathArg.isEmpty() ? certPath() : certPathArg;
        const QString key = keyPathArg.isEmpty() ? keyPath() : keyPathArg;
        if (!QFileInfo::exists(cert) || !QFileInfo::exists(key)) {
            if (!ensureSelfSignedCert()) {
                qCWarning(verzetaRemote)
                    << "startRemoteServer: TLS requested but cert generation failed";
                return false;
            }
        }
        args << QStringLiteral("--tls") << QStringLiteral("--cert") << cert
             << QStringLiteral("--key") << key;
    }
    m_remoteServerProcess->start(bin, args);
    if (!m_remoteServerProcess->waitForStarted(2000)) {
        qCWarning(verzetaRemote) << "startRemoteServer: process failed to start:"
                                 << m_remoteServerProcess->errorString();
        return false;
    }
    m_remoteServerBindAddr = addr;
    m_remoteServerPort = p;
    qCInfo(verzetaRemote) << "startRemoteServer: spawned verzeta-remote pid"
                          << m_remoteServerProcess->processId() << "on" << addr << ":" << p
                          << (useTls ? "(TLS)" : "(plain)");
    emit remoteServerRunningChanged();
    return true;
}

void WireHostBridge::stopRemoteServer() {
    if (!remoteServerRunning())
        return;
    qCInfo(verzetaRemote) << "stopRemoteServer: terminating pid"
                          << m_remoteServerProcess->processId();
    m_remoteServerProcess->terminate();
    if (!m_remoteServerProcess->waitForFinished(2000)) {
        m_remoteServerProcess->kill();
        m_remoteServerProcess->waitForFinished(1000);
    }
    // onRemoteServerProcessFinished() emits the signal.
}

void WireHostBridge::onRemoteServerProcessFinished() {
    qCInfo(verzetaRemote) << "verzeta-remote process exited";
    emit remoteServerRunningChanged();
}

namespace {

/** Run a one-shot CLI invocation against the verzeta-remote binary,
 *  capturing stdout. Returns empty string on failure or timeout. */
QByteArray runOneShot(const QString& binary, const QStringList& args, int timeoutMs = 5000) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(binary, args);
    if (!p.waitForStarted(2000))
        return {};
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    return p.readAllStandardOutput().trimmed();
}

}  // namespace

WireAuth* WireHostBridge::authOrNull() {
    if (m_auth)
        return m_auth.get();
    auto a = std::make_unique<WireAuth>();
    if (!a->open()) {
        qCWarning(verzetaRemote) << "authOrNull: failed to open wire-state.db — admin "
                                    "actions will be unavailable until the path is writable";
        return nullptr;
    }
    m_auth = std::move(a);
    return m_auth.get();
}

QString WireHostBridge::generatePairCode() {
    if (auto* a = authOrNull())
        return a->generatePairingCode();
    return {};
}

QVariantList WireHostBridge::listPairedClients() {
    QVariantList result;
    auto* a = authOrNull();
    if (!a)
        return result;
    for (const PairedClient& c : a->listClients(true)) {
        QVariantMap m;
        m[QStringLiteral("id")] = c.id;
        m[QStringLiteral("name")] = c.name;
        m[QStringLiteral("created_at")] =
            c.createdAt.isValid() ? QVariant(c.createdAt.toMSecsSinceEpoch()) : QVariant();
        m[QStringLiteral("last_seen_at")] =
            c.lastSeenAt.isValid() ? QVariant(c.lastSeenAt.toMSecsSinceEpoch()) : QVariant();
        m[QStringLiteral("revoked")] = !c.isActive();
        result.append(m);
    }
    return result;
}

bool WireHostBridge::revokeClient(const QString& clientId) {
    if (clientId.isEmpty())
        return false;
    auto* a = authOrNull();
    if (!a)
        return false;
    return a->revokeClient(clientId);
}

// ---------------------------------------------------------------------------
// Persistent settings (QSettings under "remote/*")
// ---------------------------------------------------------------------------

namespace {
QString remoteCertDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QStringLiteral("/verzeta-remote");
    QDir().mkpath(dir);
    return dir;
}
}  // namespace

QString WireHostBridge::preferredBindAddr() const {
    QSettings s;
    return s.value(QStringLiteral("remote/bindAddr"), QStringLiteral("0.0.0.0")).toString();
}
void WireHostBridge::setPreferredBindAddr(const QString& addr) {
    QSettings s;
    s.setValue(QStringLiteral("remote/bindAddr"), addr);
}

int WireHostBridge::preferredPort() const {
    QSettings s;
    return s.value(QStringLiteral("remote/port"), 9180).toInt();
}
void WireHostBridge::setPreferredPort(int port) {
    QSettings s;
    s.setValue(QStringLiteral("remote/port"), port);
}

bool WireHostBridge::autoStartEnabled() const {
    QSettings s;
    return s.value(QStringLiteral("remote/autoStart"), false).toBool();
}
void WireHostBridge::setAutoStartEnabled(bool enabled) {
    QSettings s;
    s.setValue(QStringLiteral("remote/autoStart"), enabled);
    s.sync();
}

bool WireHostBridge::tlsEnabled() const {
    QSettings s;
    return s.value(QStringLiteral("remote/tls"), false).toBool();
}
void WireHostBridge::setTlsEnabled(bool enabled) {
    QSettings s;
    s.setValue(QStringLiteral("remote/tls"), enabled);
}

QString WireHostBridge::certPath() const {
    return remoteCertDir() + QStringLiteral("/cert.pem");
}
QString WireHostBridge::keyPath() const {
    return remoteCertDir() + QStringLiteral("/key.pem");
}

QString WireHostBridge::certFingerprint() {
    QFile f(certPath());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray pem = f.readAll();
    if (pem.isEmpty())
        return {};
    // Convert PEM → DER → SHA-256 via openssl x509 -fingerprint -sha256.
    // Simpler than re-implementing the PEM parser inline.
    QProcess p;
    p.start(QStringLiteral("openssl"),
            {QStringLiteral("x509"),
             QStringLiteral("-noout"),
             QStringLiteral("-fingerprint"),
             QStringLiteral("-sha256"),
             QStringLiteral("-in"),
             certPath()});
    if (!p.waitForStarted(2000))
        return {};
    if (!p.waitForFinished(3000)) {
        p.kill();
        return {};
    }
    const QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    // Output format: "sha256 Fingerprint=AB:CD:..." — strip the prefix.
    const int eq = out.indexOf(QChar('='));
    return eq >= 0 ? out.mid(eq + 1) : out;
}

bool WireHostBridge::regenerateSelfSignedCert() {
    QFile::remove(certPath());
    QFile::remove(keyPath());
    return ensureSelfSignedCert();
}

bool WireHostBridge::ensureSelfSignedCert() {
    const QString cert = certPath();
    const QString key = keyPath();
    if (QFileInfo::exists(cert) && QFileInfo::exists(key))
        return true;

    QProcess openssl;
    openssl.start(QStringLiteral("openssl"),
                  {
                      QStringLiteral("req"),
                      QStringLiteral("-x509"),
                      QStringLiteral("-newkey"),
                      QStringLiteral("rsa:2048"),
                      QStringLiteral("-nodes"),
                      QStringLiteral("-days"),
                      QStringLiteral("3650"),
                      QStringLiteral("-keyout"),
                      key,
                      QStringLiteral("-out"),
                      cert,
                      QStringLiteral("-subj"),
                      QStringLiteral("/CN=Verzeta Studio Remote/O=Verzeta"),
                      QStringLiteral("-addext"),
                      QStringLiteral("subjectAltName=IP:0.0.0.0,IP:127.0.0.1,DNS:localhost"),
                  });
    if (!openssl.waitForStarted(2000)) {
        qCWarning(verzetaRemote) << "ensureSelfSignedCert: openssl not found in PATH";
        return false;
    }
    if (!openssl.waitForFinished(15000)) {
        openssl.kill();
        return false;
    }
    if (openssl.exitCode() != 0) {
        qCWarning(verzetaRemote) << "ensureSelfSignedCert: openssl exit" << openssl.exitCode()
                                 << "stderr:" << openssl.readAllStandardError();
        return false;
    }
    QFile::setPermissions(key,
                          QFile::ReadOwner | QFile::WriteOwner);  // 0600
    qCInfo(verzetaRemote) << "ensureSelfSignedCert: created self-signed cert at" << cert;
    return true;
}

QStringList WireHostBridge::lanAddresses() const {
    QStringList out;
    for (const QHostAddress& a : QNetworkInterface::allAddresses()) {
        if (a.isLoopback())
            continue;
        if (a.protocol() != QAbstractSocket::IPv4Protocol)
            continue;
        out.append(a.toString());
    }
    return out;
}

}  // namespace Verzeta::Remote

#include "wire-host-bridge.moc"

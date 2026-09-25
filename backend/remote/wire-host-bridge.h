// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-host-bridge.h
 * @brief Host-side bridge between verzeta-studio and the verzeta-remote
 *        binary. The ONLY new piece of code linked into verzeta-studio
 *        for the remote subsystem.
 *
 *        ARCHITECTURE (what makes this safe by construction):
 *
 *          - WireHostBridge runs on its own dedicated QThread. Network
 *            I/O (QLocalServer / QLocalSocket) happens on that thread,
 *            never on main.
 *          - The bridge subscribes to existing host signals via
 *            Qt::QueuedConnection, Qt's standard cross-thread delivery.
 *            When a host signal fires on main thread, the bridge slot
 *            runs on the bridge thread; the bridge serializes the
 *            payload to JSON and writes it to the LocalSocket. NO
 *            BlockingQueuedConnection. NO polling. NO file watchers.
 *          - For commands FROM the remote binary, the bridge receives
 *            an `invoke` frame and dispatches via QMetaObject::invokeMethod
 *            with Qt::QueuedConnection on EXISTING host Q_INVOKABLE
 *            methods. Fire-and-forget. Main runs the method exactly
 *            like a QML user clicking a button. NO new methods are
 *            added to host classes by this subsystem.
 *          - Inputs are validated on the bridge thread BEFORE crossing
 *            into main. UUID shape, bounded strings, enum membership.
 *          - try/catch wraps the entire IPC frame handler (std::exception
 *            and ...) so a malformed frame can't propagate.
 *          - The bridge holds non-owning QPointer<HostService> for every
 *            service it forwards from. If a host service is destroyed
 *            (it shouldn't be, since they live as long as AppController),
 *            the QPointer goes null and the slot no-ops.
 *
 *        RUNTIME LIFETIME:
 *          - Constructed by AppController at the end of initialize().
 *            ONE std::make_unique<WireHostBridge>() line, ONE
 *            unique_ptr<WireHostBridge> member field.
 *          - The bridge spawns its own QThread, moves itself onto it,
 *            and sets up the QLocalServer there. Main thread is then
 *            free of any wire concerns.
 *          - Destructor on AppController teardown joins the bridge
 *            thread cleanly.
 *
 *        WHAT THIS FILE DOES NOT DO:
 *          - It does not add Q_INVOKABLE to any existing host class.
 *          - It does not add signals to any existing host class.
 *          - It does not modify any existing host source file other
 *            than ONE construction line in AppController.
 *          - It does not run a WebSocket server. That's the job of
 *            the separate verzeta-remote binary which connects here
 *            via QLocalSocket.
 *
 * @layer Service (presentation; remote-access bridge)
 * @dependencies Qt6::Core, Qt6::Network (for QLocalServer/QLocalSocket).
 */

#pragma once

#include <functional>
#include <memory>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>

class QProcess;

class QLocalServer;
class QLocalSocket;
class QThread;

class AgentRegistry;
class AgentService;
class AgentSettingsController;
class AudioService;
class CanvasAiActions;
class CanvasConsoleModel;
class CanvasRunner;
class CanvasService;
class ChatController;
class ConversationController;
class ConversationService;
class FileService;
class HeartbeatConfigService;
class HeartbeatSubagentService;
class ImageService;
class McpService;
class AuditService;
class MembershipService;
class PollService;
class MessageService;
class ProjectTemplateService;
// Workspace mount registry — forward-decl at GLOBAL scope per the same
// Qt-QPointer landmine; the registry lives at TU scope and is QPointer-
// referenced from the bridge's Services struct.
class FolderMountRegistry;
class PlanService;
class RagService;
class SettingsService;
class SkillService;

namespace Search {
class WebSearchProviderRegistry;
}
class TaskController;
class ToolService;

namespace Verzeta::Session {
class SessionRouter;
}

namespace Verzeta::Remote {

class HostBridgePeer;
class WireAuth;

/**
 * @brief Typed reply delivered to a `BridgeClientRpcCallback` when a
 *        host-initiated client-RPC resolves (success, error, timeout,
 *        bridge offline, peer disconnect, or destructor drain). Owned
 *        by the per-peer dispatcher's pending entry; copied into the
 *        callback at resolution time.
 *
 * Mirrors `Verzeta::Remote::ClientRpcReply` from wire-session.h on the
 * daemon side. The bridge and the daemon use distinct struct types so
 * the two binaries can evolve independently; the JSON shape on the
 * IPC frame is the canonical contract.
 */
struct BridgeClientRpcReply {
    /** True only when the wire client replied with `ok:true` and the
     *  data field carries a usable JSON value. False for protocol
     *  errors, timeouts, bridge-offline rejections, peer-disconnect
     *  drains, and client-reported failures. */
    bool ok = false;

    /** Machine-readable error category. Empty on success. Defined
     *  categories: "timeout", "bridge_offline", "bridge_disconnected",
     *  "bridge_destroyed", "too_many_pending", "rate_limited",
     *  "invalid_argument", "client_not_found", "client_error",
     *  "malformed_response". */
    QString errorKind;

    /** Human-readable diagnostic appended to the error. Empty on
     *  success. */
    QString errorDetail;

    /** Client-provided response data on `ok==true`. Null on failure. */
    QJsonValue data;
};

/**
 * @brief Callback invoked when a host-initiated client RPC terminates.
 *        Fired exactly once per dispatched call, when either the matching
 *        `client_rpc_reply` arrives from the daemon, the bridge-side
 *        timeout sweeper expires the entry, the IPC peer disconnects,
 *        or the bridge is torn down. Never fired twice for the same
 *        request. Fires on the bridge worker thread; callers requiring
 *        different thread residency MUST marshal explicitly.
 */
using BridgeClientRpcCallback = std::function<void(const BridgeClientRpcReply&)>;

/**
 * @brief Host-side bridge between verzeta-studio and the verzeta-remote
 *        daemon.
 *
 * Owns the QLocalServer on a dedicated thread, subscribes to existing
 * host signals, and forwards every IPC frame to the daemon. Exposes a
 * Q_INVOKABLE admin surface to QML so the user can start / stop the
 * daemon, manage TLS certificates, and list / revoke paired clients.
 */
class WireHostBridge : public QObject {
    Q_OBJECT
    // Live properties bound by the Settings → Remote Access dialog. The
    // QML side reads these directly and re-renders on remoteRunningChanged.
    Q_PROPERTY(bool remoteRunning READ remoteServerRunning NOTIFY remoteServerRunningChanged)
    Q_PROPERTY(QString remoteUrl READ remoteServerUrl NOTIFY remoteServerRunningChanged)
  public:
    /**
     * @brief Non-owning service pointers the bridge dispatches invokes to
     *        and subscribes to for relayed signals.
     *
     * AppController retains ownership of every object. Any pointer may be
     * null: invokes naming that target then fail to resolve, and the
     * matching signal subscriptions are skipped. Unless noted otherwise,
     * each field is the object an invoke resolves to when its target
     * names the matching Targets constant.
     *
     * The bridge subscribes with Qt::QueuedConnection because it lives
     * on its own thread; Qt marshals each signal to it, so no sender
     * ever blocks on the bridge.
     */
    struct Services {
        QPointer<MessageService> msg;  ///< Targets::MessageService; source of message signals.
        QPointer<ConversationService>
            convSvc;  ///< Targets::ConversationService; source of conversation signals.
        QPointer<ConversationController>
            convCtrl;  ///< Targets::Conversations; source of the rename signal.
        /// Targets::ChatController for the local session. Wire clients
        /// get their own ChatController through sessionRouter instead.
        QPointer<ChatController> chatCtrl;
        /// Targets::AgentSettings for the local session; source of the
        /// local settings signals, and read for the active provider and
        /// model snapshot sent with model changes.
        QPointer<AgentSettingsController> agentSettings;
        QPointer<AgentRegistry> agentRegistry;  ///< Targets::AgentRegistry.
        QPointer<AgentService> agentService;    ///< Targets::AgentService; source of agent signals.
        QPointer<ToolService> toolService;      ///< Targets::ToolService.
        QPointer<McpService> mcpService;        ///< Targets::McpService.
        QPointer<SkillService> skillService;    ///< Targets::SkillService.
        QPointer<MembershipService>
            membershipService;  ///< Targets::MembershipService; source of membership signals.
        QPointer<PollService> pollService;  ///< Targets::PollService; source of poll signals.
        /// Targets::AuditService; its activityLogged signal is relayed as
        /// `activity.logged`. Declared with a global-scope `::` because an
        /// unqualified name would resolve to Verzeta::Remote::AuditService,
        /// and QPointer needs the complete type.
        QPointer<::AuditService> auditService;
        /// Targets::ProjectTemplateService; source of project template
        /// signals. Global-scope for the same reason as auditService.
        QPointer<::ProjectTemplateService> projectTemplateService;
        /// Targets::FolderMountRegistry. Process-wide registry of
        /// client-owned workspace mounts, keyed by folder; serves the
        /// `workspace.mount.*` ops.
        QPointer<::FolderMountRegistry> folderMountRegistry;
        QPointer<HeartbeatConfigService> heartbeatConfig;  ///< Targets::HeartbeatConfigService;
                                                           ///< source of heartbeat config signals.
        QPointer<HeartbeatSubagentService>
            heartbeatSubagent;                    ///< Targets::HeartbeatSubagentService.
        QPointer<TaskController> taskController;  ///< Targets::TaskController.
        QPointer<RagService> ragService;          ///< Targets::RagService.
        QPointer<PlanService> planService;        ///< Targets::PlanService; source of plan signals.
        QPointer<FileService> fileService;        ///< Targets::FileService.
        QPointer<ImageService> imageService;    ///< Targets::ImageService; source of image signals.
        QPointer<AudioService> audioService;    ///< Targets::AudioService; source of audio signals.
        QPointer<CanvasService> canvasService;  ///< Targets::Canvas; source of canvas signals.
        QPointer<CanvasRunner> canvasRunner;    ///< Targets::CanvasRunner; source of run signals.
        QPointer<CanvasAiActions> canvasAiActions;  ///< Targets::CanvasAiActions.
        QPointer<CanvasConsoleModel>
            canvasConsole;  ///< Targets::CanvasConsoleModel; source of console signals.
        QPointer<SettingsService> settings;  ///< Targets::SettingsService.
        QPointer<Search::WebSearchProviderRegistry>
            webSearchRegistry;  ///< Targets::WebSearchProviders.
        /// Per-client ChatController registry. The bridge looks up or
        /// creates a wire client's own ChatController and
        /// AgentSettingsController here, and destroys them all when the
        /// IPC peer disconnects. A raw pointer because AppController owns
        /// it; null is allowed (tests), and every use checks it first.
        Verzeta::Session::SessionRouter* sessionRouter = nullptr;
    };

    /**
     * @brief Constructs the bridge on the main thread and immediately
     *        spawns its dedicated QThread.
     * @param services  Bundle of non-owning service pointers (any
     *                  field may be null; the corresponding signal
     *                  subscriptions no-op when absent).
     * @param parent    Optional Qt parent.
     */
    explicit WireHostBridge(Services services, QObject* parent = nullptr);
    ~WireHostBridge() override;

    WireHostBridge(const WireHostBridge&) = delete;
    WireHostBridge& operator=(const WireHostBridge&) = delete;

    /**
     * @brief Returns the QLocalSocket path the bridge listens on.
     * @returns Absolute socket path. Diagnostic only; the remote
     *          process discovers the path through the helper in
     *          wire-protocol.h.
     */
    QString listenSocketName() const;

    // -----------------------------------------------------------------
    // Admin API for QML's Remote Access dialog. Each method spawns the
    // already-shipped `verzeta-remote` binary (CLI flags) or starts /
    // stops it as a child process. WireHostBridge itself runs on the
    // main thread, so these Q_INVOKABLEs are safe to call directly
    // from QML — only the inner HostBridgePeer is on a worker thread.
    // -----------------------------------------------------------------

    /**
     * @brief Returns the absolute path of the `verzeta-remote` binary
     *        that ships next to verzeta-studio.
     * @returns Path string, or empty when the file does not exist
     *          (e.g. running from a half-installed tree).
     */
    Q_INVOKABLE QString remoteBinaryPath() const;

    /**
     * @brief Reports whether the bridge has spawned the remote daemon
     *        under our process group.
     * @returns True when the daemon child process is alive. Drives the
     *          "Remote ON / OFF" switch in the dialog.
     */
    Q_INVOKABLE bool remoteServerRunning() const;

    /**
     * @brief Returns the WebSocket URL the daemon listens on.
     * @returns `ws://bind:port/` string, or empty when the daemon is
     *          not running. Android pastes this into Add Host.
     */
    Q_INVOKABLE QString remoteServerUrl() const;

    /**
     * @brief Starts the verzeta-remote daemon as a child process.
     * @param bindAddr  Bind address. Empty falls back to the persisted
     *                  setting (or `0.0.0.0` on first run).
     * @param port      TCP port. Zero falls back to the persisted
     *                  setting (or 9180 on first run).
     * @param useTls    When true, starts the daemon with `--tls --cert
     *                  --key` pointing at `certPath` / `keyPath`.
     * @param certPath  Server certificate path (when `useTls`).
     * @param keyPath   Private key path (when `useTls`).
     * @returns True on successful spawn. Idempotent: a no-op when the
     *          daemon is already running.
     */
    Q_INVOKABLE bool startRemoteServer(const QString& bindAddr = QString(),
                                       int port = 0,
                                       bool useTls = false,
                                       const QString& certPath = QString(),
                                       const QString& keyPath = QString());

    /**
     * @brief Terminates the daemon child process.
     *
     * Sends SIGTERM, then SIGKILL on timeout. Idempotent: a no-op when
     * the daemon is not running.
     */
    Q_INVOKABLE void stopRemoteServer();

    // -- Persistent admin settings (QSettings under "remote/*" key) --

    /**
     * @brief Returns the persisted bind address.
     * @returns Stored bind string. Defaults to "0.0.0.0".
     */
    Q_INVOKABLE QString preferredBindAddr() const;

    /**
     * @brief Persists a new bind address for next daemon start.
     * @param addr  Bind address string (e.g. "0.0.0.0", "127.0.0.1").
     */
    Q_INVOKABLE void setPreferredBindAddr(const QString& addr);

    /**
     * @brief Returns the persisted TCP port.
     * @returns Stored port number. Defaults to 9180.
     */
    Q_INVOKABLE int preferredPort() const;

    /**
     * @brief Persists a new TCP port for next daemon start.
     * @param port  Port number (1-65535).
     */
    Q_INVOKABLE void setPreferredPort(int port);

    /**
     * @brief Returns whether the daemon should auto-start at app launch.
     * @returns True when auto-start is enabled. Defaults to false.
     */
    Q_INVOKABLE bool autoStartEnabled() const;

    /**
     * @brief Persists the auto-start flag.
     * @param enabled  True to spawn the daemon at app launch.
     */
    Q_INVOKABLE void setAutoStartEnabled(bool enabled);

    /**
     * @brief Returns the persisted TLS toggle.
     * @returns True when the daemon should run TLS. Defaults to false.
     */
    Q_INVOKABLE bool tlsEnabled() const;

    /**
     * @brief Persists the TLS toggle.
     * @param enabled  True to start the daemon under TLS at next start.
     */
    Q_INVOKABLE void setTlsEnabled(bool enabled);

    /**
     * @brief Returns the persisted server certificate path.
     * @returns PEM path. Defaults to `<AppData>/verzeta-remote/cert.pem`.
     *
     * The file is auto-generated by ensureSelfSignedCert() when
     * missing and TLS is enabled.
     */
    Q_INVOKABLE QString certPath() const;

    /**
     * @brief Returns the persisted private-key path.
     * @returns PEM path; companion to certPath().
     */
    Q_INVOKABLE QString keyPath() const;

    /**
     * @brief Computes the SHA-256 fingerprint of the persisted cert.
     * @returns Human-readable colon-separated hex (e.g. "AB:CD:..."),
     *          or empty when no cert file exists. Android clients pin
     *          this string.
     */
    Q_INVOKABLE QString certFingerprint();

    /**
     * @brief Generates a self-signed RSA-2048 certificate when missing.
     * @returns True on success, or when cert + key already exist.
     *
     * Shells out to `openssl req -x509 -newkey rsa:2048 -nodes
     * -days 3650`.
     */
    Q_INVOKABLE bool ensureSelfSignedCert();

    /**
     * @brief Force-regenerates the self-signed cert + key, overwriting
     *        whatever is on disk.
     * @returns True on success.
     *
     * Used by the dialog's "Regenerate certificate" button when the
     * user wants a fresh fingerprint (e.g. after a security audit, or
     * to rotate the pin on a paired Android device).
     */
    Q_INVOKABLE bool regenerateSelfSignedCert();

    /**
     * @brief Returns the LAN IPv4 addresses of this host.
     * @returns List of dotted-quad addresses for the dialog's
     *          "Android pastes one of these" hint. Filters out
     *          loopback and IPv6 link-local addresses.
     */
    Q_INVOKABLE QStringList lanAddresses() const;

    /**
     * @brief Runs `verzeta-remote --pair-code` synchronously.
     * @returns Six-digit code printed to stdout, or empty on failure.
     *
     * The wire binary persists the code in `wire-state.db`, so a
     * subsequently started daemon (or one already running) consumes
     * the same code when Android pairs.
     */
    Q_INVOKABLE QString generatePairCode();

    /**
     * @brief Runs `verzeta-remote --list-clients` synchronously.
     * @returns One QVariantMap per client with keys: `id`, `name`,
     *          `created_at`, `last_seen_at`, `revoked`.
     */
    Q_INVOKABLE QVariantList listPairedClients();

    /**
     * @brief Runs `verzeta-remote --revoke \<id\>` synchronously.
     * @param clientId  Paired client UUID to revoke.
     * @returns True on successful revocation.
     */
    Q_INVOKABLE bool revokeClient(const QString& clientId);

    /** Maximum number of in-flight host-initiated client RPCs the
     *  bridge holds against the active IPC peer. Beyond this cap
     *  `dispatchClientRequest` rejects synchronously so a misbehaving
     *  caller cannot exhaust the bridge's heap through accumulated
     *  pending callbacks. Mirrors WireSession's per-session cap. */
    static constexpr int kMaxPendingBridgeRpc = 256;

    /** Default deadline for a host-initiated client RPC. Matches
     *  WireSession::kClientRpcDefaultTimeoutMs to keep semantics
     *  symmetric across the bridge ↔ daemon boundary. */
    static constexpr int kBridgeRpcDefaultTimeoutMs = 15000;

    /** Period of the bridge-side timeout sweeper. Mirrors WireSession's
     *  500 ms cadence; sub-second worst-case notification without
     *  burning CPU. */
    static constexpr int kBridgeRpcSweepIntervalMs = 500;

    /**
     * @brief Issues a host-initiated client RPC against the paired-
     *        client whose authenticated UUID matches `clientId`.
     *        Marshals the dispatch onto the bridge worker thread,
     *        writes an `IpcType::ClientRpcDispatch` frame to the
     *        active IPC peer, records the pending entry, and resolves
     *        the callback when the matching `IpcType::ClientRpcReply`
     *        arrives, the bridge-side timeout sweeper expires the
     *        entry, the IPC peer disconnects, or the bridge is torn
     *        down.
     *
     *        Thread residency: callable from any thread. Validation
     *        and synchronous rejection (empty op, null callback) run
     *        on the caller's thread; the dispatch and IPC write run
     *        on the bridge worker thread (via
     *        `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`).
     *        Completion callback fires on the bridge worker thread;
     *        callers requiring different thread residency MUST marshal
     *        explicitly.
     *
     *        Capacity + rate: the bridge caps in-flight requests at
     *        `kMaxPendingBridgeRpc` per peer; overflow rejects with
     *        `kind="too_many_pending"`. No bridge-side outbound rate-
     *        limit (the daemon-side WireSession imposes one once the
     *        frame is translated to the WS layer; bridge-side traffic
     *        is intra-process IPC where bandwidth is not the bottleneck).
     *
     *        Peer-disconnect drain: when the IPC peer (daemon) detaches
     *        (clean shutdown, crash, or supersede), every still-
     *        pending callback fires exactly once with
     *        `kind="bridge_disconnected"` so callers' threads unblock
     *        cleanly without leaking std::function captures.
     *
     * @param clientId   Authenticated UUID of the target paired-client.
     *                   Empty rejects with `kind="invalid_argument"`.
     * @param op         Wire op name routed at the WireSession on the
     *                   daemon (e.g. "vfs.read"). Must be non-empty.
     * @param args       Request-specific JSON object passed verbatim
     *                   to the wire client.
     * @param callback   Resolver fired exactly once. Move-captured;
     *                   must be non-null.
     * @param timeoutMs  Deadline (ms) after which the sweeper fires
     *                   the callback with `kind="timeout"`. Clamped
     *                   to a minimum of 100 ms; default
     *                   `kBridgeRpcDefaultTimeoutMs`.
     */
    void dispatchClientRequest(const QString& clientId,
                               const QString& op,
                               const QJsonObject& args,
                               BridgeClientRpcCallback callback,
                               int timeoutMs = kBridgeRpcDefaultTimeoutMs);

  signals:
    /**
     * @brief Emitted whenever the daemon's running state flips.
     *
     * The dialog binds its UI to this so the switch follows external
     * events (auto-restart on crash, manual stop from another client).
     */
    void remoteServerRunningChanged();

  private slots:
    /**
     * @brief Internal: invoked when the daemon QProcess exits.
     */
    void onRemoteServerProcessFinished();

  private:
    /**
     * @brief Returns the lazily-opened WireAuth instance, or nullptr
     *        when wire-state.db cannot be opened.
     */
    WireAuth* authOrNull();

    Services m_services;
    QThread* m_thread = nullptr;
    std::unique_ptr<HostBridgePeer> m_peer;  // owned on bridge thread

    QProcess* m_remoteServerProcess = nullptr;
    QString m_remoteServerBindAddr;
    int m_remoteServerPort = 0;

    std::unique_ptr<WireAuth> m_auth;
};

}  // namespace Verzeta::Remote

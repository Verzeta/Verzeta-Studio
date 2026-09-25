// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-session.h
 * @brief Per-WebSocket-client session inside the verzeta-remote process.
 *
 *        Owns the QWebSocket. Validates inbound frames. Translates
 *        ops into invokeAsync / invokeFireAndForget calls on the
 *        WireHostClient (which owns the QLocalSocket to the host
 *        bridge). Subscribes to host signals via the WireHostClient
 *        and forwards filtered events to its socket.
 *
 *        Per-session caps: 256 conversation subscriptions max, 200
 *        ops/sec rate limit (sliding window).
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::WebSockets, WireAuth, WireHostClient,
 *               WireDbReader.
 */

#pragma once

#include <QTimer>

#include <functional>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>

class QWebSocket;

namespace Verzeta::Remote {

class WireAuth;
class WireDbReader;
class WireHostClient;

/**
 * @brief Typed reply delivered to a `ClientRpcCallback` when a host-
 *        initiated request resolves (success, error, timeout, or
 *        session-close drain). Owned by the dispatcher's per-pending
 *        entry; copied into the callback at resolution time.
 */
struct ClientRpcReply {
    /** True only when the client replied with `ok:true` and the data
     *  field carries a usable JSON value. False for protocol errors,
     *  timeouts, session-close drains, and client-reported failures. */
    bool ok = false;

    /** Machine-readable error category. Empty on success. Defined
     *  categories: "timeout", "session_closed", "session_destroyed",
     *  "too_many_pending", "client_error" (echoes the client's
     *  error.kind verbatim), "malformed_response" (host could not
     *  parse the client's reply). */
    QString errorKind;

    /** Human-readable diagnostic appended to the error. Empty on
     *  success. */
    QString errorDetail;

    /** Client-provided response data on `ok==true`. Null on failure. */
    QJsonValue data;
};

/**
 * @brief Callback invoked when a host-initiated client request
 *        terminates. Fired exactly once per outbound request, when either
 *        the matching `client_response` arrives, the timeout sweeper
 *        expires the entry, or the session is torn down. Never fired
 *        twice for the same request. May fire on the wire-server
 *        thread; callers that need different thread residency must
 *        marshal explicitly.
 */
using ClientRpcCallback = std::function<void(const ClientRpcReply&)>;

/**
 * @brief Per-WebSocket-client session. Owns the QWebSocket, dispatches
 *        the wire op surface, and routes invokes per-client through
 *        the host bridge.
 */
class WireSession : public QObject {
    Q_OBJECT
  public:
    /**
     * @brief Constructs the session with its socket and the shared
     *        service references.
     * @param socket      QWebSocket owned by this session (parent is
     *                    reassigned). Must not be null.
     * @param auth        Non-owning auth store.
     * @param hostClient  Non-owning IPC client to the host bridge.
     * @param db          Non-owning read-only DB reader.
     * @param parent      Optional Qt parent.
     */
    WireSession(QWebSocket* socket,
                WireAuth* auth,
                WireHostClient* hostClient,
                WireDbReader* db,
                QObject* parent = nullptr);
    ~WireSession() override;

    /** Maximum number of in-flight host-initiated requests per session.
     *  Beyond this cap `sendClientRequest` rejects immediately so a
     *  misbehaving caller cannot exhaust memory through accumulated
     *  pending callbacks. */
    static constexpr int kMaxPendingClientRequests = 256;

    /** Maximum number of host-initiated requests this session may emit
     *  per sliding 1-second window. Defends against host-side flood
     *  caused by a runaway agent / cascade producing rapid
     *  tool-driven I/O. */
    static constexpr int kMaxOutboundOpsPerSecond = 500;

    /** Default deadline for a host-initiated request. Matches the
     *  inbound async-invoke timeout to keep behaviour symmetric. */
    static constexpr int kClientRpcDefaultTimeoutMs = 15000;

    /** Period of the timeout sweeper that walks the pending map and
     *  fires expired callbacks. 500 ms gives sub-second worst-case
     *  notification without burning CPU. Same cadence as the host-
     *  client invoke timeout sweeper. */
    static constexpr int kClientRpcSweepIntervalMs = 500;

    /**
     * @brief Reports whether the session has completed pairing /
     *        bearer-token auth.
     * @returns True when `m_clientId` is non-empty.
     */
    bool isAuthenticated() const noexcept { return !m_clientId.isEmpty(); }

    /**
     * @brief Returns the paired-client UUID this session is bound to.
     * @returns Client UUID, or empty when pre-auth.
     */
    QString clientId() const noexcept { return m_clientId; }

    /**
     * @brief Reports whether the connected client advertised a named
     *        wire capability during authentication.
     *
     * Clients may declare optional capabilities (for example the ability
     * to execute shell commands inside a mounted workspace) in the
     * `capabilities` array of their `auth.token` / `auth.pair` request.
     * A capability the client never advertised gates host-initiated
     * routing: a host request that depends on the capability is refused
     * for this session so the host can fall back to a local code path
     * rather than dispatching work the client cannot perform. Clients
     * that advertise nothing report false for every capability.
     *
     * @param capability  Capability token, e.g. `"vfs.execute"`.
     * @returns True when the client advertised @p capability.
     */
    bool hasCapability(const QString& capability) const noexcept {
        return m_capabilities.contains(capability);
    }

    /**
     * @brief Issue a host-initiated request to the connected client and
     *        register a callback for its reply. Writes a JSON envelope
     *        of type `request` over the WebSocket; records the pending
     *        entry in a per-session map keyed on a freshly-generated
     *        request UUID; resolves the callback when the matching
     *        `client_response` arrives, the timeout sweeper expires
     *        the entry, or the session is destroyed.
     *
     *        Thread residency: socket I/O performed on the session's
     *        event-thread, which is also the timer thread. Callers that
     *        invoke this method from another thread MUST marshal via
     *        `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)`.
     *
     *        Capacity: the session caps in-flight requests at
     *        `kMaxPendingClientRequests` (256). Issuing beyond the cap
     *        rejects immediately with `kind="too_many_pending"`.
     *
     *        Outbound rate-limit: the session caps host-emitted
     *        requests at `kMaxOutboundOpsPerSecond` (500) per sliding
     *        1-second window. Exceeding the rate rejects with
     *        `kind="rate_limited"`.
     *
     *        Session-close drain: when the WebSocket closes (or the
     *        session is destroyed) every still-pending callback is
     *        invoked exactly once with `kind="session_closed"` /
     *        `kind="session_destroyed"` so callers' threads unblock
     *        cleanly without leaking std::function captures.
     *
     * @param op         Logical command name; consumed by the client's
     *                   dispatcher. Must be non-empty.
     * @param args       Request-specific payload object. May be empty.
     * @param callback   Resolver fired exactly once. Move-captured;
     *                   must be non-null.
     * @param timeoutMs  Time budget after which the timeout sweeper
     *                   fires the callback with `kind="timeout"`.
     *                   Clamped to a minimum of 100 ms; default
     *                   `kClientRpcDefaultTimeoutMs` (15 000 ms).
     * @returns The absolute deadline (epoch ms) at which the timeout
     *          fires, on successful enqueue. Returns 0 when the call
     *          was rejected because of capacity, the rate limit, a
     *          closed socket or an empty op; the callback has already been fired with
     *          the matching error in that case.
     */
    qint64 sendClientRequest(const QString& op,
                             const QJsonObject& args,
                             ClientRpcCallback callback,
                             int timeoutMs = kClientRpcDefaultTimeoutMs);

    /**
     * @brief Force-close this session immediately.
     *        Used by the server's supersede path when a new session
     *        binds to the same client_id as this one.
     *
     *        Drains any pending host-initiated requests with the
     *        supplied `kind` so callers blocked on those futures see
     *        the resolution before the session is removed from the
     *        server's pool.  Closes the underlying WebSocket; the
     *        normal disconnect signal then fires and the session is
     *        removed and scheduled for deletion via the existing
     *        `onSessionDisconnected` path.
     *
     * @param kind  Drain reason ("session_superseded", etc).
     */
    void forceClose(const QString& kind);

  signals:
    /**
     * @brief Emitted when the WebSocket disconnects.
     * @param self  Pointer to this session for the server's
     *              housekeeping.
     */
    void disconnected(WireSession* self);

    /**
     * @brief Emitted when this session's `m_clientId` is bound to a
     *        client identity by `auth.pair` or `auth.token`.
     *
     *        The server uses this to evict any OTHER session in
     *        `m_sessions` whose `clientId()` matches, so a same-client
     * reconnect replaces any zombie session whose socket
     *        disconnect has not yet been observed.  Without this
     *        eviction, `sessionForClientId` may return the dead
     *        zombie first on its linear scan and every host-initiated
     *        RPC (vfs.write etc.) dispatches into a closed socket.
     *
     * @param clientId  Bound identity (UUID).
     * @param self      Pointer to this session.
     */
    void clientIdRebound(const QString& clientId, WireSession* self);

  private slots:
    /**
     * @brief QWebSocket::textMessageReceived handler that validates and
     *        dispatches inbound op frames.
     * @param text  UTF-8 frame body.
     */
    void onTextMessageReceived(const QString& text);

    /**
     * @brief QWebSocket::disconnected handler that emits this session's
     *        `disconnected` signal so the server can reap it.
     */
    void onSocketDisconnected();

    /**
     * @brief WireHostClient::hostSignal handler that converts the bridge
     *        signal into the matching wire event and forwards to the
     *        client (with conv-subscription / DB enrichment filters).
     * @param name  Signals::* identifier.
     * @param args  Signal payload.
     */
    void onHostSignal(const QString& name, const QJsonObject& args);

  private:
    void dispatchOp(const QString& op, const QString& reqId, const QJsonObject& params);

    void sendOk(const QString& reqId, const QJsonValue& data);
    void sendErr(const QString& reqId, const QString& kind, const QString& detail);
    void sendEvent(const QString& event, const QJsonObject& data);
    void sendHello();

    void invokeFire(const QString& target, const QString& method, const QJsonArray& args);

    /**
     * @brief Honest-ack guard for fire-and-forget ops: when the
     *        daemon's host-bridge IPC is down, invokeFire drops the
     *        call silently, and acking {queued:true} would lie to the
     *        client (deletes that come back, chat messages that
     *        vanish). Replies server_error "host bridge offline" and
     *        returns false when the host is unreachable.
     * @param reqId Request id to reply on when offline.
     * @returns true iff the host bridge is connected and the
     *          fire-and-forget will actually be delivered.
     */
    bool requireHostOnline(const QString& reqId);
    void invokeAsyncSession(const QString& target,
                            const QString& method,
                            const QJsonArray& args,
                            const QString& returnType,
                            int timeoutMs,
                            std::function<void(bool, const QJsonValue&, const QString&)> callback);
    void
    invokePropertyGetSession(const QString& target,
                             const QString& property,
                             int timeoutMs,
                             std::function<void(bool, const QJsonValue&, const QString&)> callback);
    void
    invokePropertySetSession(const QString& target,
                             const QString& property,
                             const QString& valueType,
                             const QJsonValue& value,
                             int timeoutMs,
                             std::function<void(bool, const QJsonValue&, const QString&)> callback);

    // Auth ops
    void opPing(const QString& reqId, const QJsonObject&);
    void opAuthPair(const QString& reqId, const QJsonObject&);
    void opAuthToken(const QString& reqId, const QJsonObject&);
    void opAuthMe(const QString& reqId, const QJsonObject&);
    void opAuthRevokeSelf(const QString& reqId, const QJsonObject&);
    void opClientsList(const QString& reqId, const QJsonObject&);
    void opClientsRevoke(const QString& reqId, const QJsonObject&);

    // Subscription ops
    void opMsgSubscribe(const QString& reqId, const QJsonObject&);
    void opMsgUnsubscribe(const QString& reqId, const QJsonObject&);

    // Conversation reads
    void opConvList(const QString& reqId, const QJsonObject&);
    void opConvGet(const QString& reqId, const QJsonObject&);
    void opMsgList(const QString& reqId, const QJsonObject&);

    // Conversation writes (fire-and-forget through bridge)
    void opMsgSend(const QString& reqId, const QJsonObject&);
    void opMsgStop(const QString& reqId, const QJsonObject&);
    void opConvCreate(const QString& reqId, const QJsonObject&);
    void opConvCreateWithAgent(const QString& reqId, const QJsonObject&);
    void opConvRename(const QString& reqId, const QJsonObject&);
    void opConvSetPinned(const QString& reqId, const QJsonObject&);
    void opConvDelete(const QString& reqId, const QJsonObject&);
    void opConvMoveToFolder(const QString& reqId, const QJsonObject&);
    void opConvIsGroup(const QString& reqId, const QJsonObject&);
    void opConvGroupMembers(const QString& reqId, const QJsonObject&);
    void opConvFolderId(const QString& reqId, const QJsonObject&);
    void opConvOpen(const QString& reqId, const QJsonObject&);

    // Folders
    void opFolderListProjects(const QString& reqId, const QJsonObject&);
    void opFolderListAll(const QString& reqId, const QJsonObject&);
    void opFolderConversations(const QString& reqId, const QJsonObject&);
    void opFolderCreate(const QString& reqId, const QJsonObject&);
    void opFolderRename(const QString& reqId, const QJsonObject&);
    void opFolderDelete(const QString& reqId, const QJsonObject&);
    void opFolderInfo(const QString& reqId, const QJsonObject&);
    void opFolderUpdateMetadata(const QString& reqId, const QJsonObject&);
    void opGroupCreate(const QString& reqId, const QJsonObject&);

    // Models
    void opModelsCatalog(const QString& reqId, const QJsonObject&);
    void opModelsSetActive(const QString& reqId, const QJsonObject&);
    void opModelsActive(const QString& reqId, const QJsonObject&);
    void opModelsForProvider(const QString& reqId, const QJsonObject&);
    void opSearchProviders(const QString& reqId, const QJsonObject&);
    void opSearchActive(const QString& reqId, const QJsonObject&);
    void opSearchSetActive(const QString& reqId, const QJsonObject&);

    // Read-only catalogs
    void opAgentList(const QString& reqId, const QJsonObject&);
    void opAgentGet(const QString& reqId, const QJsonObject&);
    void opToolList(const QString& reqId, const QJsonObject&);
    void opMcpList(const QString& reqId, const QJsonObject&);
    void opMcpServerTools(const QString& reqId, const QJsonObject&);
    void opSkillList(const QString& reqId, const QJsonObject&);
    void opSkillGet(const QString& reqId, const QJsonObject&);

    void opPollList(const QString& reqId, const QJsonObject&);
    void opPollResults(const QString& reqId, const QJsonObject&);
    void opPollStart(const QString& reqId, const QJsonObject&);
    void opPollVote(const QString& reqId, const QJsonObject&);
    void opPollClose(const QString& reqId, const QJsonObject&);

    void opActivityForProject(const QString& reqId, const QJsonObject&);
    void opActivityForConversation(const QString& reqId, const QJsonObject&);
    void opActivityByTurn(const QString& reqId, const QJsonObject&);

    void opProjectTemplateList(const QString& reqId, const QJsonObject&);
    void opProjectTemplateListLanding(const QString& reqId, const QJsonObject&);
    void opProjectTemplateGet(const QString& reqId, const QJsonObject&);
    void opProjectTemplateRoster(const QString& reqId, const QJsonObject&);
    void opProjectTemplateCreateProject(const QString& reqId, const QJsonObject&);
    void opProjectTemplatePinUser(const QString& reqId, const QJsonObject&);
    void opProjectTemplateSaveAsNew(const QString& reqId, const QJsonObject&);
    void opProjectTemplateDeleteUser(const QString& reqId, const QJsonObject&);

    // Workspace mount registry — 5 ops mirror the FolderMountRegistry
    // Q_INVOKABLE surface 1:1. Each dispatches via the bridge to the
    // process-wide FolderMountRegistry target; replies carry the
    // typed `{ok, error?}` map back to the calling client.
    void opWorkspaceMountRegister(const QString& reqId, const QJsonObject&);
    void opWorkspaceMountUnregister(const QString& reqId, const QJsonObject&);
    void opWorkspaceMountUpdateTree(const QString& reqId, const QJsonObject&);
    void opWorkspaceMountUpdateTier(const QString& reqId, const QJsonObject&);
    void opWorkspaceMountForFolder(const QString& reqId, const QJsonObject&);

    // Membership
    void opFolderMembers(const QString& reqId, const QJsonObject&);
    void opFolderMembersSet(const QString& reqId, const QJsonObject&);
    void opFolderMemberAdd(const QString& reqId, const QJsonObject&);
    void opFolderMemberOverrideSet(const QString& reqId, const QJsonObject&);
    void opFolderMemberRemove(const QString& reqId, const QJsonObject&);
    void opConvMembers(const QString& reqId, const QJsonObject&);
    void opConvMembersSet(const QString& reqId, const QJsonObject&);
    void opConvMemberAdd(const QString& reqId, const QJsonObject&);
    void opConvMemberRemove(const QString& reqId, const QJsonObject&);
    void opConvCoordinatorSet(const QString& reqId, const QJsonObject&);

    // Project docs / kickoff
    void opFolderDocuments(const QString& reqId, const QJsonObject&);
    void opFolderDocumentUpload(const QString& reqId, const QJsonObject&);
    void opFolderDocumentRemove(const QString& reqId, const QJsonObject&);
    void opFolderKickoffIndividual(const QString& reqId, const QJsonObject&);
    void opFolderKickoffGroup(const QString& reqId, const QJsonObject&);
    void opFolderMemberChatOpen(const QString& reqId, const QJsonObject&);

    // Preferred skills
    void opSkillPreferredList(const QString& reqId, const QJsonObject&);
    void opSkillPreferredSet(const QString& reqId, const QJsonObject&);
    void opSkillExposeOnly(const QString& reqId, const QJsonObject&);
    void opSkillSetExposeOnly(const QString& reqId, const QJsonObject&);
    void opSkillOverrideParent(const QString& reqId, const QJsonObject&);
    void opSkillSetOverrideParent(const QString& reqId, const QJsonObject&);

    // Heartbeats
    void opHbConfigsForFolder(const QString& reqId, const QJsonObject&);
    void opHbConfigsForConv(const QString& reqId, const QJsonObject&);
    void opHbConfigById(const QString& reqId, const QJsonObject&);
    void opHbUpsert(const QString& reqId, const QJsonObject&);
    void opHbRemove(const QString& reqId, const QJsonObject&);
    void opHbRunNow(const QString& reqId, const QJsonObject&);
    void opHbCancelRun(const QString& reqId, const QJsonObject&);
    void opHbRecentRuns(const QString& reqId, const QJsonObject&);
    void opHbNextFires(const QString& reqId, const QJsonObject&);
    void opHbConfigChanges(const QString& reqId, const QJsonObject&);
    void opHbConfigStatus(const QString& reqId, const QJsonObject&);
    void opHbManualPost(const QString& reqId, const QJsonObject&);
    void opHbManualDismiss(const QString& reqId, const QJsonObject&);
    void opHbCapForConv(const QString& reqId, const QJsonObject&);
    void opHbCapForFolder(const QString& reqId, const QJsonObject&);

    // Per-conv settings
    void opConvSettingsGet(const QString& reqId, const QJsonObject&);
    void opConvSettingsSave(const QString& reqId, const QJsonObject&);
    void opConvPrimaryAgent(const QString& reqId, const QJsonObject&);
    void opConvPrimaryAgentSet(const QString& reqId, const QJsonObject&);
    void opConvHbGate(const QString& reqId, const QJsonObject&);
    void opConvHbGateSet(const QString& reqId, const QJsonObject&);
    void opAgentPattern(const QString& reqId, const QJsonObject&);
    void opAgentPatternSet(const QString& reqId, const QJsonObject&);
    void opAgentRequireConfirm(const QString& reqId, const QJsonObject&);
    void opAgentRequireConfirmSet(const QString& reqId, const QJsonObject&);
    void opToolsEnabled(const QString& reqId, const QJsonObject&);
    void opToolsEnabledSet(const QString& reqId, const QJsonObject&);

    // Plans / tasks
    void opPlanListForConv(const QString& reqId, const QJsonObject&);
    void opPlanGet(const QString& reqId, const QJsonObject&);
    void opTaskStart(const QString& reqId, const QJsonObject&);
    void opPlanStop(const QString& reqId, const QJsonObject&);
    void opPlanStopAllInConv(const QString& reqId, const QJsonObject&);
    void opStepRetry(const QString& reqId, const QJsonObject&);
    void opStepOverrideDone(const QString& reqId, const QJsonObject&);
    void opStepSkip(const QString& reqId, const QJsonObject&);

    // Tool-call activity + agent confirmation
    void opToolCallListForConv(const QString& reqId, const QJsonObject&);
    void opToolCallListForMessage(const QString& reqId, const QJsonObject&);
    void opToolCallListForTurn(const QString& reqId, const QJsonObject&);
    void opToolCallGet(const QString& reqId, const QJsonObject&);
    void opToolCallApprove(const QString& reqId, const QJsonObject&);
    void opToolCallDeny(const QString& reqId, const QJsonObject&);

    // Attachments / artifacts / images / audio
    void opAttachmentListForMsg(const QString& reqId, const QJsonObject&);
    void opAttachmentGet(const QString& reqId, const QJsonObject&);
    void opArtifactListForConv(const QString& reqId, const QJsonObject&);
    void opArtifactDownload(const QString& reqId, const QJsonObject&);
    void opImageListForConv(const QString& reqId, const QJsonObject&);
    void opImageDownload(const QString& reqId, const QJsonObject&);
    void opAudioListForConv(const QString& reqId, const QJsonObject&);
    void opAudioDownload(const QString& reqId, const QJsonObject&);
    void opMsgSendWithAttachments(const QString& reqId, const QJsonObject&);

    // Canvas
    void opCanvasActiveForConv(const QString& reqId, const QJsonObject&);
    void opCanvasHistoryForConv(const QString& reqId, const QJsonObject&);
    void opCanvasDiskMirrorPath(const QString& reqId, const QJsonObject&);
    void opCanvasSuggestedExportName(const QString& reqId, const QJsonObject&);
    void opCanvasReadSlice(const QString& reqId, const QJsonObject&);
    void opCanvasOpen(const QString& reqId, const QJsonObject&);
    void opCanvasEdit(const QString& reqId, const QJsonObject&);
    void opCanvasClose(const QString& reqId, const QJsonObject&);
    void opCanvasSwitchTo(const QString& reqId, const QJsonObject&);
    void opCanvasToolsList(const QString& reqId, const QJsonObject&);
    void opCanvasToolsRun(const QString& reqId, const QJsonObject&);
    void opCanvasAiActionsList(const QString& reqId, const QJsonObject&);
    void opCanvasAiActionsTrigger(const QString& reqId, const QJsonObject&);
    void opCanvasRunStart(const QString& reqId, const QJsonObject&);
    void opCanvasRunCancel(const QString& reqId, const QJsonObject&);
    /**
     * @brief Writes one line to a running canvas script's stdin, so a
     *        wire client can answer an `input()` prompt the desktop app
     *        answers through its console input bar.
     * @param reqId  Request id to reply on.
     * @param params `{ text: string }`: the answer, without a trailing
     *               newline (CanvasRunner appends exactly one).
     *
     * Rejects a missing or non-string `text` with `invalid_params`, and a
     * body above kMaxStdinBytes with `payload_too_large`. Embedded
     * newlines are preserved: a multi-line answer is legitimate.
     */
    void opCanvasRunSendInput(const QString& reqId, const QJsonObject& params);
    /**
     * @brief Closes a running canvas script's stdin, so a script blocked
     *        in `sys.stdin.read()` observes EOF.
     * @param reqId Request id to reply on.
     *
     * Idempotent; a no-op on the host when nothing is running.
     */
    void opCanvasRunEof(const QString& reqId, const QJsonObject&);
    void opCanvasRunSendToIde(const QString& reqId, const QJsonObject&);
    void opCanvasRunSandboxState(const QString& reqId, const QJsonObject&);
    void opCanvasRunIsRunning(const QString& reqId, const QJsonObject&);
    void opCanvasRunSupportedLangs(const QString& reqId, const QJsonObject&);
    void opCanvasRunSupportedIdeLangs(const QString& reqId, const QJsonObject&);
    void opCanvasConsoleClear(const QString& reqId, const QJsonObject&);

    // Helpers
    bool requireAuth(const QString& reqId);
    bool checkRateLimit();

    /**
     * @brief Resolves a `client_response` frame against the per-session
     *        pending-requests map. Invoked from `onTextMessageReceived`
     *        BEFORE the inbound rate-limit check and op extraction, because
     *        the response is elicited transport, not a new op, and
     *        must not be billed against the inbound op budget.
     *        Unknown `request_id`s are silently dropped (the matching
     *        callback already fired via the timeout sweeper or the
     *        session-close drain).
     * @param root  Parsed frame body. Caller has already validated
     *              the frame is a JSON object.
     */
    void onClientResponse(const QJsonObject& root);

    /**
     * @brief Periodic sweep that walks the pending-requests map and
     *        fires the timeout callback for every entry whose
     *        deadline has passed. Runs on the session's event thread
     *        at `kClientRpcSweepIntervalMs` cadence.
     */
    void onClientRpcTimeoutSweep();

    /**
     * @brief Atomically drains every pending callback with the given
     *        error kind. Used on socket disconnect (kind =
     *        "session_closed") and in the destructor (kind =
     *        "session_destroyed"). After the drain the pending map is
     *        empty and the sweeper is harmless even if it fires once
     *        more before stopping.
     * @param kind    Error category propagated into each callback.
     * @param detail  Human-readable diagnostic appended to the error.
     */
    void drainPendingClientRequests(const QString& kind, const QString& detail);

    /**
     * @brief Sliding-window rate-limit check for host-emitted requests.
     *        Mirrors `checkRateLimit` but on a separate counter so
     *        outbound traffic budget never starves inbound ops nor
     *        vice versa.
     * @returns True if the request fits within the window; false if
     *          the cap is hit. Updates the window only on `true`.
     */
    bool checkOutboundRateLimit();

    // Async invoke wrapper that responds to the wire client when host
    // returns. Maps host result to OK / error envelope.
    void asyncInvoke(const QString& reqId,
                     const QString& target,
                     const QString& method,
                     const QJsonArray& args,
                     const QString& returnType,
                     int timeoutMs = 15000);

    /** asyncInvoke variant that wraps a QString return value in
     *  {idKey: "..."} so Android's parseId-style parsers find the
     *  id under the expected key. Used by create-style ops that
     *  return a bare uuid (newConversation, createFolder, etc.). */
    void asyncInvokeWrapId(const QString& reqId,
                           const QString& target,
                           const QString& method,
                           const QJsonArray& args,
                           const QString& idKey = QStringLiteral("id"),
                           int timeoutMs = 15000);

    QPointer<QWebSocket> m_socket;
    WireAuth* m_auth = nullptr;        // non-owning
    WireHostClient* m_host = nullptr;  // non-owning
    WireDbReader* m_db = nullptr;      // non-owning

    QString m_clientId;
    QString m_clientName;
    // Named wire capabilities the client advertised at auth (e.g.
    // "vfs.execute"). Empty for clients that advertise none. Read by
    // hasCapability() to gate host-initiated routing.
    QSet<QString> m_capabilities;
    QSet<QString> m_subscribedConvIds;
    QQueue<qint64> m_recentOpsMs;


    static constexpr int kMaxSubscriptions = 256;
    static constexpr int kMaxOpsPerSecond = 200;

    /**
     * @brief One entry in the per-session pending-requests map.
     *        Owned by the map; copied into the `ClientRpcCallback`
     *        invocation by value at resolution time.
     */
    struct PendingClientRequest {
        /** Wire op name (diagnostic-only on host side; client owns
         *  the dispatch). */
        QString op;
        /** Absolute deadline (epoch ms) at which the timeout sweeper
         *  fires this entry. */
        qint64 expiresAtMs = 0;
        /** Resolver invoked exactly once per entry. */
        ClientRpcCallback callback;
    };

    /** In-flight host-initiated requests awaiting `client_response`.
     *  Keyed on the host-generated request UUID. Bounded by
     *  `kMaxPendingClientRequests`. */
    QHash<QString, PendingClientRequest> m_pendingClientRequests;

    /** Periodic sweeper that walks `m_pendingClientRequests` and fires
     *  timeouts. Parented to the session so RAII teardown joins
     *  cleanly with the session lifecycle. */
    QTimer m_clientRpcSweepTimer;

    /** Sliding 1-second window of recent host-emitted request
     *  timestamps. Backs `checkOutboundRateLimit`. */
    QQueue<qint64> m_recentOutboundOpsMs;
};

}  // namespace Verzeta::Remote

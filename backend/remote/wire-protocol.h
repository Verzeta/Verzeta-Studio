// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-protocol.h
 * @brief Verzeta Remote: IPC + wire protocol constants. SHARED HEADER.
 *
 *        This file is included by BOTH the host-side bridge component
 *        (compiled into verzeta-studio) AND the separate verzeta-remote
 *        binary. It's the only place the two sides agree on what
 *        message names + envelopes look like.
 *
 *        The remote subsystem is a SEPARATE PROCESS. It cannot crash,
 *        kill, or directly access host memory. The contract enforced
 *        here is:
 *
 *          1. WireHostBridge runs ONE thread inside verzeta-studio. Its
 *             only job is to (a) subscribe to existing host signals and
 *             forward them over QLocalSocket to verzeta-remote, and (b)
 *             receive command frames from verzeta-remote and dispatch
 *             them via QMetaObject::invokeMethod(Qt::QueuedConnection)
 *             on existing host Q_INVOKABLEs. ZERO new methods on host
 *             classes. ZERO BlockingQueuedConnection. ZERO polling.
 *
 *          2. verzeta-remote is a stand-alone binary. It owns the
 *             QWebSocketServer (Android-facing), its own SQLite database
 *             (paired clients), and the QLocalSocket connection to the
 *             host bridge. If verzeta-remote crashes, host doesn't
 *             notice. If host crashes, verzeta-remote keeps running
 *             with no live bridge; clients see a transient error.
 *
 *          3. Frame format on the QLocalSocket is:
 *                 [4-byte big-endian length][JSON payload]
 *             with hard cap kMaxWireFrameBytes per direction. Anything
 *             larger is rejected at parse time on both sides.
 *
 * @layer Service (presentation)
 * @dependencies Qt6::Core only.
 */

#pragma once

#include <QtGlobal>

#include <QString>

namespace Verzeta::Remote {

/**
 * @brief Name the host bridge's QLocalServer binds and the remote
 *        binary connects to.
 *
 * Canonical fixed name by default. The spawned verzeta-remote child
 * discovers the host by this name. The `VERZETA_BRIDGE_SOCKET`
 * environment variable overrides it (the child inherits the host's
 * environment, so both sides stay in agreement). Tests MUST set the
 * override to a unique per-process name: the bridge's start-up does a
 * stale-socket removeServer() + listen(), so two processes using the
 * same name STEAL the socket from each other; a test run would
 * otherwise disconnect a live app instance's daemon and the app's
 * reconnecting daemon would dial into the test's server.
 * @returns The socket name to bind/connect.
 */
inline QString hostBridgeSocketName() {
    const QString env = qEnvironmentVariable("VERZETA_BRIDGE_SOCKET");
    return env.isEmpty() ? QStringLiteral("verzeta-host-bridge") : env;
}

/** The single hard cap on any wire frame (the ENCODED bytes of one
 *  message on the socket), applied identically to the client WebSocket
 *  (verzeta-remote's server, for both the VS Code extension and Android)
 *  and to either direction of the host-bridge IPC channel. One number
 *  for the whole transport: a frame accepted by the WebSocket is small
 *  enough to forward over the bridge without re-checking against a
 *  different limit. Defends against pathological frames; legitimate
 *  payloads (canvas/message bodies, image base64) fit with headroom
 *  because user content is independently bounded by kMaxContentBytes
 *  below, which leaves room for base64 (+~33%) and the JSON/IPC
 *  envelopes. */
inline constexpr qint64 kMaxWireFrameBytes = 24LL * 1024 * 1024;

/** The single hard cap on DECODED user content carried inside one frame
 *  (an attachment's raw file bytes, canvas text, or an uploaded
 *  document). Sized below kMaxWireFrameBytes so that after base64
 *  expansion (+~33%) plus the surrounding JSON and IPC envelopes the
 *  resulting frame still fits the transport cap: 16 MiB of content
 *  encodes to ~21.3 MiB, comfortably under the 24 MiB frame cap. Every
 *  bulk content-size guard (inbound staging, outbound media/attachment
 *  reads, canvas open/edit) references THIS constant. The one narrower
 *  cap is kMaxStdinBytes below, which bounds interactive keystrokes
 *  rather than documents. */
inline constexpr qint64 kMaxContentBytes = 16LL * 1024 * 1024;

/** Cap on one interactive stdin write forwarded to a running canvas
 *  script (`canvas.run.send_input`). Deliberately far below
 *  kMaxContentBytes: this carries a line a human typed in answer to an
 *  `input()` prompt, not a document. A generous multi-line paste still
 *  fits, while a 16 MiB write into a sandboxed child's stdin pipe does
 *  not. Newlines are NOT stripped: a multi-line answer is legitimate,
 *  and the bwrap sandbox, not this cap, is the security boundary. */
inline constexpr qint64 kMaxStdinBytes = 8LL * 1024;

// The whole two-cap scheme rests on this invariant: a payload at the
// content cap, once base64-expanded (×4/3), must still fit inside one
// wire frame with room left for the JSON/IPC envelope. Enforced at
// compile time so the relationship can never silently regress if either
// constant is retuned.
static_assert(kMaxContentBytes * 4 / 3 < kMaxWireFrameBytes,
              "base64-expanded content must fit within a wire frame "
              "with envelope headroom (see kMaxWireFrameBytes / "
              "kMaxContentBytes)");
static_assert(kMaxStdinBytes < kMaxContentBytes,
              "the interactive stdin cap must stay well below the bulk "
              "content cap (see kMaxStdinBytes / kMaxContentBytes)");

// ---------------------------------------------------------------------------
// IPC frame "type" field — every IPC frame is a JSON object with at least
// a "type" string. Switch on this to dispatch.
// ---------------------------------------------------------------------------

/**
 * @brief Values of the `type` field on IPC frames between the host bridge
 *        and the verzeta-remote daemon. Every frame is a JSON object with at
 *        least a `type` string; receivers switch on it to dispatch.
 */
namespace IpcType {
/// Bridge to daemon, sent once when the daemon connects. Carries
/// `host` and `protocol` ("verzeta-bridge-1"). The daemon accepts it for
/// diagnostics only and takes no action on it.
inline constexpr auto Hello = "hello";
/// Reserved. Not currently sent or handled by the bridge or the daemon.
inline constexpr auto Subscribe = "subscribe";
/// Reserved. Not currently sent or handled by the bridge or the daemon.
inline constexpr auto Unsubscribe = "unsubscribe";
/// Daemon to bridge: call `method` with `args` on the host object named
/// by `target`, for the paired client in the optional `client_id`. When
/// the daemon needs a result it also sends `request_id` and
/// `return_type`, and the bridge answers with an InvokeResponse.
inline constexpr auto Invoke = "invoke";
/// Bridge to daemon: the result of an Invoke. Echoes `request_id` and
/// carries `ok`, then `data` on success or `error` on failure.
inline constexpr auto InvokeResponse = "invoke_response";
/// Bridge to daemon: a host signal, named in `name`, with its arguments
/// in `args`. The daemon re-emits it as WireHostClient::hostSignal for
/// its sessions to relay to paired clients.
inline constexpr auto Signal = "signal";
/// Reserved. Not currently sent or handled by the bridge or the daemon.
inline constexpr auto Error = "error";

/// Reads a Q_PROPERTY: `target->property(name)` runs on the target's
/// thread (main) through a BlockingQueuedConnection. The worker waits
/// for main to run and return, and main never waits on the worker, so
/// this cannot deadlock. Uses existing Q_PROPERTY accessors only.
inline constexpr auto PropertyGet = "property_get";
/// Writes a Q_PROPERTY through a QueuedConnection, fire and forget.
/// Uses existing Q_PROPERTY accessors only.
inline constexpr auto PropertySet = "property_set";

/// Bridge to daemon: a host-initiated RPC for one paired client. Carries
/// `request_id` (a bridge-generated UUID), `client_id` (the target
/// client), `op` (the wire op name), `args` (a JSON object) and
/// `timeout_ms`. The daemon finds the WireSession for `client_id` and
/// sends the client a wire `request` frame through sendClientRequest.
/// The client's `client_response` comes back as a ClientRpcReply.
inline constexpr auto ClientRpcDispatch = "client_rpc_dispatch";

/// Daemon to bridge: the reply to a ClientRpcDispatch. Echoes
/// `request_id` and carries either `{ok: true, data}` or
/// `{ok: false, error: {kind, detail}}` on failure (timeout, client
/// disconnected, unknown client_id, payload too large, and so on).
inline constexpr auto ClientRpcReply = "client_rpc_reply";
}  // namespace IpcType

// ---------------------------------------------------------------------------
// WebSocket frame "type" field — every WS frame between the verzeta-remote
// daemon and a paired client (Verzeta-Android, Verzeta-VSCode extension)
// is a JSON object with at least a "type" string. The constants below
// centralise the wire-side type tags. Two of them are pre-existing inline
// literals in the daemon's send paths (`response`, `event`); the other
// two are the new bidirectional pair (`request` is sent host → client to
// elicit a client-side action whose result returns as `client_response`).
// All four are recognised by every conforming wire client; unknown types
// are ignored (forward-compatible).
// ---------------------------------------------------------------------------

namespace WsType {
/** Reply envelope to a client-initiated `op` frame. ok+data on
 *  success, ok=false+error{kind,detail} on failure. */
inline constexpr auto Response = "response";

/** Host-pushed asynchronous notification (signal/event broadcast).
 *  No correlation id; not subject to client reply. */
inline constexpr auto Event = "event";

/** Host-initiated request directed at a specific connected client.
 *  Carries `request_id` (host-generated UUID), `op`, `args`. The
 *  client is expected to reply with a matching `client_response`
 *  frame inside the request's timeout window. Outbound rate-limit
 *  on host side prevents flood; per-session pending-map caps the
 *  in-flight count. Unknown ops are answered with
 *  `client_response{ok:false, error:{kind:"unknown_op"}}`. */
inline constexpr auto Request = "request";

/** Client → host reply to a previously-received `request` frame.
 *  Echoes `request_id`. Either ok+data on success or
 *  ok=false+error{kind,detail} on failure. Not subject to the
 *  inbound op rate-limit (it is elicited transport, not a new op). */
inline constexpr auto ClientResponse = "client_response";
}  // namespace WsType

// ---------------------------------------------------------------------------
// Signal names the bridge forwards from host → remote. EXACT match to the
// existing Qt signal name on the existing host service. The remote binary
// names them by string; the bridge resolves to the actual signal pointer
// at construction time.
// ---------------------------------------------------------------------------

/**
 * @brief Names of the host signals the bridge forwards to the daemon.
 *
 * Each value is `Service.signalName`, an exact match for an existing Qt
 * signal on that host service. The daemon names signals by string, and the
 * bridge resolves each name to the actual signal when it is constructed.
 */
namespace Signals {
// MessageService
/// Relays MessageService::messageAdded().
inline constexpr auto MessageAdded = "MessageService.messageAdded";
/// Relays MessageService::messageUpdated().
inline constexpr auto MessageUpdated = "MessageService.messageUpdated";
/// Relays MessageService::messageDeleted().
inline constexpr auto MessageDeleted = "MessageService.messageDeleted";
/// Relays MessageService::messageStreamingStarted().
inline constexpr auto MessageStreamingStarted = "MessageService.messageStreamingStarted";
/// Relays MessageService::messageContentStreamed().
inline constexpr auto MessageContentStreamed = "MessageService.messageContentStreamed";
/// Relays MessageService::messageContentRewritten().
inline constexpr auto MessageContentRewritten = "MessageService.messageContentRewritten";
/// Relays MessageService::messageStreamingAborted().
inline constexpr auto MessageStreamingAborted = "MessageService.messageStreamingAborted";
/// Relays MessageService::toolCallAdded().
inline constexpr auto ToolCallAdded = "MessageService.toolCallAdded";
/// Relays MessageService::toolCallUpdated().
inline constexpr auto ToolCallUpdated = "MessageService.toolCallUpdated";
/// Relays MessageService::contextFillMeasured().
inline constexpr auto ContextFillMeasured = "MessageService.contextFillMeasured";
/// Relays MessageService::userMentioned().
inline constexpr auto UserMentioned = "MessageService.userMentioned";

// ConversationService
/// Relays ConversationService::conversationCreated().
inline constexpr auto ConversationCreated = "ConversationService.conversationCreated";
/// Relays ConversationService::conversationUpdated().
inline constexpr auto ConversationUpdated = "ConversationService.conversationUpdated";
/// Relays ConversationService::conversationDeleted().
inline constexpr auto ConversationDeleted = "ConversationService.conversationDeleted";
/// Relays ConversationService::folderCreated().
inline constexpr auto FolderCreated = "ConversationService.folderCreated";
/// Relays ConversationService::folderUpdated().
inline constexpr auto FolderUpdated = "ConversationService.folderUpdated";
/// Relays ConversationService::folderDeleted().
inline constexpr auto FolderDeleted = "ConversationService.folderDeleted";

// ConversationController
/// Relays ConversationController::conversationRenamed().
inline constexpr auto ConversationRenamed = "ConversationController.conversationRenamed";

// AgentSettingsController
/// Relays AgentSettingsController::activeProviderChanged().
inline constexpr auto ActiveProviderChanged = "AgentSettingsController.activeProviderChanged";
/// Relays AgentSettingsController::agentPatternChanged().
inline constexpr auto AgentPatternChanged = "AgentSettingsController.agentPatternChanged";
/// Relays AgentSettingsController::requireConfirmationChanged().
inline constexpr auto RequireConfirmationChanged =
    "AgentSettingsController.requireConfirmationChanged";
/// Relays AgentSettingsController::toolsEnabledChanged().
inline constexpr auto ToolsEnabledChanged = "AgentSettingsController.toolsEnabledChanged";
/// Relays AgentSettingsController::activeConversationSettingsChanged().
inline constexpr auto ActiveConversationSettingsChanged =
    "AgentSettingsController.activeConversationSettingsChanged";
/// Relays AgentSettingsController::modelsRefreshed().
inline constexpr auto ModelsRefreshed = "AgentSettingsController.modelsRefreshed";

// MembershipService
/// Relays MembershipService::projectMembersChanged().
inline constexpr auto ProjectMembersChanged = "MembershipService.projectMembersChanged";
/// Relays MembershipService::conversationMembersChanged().
inline constexpr auto ConversationMembersChanged = "MembershipService.conversationMembersChanged";

// HeartbeatConfigService
/// Relays HeartbeatConfigService::configChanged().
inline constexpr auto HeartbeatConfigChanged = "HeartbeatConfigService.configChanged";
/// Relays HeartbeatConfigService::configRemoved().
inline constexpr auto HeartbeatConfigRemoved = "HeartbeatConfigService.configRemoved";

// PlanService
/// Relays PlanService::planCreated().
inline constexpr auto PlanCreated = "PlanService.planCreated";
/// Relays PlanService::planUpdated().
inline constexpr auto PlanUpdated = "PlanService.planUpdated";
/// Relays PlanService::planDeleted().
inline constexpr auto PlanDeleted = "PlanService.planDeleted";
/// Relays PlanService::stepUpdated().
inline constexpr auto StepUpdated = "PlanService.stepUpdated";

// AgentService (tool confirmation + step lifecycle)
/// Relays AgentService::toolCallRequested().
inline constexpr auto AgentToolCallRequested = "AgentService.toolCallRequested";
/// Relays AgentService::toolCallCompleted().
inline constexpr auto AgentToolCallCompleted = "AgentService.toolCallCompleted";
/// Relays AgentService::agentStepStarted().
inline constexpr auto AgentStepStarted = "AgentService.agentStepStarted";
/// Relays AgentService::agentStepCompleted().
inline constexpr auto AgentStepCompleted = "AgentService.agentStepCompleted";
/// Relays AgentService::isRunningChanged().
inline constexpr auto AgentIsRunningChanged = "AgentService.isRunningChanged";
/// Relays AgentService::iterationChanged().
inline constexpr auto AgentIterationChanged = "AgentService.iterationChanged";

// ImageService / AudioService
/// Relays ImageService::imageGenerated().
inline constexpr auto ImageGenerated = "ImageService.imageGenerated";
/// Relays AudioService::audioGenerated().
inline constexpr auto AudioGenerated = "AudioService.audioGenerated";

/// Relays ChatController::userMessageQueued() from the per-client wire
/// instances, emitted when a wire client's send is queued behind work
/// holding the shared ModelRouter slot. SessionRouter merges these into
/// one signal tagged with the session id, which the bridge subscribes to
/// once, so each wire session can filter to its own client.
inline constexpr auto UserMessageQueued = "ChatController.userMessageQueued";

// CanvasService + CanvasRunner
/// Relays CanvasService::canvasOpened().
inline constexpr auto CanvasOpened = "CanvasService.canvasOpened";
/// Relays CanvasService::canvasUpdated().
inline constexpr auto CanvasUpdated = "CanvasService.canvasUpdated";
/// Relays CanvasService::canvasClosed().
inline constexpr auto CanvasClosed = "CanvasService.canvasClosed";
/// Relays CanvasService::errorOccurred().
inline constexpr auto CanvasErrorOccurred = "CanvasService.errorOccurred";
/// Relays CanvasConsoleModel::lineAppended().
inline constexpr auto CanvasConsoleLineAppended = "CanvasConsoleModel.lineAppended";
/// Relays CanvasRunner::runningChanged().
inline constexpr auto CanvasRunningChanged = "CanvasRunner.runningChanged";

// PollService
/// Relays PollService::pollCreated().
inline constexpr auto PollCreated = "PollService.pollCreated";
/// Relays PollService::pollUpdated().
inline constexpr auto PollUpdated = "PollService.pollUpdated";
/// Relays PollService::pollClosed().
inline constexpr auto PollClosed = "PollService.pollClosed";
/// Relays PollService::voteCast().
inline constexpr auto VoteCast = "PollService.voteCast";

/// Relays AuditService::activityLogged(). The daemon forwards it as
/// `activity.logged`, and the payload carries projectFolderId and
/// conversationId so each session can filter to the scope it shows.
inline constexpr auto ActivityLogged = "AuditService.activityLogged";

/// Relays ProjectTemplateService::projectCreatedFromTemplate(),
/// emitted when a template creates a new project, for example from the
/// Android Quick Start flow. Forwarded to every paired client as
/// `project_template.project_created` so it can show the new folder.
inline constexpr auto ProjectTemplateProjectCreated =
    "ProjectTemplateService.projectCreatedFromTemplate";
/// Relays ProjectTemplateService::catalogChanged(), emitted when the
/// user-template catalog changes (save as new, pin toggle, delete).
/// Forwarded to every paired client as `project_template.catalog_changed`
/// so it can refresh its template list.
inline constexpr auto ProjectTemplateCatalogChanged = "ProjectTemplateService.catalogChanged";
}  // namespace Signals

// ---------------------------------------------------------------------------
// Invokable target names the remote can ask the bridge to dispatch via
// QMetaObject::invokeMethod(Qt::QueuedConnection). EXACT match to existing
// host service object names registered as QML context properties.
// ---------------------------------------------------------------------------

/**
 * @brief Host object names the daemon may name as an Invoke target.
 *
 * The bridge dispatches to the named object with
 * QMetaObject::invokeMethod(Qt::QueuedConnection). Each value is an exact
 * match for the object's QML context property name.
 */
namespace Targets {
/// Resolves to ChatController. Routed per paired client: a wire client
/// gets its own instance from SessionRouter.
inline constexpr auto ChatController = "ChatController";
/// Resolves to ConversationController, under its QML context name.
inline constexpr auto Conversations = "Conversations";
/// Resolves to ConversationService.
inline constexpr auto ConversationService = "ConversationService";
/// Resolves to MessageService.
inline constexpr auto MessageService = "MessageService";
/// Resolves to AgentSettingsController, under its QML context name. Routed
/// per paired client: a wire client gets its own instance from
/// SessionRouter.
inline constexpr auto AgentSettings = "AgentSettings";
/// Resolves to AgentRegistry.
inline constexpr auto AgentRegistry = "AgentRegistry";
/// Resolves to AgentService.
inline constexpr auto AgentService = "AgentService";
/// Resolves to ToolService.
inline constexpr auto ToolService = "ToolService";
/// Resolves to McpService.
inline constexpr auto McpService = "McpService";
/// Resolves to SkillService.
inline constexpr auto SkillService = "SkillService";
/// Resolves to MembershipService.
inline constexpr auto MembershipService = "MembershipService";
/// Resolves to HeartbeatConfigService.
inline constexpr auto HeartbeatConfigService = "HeartbeatConfigService";
/// Resolves to HeartbeatSubagentService.
inline constexpr auto HeartbeatSubagentService = "HeartbeatSubagentService";
/// Resolves to TaskController.
inline constexpr auto TaskController = "TaskController";
/// Resolves to PlanService.
inline constexpr auto PlanService = "PlanService";
/// Resolves to RagService.
inline constexpr auto RagService = "RagService";
/// Resolves to FileService.
inline constexpr auto FileService = "FileService";
/// Resolves to ImageService.
inline constexpr auto ImageService = "ImageService";
/// Resolves to AudioService.
inline constexpr auto AudioService = "AudioService";
/// Resolves to CanvasService, under its QML context name.
inline constexpr auto Canvas = "Canvas";
/// Resolves to CanvasRunner.
inline constexpr auto CanvasRunner = "CanvasRunner";
/// Resolves to CanvasAiActions.
inline constexpr auto CanvasAiActions = "CanvasAiActions";
/// Resolves to CanvasConsoleModel.
inline constexpr auto CanvasConsoleModel = "CanvasConsoleModel";
/// Resolves to SettingsService.
inline constexpr auto SettingsService = "SettingsService";
/// AppController. Not dispatched by the bridge: its resolver has no entry
/// for this name, so an invoke naming it finds no target.
inline constexpr auto AppController = "AppController";
/// Resolves to Search::WebSearchProviderRegistry, under its QML context
/// name.
inline constexpr auto WebSearchProviders = "WebSearchProviders";
/// Resolves to PollService. Shared rather than routed per client,
/// because all poll state is conversation-scoped and the service keeps
/// no per-client state.
inline constexpr auto PollService = "PollService";

/// Resolves to AuditService. Shared rather than routed per client, because
/// activity_log is append-only and every row carries its scope, so a read
/// by folder or conversation id already returns only that client's data.
inline constexpr auto AuditService = "AuditService";

/// Resolves to ProjectTemplateService. Shared: the template catalog is
/// process-wide and project creation writes through shared services.
/// Wire sessions dispatch the eight `project_template.*` ops here.
inline constexpr auto ProjectTemplateService = "ProjectTemplateService";

/// Resolves to FolderMountRegistry, the registry of client-owned
/// workspace mounts. Shared: one registry serves every paired client,
/// keyed by folder. Wire sessions dispatch the five `workspace.mount.*`
/// ops here.
inline constexpr auto FolderMountRegistry = "FolderMountRegistry";
}  // namespace Targets

}  // namespace Verzeta::Remote

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file session-router.h
 * @brief Per-frontend ChatController registry. Each "session"
 *        represents one window into the Verzeta Studio backend: the
 *        desktop QML is the "local" session, and each paired remote
 *        (Android / iPad / etc.) WebSocket session gets its own.
 *
 *        Responsibilities:
 *          - Hold a non-owning pointer to AppController's
 *            ChatController registered as the "local" session
 *            (lifecycle stays with AppController).
 *          - Own ChatController instances created for wire sessions
 *            via createWireSession (wire-host-bridge calls this on
 *            WS connect and tears them down on disconnect).
 *          - Serve sessionFor(sessionId) lookups so wire ops route to
 *            their own session's ChatController instance instead of
 *            hijacking the local one.
 *          - (Historical) Serialize ModelRouter foreground-slot access
 *            across all registered ChatControllers. Cross-session
 *            serialization is now the app-level
 *            Chat::ProviderScheduler's job (per-provider serialize,
 *            parallel across providers). SessionRouter no longer gates
 *            dispatch; ConversationRun does NOT consult
 *            anySessionGenerating() anymore. The generation-state
 *            tracking + slotAvailable signal are retained as a harmless
 *            "a session finished, re-try any per-conversation queued
 *            sends" pump and for diagnostics; they impose no global
 *            single-flight serialization.
 *          - Spawn + register a per-session AgentSettingsController
 *            alongside each wire ChatController so per-conv agent /
 *            tool / RAG settings writes from a wire client don't
 *            clobber the local instance's active conversation.
 *
 *        Multi-tenancy is achieved by instantiating ChatController
 *        multiple times, NOT by adding sessionId-keyed state to its
 *        members, so the class's existing single-tenant code path stays
 *        unchanged.
 *
 * @layer Service (UI orchestration)
 * @dependencies ChatController (forward-declared); ConversationService,
 *               MessageService, ModelRouter, ExportService for the
 *               wire-session factory ctor.
 *
 * \@threading Strictly main-thread. Every public method asserts via
 *            VERZETA_ASSERT_MAIN_THREAD().
 *
 * \@ownership AppController owns SessionRouter via std::unique_ptr.
 *            SessionRouter owns the wire-session ChatController
 *            instances it creates (std::unique_ptr in m_wireSessions).
 *            The "local" session ChatController is NOT owned by
 *            SessionRouter. AppController's existing m_chatController
 *            unique_ptr is the canonical owner; SessionRouter holds a
 *            non-owning QPointer for lookup.
 */
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class AgentSettingsController;
class ChatController;
class ConversationService;
class MessageService;
class ModelRouter;
class ExportService;

namespace Verzeta::Session {

/**
 * @brief Canonical session id for the desktop QML session.
 *
 * Constant string used as the sessionId key when AppController
 * registers the QML-facing ChatController instance. Wire sessions
 * use the paired-client UUID from `wire_clients.id` instead.
 */
inline constexpr auto kLocalSessionId = "local";

/**
 * @brief Per-frontend ChatController registry.
 */
class SessionRouter : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Setup callback applied to every wire-side ChatController.
     *
     *        AppController registers this once after building all
     *        engine services (ToolService, RagService, AgentService,
     *        TaskGateService, SlashCommandService, CanvasService,
     *        SkillService, HeartbeatConfigService, etc.). createWireSession
     *        invokes the callback for each created ChatController so
     *        the wire-side instance has the SAME setter graph the
     *        local ChatController has: same shared engine services,
     *        identical sendMessage capability surface.
     */
    using ChatControllerSetupFn = std::function<void(ChatController*)>;

    /**
     * @brief Setup callback applied to every wire-side
     *        AgentSettingsController. AppController registers this
     *        once during initialize(). Typically lighter than the
     *        ChatController setup callback because AgentSettings only
     *        needs ConvSvc + ModelRouter (which the ctor already
     *        receives via SessionRouter's stored refs). The callback
     *        is the right hook for any future per-instance wiring
     *        beyond the basic ctor.
     */
    using AgentSettingsSetupFn = std::function<void(AgentSettingsController*)>;

    /**
     * @param convs       Shared ConversationService (DB layer).
     * @param msgs        Shared MessageService (DB layer).
     * @param router      Shared ModelRouter.
     * @param exportSvc   Shared ExportService.
     * @param parent      Qt parent (typically AppController).
     *
     * The four engine references are stored so createWireSession can
     * pass them to the ChatController ctor for each created session.
     * They must outlive this SessionRouter.
     */
    explicit SessionRouter(ConversationService& convs,
                           MessageService& msgs,
                           ModelRouter& router,
                           ExportService& exportSvc,
                           QObject* parent = nullptr);
    ~SessionRouter() override;

    SessionRouter(const SessionRouter&) = delete;
    SessionRouter& operator=(const SessionRouter&) = delete;

    /**
     * @brief Install the per-session ChatController configurator.
     *
     *        AppController calls this once during initialize(), AFTER
     *        all engine services are constructed and AFTER local
     *        ChatController has been fully configured (including all
     *        ChatController::set* setter calls AppController applies).
     *        The callback captures references to AppController's
     *        engine services and applies the SAME setters to each
     *        wire-side ChatController createWireSession spawns.
     *
     *        Calling this AFTER createWireSession has already run for
     *        a session is permitted; previously-created wire sessions
     *        will NOT be retroactively configured. (Practically, the
     *        AppController call site precedes any wire client
     *        connecting, so this race shouldn't happen.)
     *
     *        Passing an empty std::function clears the callback;
     *        subsequent createWireSession calls will warn and return
     *        unconfigured ChatControllers (test scaffolding may use
     *        this; production must always set a real callback).
     * @param fn Setup callback to install; empty std::function clears.
     */
    void setChatControllerSetupFn(ChatControllerSetupFn fn);

    /**
     * @brief Install the per-session AgentSettingsController
     *        configurator. Called once by AppController during
     *        initialize(), after registerLocalAgentSettings, so wire
     *        AS instances spawned afterward receive the same setter
     *        recipe AppController applies to the local AS.
     * @param fn Setup callback to install; empty std::function clears.
     */
    void setAgentSettingsSetupFn(AgentSettingsSetupFn fn);

    /**
     * @brief Register the desktop QML's existing ChatController as
     *        the "local" session.
     *
     * AppController constructs its m_chatController as today (no
     * change to that line) and immediately calls this with the same
     * pointer. SessionRouter stores a non-owning QPointer; the actual
     * lifecycle remains AppController's.
     *
     * @param localCC Non-null pointer to AppController's ChatController.
     */
    void registerLocalSession(ChatController* localCC);

    /**
     * @brief Register the desktop QML's existing
     *        AgentSettingsController as the "local" AS. AppController
     *        constructs its m_agentSettings as today and immediately
     *        calls this with the same pointer; SessionRouter stores a
     *        non-owning QPointer. Like registerLocalSession, lifetime
     *        stays AppController's.
     * @param localAS Non-null pointer to AppController's
     *                AgentSettingsController.
     */
    void registerLocalAgentSettings(AgentSettingsController* localAS);

    /**
     * @brief Construct a wire-side ChatController and register it
     *        under the given sessionId.
     *
     *        The new ChatController is constructed with the four
     *        engine references this SessionRouter holds (same as the
     *        local ChatController's ctor takes). The setup callback
     *        installed via setChatControllerSetupFn (if any) is
     *        then invoked on the new instance to apply all the
     *        non-owning service-pointer setters AppController applies
     *        to local ChatController. Without the setup callback the
     *        ChatController will be missing its tool/rag/agent/task/
     *        canvas/skill/heartbeat plumbing. It is usable for read-only
     *        ops, but sendMessage's full feature set needs it.
     *
     *        Calling this with the same sessionId twice is a no-op
     *        for the second call; the existing instance pointer is
     *        returned with a warning log.
     *
     * @param sessionId Paired-client UUID from wire_clients.id. Must
     *                  not be empty and must not equal kLocalSessionId.
     * @return Non-owning pointer to the new ChatController. nullptr
     *         only on argument validation failure (empty / local id).
     */
    ChatController* createWireSession(const QString& sessionId);

    /**
     * @brief Look up the per-client AgentSettings instance for a
     *        session, lazy-creating it on first call if absent. The
     *        wire-host-bridge's resolveTargetForClient uses this to
     *        route AgentSettings target invocations to the per-client
     *        instance instead of the shared local one. Without this
     *        routing, a wire client's settings writes would clobber
     *        the local instance's active conv.
     *
     *        Created instances are wired so the matching wire CC's
     *        activeConversationChanged signal updates the AS's
     *        m_activeConvId. The wire AS therefore tracks the
     *        wire CC's conv switches automatically, mirroring how
     *        AppController wires LOCAL CC's activeConversationChanged
     *        to LOCAL AS.
     *
     *        nullptr only on argument validation failure (empty /
     *        local id). Caller does NOT own the returned pointer.
     *
     * @param sessionId Wire client UUID (must not be empty / local).
     * @return Non-owning AgentSettingsController*, or nullptr.
     */
    AgentSettingsController* createWireAgentSettings(const QString& sessionId);

    /**
     * @brief Look up an AgentSettings instance by session id. Returns
     *        the local AS for kLocalSessionId (registered via
     *        registerLocalAgentSettings) and the wire AS for any
     *        other id; nullptr if absent.
     * @param sessionId Session id (kLocalSessionId or wire UUID).
     * @returns Non-owning AgentSettingsController pointer, or nullptr
     *          if no session with that id is registered.
     */
    AgentSettingsController* agentSettingsFor(const QString& sessionId) const;

    /**
     * @brief Non-owning accessor for the local AgentSettings pointer.
     *        Equivalent to calling agentSettingsFor with
     *        kLocalSessionId. Cheap.
     * @returns The local AgentSettingsController pointer, or nullptr
     *          if registerLocalAgentSettings has not been called.
     */
    AgentSettingsController* localAgentSettings() const;

    /**
     * @brief Tear down a wire session (e.g. on WS disconnect).
     *        No-op for local session id (which is AppController-owned).
     *
     * @param sessionId Session to destroy.
     */
    void destroySession(const QString& sessionId);

    /**
     * @brief Tear down ALL wire sessions. Used by wire-host-bridge
     *        when the verzeta-remote IPC connection drops, since all wire
     *        clients are by definition gone with it. Local session is
     *        unaffected (AppController-owned).
     *
     *        Safe to call when there are no wire sessions (no-op).
     */
    void destroyAllWireSessions();

    /**
     * @brief Look up a session's ChatController by id.
     *
     * @param sessionId Session id (kLocalSessionId or wire UUID).
     * @return Non-owning ChatController pointer, or nullptr if absent.
     */
    ChatController* sessionFor(const QString& sessionId) const;

    /**
     * @brief The desktop QML's ChatController. Equivalent to
     *        `sessionFor(kLocalSessionId)` but cheap and cannot fail
     *        once registerLocalSession has run.
     * @returns The local ChatController pointer, or nullptr if
     *          registerLocalSession has not been called.
     */
    ChatController* localSession() const;

    /**
     * @brief Number of currently-registered sessions (local + wire).
     * @returns Total session count: at least 1 once
     *          registerLocalSession has run, plus one per active wire
     *          client.
     */
    int sessionCount() const;

    /**
     * @brief True iff any registered ChatController instance currently
     *        has its raw m_isGenerating set.
     *
     *        This is NO LONGER a dispatch gate:
     *        cross-session per-provider serialization moved to
     *        Chat::ProviderScheduler, so ConversationRun does not call
     *        this before dispatching. Retained for diagnostics / tests
     *        and as the predicate behind the slotAvailable pump.
     *
     *        Tracked via subscription to each registered CC's
     *        isGeneratingChanged signal. Internal m_genStateByCC
     *        records the per-instance raw state and m_generatingCount
     *        is the sum.
     * @returns true if any registered ChatController is currently
     *          generating; false otherwise.
     */
    bool anySessionGenerating() const;

  signals:
    /**
     * @brief Emitted whenever m_generatingCount drops to 0 (the last
     *        in-flight ChatController just finished). ChatControllers
     *        subscribe and call onSlotAvailable() to re-try any
     *        per-conversation queued sends / sub-agent reactions.
     *
     *        This is now only a "try draining again" pump;
     *        it is NOT a serialization mechanism. Each run drains based
     *        purely on its own idle state, and the actual dispatch
     *        serializes per provider through Chat::ProviderScheduler. The
     *        old cooperative-single-slot contention is gone.
     */
    void slotAvailable();

    /**
     * @brief Fan-in for per-wire-CC `userMessageQueued` emits.
     *        ChatController fires this when a sendMessage call had to
     *        queue behind another instance's in-flight slot.
     *        SessionRouter tags it with the session id (the wire
     *        client's UUID) so the bridge can forward via IPC and
     *        wire-session can filter by m_clientId; the queue
     *        notification is per-client (each wire client sees only
     *        its own queued events). The local CC's userMessageQueued
     *        is NOT funneled here; it's wired to local QML directly
     *        and doesn't need an IPC round-trip.
     * @param sessionId Wire client UUID whose CC had to queue.
     * @param text      The user-typed text that was queued.
     */
    void wireSessionUserMessageQueued(const QString& sessionId, const QString& text);

  private slots:
    /**
     * @brief Re-evaluates a ChatController's generation state when it
     *        emits isGeneratingChanged. Updates m_genStateByCC +
     *        m_generatingCount accordingly. Emits slotAvailable when
     *        the count transitions to 0.
     */
    void onChatGeneratingChanged();

  private:
    ConversationService& m_convs;
    MessageService& m_msgs;
    ModelRouter& m_router;
    ExportService& m_exportSvc;

    /// Non-owning. Lifetime is AppController's. QPointer guards
    /// against AppController teardown order surprises.
    QPointer<ChatController> m_localCC;

    /// Owned. Wire sessions only; local is excluded so we never
    /// accidentally double-delete AppController's ChatController.
    /// std::map (not QHash) because std::map supports move-only
    /// values; QHash internally copies values which conflicts with
    /// unique_ptr's deleted copy ctor.
    std::map<QString, std::unique_ptr<ChatController>> m_wireSessions;

    /// Setup callback invoked on each wire-side ChatController to
    /// apply the same setter recipe AppController applies to the
    /// local instance. nullptr until AppController calls
    /// setChatControllerSetupFn.
    ChatControllerSetupFn m_setupFn;

    /// Non-owning. Local AgentSettings registered by AppController
    /// (lifetime stays AppController's).
    QPointer<AgentSettingsController> m_localAS;

    /// Owned. Wire-side AgentSettings instances keyed by sessionId.
    /// std::map (not QHash) because std::map supports move-only
    /// values; QHash internally copies values which conflicts with
    /// unique_ptr's deleted copy ctor.
    std::map<QString, std::unique_ptr<AgentSettingsController>> m_wireAgentSettings;

    /// Setup callback for wire-side AgentSettings. nullptr until
    /// AppController calls setAgentSettingsSetupFn.
    AgentSettingsSetupFn m_settingsSetupFn;

    /**
     * @brief Per-CC raw isGenerating state cache, keyed by raw
     *        ChatController pointer. We don't use QPointer for the key
     *        because QPointer doesn't hash by default; the connect()
     *        lambdas guard against destroyed CCs by capturing QPointer
     *        separately. Entries removed on destroySession +
     *        destroyAllWireSessions.
     */
    QHash<ChatController*, bool> m_genStateByCC;

    /**
     * @brief Sum of m_genStateByCC values. Drops to 0 → emits
     *        slotAvailable so queued ChatControllers drain their
     *        m_queuedUserText.
     */
    int m_generatingCount = 0;

    /**
     * @brief Wire ChatController gen-state subscription. Connects
     *        cc->isGeneratingChanged → onChatGeneratingChanged + sets
     *        cc->setSessionRouter(this). Called by
     *        registerLocalSession + createWireSession.
     */
    void wireGenerationTracking(ChatController* cc);
};

}  // namespace Verzeta::Session

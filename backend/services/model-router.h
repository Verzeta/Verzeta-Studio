// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file model-router.h
 * @brief Central dispatch hub that routes LLM requests to the active provider.
 *        Manages provider registration, switching, and signal forwarding.
 * @layer Service
 * @dependencies ILLMProvider (API), Qt6::Core
 */

#pragma once

#include "../api/llm-interface.h"

#include <map>
#include <memory>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

/**
 * @brief Central LLM request router with dual dispatch slots.
 *
 * ModelRouter owns all provider instances and routes sendRequest() calls
 * to whichever provider is currently active. It re-emits all provider
 * signals (chunkReceived, requestFinished, requestError) so that consumers
 * only need to connect to the router, not to individual providers.
 *
 * Two independent dispatch slots:
 *   - FOREGROUND slot: route() / cancelCurrent() / chunkReceived /
 *     requestFinished / requestError. Used by ChatController for the
 *     user-driven turn.
 *   - BACKGROUND slot: routeBackground() / cancelBackground() /
 *     backgroundChunkReceived / backgroundRequestFinished /
 *     backgroundRequestError. Used by HeartbeatSubagentService for
 *     proactive autonomous activity.
 *
 * Each slot has its own provider instance map (m_providers vs m_bgProviders),
 * its own active-provider pointer, and its own current-request-id field.
 * The two slots never share state and never cross-subscribe, so a foreground
 * request and a background request can be in flight simultaneously without
 * corrupting each other's signal routing.
 *
 * Ownership:
 *   - Providers are transferred via unique_ptr in registerProvider() for
 *     the foreground slot, or registerBackgroundProvider() for the
 *     background slot.
 *   - ModelRouter owns and destroys both maps.
 *
 * Signal threading:
 *   - All signals are emitted on the thread that called route()/routeBackground()
 *     or on the provider's internal thread (for async providers). Consumers
 *     connected with Qt::QueuedConnection handle the cross-thread case.
 */
class ModelRouter : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString activeProviderId READ activeProviderId NOTIFY activeProviderChanged)
    Q_PROPERTY(QString activeModelName READ activeModelName NOTIFY activeProviderChanged)

  public:
    /**
     * @brief Constructs the ModelRouter with no registered providers.
     * @param parent Optional Qt parent.
     */
    explicit ModelRouter(QObject* parent = nullptr);

    /**
     * @brief Registers a provider. Transfers ownership to the router.
     * @param provider Unique pointer to a provider implementation.
     * @sideeffects Stores the provider; does NOT set it as active.
     *              Connects the provider's modelsRefreshed signal, which
     *              the router re-emits with the provider id. The streaming
     *              signals are not connected here; route() wires them per
     *              request.
     *              Emits `providersChanged()` after the provider is
     *              accepted into `m_providers` so QML provider-list
     *              bindings re-evaluate.
     */
    void registerProvider(std::unique_ptr<ILLMProvider> provider);

    /**
     * @brief Removes a previously-registered foreground provider and
     *        destroys its instance.
     * @param providerId  Provider identifier as returned by
     *                    `ILLMProvider::providerId()`.  No-op when the
     *                    id is unknown.
     * @sideeffects Disconnects every signal wired to the provider;
     *              clears `m_active` when it was pointing at this
     *              instance; emits `providersChanged()` after removal.
     *
     * Intended for runtime-managed providers: custom OpenAI-API-
     * compatible servers added or removed by the user through
     * `CustomServerRegistry`.  Built-in providers wired at
     * AppController init are never unregistered during the
     * application lifetime.
     */
    void unregisterProvider(const QString& providerId);

    /**
     * @brief Returns a non-owning pointer to the provider for the given ID.
     * @param providerId Provider identifier string.
     * @return Pointer to provider, or nullptr if not registered.
     */
    ILLMProvider* providerForId(const QString& providerId) const;

    /**
     * @brief Returns the currently active provider.
     * @return Pointer to active provider, or nullptr if none set.
     */
    ILLMProvider* activeProvider() const;

    /**
     * @brief Sets the UI-selected default provider and model.
     * @param providerId Provider identifier. Must already be registered.
     * @param modelName  Model name within that provider.
     * @sideeffects Records the default that route() uses for a request
     *              whose config carries no providerId. No signals are
     *              connected here; route() wires the streaming relay per
     *              request. Emits activeProviderChanged().
     *
     *              Also mirrors the selection into the background slot
     *              when a background provider with the same id is
     *              registered, emitting activeBackgroundProviderChanged().
     *              When none is, the background slot is cleared, so a
     *              heartbeat request with no provider of its own fails with
     *              a clear error instead of reaching a previously selected
     *              provider.
     *
     * If providerId is not registered, logs a warning, emits requestError()
     * and changes nothing.
     */
    void setActiveProvider(const QString& providerId, const QString& modelName);

    /**
     * @brief The currently active foreground provider id.
     * @returns Active provider id, or empty when none is selected.
     */
    QString activeProviderId() const;

    /**
     * @brief The model name selected on the active foreground provider.
     * @returns Active model name, or empty when none is selected.
     */
    QString activeModelName() const;

    /**
     * @brief Returns the IDs of all registered providers.
     * @return List of provider ID strings.
     */
    QStringList registeredProviderIds() const;

    /**
     * @brief Returns all available models across all registered providers.
     * @return Map of providerId → model name list.
     * @complexity O(n) where n = number of registered providers.
     */
    std::map<QString, QStringList> allAvailableModels() const;

    /**
     * @brief Resolves a model's max context window through its provider.
     * @param providerId Provider identifier (as in `LlmConfig::providerId`).
     * @param model      Model identifier within that provider.
     * @returns The provider's `contextWindowFor(model)` value, or 0 when the
     *          provider is not registered (or reports unknown).
     *
     * Thin delegate over `providerForId()`. The actual resolution policy
     * (published map / live cache / single-flight async warm) lives entirely
     * in each provider; this router method holds no context state. Non-const
     * because providers may warm an internal cache on a miss.
     */
    int contextWindowFor(const QString& providerId, const QString& model);

    /**
     * @brief Routes a request to the provider named by its config.
     *
     *        Per-request routing: the target provider is resolved
     *        from `request.config.providerId`:
     *          - empty providerId  → the active provider (m_active),
     *            preserving prior-default behaviour for callers that
     *            do not stamp config.providerId;
     *          - providerId == m_activeProviderId → m_active;
     *          - any other registered providerId  → that provider
     *            (this is what makes a per-agent provider override
     *            real at dispatch; see RequestBuilder);
     *          - an UNREGISTERED providerId        → emits requestError
     *            and dispatches nothing (defense-in-depth backstop;
     *            RequestBuilder's fallback normally prevents this).
     *
     *        `m_active` is NOT swapped. It reflects the UI-selected
     *        default; route() may dispatch elsewhere per-request
     *        without touching QML's active-provider bindings.
     *
     *        The chunk/finished/error signal relay is wired PER REQUEST
     *        inside route() (capturing this request's id) and torn down
     *        on the terminal signal (requestFinished / requestError).
     *        A straggler signal arriving after the terminal signal is
     *        therefore not relayed at all.
     * @param request The complete LLM request payload.
     * @sideeffects Connects a per-request relay on the target provider,
     *              then calls target->sendRequest(request). If no target
     *              can be resolved, emits requestError() and returns.
     */
    Q_INVOKABLE void route(const LlmRequest& request);

    /**
     * @brief Cancels in-flight foreground request(s).
     *
     * With the foreground slot no longer single-in-flight,
     * a bare "cancel current" cannot name one request. This now cancels
     * EVERY in-flight foreground target (each relay's provider) and tears
     * their relays down. When nothing is in flight it falls back to
     * cancelling m_active, so a bare cancelCurrent() stays a safe
     * no-op-or-cancel. Callers that own a specific request id should use
     * cancelRequest(requestId) to cancel only their own turn.
     */
    Q_INVOKABLE void cancelCurrent();

    /**
     * @brief Cancels one specific in-flight foreground request by id.
     *
     * Cancels the provider that this request was dispatched to and tears
     * down its relay, leaving every OTHER in-flight foreground request
     * untouched. The correct cancel under concurrent multi-chat: a run
     * stopping its own turn must not cancel another conversation's
     * parallel turn on a different provider.
     *
     * @param requestId The request to cancel. No-op when no relay exists
     *                  for it (already finished / never dispatched).
     */
    Q_INVOKABLE void cancelRequest(quint64 requestId);

    /**
     * @brief Allocates a process-wide unique foreground request id.
     *
     * The single source of truth for foreground LlmRequest::requestId
     * values. Every foreground dispatcher (ChatController /
     * ConversationRun for the direct path AND AgentService for each agent
     * reasoning step) stamps its requests from here so that, with
     * concurrent multi-chat in flight on the shared ModelRouter, no two
     * live requests share an id. Id collisions across separate counters
     * would let one run's requestId-gated terminal/chunk handler
     * mis-consume another run's signals.
     *
     * @returns A monotonically increasing id, never 0 (0 is the "no
     *          in-flight request" sentinel used by the stale-signal
     *          guards). Thread-safe (atomic), though all foreground
     *          dispatch is on the main thread.
     */
    static quint64 nextRequestId();

    // -----------------------------------------------------------------------
    // Background slot — heartbeat-only dispatch path.
    // -----------------------------------------------------------------------

    /**
     * @brief Registers a provider for the BACKGROUND slot. Transfers ownership.
     * @param provider Unique pointer to a provider implementation.
     * @sideeffects Stores in m_bgProviders. Background providers must be
     *              constructed with their own HttpClient instances so they
     *              do not share single-flight HTTP state with foreground
     *              providers of the same provider id.  Emits
     *              `providersChanged()` after registration.
     *
     * Background providers do NOT participate in the modelsRefreshed signal
     * forwarding; model lists are surfaced by foreground providers only.
     */
    void registerBackgroundProvider(std::unique_ptr<ILLMProvider> provider);

    /**
     * @brief Removes a previously-registered background provider and
     *        destroys its instance.
     * @param providerId  Provider identifier as returned by
     *                    `ILLMProvider::providerId()`.  No-op when the
     *                    id is unknown.
     * @sideeffects Clears `m_bgActive` and `m_bgCurrentTarget` when either
     *              was pointing at this instance; emits `providersChanged()`
     *              after removal.
     */
    void unregisterBackgroundProvider(const QString& providerId);

    /**
     * @brief Routes a request through the BACKGROUND slot.
     *
     *        Per-request routing, mirroring route() (see its header
     *        doc). The target background provider is resolved from
     *        `request.config.providerId`:
     *          - empty providerId  → the active bg provider (m_bgActive);
     *          - providerId == m_bgActiveProviderId → m_bgActive;
     *          - any other registered bg providerId → that bg provider
     *            (this is what makes a per-agent heartbeat provider
     *            override real at dispatch; see
     *            HeartbeatSubagentService::buildSubagentRequest);
     *          - an UNREGISTERED bg providerId → emits
     *            backgroundRequestError and dispatches nothing.
     *
     *        Independent of the foreground slot: a foreground route()
     *        in flight does NOT affect this dispatch. The bg relay is
     *        wired per-request inside routeBackground() and torn down
     *        on the terminal signal.
     * @param request The complete LLM request payload.
     * @sideeffects Connects a per-request bg relay on the target, then
     *              calls target->sendRequest(request). If no target can
     *              be resolved, emits backgroundRequestError() and returns.
     */
    Q_INVOKABLE void routeBackground(const LlmRequest& request);

    /**
     * @brief Cancels the in-flight background request on its actual
     *        target provider (which may differ from m_bgActive when an
     *        agent provider override routed it elsewhere). Falls back to
     *        m_bgActive when no background request is in flight.
     *        Foreground slot is untouched.
     */
    Q_INVOKABLE void cancelBackground();

    /**
     * @brief Returns the currently active BACKGROUND provider.
     * @return Pointer to bg active provider, or nullptr if none set.
     */
    ILLMProvider* bgActiveProvider() const;

    /**
     * @brief Returns a non-owning pointer to the BACKGROUND provider for the
     *        given id.
     * @param providerId Provider identifier string.
     * @return Pointer to bg provider, or nullptr if not registered.
     */
    ILLMProvider* bgProviderForId(const QString& providerId) const;

  signals:
    /**
     * @brief Re-emitted from the active provider's chunkReceived().
     * @param requestId Monotonic id of the originating request, set at
     *                  route() time. Subscribers gate their
     *                  continuation on matching this against their
     *                  local current-request id.
     * @param chunk     The streamed chunk payload (delta text / tool
     *                  call fragment / role marker).
     */
    void chunkReceived(quint64 requestId, const LlmChunk& chunk);

    /**
     * @brief Re-emitted from the active provider's requestFinished().
     * @param requestId    The originating request id (matches the
     *                     stamp on every chunk for this request).
     * @param finishReason Provider-reported reason ("stop",
     *                     "tool_call", "length", "cancelled",
     *                     "error", etc.).
     * @param totalTokens  Cumulative token count for the request, or 0
     *                     when the provider does not report it.
     */
    void requestFinished(quint64 requestId, const QString& finishReason, int totalTokens);

    /**
     * @brief Re-emitted from the active provider's requestError().
     * @param requestId    The originating request id whose dispatch
     *                     produced the error.
     * @param errorMessage Human-readable error text from the provider.
     */
    void requestError(quint64 requestId, const QString& errorMessage);

    /**
     * @brief Emitted when the active provider or model changes.
     * @param providerId Id of the newly-active provider (empty when
     *                   no provider is active).
     */
    void activeProviderChanged(const QString& providerId);

    /**
     * @brief Emitted after the registered-provider set has changed:
     *        a runtime-registered provider was added or removed.
     *
     * AgentSettingsController re-fires its own
     * `availableProvidersChanged` on receipt so QML provider-list
     * bindings re-evaluate without polling.  The signal fires once
     * per accepted register or unregister; it does NOT fire when a
     * register is rejected (null pointer, id collision).
     */
    void providersChanged();

    /**
     * @brief Re-emitted from any registered provider's
     *        modelsRefreshed(), meaning the catalog of available models
     *        changed for that provider.
     * @param providerId Id of the provider whose model list updated.
     * @param models     New list of model names for that provider.
     */
    void modelsRefreshed(const QString& providerId, const QStringList& models);

    // -----------------------------------------------------------------------
    // Background slot signals — same shape as foreground but on a
    // distinct channel so HeartbeatSubagentService never sees
    // foreground chunks and ChatController never sees background
    // chunks.
    // -----------------------------------------------------------------------

    /**
     * @brief Re-emitted from the active BACKGROUND provider's
     *        chunkReceived().
     * @param requestId Monotonic id stamped by routeBackground() at
     *                  dispatch time.
     * @param chunk     The streamed chunk payload from the background
     *                  request.
     */
    void backgroundChunkReceived(quint64 requestId, const LlmChunk& chunk);

    /**
     * @brief Re-emitted from the active BACKGROUND provider's
     *        requestFinished().
     * @param requestId    Background request id stamped at dispatch.
     * @param finishReason Provider-reported reason for completion.
     * @param totalTokens  Cumulative token count for the background
     *                     request, or 0 when not reported.
     */
    void backgroundRequestFinished(quint64 requestId, const QString& finishReason, int totalTokens);

    /**
     * @brief Re-emitted from the active BACKGROUND provider's
     *        requestError().
     * @param requestId    Background request id whose dispatch
     *                     produced the error.
     * @param errorMessage Human-readable error text from the provider.
     */
    void backgroundRequestError(quint64 requestId, const QString& errorMessage);

    /**
     * @brief Emitted when the active BACKGROUND provider mirror
     *        updates (driven by setActiveProvider when a matching bg
     *        provider exists).
     * @param providerId Id of the newly-active background provider.
     */
    void activeBackgroundProviderChanged(const QString& providerId);

  private:
    // Foreground slot state.
    std::map<QString, std::unique_ptr<ILLMProvider>> m_providers;
    ILLMProvider* m_active = nullptr;
    QString m_activeProviderId;
    QString m_activeModelName;

    /**
     * @brief Id of the most recently routed FOREGROUND request. Stamped on
     *        every foreground signal relay so subscribers can discriminate
     *        stale signals. Zero means "no foreground request in flight".
     */
    quint64 m_currentRequestId = 0;

    /**
     * @brief One foreground request's relay state.
     *
     * The foreground slot is no longer single-in-flight.
     * With concurrent multi-chat, several foreground requests can be in
     * flight at once (one per provider, serialized per provider by
     * Chat::ProviderScheduler). Each in-flight request therefore needs
     * its OWN relay so its terminal signal tears down ONLY its own
     * connections. A previous design with shared single-slot members
     * had request A's terminal disconnect request B's relay (a real bug
     * surfaced by parallel dispatch). Keyed by requestId in
     * m_fgRelays below.
     */
    struct ForegroundRelay {
        ILLMProvider* target = nullptr;
        QMetaObject::Connection chunk;
        QMetaObject::Connection finished;
        QMetaObject::Connection error;
    };

    /**
     * @brief Active foreground relays keyed by requestId. An entry exists
     *        for the lifetime of one request (added in route(), removed
     *        on its terminal signal). The modelsRefreshed connection
     *        wired per-provider in registerProvider() is separate and
     *        stays live regardless.
     */
    QHash<quint64, ForegroundRelay> m_fgRelays;

    // Background slot state.
    std::map<QString, std::unique_ptr<ILLMProvider>> m_bgProviders;
    ILLMProvider* m_bgActive = nullptr;
    QString m_bgActiveProviderId;
    QString m_bgActiveModelName;

    /**
     * @brief Id of the most recently routed BACKGROUND request. Independent
     *        of m_currentRequestId; a foreground turn cannot overwrite this.
     *        Zero means "no background request in flight".
     */
    quint64 m_currentBgRequestId = 0;

    /**
     * @brief The bg provider the in-flight background request was
     *        dispatched to. Set by routeBackground(); cleared by
     *        teardownBackgroundRelay() on the terminal signal. May be a
     *        provider OTHER than m_bgActive when an agent provider
     *        override routed the heartbeat request elsewhere. Null when
     *        no background request is in flight.
     */
    ILLMProvider* m_bgCurrentTarget = nullptr;

    /** @brief Connection handles for the CURRENT background request's
     *         relay (parallel to the foreground m_fg*Conn members).
     *         Wired per-request inside routeBackground() and torn down
     *         on the terminal signal, not tied to the active bg
     *         provider's lifetime. */
    QMetaObject::Connection m_bgChunkConn;
    QMetaObject::Connection m_bgFinishedConn;
    QMetaObject::Connection m_bgErrorConn;

    /**
     * @brief Wires a per-request foreground relay on `target` that tags
     *        every chunk/finished/error with `requestId` (a fixed value
     *        captured here, NOT read from m_currentRequestId at emit
     *        time) and tears itself down on the terminal signal.
     * @param target    The provider this request was dispatched to.
     * @param requestId The id stamped onto every relayed signal AND the
     *                  key under which the relay is stored in m_fgRelays.
     * @sideeffects Inserts a ForegroundRelay into m_fgRelays[requestId].
     */
    void connectForegroundRelay(ILLMProvider* target, quint64 requestId);

    /**
     * @brief Disconnects and removes ONE foreground request's relay.
     *        Called on that request's terminal signal from inside the
     *        relay. Safe to call for a requestId with no relay (no-op),
     *        which is how a straggler after teardown is ignored.
     * @param requestId The request whose relay to tear down.
     */
    void teardownForegroundRelay(quint64 requestId);

    /**
     * @brief Wires a per-request background relay on `target` that tags
     *        every bg chunk/finished/error with `requestId` (captured by
     *        value) and tears itself down on the terminal signal.
     *        Background-slot mirror of connectForegroundRelay().
     * @param target    The bg provider this request was dispatched to.
     * @param requestId The id stamped onto every relayed bg signal.
     */
    void connectBackgroundRelay(ILLMProvider* target, quint64 requestId);

    /**
     * @brief Disconnects the current background request's relay and
     *        clears m_bgCurrentTarget. Background-slot mirror of
     *        teardownForegroundRelay(). Safe to call when no bg relay is
     *        connected.
     */
    void teardownBackgroundRelay();
};

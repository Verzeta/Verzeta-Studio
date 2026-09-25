// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file model-router.cpp
 * @brief Implementation of the ModelRouter central dispatch service.
 *        Manages provider registration, active-provider switching, and
 *        transparent signal forwarding to consumers.
 * @layer Service
 * @dependencies ILLMProvider (API), Qt6::Core
 */

#include "model-router.h"

#include "../utils/logger.h"

#include <map>
#include <QAtomicInteger>
#include <QSet>

namespace {
/// Process-wide source of unique foreground request ids (see
/// ModelRouter::nextRequestId). All foreground dispatchers draw from here
/// so concurrent in-flight requests never share an id.
QAtomicInteger<quint64> g_foregroundRequestIdCounter{0};
}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/**
 * @brief Constructs the ModelRouter with no registered providers.
 * @param parent Optional Qt parent.
 */
ModelRouter::ModelRouter(QObject* parent) : QObject(parent) {}

/**
 * @brief Allocates a process-wide unique foreground request id.
 * @returns A monotonically increasing id; the first id is 1 (never the 0
 *          "no in-flight" sentinel).
 */
quint64 ModelRouter::nextRequestId() {
    return g_foregroundRequestIdCounter.fetchAndAddOrdered(1) + 1;
}

// ---------------------------------------------------------------------------
// Provider registration
// ---------------------------------------------------------------------------

/*
 * @brief Registers a provider. Transfers ownership to the router.
 * @param provider Unique pointer to a provider implementation.
 * @sideeffects Stores the provider in m_providers under its providerId.
 *              Connects modelsRefreshed for all providers (not just the active one).
 */
void ModelRouter::registerProvider(std::unique_ptr<ILLMProvider> provider) {
    if (!provider) {
        qCWarning(verzetaLlm) << "ModelRouter: attempted to register null provider";
        return;
    }

    const QString id = provider->providerId();
    if (m_providers.contains(id)) {
        qCWarning(verzetaLlm) << "ModelRouter: provider already registered:" << id;
        return;
    }

    ILLMProvider* rawPtr = provider.get();

    // Forward modelsRefreshed from all providers (even inactive ones)
    connect(rawPtr, &ILLMProvider::modelsRefreshed, this, [this, id](const QStringList& models) {
        emit modelsRefreshed(id, models);
    });

    m_providers.emplace(id, std::move(provider));
    qCInfo(verzetaLlm) << "ModelRouter: registered provider:" << id;
    emit providersChanged();
}

/*
 * @brief Removes a previously-registered foreground provider.
 * @param providerId The id to remove.
 * @sideeffects Disconnects all signals from the provider; clears
 *              m_active when it was pointing at this instance;
 *              destroys the provider via unique_ptr; emits
 *              providersChanged.  No-op when providerId is unknown.
 */
void ModelRouter::unregisterProvider(const QString& providerId) {
    const auto it = m_providers.find(providerId);
    if (it == m_providers.end()) {
        return;
    }
    ILLMProvider* rawPtr = it->second.get();

    // If this is the current active provider, disconnect the per-
    // request relay and clear m_active so a subsequent route() does
    // not dereference a freed pointer.  Same defensive pattern the
    // setActiveProvider switch path applies.
    if (m_active == rawPtr) {
        disconnect(rawPtr, nullptr, this, nullptr);
        m_active = nullptr;
        m_activeProviderId.clear();
        m_activeModelName.clear();
        emit activeProviderChanged(QString());
    } else {
        // Inactive provider — only the modelsRefreshed forwarder is
        // wired by registerProvider; disconnect it before destroying.
        disconnect(rawPtr, nullptr, this, nullptr);
    }
    // Drop any in-flight foreground relays that targeted this provider —
    // the disconnect() above already severed their connections, but the
    // map entries must go too so a later terminal / cancel cannot read a
    // dangling target. (relays are per-request now.)
    for (auto rit = m_fgRelays.begin(); rit != m_fgRelays.end();) {
        if (rit->target == rawPtr) {
            rit = m_fgRelays.erase(rit);
        } else {
            ++rit;
        }
    }

    m_providers.erase(it);
    qCInfo(verzetaLlm) << "ModelRouter: unregistered provider:" << providerId;
    emit providersChanged();
}

// ---------------------------------------------------------------------------
// Provider access
// ---------------------------------------------------------------------------

/**
 * @brief Returns a non-owning pointer to the provider for the given ID.
 * @param providerId Provider identifier string.
 * @return Pointer to provider, or nullptr if not registered.
 */
ILLMProvider* ModelRouter::providerForId(const QString& providerId) const {
    const auto it = m_providers.find(providerId);
    if (it == m_providers.end()) {
        return nullptr;
    }
    return it->second.get();
}

int ModelRouter::contextWindowFor(const QString& providerId, const QString& model) {
    // Resolve the SAME target route() will dispatch to, so the context window
    // we report matches the provider that actually serves the turn. route()
    // treats an empty providerId (or one equal to the active id) as "use the
    // active provider" — the common path, since RequestBuilder may not have
    // stamped a per-agent override. Mirroring that here is essential: without
    // it an empty providerId silently resolved to no provider → 0 → the
    // budgeter kept the default window and model-aware sizing never applied.
    ILLMProvider* target = nullptr;
    if (providerId.isEmpty() || providerId == m_activeProviderId) {
        target = m_active;
    } else {
        target = providerForId(providerId);
    }
    if (!target) {
        return 0;
    }
    // An empty model likewise means "the active model" (route() carries the
    // request's model; when absent the active selection is what runs).
    const QString resolvedModel = model.isEmpty() ? m_activeModelName : model;
    return target->contextWindowFor(resolvedModel);
}

/**
 * @brief Returns the currently active provider.
 * @return Pointer to active provider, or nullptr if none set.
 */
ILLMProvider* ModelRouter::activeProvider() const {
    return m_active;
}

// ---------------------------------------------------------------------------
// Active provider management
// ---------------------------------------------------------------------------

/*
 * @brief Sets the active provider and model name.
 * @param providerId Provider identifier. Must already be registered.
 * @param modelName  Model name within that provider.
 *
 * Side effects, including the background-slot mirror, are documented on
 * the declaration in model-router.h.
 */
void ModelRouter::setActiveProvider(const QString& providerId, const QString& modelName) {
    const auto it = m_providers.find(providerId);
    if (it == m_providers.end()) {
        qCWarning(verzetaLlm) << "ModelRouter: unknown provider ID:" << providerId;
        emit requestError(m_currentRequestId,
                          QStringLiteral("The provider %1 is not available.").arg(providerId));
        return;
    }

    m_activeProviderId = providerId;
    m_activeModelName = modelName;
    m_active = it->second.get();

    qCInfo(verzetaLlm) << "ModelRouter: active provider set to" << providerId
                       << "model:" << modelName;
    emit activeProviderChanged(providerId);

    const auto bgIt = m_bgProviders.find(providerId);
    if (bgIt != m_bgProviders.end()) {
        m_bgActiveProviderId = providerId;
        m_bgActiveModelName = modelName;
        m_bgActive = bgIt->second.get();
        qCInfo(verzetaLlm) << "ModelRouter: bg active provider mirrored to" << providerId
                           << "model:" << modelName;
        emit activeBackgroundProviderChanged(providerId);
    } else {
        // FG provider has no bg counterpart — clear bg active so an
        // override-free heartbeat dispatch surfaces a clean error.
        const bool hadBgActive = (m_bgActive != nullptr);
        m_bgActive = nullptr;
        m_bgActiveProviderId.clear();
        m_bgActiveModelName.clear();
        if (hadBgActive) {
            qCDebug(verzetaLlm) << "ModelRouter: no bg counterpart for" << providerId
                                << "— bg slot cleared";
            emit activeBackgroundProviderChanged(QString());
        }
    }
}

QString ModelRouter::activeProviderId() const {
    return m_activeProviderId;
}

QString ModelRouter::activeModelName() const {
    return m_activeModelName;
}

/**
 * @brief Returns the IDs of all registered providers.
 * @return List of provider ID strings.
 */
QStringList ModelRouter::registeredProviderIds() const {
    QStringList ids;
    ids.reserve(static_cast<int>(m_providers.size()));
    for (const auto& [id, _] : m_providers) {
        ids.append(id);
    }
    return ids;
}

/**
 * @brief Returns all available models across all registered providers.
 * @return Map of providerId → model name list.
 */
std::map<QString, QStringList> ModelRouter::allAvailableModels() const {
    std::map<QString, QStringList> result;
    for (const auto& [id, provider] : m_providers) {
        result.emplace(id, provider->availableModels());
    }
    return result;
}

// ---------------------------------------------------------------------------
// Request routing
// ---------------------------------------------------------------------------

/*
 * @brief Routes a request to the provider named by request.config.
 *        See the header doc for the full per-request resolution rules.
 * @param request The complete LLM request payload.
 */
void ModelRouter::route(const LlmRequest& request) {
    // the foreground slot is multi-in-flight. route() must
    // NOT tear down other in-flight relays — each request owns its own
    // relay keyed by requestId and tears down only itself on its
    // terminal. A stale relay for THIS exact id (a re-dispatch reusing an
    // id, which the process-unique counter prevents) would be replaced
    // below by the insert; clear it first to be safe.
    teardownForegroundRelay(request.requestId);

    // Remember the request id for this round-trip. Kept for any
    // consumer that reads m_currentRequestId directly; the relay
    // itself now captures the id per-request (see connectForegroundRelay)
    // rather than reading this field at emit time.
    m_currentRequestId = request.requestId;

    const QString& wantId = request.config.providerId;
    ILLMProvider* target = nullptr;
    if (wantId.isEmpty() || wantId == m_activeProviderId) {
        // Empty providerId means the earlier behaviour (use the active
        // provider). Matching the active id → same instance.
        target = m_active;
    } else {
        target = providerForId(wantId);
    }

    if (!target) {
        qCWarning(verzetaLlm) << "ModelRouter: no provider for request" << m_currentRequestId
                              << "— config.providerId=" << wantId;
        emit requestError(
            m_currentRequestId,
            wantId.isEmpty()
                ? QStringLiteral("No LLM provider is active. Choose a provider and model first.")
                : QStringLiteral("The provider %1 is not available.").arg(wantId));
        return;
    }

    // Wire the per-request relay on the resolved target, then dispatch.
    // The relay tears itself down on the terminal signal.
    connectForegroundRelay(target, request.requestId);

    qCDebug(verzetaLlm) << "ModelRouter: routing request" << m_currentRequestId << "to"
                        << target->providerId();
    target->sendRequest(request);
}

/**
 * @brief Cancels the in-flight foreground request on its actual target
 *        provider (which may differ from m_active when an agent
 *        provider override routed it elsewhere). Falls back to m_active
 *        when no request is in flight.
 */
void ModelRouter::cancelCurrent() {
    if (m_fgRelays.isEmpty()) {
        // Nothing in flight — fall back to cancelling the active provider
        // so a bare cancelCurrent() stays a safe no-op-or-cancel.
        if (m_active) {
            m_active->cancelRequest();
            qCDebug(verzetaLlm) << "ModelRouter: cancelCurrent fallback —"
                                << "cancelled active provider" << m_active->providerId();
        }
        return;
    }
    // Cancel every in-flight foreground request. Snapshot keys first
    // because each terminal teardown mutates m_fgRelays. De-duplicate the
    // provider cancel via a visited set (two requests may share a
    // provider once per-provider limits rise above 1).
    const QList<quint64> ids = m_fgRelays.keys();
    QSet<ILLMProvider*> cancelled;
    for (quint64 id : ids) {
        auto it = m_fgRelays.constFind(id);
        if (it == m_fgRelays.constEnd() || !it->target)
            continue;
        if (cancelled.contains(it->target))
            continue;
        cancelled.insert(it->target);
        it->target->cancelRequest();
        qCDebug(verzetaLlm) << "ModelRouter: cancelCurrent cancelled request on"
                            << it->target->providerId();
    }
}

void ModelRouter::cancelRequest(quint64 requestId) {
    auto it = m_fgRelays.constFind(requestId);
    if (it == m_fgRelays.constEnd() || !it->target) {
        return;  // not in flight
    }
    ILLMProvider* target = it->target;
    target->cancelRequest();
    qCDebug(verzetaLlm) << "ModelRouter: cancelled request" << requestId << "on"
                        << target->providerId();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Wires a per-request foreground relay on `target`.
 *
 *        Each relay lambda captures `requestId` by
 *        value (a fixed id for the lifetime of this one request), so
 *        relayed chunk/finished/error signals carry the id of the
 *        request that actually originated them, NOT whatever the most
 *        recent route() stamped. The terminal-signal lambdas
 *        (requestFinished / requestError) tear the relay down after
 *        re-emitting, so a straggler arriving after the terminal signal
 *        is not relayed at all. Disconnecting a connection from inside
 *        its own slot is explicitly supported by Qt.
 * @param target    The provider this request was dispatched to.
 * @param requestId The id stamped onto every relayed signal.
 */
void ModelRouter::connectForegroundRelay(ILLMProvider* target, quint64 requestId) {
    if (!target) {
        return;
    }

    ForegroundRelay relay;
    relay.target = target;
    relay.chunk = connect(
        target, &ILLMProvider::chunkReceived, this, [this, requestId](const LlmChunk& chunk) {
            emit chunkReceived(requestId, chunk);
        });
    relay.finished = connect(target,
                             &ILLMProvider::requestFinished,
                             this,
                             [this, requestId](const QString& finishReason, int totalTokens) {
                                 // Terminal signal — tear THIS request's relay down
                                 // BEFORE re-emitting. emit requestFinished can
                                 // synchronously re-enter route() (an agent pattern
                                 // dispatching its next turn straight from the finished
                                 // handler — planner / router / multi-agent all do
                                 // this). Tearing down first keeps the re-entry safe;
                                 // keying by requestId means only this request's relay
                                 // is removed, never a concurrent one's.
                                 teardownForegroundRelay(requestId);
                                 emit requestFinished(requestId, finishReason, totalTokens);
                             });
    relay.error = connect(
        target, &ILLMProvider::requestError, this, [this, requestId](const QString& errorMessage) {
            // Terminal signal — tear down before re-emitting,
            // same re-entrancy reasoning as requestFinished.
            teardownForegroundRelay(requestId);
            emit requestError(requestId, errorMessage);
        });

    m_fgRelays.insert(requestId, relay);
}

/**
 * @brief Disconnects the current foreground request's relay and clears
 *        m_fgCurrentTarget. The modelsRefreshed connection wired
 *        per-provider in registerProvider() is separate and stays live.
 *        Safe to call when no relay is connected (disconnect on a null
 *        Connection is a Qt no-op).
 */
void ModelRouter::teardownForegroundRelay(quint64 requestId) {
    auto it = m_fgRelays.find(requestId);
    if (it == m_fgRelays.end()) {
        return;  // no relay for this id (already torn down / never wired)
    }
    QObject::disconnect(it->chunk);
    QObject::disconnect(it->finished);
    QObject::disconnect(it->error);
    m_fgRelays.erase(it);
}


/**
 * @brief Registers a provider for the BACKGROUND slot. Transfers ownership.
 *
 * Background providers MUST be constructed with their own HttpClient
 * instances (or, for in-process providers like LlamaCpp, their own
 * provider instance). They share NO state with the foreground provider
 * of the same provider id. This is what gives heartbeat dispatch
 * concurrency safety relative to user-driven foreground turns.
 *
 * Background providers do NOT connect modelsRefreshed forwarding;
 * model list discovery is a foreground-slot concern.
 */
void ModelRouter::registerBackgroundProvider(std::unique_ptr<ILLMProvider> provider) {
    if (!provider) {
        qCWarning(verzetaLlm) << "ModelRouter: attempted to register null bg provider";
        return;
    }

    const QString id = provider->providerId();
    if (m_bgProviders.contains(id)) {
        qCWarning(verzetaLlm) << "ModelRouter: bg provider already registered:" << id;
        return;
    }

    m_bgProviders.emplace(id, std::move(provider));
    qCInfo(verzetaLlm) << "ModelRouter: registered bg provider:" << id;
    emit providersChanged();
}

/*
 * @brief Removes a previously-registered background provider.
 * @param providerId The id to remove.
 * @sideeffects Clears m_bgActive when it was pointing at this
 *              instance; clears m_bgCurrentTarget when it was
 *              pointing at this instance; destroys the provider via
 *              unique_ptr; emits providersChanged.  No-op when
 *              providerId is unknown.
 */
void ModelRouter::unregisterBackgroundProvider(const QString& providerId) {
    const auto it = m_bgProviders.find(providerId);
    if (it == m_bgProviders.end()) {
        return;
    }
    ILLMProvider* rawPtr = it->second.get();
    if (m_bgActive == rawPtr) {
        m_bgActive = nullptr;
        m_bgActiveProviderId.clear();
        m_bgActiveModelName.clear();
    }
    if (m_bgCurrentTarget == rawPtr) {
        m_bgCurrentTarget = nullptr;
    }
    m_bgProviders.erase(it);
    qCInfo(verzetaLlm) << "ModelRouter: unregistered bg provider:" << providerId;
    emit providersChanged();
}

/**
 * @brief Returns a non-owning pointer to the BACKGROUND provider for the
 *        given id.
 */
ILLMProvider* ModelRouter::bgProviderForId(const QString& providerId) const {
    const auto it = m_bgProviders.find(providerId);
    if (it == m_bgProviders.end()) {
        return nullptr;
    }
    return it->second.get();
}

/**
 * @brief Returns the currently active BACKGROUND provider.
 */
ILLMProvider* ModelRouter::bgActiveProvider() const {
    return m_bgActive;
}

/**
 * @brief Routes a request through the BACKGROUND slot. Per-request
 *        routing; see the header doc for the full resolution rules.
 *
 * Independent of the foreground slot: the foreground slot's m_active,
 * m_currentRequestId, and relay are NOT touched. A foreground request
 * in flight continues to receive its chunks on the foreground signals;
 * this dispatch's chunks fire on the bg signals.
 */
void ModelRouter::routeBackground(const LlmRequest& request) {
    // Defensive: clear any bg relay a prior request left behind.
    teardownBackgroundRelay();

    m_currentBgRequestId = request.requestId;

    const QString& wantId = request.config.providerId;
    ILLMProvider* target = nullptr;
    if (wantId.isEmpty() || wantId == m_bgActiveProviderId) {
        target = m_bgActive;
    } else {
        target = bgProviderForId(wantId);
    }

    if (!target) {
        qCWarning(verzetaLlm) << "ModelRouter: no bg provider for request" << m_currentBgRequestId
                              << "— config.providerId=" << wantId;
        emit backgroundRequestError(
            m_currentBgRequestId,
            wantId.isEmpty()
                ? QStringLiteral("No background LLM provider is active")
                : QStringLiteral("Background provider not registered: %1").arg(wantId));
        return;
    }

    connectBackgroundRelay(target, request.requestId);

    qCDebug(verzetaLlm) << "ModelRouter: bg routing request" << m_currentBgRequestId << "to"
                        << target->providerId();
    target->sendRequest(request);
}

/**
 * @brief Cancels the in-flight background request on its actual target
 *        provider (which may differ from m_bgActive under an agent
 *        provider override). Falls back to m_bgActive when no bg request
 *        is in flight. Foreground slot is untouched.
 */
void ModelRouter::cancelBackground() {
    ILLMProvider* target = m_bgCurrentTarget ? m_bgCurrentTarget : m_bgActive;
    if (target) {
        target->cancelRequest();
        qCDebug(verzetaLlm) << "ModelRouter: bg cancelled on" << target->providerId();
    }
}

/**
 * @brief Wires a per-request background relay on `target`. Background-slot
 *        mirror of connectForegroundRelay(). Each lambda captures
 *        `requestId` by value, emits on the bg signal channel, and the
 *        terminal lambdas tear the relay down BEFORE re-emitting (same
 *        re-entrancy reasoning as the foreground path).
 */
void ModelRouter::connectBackgroundRelay(ILLMProvider* target, quint64 requestId) {
    if (!target) {
        return;
    }
    m_bgCurrentTarget = target;

    m_bgChunkConn = connect(
        target, &ILLMProvider::chunkReceived, this, [this, requestId](const LlmChunk& chunk) {
            emit backgroundChunkReceived(requestId, chunk);
        });
    m_bgFinishedConn =
        connect(target,
                &ILLMProvider::requestFinished,
                this,
                [this, requestId](const QString& finishReason, int totalTokens) {
                    teardownBackgroundRelay();
                    emit backgroundRequestFinished(requestId, finishReason, totalTokens);
                });
    m_bgErrorConn = connect(
        target, &ILLMProvider::requestError, this, [this, requestId](const QString& errorMessage) {
            teardownBackgroundRelay();
            emit backgroundRequestError(requestId, errorMessage);
        });
}

/**
 * @brief Disconnects the current background request's relay and clears
 *        m_bgCurrentTarget. Background-slot mirror of
 *        teardownForegroundRelay(). Safe to call when no relay is wired.
 */
void ModelRouter::teardownBackgroundRelay() {
    QObject::disconnect(m_bgChunkConn);
    QObject::disconnect(m_bgFinishedConn);
    QObject::disconnect(m_bgErrorConn);
    m_bgChunkConn = {};
    m_bgFinishedConn = {};
    m_bgErrorConn = {};
    m_bgCurrentTarget = nullptr;
}

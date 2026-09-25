// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file session-router.cpp
 * @brief Implementation of the per-frontend ChatController registry.
 *        See session-router.h for the architectural rationale.
 * @layer Service
 * @dependencies ChatController, the four engine refs (ConversationService,
 *               MessageService, ModelRouter, ExportService).
 */

#include "session-router.h"

#include "../../utils/logger.h"
#include "../../utils/thread-discipline.h"
#include "../agent-settings-controller.h"
#include "../chat-controller.h"

namespace Verzeta::Session {

SessionRouter::SessionRouter(ConversationService& convs,
                             MessageService& msgs,
                             ModelRouter& router,
                             ExportService& exportSvc,
                             QObject* parent)
    : QObject(parent), m_convs(convs), m_msgs(msgs), m_router(router), m_exportSvc(exportSvc) {
    qCInfo(verzetaUi) << "SessionRouter initialized";
}

SessionRouter::~SessionRouter() {
    // m_wireSessions's unique_ptrs auto-delete the owned ChatControllers
    // in destructor order. Local session is owned by AppController so
    // m_localCC is just a non-owning QPointer; nothing to release here.
    qCInfo(verzetaUi) << "SessionRouter destroyed; " << m_wireSessions.size()
                      << "wire session(s) torn down";
}

void SessionRouter::registerLocalSession(ChatController* localCC) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!localCC) {
        qCWarning(verzetaUi) << "SessionRouter::registerLocalSession: null ChatController";
        return;
    }
    if (m_localCC) {
        qCWarning(verzetaUi) << "SessionRouter::registerLocalSession: local session already "
                                "registered; replacing pointer (was the previous "
                                "ChatController torn down?)";
    }
    m_localCC = localCC;
    wireGenerationTracking(localCC);
    qCInfo(verzetaUi) << "SessionRouter: local session registered";
}

void SessionRouter::registerLocalAgentSettings(AgentSettingsController* localAS) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!localAS) {
        qCWarning(verzetaUi) << "SessionRouter::registerLocalAgentSettings: null pointer";
        return;
    }
    if (m_localAS) {
        qCWarning(verzetaUi) << "SessionRouter::registerLocalAgentSettings: local AS "
                                "already registered; replacing pointer";
    }
    m_localAS = localAS;
    qCInfo(verzetaUi) << "SessionRouter: local AgentSettings registered";
}

void SessionRouter::setAgentSettingsSetupFn(AgentSettingsSetupFn fn) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_settingsSetupFn = std::move(fn);
    qCInfo(verzetaUi) << "SessionRouter: AgentSettings setup callback"
                      << (m_settingsSetupFn ? "installed" : "cleared");
}

void SessionRouter::setChatControllerSetupFn(ChatControllerSetupFn fn) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_setupFn = std::move(fn);
    qCInfo(verzetaUi) << "SessionRouter: ChatController setup callback"
                      << (m_setupFn ? "installed" : "cleared");
}

ChatController* SessionRouter::createWireSession(const QString& sessionId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (sessionId.isEmpty()) {
        qCWarning(verzetaUi) << "SessionRouter::createWireSession: empty sessionId rejected";
        return nullptr;
    }
    if (sessionId == QString::fromUtf8(kLocalSessionId)) {
        qCWarning(verzetaUi) << "SessionRouter::createWireSession: cannot create wire "
                                "session with local session id";
        return nullptr;
    }
    auto existing = m_wireSessions.find(sessionId);
    if (existing != m_wireSessions.end()) {
        qCWarning(verzetaUi) << "SessionRouter::createWireSession: session already exists:"
                             << sessionId;
        return existing->second.get();
    }

    auto cc = std::make_unique<ChatController>(m_router, m_convs, m_msgs, m_exportSvc, this);
    ChatController* raw = cc.get();

    // Apply the setter recipe AppController uses for the local
    // ChatController. Without this the wire-side instance is missing
    // its tool/rag/agent/canvas/task/skill/heartbeat plumbing.
    if (m_setupFn) {
        m_setupFn(raw);
    } else {
        qCWarning(verzetaUi) << "SessionRouter::createWireSession: setup fn not "
                                "installed; wire ChatController for sessionId"
                             << sessionId
                             << "is unconfigured (sendMessage will lack tools/rag/"
                                "agent/etc.). AppController must call "
                                "setChatControllerSetupFn during initialize().";
    }

    wireGenerationTracking(raw);

    m_wireSessions.emplace(sessionId, std::move(cc));

    QObject::connect(
        raw, &ChatController::userMessageQueued, this, [this, sessionId](const QString& text) {
            emit wireSessionUserMessageQueued(sessionId, text);
        });

    auto as = std::make_unique<AgentSettingsController>(m_convs, m_router, this);
    AgentSettingsController* asRaw = as.get();
    if (m_settingsSetupFn) {
        m_settingsSetupFn(asRaw);
    }
    QObject::connect(raw, &ChatController::activeConversationChanged, asRaw, [raw, asRaw]() {
        asRaw->setActiveConversationId(raw->activeConversationId());
    });
    m_wireAgentSettings.emplace(sessionId, std::move(as));

    qCInfo(verzetaUi) << "SessionRouter: created wire session" << sessionId;
    return raw;
}

AgentSettingsController* SessionRouter::createWireAgentSettings(const QString& sessionId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (sessionId.isEmpty())
        return nullptr;
    if (sessionId == QString::fromUtf8(kLocalSessionId))
        return nullptr;

    // If the AS already exists, return it. Otherwise lazy-create the
    // full session — createWireSession spawns CC + AS in lockstep.
    auto it = m_wireAgentSettings.find(sessionId);
    if (it != m_wireAgentSettings.end())
        return it->second.get();

    if (!createWireSession(sessionId))
        return nullptr;

    it = m_wireAgentSettings.find(sessionId);
    return it == m_wireAgentSettings.end() ? nullptr : it->second.get();
}

AgentSettingsController* SessionRouter::agentSettingsFor(const QString& sessionId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (sessionId == QString::fromUtf8(kLocalSessionId)) {
        return m_localAS.data();
    }
    const auto it = m_wireAgentSettings.find(sessionId);
    return it == m_wireAgentSettings.end() ? nullptr : it->second.get();
}

AgentSettingsController* SessionRouter::localAgentSettings() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_localAS.data();
}

bool SessionRouter::anySessionGenerating() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_generatingCount > 0;
}

void SessionRouter::onChatGeneratingChanged() {
    VERZETA_ASSERT_MAIN_THREAD();
    auto* cc = qobject_cast<ChatController*>(sender());
    if (!cc)
        return;

    const bool nowGen = cc->isAnyConvGenerating();
    const bool wasGen = m_genStateByCC.value(cc, false);
    if (nowGen == wasGen)
        return;  // false alarm — value didn't actually flip

    m_genStateByCC[cc] = nowGen;
    if (nowGen) {
        ++m_generatingCount;
    } else {
        --m_generatingCount;
        if (m_generatingCount < 0) {
            // Defensive: never go negative even if a destroyed CC's
            // signal slipped through. Re-clamp; log once per occurrence.
            qCWarning(verzetaUi) << "SessionRouter::onChatGeneratingChanged: count went "
                                    "negative — clamping to 0 (destroyed CC?)";
            m_generatingCount = 0;
        }
        if (m_generatingCount == 0) {
            // Last in-flight CC just released the slot. Notify any
            // queued ChatControllers to drain.
            emit slotAvailable();
        }
    }
}

void SessionRouter::wireGenerationTracking(ChatController* cc) {
    if (!cc)
        return;
    // Initialize cache to current state (false at construction time).
    m_genStateByCC.insert(cc, cc->isAnyConvGenerating());
    // Subscribe to gen-state transitions. Direct connection — same
    // thread, synchronous. Auto-disconnects when either side dies via
    // Qt's normal connection lifetime.
    QObject::connect(
        cc, &ChatController::isGeneratingChanged, this, &SessionRouter::onChatGeneratingChanged);
    // Install the back-pointer so this CC's sendMessage can gate on
    // anySessionGenerating() and subscribe to slotAvailable.
    cc->setSessionRouter(this);
    QObject::connect(this, &SessionRouter::slotAvailable, cc, [cc]() { cc->onSlotAvailable(); });
}

void SessionRouter::destroySession(const QString& sessionId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (sessionId == QString::fromUtf8(kLocalSessionId)) {
        // Local lifecycle is AppController's. Forget our cached
        // pointer but do NOT delete.
        if (m_localCC) {
            const bool wasGen = m_genStateByCC.value(m_localCC, false);
            m_genStateByCC.remove(m_localCC);
            if (wasGen && m_generatingCount > 0) {
                --m_generatingCount;
                if (m_generatingCount == 0)
                    emit slotAvailable();
            }
        }
        m_localCC.clear();
        m_localAS.clear();
        qCInfo(verzetaUi) << "SessionRouter: local session unregistered";
        return;
    }
    auto it = m_wireSessions.find(sessionId);
    if (it == m_wireSessions.end()) {
        qCInfo(verzetaUi) << "SessionRouter::destroySession: no session" << sessionId;
        return;
    }
    ChatController* dying = it->second.get();
    const bool wasGen = m_genStateByCC.value(dying, false);
    m_genStateByCC.remove(dying);
    if (wasGen && m_generatingCount > 0) {
        --m_generatingCount;
        if (m_generatingCount == 0)
            emit slotAvailable();
    }
    m_wireSessions.erase(it);  // unique_ptr in mapped value auto-deletes

    auto asIt = m_wireAgentSettings.find(sessionId);
    if (asIt != m_wireAgentSettings.end()) {
        m_wireAgentSettings.erase(asIt);
    }

    qCInfo(verzetaUi) << "SessionRouter: destroyed wire session" << sessionId;
}

void SessionRouter::destroyAllWireSessions() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_wireSessions.empty())
        return;
    const auto count = m_wireSessions.size();
    int decrement = 0;
    for (auto& [id, cc] : m_wireSessions) {
        ChatController* raw = cc.get();
        if (m_genStateByCC.value(raw, false))
            ++decrement;
        m_genStateByCC.remove(raw);
    }
    if (decrement > 0) {
        m_generatingCount = qMax(0, m_generatingCount - decrement);
        if (m_generatingCount == 0)
            emit slotAvailable();
    }
    m_wireSessions.clear();
    m_wireAgentSettings.clear();
    qCInfo(verzetaUi) << "SessionRouter: destroyed all" << count
                      << "wire session(s) (typically called on "
                         "verzeta-remote IPC disconnect)";
}

ChatController* SessionRouter::sessionFor(const QString& sessionId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (sessionId == QString::fromUtf8(kLocalSessionId)) {
        return m_localCC.data();
    }
    const auto it = m_wireSessions.find(sessionId);
    return it == m_wireSessions.end() ? nullptr : it->second.get();
}

ChatController* SessionRouter::localSession() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_localCC.data();
}

int SessionRouter::sessionCount() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return (m_localCC ? 1 : 0) + m_wireSessions.size();
}

}  // namespace Verzeta::Session

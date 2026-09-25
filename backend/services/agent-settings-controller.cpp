// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent-settings-controller.cpp
 * @brief Implementation of the `AgentSettings` QML singleton, which owns
 *        provider/model + per-conversation settings + agent-pattern
 *        + tool toggle. ChatController subscribes to a few internal
 *        signals (agentPatternChanged, requireConfirmationChanged,
 *        toolsEnabledChanged) to keep its synchronous sendMessage
 *        reads consistent.
 * @layer Service
 * @dependencies ConversationService, ModelRouter (references).
 */

#include "agent-settings-controller.h"

#include "../api/llm-interface.h"
#include "../models/conversation.h"
#include "../models/llm-config.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "conversation-service.h"
#include "model-router.h"
#include "model-sampling-profile.h"
#include "settings-service.h"

AgentSettingsController::AgentSettingsController(ConversationService& convSvc,
                                                 ModelRouter& router,
                                                 QObject* parent)
    : QObject(parent), m_convSvc(convSvc), m_router(router) {
    // Forward ModelRouter signals onto our QML-facing channels so
    // QML never holds a ModelRouter reference. activeProviderChanged
    // covers BOTH activeProvider and activeModel Q_PROPERTYs (they
    // share the NOTIFY).
    connect(&m_router,
            &ModelRouter::activeProviderChanged,
            this,
            &AgentSettingsController::activeProviderChanged);
    connect(
        &m_router, &ModelRouter::modelsRefreshed, this, &AgentSettingsController::modelsRefreshed);
    // Runtime provider-set changes (custom OpenAI-API-compatible
    // servers being added or removed via CustomServerRegistry) need
    // to re-fire the QML availableProviders binding so the provider
    // dropdown picks them up without a refresh.
    connect(&m_router,
            &ModelRouter::providersChanged,
            this,
            &AgentSettingsController::availableProvidersChanged);

    connect(&m_convSvc, &ConversationService::conversationUpdated, this, [this](const QString& id) {
        if (id == m_activeConvId) {
            refreshFromActiveConv();
            emit activeConversationSettingsChanged();
        }
    });

    qCInfo(verzetaUi) << "AgentSettingsController initialized";
}

AgentSettingsController::~AgentSettingsController() {
    qCInfo(verzetaUi) << "AgentSettingsController destroyed";
}

// ---------------------------------------------------------------------------
// Active conversation cache
// ---------------------------------------------------------------------------

void AgentSettingsController::setSettingsService(SettingsService* settings) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_settings == settings)
        return;
    m_settings = settings;
    if (!m_settings)
        return;
    // Re-fire availableProvidersChanged when any provider-relevant
    // settings row mutates. Cloud API keys come through as
    // "apikey.<provider>" keys; URLs/paths use the literal column
    // names ("ollama_base_url", "provider_base_url_<id>",
    // "llamacpp_model_path"). Any one of those changing can flip
    // a provider's configured-state.
    connect(m_settings, &SettingsService::settingsChanged, this, [this](const QString& key) {
        if (key.startsWith(QStringLiteral("apikey.")) || key == QStringLiteral("ollama_base_url") ||
            key == QStringLiteral("llamacpp_model_path") ||
            key.startsWith(QStringLiteral("provider_base_url_"))) {
            emit availableProvidersChanged();
        }
    });
    // Emit once now in case the QML binding was evaluated before the
    // setter attached (typical AppController init order).
    emit availableProvidersChanged();
}

void AgentSettingsController::setActiveConversationId(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId == id)
        return;
    m_activeConvId = id;
    // RC3 fix (b): apply a thinking choice the user made before this
    // conversation existed (fresh chat → first message creates the row).
    if (!id.isEmpty() && m_pendingThinking >= 0) {
        const auto conv = m_convSvc.getConversation(id);
        if (conv.has_value()) {
            LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
            const bool want = (m_pendingThinking == 1);
            if (cfg.thinkingMode != want) {
                cfg.thinkingMode = want;
                m_convSvc.updateLlmConfig(id, cfg);
            }
        }
        m_pendingThinking = -1;
    }
    refreshFromActiveConv();
    emit activeConversationSettingsChanged();
}

void AgentSettingsController::refreshFromActiveConv() {
    QString newPattern = QStringLiteral("direct");
    bool newRequireConf = false;
    bool newToolsEnabled = true;

    if (!m_activeConvId.isEmpty()) {
        const auto conv = m_convSvc.getConversation(m_activeConvId);
        if (conv.has_value()) {
            const LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
            // Empty stored agentPattern resolves to "direct" so
            // Older rows that never had the field carry the
            // historical default.
            newPattern = cfg.agentPattern.isEmpty() ? QStringLiteral("direct") : cfg.agentPattern;
            newRequireConf = cfg.requireConfirmation;
            newToolsEnabled = cfg.toolsEnabled;
        }
    }

    if (newPattern != m_agentPatternName) {
        m_agentPatternName = newPattern;
        emit agentPatternChanged(newPattern);
    }
    if (newRequireConf != m_requireConfirmation) {
        m_requireConfirmation = newRequireConf;
        emit requireConfirmationChanged(newRequireConf);
    }
    if (newToolsEnabled != m_toolsEnabled) {
        m_toolsEnabled = newToolsEnabled;
        emit toolsEnabledChanged();
    }
}

// ---------------------------------------------------------------------------
// Provider / model
// ---------------------------------------------------------------------------

void AgentSettingsController::setModel(const QString& providerId, const QString& modelName) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_router.setActiveProvider(providerId, modelName);

    // Persist on the active conversation if one is set, so re-opening
    // it later restores the same selection.
    if (!m_activeConvId.isEmpty()) {
        const auto conv = m_convSvc.getConversation(m_activeConvId);
        if (conv.has_value()) {
            LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
            cfg.providerId = providerId;
            cfg.modelName = modelName;
            m_convSvc.updateLlmConfig(m_activeConvId, cfg);
        }
    }
}

QStringList AgentSettingsController::modelsForProvider(const QString& providerId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    // Hide model lists for providers the user has not configured —
    // otherwise the hardcoded default lists every provider ships
    // (e.g. OpenRouter's empty list, LlamaCpp Remote's ["current"],
    // DeepSeek's ["deepseek-chat", "deepseek-reasoner"]) surface
    // as stale-looking choices in the chat / settings dropdowns
    // even for providers that have never been set up.
    if (m_settings && !m_settings->isProviderConfigured(providerId)) {
        return {};
    }
    const ILLMProvider* provider = m_router.providerForId(providerId);
    if (!provider)
        return {};
    // availableModels() is non-const but logically read-only; same
    // pattern the deleted ChatController accessor used.
    return const_cast<ILLMProvider*>(provider)->availableModels();
}

QVariantList AgentSettingsController::availableProviders() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList result;
    const QStringList ids = m_router.registeredProviderIds();
    for (const QString& id : ids) {
        // Filter out providers the user has not configured. Without
        // a SettingsService attached we fall back to legacy behaviour
        // (return every registered provider) so test fixtures and
        // bare instantiations still work.
        if (m_settings && !m_settings->isProviderConfigured(id))
            continue;
        const ILLMProvider* p = m_router.providerForId(id);
        if (p) {
            QVariantMap entry;
            entry[QStringLiteral("providerId")] = id;
            entry[QStringLiteral("displayName")] = p->displayName();
            // Capability flags surface here so paired wire clients
            // (Android) and the desktop QML provider dropdowns can
            // render the per-provider capability glyphs without a
            // separate round-trip.  Custom OpenAI-API-compatible
            // servers report whatever the user declared in the setup
            // sheet; the existing providers report their built-in
            // capability.
            entry[QStringLiteral("supportsStreaming")] = p->supportsStreaming();
            entry[QStringLiteral("supportsToolCalling")] = p->supportsToolCalling();
            entry[QStringLiteral("supportsVision")] = p->supportsVision();
            result.append(entry);
        }
    }
    return result;
}

QString AgentSettingsController::activeProvider() const {
    return m_router.activeProviderId();
}

QString AgentSettingsController::activeModel() const {
    return m_router.activeModelName();
}

// ---------------------------------------------------------------------------
// Per-conversation settings (read paths)
// ---------------------------------------------------------------------------

namespace {
LlmConfig configFor(ConversationService& convSvc, const QString& convId) {
    if (convId.isEmpty())
        return LlmConfig{};
    const auto conv = convSvc.getConversation(convId);
    if (!conv.has_value())
        return LlmConfig{};
    LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
    if (cfg.temperature == 0.0 && cfg.maxTokens == 0) {
        cfg.temperature = 0.7;
        cfg.maxTokens = 4096;
        cfg.stream = true;
    }
    return cfg;
}
}  // namespace

QString AgentSettingsController::activeSystemPrompt() const {
    if (m_activeConvId.isEmpty())
        return {};
    const auto conv = m_convSvc.getConversation(m_activeConvId);
    return conv.has_value() ? conv->systemPrompt : QString{};
}

double AgentSettingsController::activeTemperature() const {
    return configFor(m_convSvc, m_activeConvId).temperature;
}

int AgentSettingsController::activeMaxTokens() const {
    return configFor(m_convSvc, m_activeConvId).maxTokens;
}

int AgentSettingsController::activeContextWindow() const {
    return configFor(m_convSvc, m_activeConvId).contextWindow;
}

bool AgentSettingsController::activeStreaming() const {
    return configFor(m_convSvc, m_activeConvId).stream;
}

bool AgentSettingsController::activeThinking() const {
    // RC3 fix (b): before a conversation row exists, reflect the stashed
    // pending choice so the UI toggle stays where the user put it.
    if (m_activeConvId.isEmpty() && m_pendingThinking >= 0) {
        return m_pendingThinking == 1;
    }
    return configFor(m_convSvc, m_activeConvId).thinkingMode;
}

QString AgentSettingsController::samplingProfileWarning() const {
    // Resolve the (provider, model) pair through the same `configFor`
    // helper the other active* readers use so we see the same view of
    // the active conversation's llm_config.  When the conv id is empty
    // (no active conv yet), `configFor` returns a default-constructed
    // `LlmConfig` whose providerId / modelName are empty, and the
    // registry returns nullptr → empty warning.
    const LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    return Verzeta::Models::effectiveProfileWarning(cfg);
}

bool AgentSettingsController::hasAppRecommendedSampling() const {
    const LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    return Verzeta::Models::ModelSamplingProfileRegistry::instance().find(cfg.providerId,
                                                                          cfg.modelName) != nullptr;
}

bool AgentSettingsController::useAppRecommendedSampling() const {
    return configFor(m_convSvc, m_activeConvId).forceAppSampling;
}

void AgentSettingsController::setUseAppRecommendedSampling(bool on) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.forceAppSampling == on)
        return;
    cfg.forceAppSampling = on;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

bool AgentSettingsController::toolsInSystemPrompt() const {
    return configFor(m_convSvc, m_activeConvId).toolsInSystemPrompt;
}

void AgentSettingsController::setToolsInSystemPrompt(bool on) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.toolsInSystemPrompt == on)
        return;
    cfg.toolsInSystemPrompt = on;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

bool AgentSettingsController::dynamicCompactEnabled() const {
    return configFor(m_convSvc, m_activeConvId).dynamicCompactEnabled;
}

void AgentSettingsController::setDynamicCompactEnabled(bool on) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.dynamicCompactEnabled == on)
        return;
    cfg.dynamicCompactEnabled = on;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

bool AgentSettingsController::implicitTaskCompletion() const {
    return configFor(m_convSvc, m_activeConvId).implicitTaskCompletion;
}

void AgentSettingsController::setImplicitTaskCompletion(bool on) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.implicitTaskCompletion == on)
        return;
    cfg.implicitTaskCompletion = on;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

bool AgentSettingsController::activeRagEnabled() const {
    return configFor(m_convSvc, m_activeConvId).ragEnabled;
}

void AgentSettingsController::setActiveRagEnabled(bool enabled) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.ragEnabled == enabled)
        return;
    cfg.ragEnabled = enabled;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

bool AgentSettingsController::activeAimEnabled() const {
    return configFor(m_convSvc, m_activeConvId).aimEnabled;
}

void AgentSettingsController::setActiveAimEnabled(bool enabled) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.aimEnabled == enabled)
        return;
    cfg.aimEnabled = enabled;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

bool AgentSettingsController::activeAcnEnabled() const {
    return configFor(m_convSvc, m_activeConvId).acnEnabled;
}

void AgentSettingsController::setActiveAcnEnabled(bool enabled) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.acnEnabled == enabled)
        return;
    cfg.acnEnabled = enabled;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

bool AgentSettingsController::activeAcnAvailable() const {
    if (m_activeConvId.isEmpty()) {
        return false;
    }
    const QString ancestor = m_convSvc.projectOrgAncestorId(m_activeConvId);
    return !ancestor.isEmpty() && m_convSvc.folderAcnEnabled(ancestor);
}

int AgentSettingsController::activeMaxAutoRounds() const {
    return configFor(m_convSvc, m_activeConvId).maxAutoRounds;
}

void AgentSettingsController::setActiveMaxAutoRounds(int rounds) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    const int clamped = qBound(0, rounds, 50);  // 0 = unbounded
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.maxAutoRounds == clamped)
        return;
    cfg.maxAutoRounds = clamped;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

int AgentSettingsController::compactEveryTurns() const {
    return configFor(m_convSvc, m_activeConvId).compactEveryTurns;
}

void AgentSettingsController::setCompactEveryTurns(int turns) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    const int clamped = qBound(0, turns, 500);
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.compactEveryTurns == clamped)
        return;
    cfg.compactEveryTurns = clamped;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

double AgentSettingsController::activeTopK() const {
    return configFor(m_convSvc, m_activeConvId).topK;
}
void AgentSettingsController::setActiveTopK(double v) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.topK == v)
        return;
    cfg.topK = v;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

double AgentSettingsController::activeTopP() const {
    return configFor(m_convSvc, m_activeConvId).topP;
}
void AgentSettingsController::setActiveTopP(double v) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.topP == v)
        return;
    cfg.topP = v;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

double AgentSettingsController::activeRepeatPenalty() const {
    return configFor(m_convSvc, m_activeConvId).repeatPenalty;
}
void AgentSettingsController::setActiveRepeatPenalty(double v) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.repeatPenalty == v)
        return;
    cfg.repeatPenalty = v;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

double AgentSettingsController::activePresencePenalty() const {
    return configFor(m_convSvc, m_activeConvId).presencePenalty;
}
void AgentSettingsController::setActivePresencePenalty(double v) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.presencePenalty == v)
        return;
    cfg.presencePenalty = v;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

double AgentSettingsController::activeFrequencyPenalty() const {
    return configFor(m_convSvc, m_activeConvId).frequencyPenalty;
}
void AgentSettingsController::setActiveFrequencyPenalty(double v) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.frequencyPenalty == v)
        return;
    cfg.frequencyPenalty = v;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

void AgentSettingsController::setActiveThinking(bool enabled) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty()) {
        // RC3 fix (b): no conversation row yet (brand-new chat). Stash the
        // choice and apply it when a conversation becomes active, instead of
        // silently dropping it (which left the conv created with thinking off).
        m_pendingThinking = enabled ? 1 : 0;
        emit activeConversationSettingsChanged();
        return;
    }
    LlmConfig cfg = configFor(m_convSvc, m_activeConvId);
    if (cfg.thinkingMode == enabled)
        return;
    cfg.thinkingMode = enabled;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
    // ConversationService::conversationUpdated will fire and our
    // listener emits activeConversationSettingsChanged.
}

void AgentSettingsController::saveConversationConfig(const QVariantMap& config) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty())
        return;

    // System prompt (separate column from llm_config).
    if (config.contains(QStringLiteral("systemPrompt"))) {
        m_convSvc.updateSystemPrompt(m_activeConvId,
                                     config.value(QStringLiteral("systemPrompt")).toString());
    }

    // LLM config — merge supplied keys into the current row.
    const auto conv = m_convSvc.getConversation(m_activeConvId);
    if (conv.has_value()) {
        LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
        cfg.temperature = config.value(QStringLiteral("temperature"), cfg.temperature).toDouble();
        cfg.maxTokens = config.value(QStringLiteral("maxTokens"), cfg.maxTokens).toInt();
        cfg.contextWindow =
            config.value(QStringLiteral("contextWindow"), cfg.contextWindow).toInt();
        cfg.stream = config.value(QStringLiteral("streaming"), cfg.stream).toBool();
        cfg.thinkingMode = config.value(QStringLiteral("thinking"), cfg.thinkingMode).toBool();
        cfg.topK = config.value(QStringLiteral("topK"), cfg.topK).toDouble();
        cfg.topP = config.value(QStringLiteral("topP"), cfg.topP).toDouble();
        cfg.repeatPenalty =
            config.value(QStringLiteral("repeatPenalty"), cfg.repeatPenalty).toDouble();
        cfg.presencePenalty =
            config.value(QStringLiteral("presencePenalty"), cfg.presencePenalty).toDouble();
        cfg.frequencyPenalty =
            config.value(QStringLiteral("frequencyPenalty"), cfg.frequencyPenalty).toDouble();
        cfg.forceAppSampling =
            config.value(QStringLiteral("forceAppSampling"), cfg.forceAppSampling).toBool();
        cfg.toolsInSystemPrompt =
            config.value(QStringLiteral("toolsInSystemPrompt"), cfg.toolsInSystemPrompt).toBool();
        cfg.dynamicCompactEnabled =
            config.value(QStringLiteral("dynamicCompactEnabled"), cfg.dynamicCompactEnabled)
                .toBool();
        cfg.implicitTaskCompletion =
            config.value(QStringLiteral("implicitTaskCompletion"), cfg.implicitTaskCompletion)
                .toBool();
        cfg.compactEveryTurns =
            qBound(0,
                   config.value(QStringLiteral("compactEveryTurns"), cfg.compactEveryTurns).toInt(),
                   500);
        cfg.ragEnabled = config.value(QStringLiteral("ragEnabled"), cfg.ragEnabled).toBool();
        cfg.aimEnabled = config.value(QStringLiteral("aimEnabled"), cfg.aimEnabled).toBool();
        cfg.acnEnabled = config.value(QStringLiteral("acnEnabled"), cfg.acnEnabled).toBool();
        cfg.maxAutoRounds =
            qBound(0, config.value(QStringLiteral("maxAutoRounds"), cfg.maxAutoRounds).toInt(), 50);
        m_convSvc.updateLlmConfig(m_activeConvId, cfg);
    }
    // ConversationService::conversationUpdated covers the
    // activeConversationSettingsChanged emission via our listener.
}

// ---------------------------------------------------------------------------
// Agent pattern + require-confirmation + tools toggle
// ---------------------------------------------------------------------------

void AgentSettingsController::setAgentPattern(const QString& pattern) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Canonicalise the pattern name to match ChatController's
    // accepted set so its slot doesn't have to re-validate.
    QString canonical;
    if (pattern == QStringLiteral("react"))
        canonical = pattern;
    else if (pattern == QStringLiteral("planner"))
        canonical = pattern;
    else if (pattern == QStringLiteral("router"))
        canonical = pattern;
    else if (pattern == QStringLiteral("multi_agent"))
        canonical = pattern;
    else if (pattern == QStringLiteral("memory"))
        canonical = pattern;
    else
        canonical = QStringLiteral("direct");

    if (m_activeConvId.isEmpty()) {
        qCWarning(verzetaUi) << "AgentSettings::setAgentPattern: no active conversation; "
                                "pattern selection ignored";
        return;
    }
    const auto conv = m_convSvc.getConversation(m_activeConvId);
    if (!conv.has_value())
        return;
    LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
    if (cfg.agentPattern == canonical)
        return;
    cfg.agentPattern = canonical;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
    qCInfo(verzetaUi) << "AgentSettings: pattern set to" << canonical << "for conv"
                      << m_activeConvId;
}

void AgentSettingsController::setRequireConfirmation(bool require) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty()) {
        qCWarning(verzetaUi) << "AgentSettings::setRequireConfirmation: no active "
                                "conversation; toggle ignored";
        return;
    }
    const auto conv = m_convSvc.getConversation(m_activeConvId);
    if (!conv.has_value())
        return;
    LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
    if (cfg.requireConfirmation == require)
        return;
    cfg.requireConfirmation = require;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

bool AgentSettingsController::toolsEnabled() const {
    return m_toolsEnabled;
}

void AgentSettingsController::setToolsEnabled(bool enabled) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_activeConvId.isEmpty()) {
        qCWarning(verzetaUi) << "AgentSettings::setToolsEnabled: no active conversation; "
                                "toggle ignored";
        return;
    }
    const auto conv = m_convSvc.getConversation(m_activeConvId);
    if (!conv.has_value())
        return;
    LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
    if (cfg.toolsEnabled == enabled)
        return;
    cfg.toolsEnabled = enabled;
    m_convSvc.updateLlmConfig(m_activeConvId, cfg);
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent-settings-controller.h
 * @brief QML-exposed controller that owns the active conversation's
 *        agent + LLM settings surface: model selection, agent
 *        pattern, tool toggle, system prompt, temperature, max
 *        tokens, context window, streaming, thinking, etc. Registered
 *        as the `AgentSettings` QML singleton by AppController,
 *        alongside `ChatController` (chat session), `Conversations`
 *        (CRUD), and `Tasks` (task lifecycle).
 * @layer Service (UI orchestration)
 * @dependencies ConversationService, ModelRouter (non-owning refs);
 *               subscribes to ChatController.activeConversationChanged
 *               via AppController wiring to keep its cached
 *               m_activeConvId in sync.
 *
 * ChatController keeps cached mirrors of m_agentPattern /
 * m_requireConfirmation / m_toolsEnabled because sendMessage reads
 * them synchronously; the mirrors stay fresh via signal
 * subscriptions to AgentSettings.
 *
 * Star-topology refusal: AgentSettingsController does NOT hold a
 * ChatController pointer. The two cross-controller flows that need
 * to reach Chat (agent-pattern / require-confirmation / tools-enabled
 * change → Chat updates its sendMessage cache) run through signals
 * Chat subscribes to. The one INBOUND flow (active-conversation id
 * for the per-conversation settings reads) is a direct subscription
 * to ChatController::activeConversationChanged; AgentSettings stores
 * a cached id and queries ConversationService for the rest.
 *
 * Threading: strictly main-thread. Every public method asserts via
 * VERZETA_ASSERT_MAIN_THREAD().
 *
 * Ownership: constructed as
 * `std::unique_ptr<AgentSettingsController>` on AppController.
 * AppController is the QObject parent. QML holds a non-owning
 * singleton pointer via `qmlRegisterSingletonInstance`.
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class ConversationService;
class ModelRouter;
class SettingsService;

/**
 * @brief QML singleton for agent + LLM settings.
 */
class AgentSettingsController : public QObject {
    Q_OBJECT

    // Read-only providers list; refreshed on ModelRouter::providersChanged.
    Q_PROPERTY(
        QVariantList availableProviders READ availableProviders NOTIFY availableProvidersChanged)

    // Active provider / model — read from ModelRouter, NOTIFY on
    // ModelRouter::activeProviderChanged.
    Q_PROPERTY(QString activeProvider READ activeProvider NOTIFY activeProviderChanged)
    Q_PROPERTY(QString activeModel READ activeModel NOTIFY activeProviderChanged)

    // Per-conversation settings; NOTIFY on
    // activeConversationSettingsChanged (fired when active conv id
    // changes OR ConversationService::conversationUpdated fires for
    // the cached active id).
    Q_PROPERTY(
        QString activeSystemPrompt READ activeSystemPrompt NOTIFY activeConversationSettingsChanged)
    Q_PROPERTY(
        double activeTemperature READ activeTemperature NOTIFY activeConversationSettingsChanged)
    Q_PROPERTY(int activeMaxTokens READ activeMaxTokens NOTIFY activeConversationSettingsChanged)
    Q_PROPERTY(
        int activeContextWindow READ activeContextWindow NOTIFY activeConversationSettingsChanged)
    Q_PROPERTY(bool activeStreaming READ activeStreaming NOTIFY activeConversationSettingsChanged)
    Q_PROPERTY(bool activeThinking READ activeThinking WRITE setActiveThinking NOTIFY
                   activeConversationSettingsChanged)

    /**
     * @brief User-facing label for the "Use app-recommended sampling"
     *        toggle.  Non-empty when the per-(provider, model)
     *        registry has a profile for the active conv; empty when
     *        no profile applies and the toggle should be hidden.
     */
    Q_PROPERTY(QString samplingProfileWarning READ samplingProfileWarning NOTIFY
                   activeConversationSettingsChanged)

    /**
     * @brief True iff a per-(provider, model) sampling profile is
     *        available for the active conversation's (provider, model)
     *        pair.  Drives whether the conv-settings toggle renders.
     */
    Q_PROPERTY(bool hasAppRecommendedSampling READ hasAppRecommendedSampling NOTIFY
                   activeConversationSettingsChanged)

    /**
     * @brief Read+write state of the "Use app-recommended sampling"
     *        toggle for the active conversation.  Defaults to true on
     *        any pair that has a profile.  When true, the registry
     *        FORCES its prescribed values at request-build time
     *        regardless of the user's advanced-sampling inputs.  When
     *        false, the registry merges additively (fills `-1` slots)
     *        and user values dominate.
     */
    Q_PROPERTY(bool useAppRecommendedSampling READ useAppRecommendedSampling WRITE
                   setUseAppRecommendedSampling NOTIFY activeConversationSettingsChanged)

    /**
     * @brief Whether the system prompt embeds the
     *        full prose tool list (~2,600 tokens on a 33-tool roster)
     *        or a 1-line protocol stub. Tool schemas always flow on
     *        the structured channel regardless. Default true.
     */
    Q_PROPERTY(bool toolsInSystemPrompt READ toolsInSystemPrompt WRITE setToolsInSystemPrompt NOTIFY
                   activeConversationSettingsChanged)

    /**
     * @brief Per-conversation dynamic-compaction
     *        gate. Default true.
     */
    Q_PROPERTY(bool dynamicCompactEnabled READ dynamicCompactEnabled WRITE setDynamicCompactEnabled
                   NOTIFY activeConversationSettingsChanged)

    /**
     * @brief Per-conversation IMPLICIT task-completion gate. Default false.
     *        When off, a task closes only via an explicit complete_task /
     *        stop_task (or the user); when on, the task gate may auto-close
     *        a task that goes quiet after a tool call.
     */
    Q_PROPERTY(bool implicitTaskCompletion READ implicitTaskCompletion WRITE
                   setImplicitTaskCompletion NOTIFY activeConversationSettingsChanged)

    /**
     * @brief Proactive compaction cadence in assistant turns
     *        (0 = off). Default 20.
     */
    Q_PROPERTY(int compactEveryTurns READ compactEveryTurns WRITE setCompactEveryTurns NOTIFY
                   activeConversationSettingsChanged)

    /**
     * @brief Per-conversation RAG enable toggle (default false). When on,
     *        this conversation embeds its latest user text and injects the
     *        top indexed passages into the system prompt, and its messages
     *        are indexed into the corpus. The embedding provider is
     *        configured globally on the Embeddings Providers page; this is
     *        the only per-conversation RAG switch. Both the chat-input RAG
     *        pill and the conversation-settings checkbox bind here.
     */
    Q_PROPERTY(bool activeRagEnabled READ activeRagEnabled WRITE setActiveRagEnabled NOTIFY
                   activeConversationSettingsChanged)

    /**
     * @brief Whether agent-memory (AIM) recall is injected for the active
     *        conversation. Default true (on for everyone unless turned off);
     *        not per-agent. Bound by the Conversation Settings memory toggle.
     */
    Q_PROPERTY(bool activeAimEnabled READ activeAimEnabled WRITE setActiveAimEnabled NOTIFY
                   activeConversationSettingsChanged)

    /**
     * @brief Whether team memory (ACN) recall is used for the active
     *        conversation. Default true; only effective inside a project/org
     *        whose ACN master switch is on. Bound by the Conversation Settings
     *        team-memory toggle (shown only for such conversations).
     */
    Q_PROPERTY(bool activeAcnEnabled READ activeAcnEnabled WRITE setActiveAcnEnabled NOTIFY
                   activeConversationSettingsChanged)

    /**
     * @brief Whether team memory (ACN) even applies to the active conversation,
     *        i.e. it lives inside a project/organization whose ACN master switch
     *        is on. The Conversation Settings team-memory toggle is shown only
     *        when this is true. Read-only.
     */
    Q_PROPERTY(
        bool activeAcnAvailable READ activeAcnAvailable NOTIFY activeConversationSettingsChanged)

    /**
     * @brief Per-conversation cap on autonomous cascade rounds per user
     *        message (group chats). Default 6; 0 = unbounded (run until the
     *        team stagnates, with a safety backstop). Larger = more autonomy
     *        before the team pauses for the user.
     */
    Q_PROPERTY(int activeMaxAutoRounds READ activeMaxAutoRounds WRITE setActiveMaxAutoRounds NOTIFY
                   activeConversationSettingsChanged)

    /// Advanced sampling: user values stored on the active conv's
    /// llm_config.  Sentinel `-1` on each means "use model default".
    /// The conv-settings toggle (above) determines whether these are
    /// effective at request-build time or overridden by the registry.
    Q_PROPERTY(double activeTopK READ activeTopK WRITE setActiveTopK NOTIFY
                   activeConversationSettingsChanged)
    Q_PROPERTY(double activeTopP READ activeTopP WRITE setActiveTopP NOTIFY
                   activeConversationSettingsChanged)
    Q_PROPERTY(double activeRepeatPenalty READ activeRepeatPenalty WRITE setActiveRepeatPenalty
                   NOTIFY activeConversationSettingsChanged)
    Q_PROPERTY(double activePresencePenalty READ activePresencePenalty WRITE
                   setActivePresencePenalty NOTIFY activeConversationSettingsChanged)
    Q_PROPERTY(double activeFrequencyPenalty READ activeFrequencyPenalty WRITE
                   setActiveFrequencyPenalty NOTIFY activeConversationSettingsChanged)

    /** @brief Whether tools are offered to the LLM for this session. */
    Q_PROPERTY(bool toolsEnabled READ toolsEnabled WRITE setToolsEnabled NOTIFY toolsEnabledChanged)

  public:
    /**
     * @param convSvc Non-owning reference; reads / writes
     *                conversation rows for the per-conversation
     *                settings. Must outlive this object.
     * @param router  Non-owning reference; setModel updates the
     *                active provider + model; activeProvider /
     *                activeModel / availableProviders / modelsForProvider
     *                read from it; modelsRefreshed forwards from it.
     * @param parent  Qt parent (AppController).
     */
    explicit AgentSettingsController(ConversationService& convSvc,
                                     ModelRouter& router,
                                     QObject* parent = nullptr);
    ~AgentSettingsController() override;

    // -----------------------------------------------------------------
    // Active-conversation tracking — wired by AppController to
    // ChatController::activeConversationChanged. The internal cache
    // is what every per-conversation property reads against.
    // -----------------------------------------------------------------

    /**
     * @brief Replaces the cached active-conversation id and re-emits
     *        activeConversationSettingsChanged.
     * @param id New active conversation UUID (empty clears).
     */
    void setActiveConversationId(const QString& id);

    /**
     * @brief Attaches a SettingsService so availableProviders() can
     *        filter out providers the user has not configured yet.
     *        Without this attached, availableProviders() falls back
     *        to returning every registered provider. This is kept so unit
     *        tests that construct a bare AgentSettingsController
     *        still pass.
     *
     *        Wires SettingsService::settingsChanged so the QML
     *        availableProviders binding re-evaluates when an API
     *        key is added/removed or a base URL is saved.
     * @param settings Non-owning pointer; pass nullptr to detach.
     */
    void setSettingsService(SettingsService* settings);

    // -----------------------------------------------------------------
    // Q_INVOKABLE — provider / model
    // -----------------------------------------------------------------

    /**
     * @brief Sets the active LLM provider and model + persists the
     *        selection on the active conversation's llm_config when
     *        one is set.
     * @param providerId Provider id (e.g. `"ollama"`, `"openai"`).
     * @param modelName  Model name within that provider.
     */
    Q_INVOKABLE void setModel(const QString& providerId, const QString& modelName);

    /**
     * @brief Available models for the given provider id.
     * @param providerId Provider id to query.
     * @returns List of model name strings; empty when the provider
     *          is unregistered or has no models loaded yet.
     */
    Q_INVOKABLE QStringList modelsForProvider(const QString& providerId) const;

    // -----------------------------------------------------------------
    // Q_INVOKABLE — per-conversation settings
    // -----------------------------------------------------------------

    /**
     * @brief Persists per-conversation LLM config + system prompt.
     *        Keys recognised in the supplied map:
     *          systemPrompt (string), temperature (double),
     *          maxTokens (int), contextWindow (int),
     *          streaming (bool), thinking (bool).
     *        No-op if the cached active conversation id is empty.
     * @param config QVariantMap with any subset of the recognised
     *               keys; absent keys retain their current value.
     */
    Q_INVOKABLE void saveConversationConfig(const QVariantMap& config);

    // -----------------------------------------------------------------
    // Q_INVOKABLE — agent settings (tools toggle stays as a Q_PROPERTY)
    // -----------------------------------------------------------------

    /**
     * @brief Sets the agent reasoning pattern. Recognised pattern
     *        names: `"direct"` | `"react"` | `"planner"` | `"router"`
     *        | `"multi_agent"` | `"memory"`. Emits
     *        agentPatternChanged with the resolved canonical name so
     *        ChatController can update its sendMessage-time cache.
     * @param pattern Pattern name string (case-insensitive).
     */
    Q_INVOKABLE void setAgentPattern(const QString& pattern);

    /**
     * @brief Toggles "ask user before each tool call" mode. Emits
     *        requireConfirmationChanged so ChatController updates
     *        its agent-config cache.
     * @param require true to require confirmation per tool call.
     */
    Q_INVOKABLE void setRequireConfirmation(bool require);

    // -----------------------------------------------------------------
    // Property accessors
    // -----------------------------------------------------------------

    /**
     * @brief Reader for the availableProviders Q_PROPERTY.
     * @returns QVariantList of provider descriptor maps; filtered
     *          to configured providers when SettingsService is
     *          attached.
     */
    QVariantList availableProviders() const;

    /**
     * @brief Reader for the activeProvider Q_PROPERTY.
     * @returns The active provider id, or empty when no provider
     *          has been selected yet.
     */
    QString activeProvider() const;

    /**
     * @brief Reader for the activeModel Q_PROPERTY.
     * @returns The active model name within the active provider.
     */
    QString activeModel() const;

    /**
     * @brief Reader for the activeSystemPrompt Q_PROPERTY.
     * @returns Per-conversation system prompt for the cached active
     *          conv; falls back to the global default when no conv
     *          is active or the conv has no override.
     */
    QString activeSystemPrompt() const;

    /**
     * @brief Reader for the activeTemperature Q_PROPERTY.
     * @returns Per-conversation sampling temperature; falls back to
     *          the global default when unset.
     */
    double activeTemperature() const;

    /**
     * @brief Reader for the activeMaxTokens Q_PROPERTY.
     * @returns Per-conversation max output tokens cap.
     */
    int activeMaxTokens() const;

    /**
     * @brief Reader for the activeContextWindow Q_PROPERTY.
     * @returns Per-conversation context-window size in tokens.
     */
    int activeContextWindow() const;

    /**
     * @brief Reader for the activeStreaming Q_PROPERTY.
     * @returns true iff token-streaming is enabled for the active
     *          conv.
     */
    bool activeStreaming() const;

    /**
     * @brief Reader for the activeThinking Q_PROPERTY.
     * @returns true iff the model's thinking / reasoning channel is
     *          enabled for the active conv.
     */
    bool activeThinking() const;

    /**
     * @brief Setter for the activeThinking Q_PROPERTY. Persists the
     *        change on the active conversation's llm_config.
     * @param enabled true to enable thinking mode.
     */
    void setActiveThinking(bool enabled);

    /**
     * @brief Reader for the samplingProfileWarning Q_PROPERTY.
     * @returns Warning text from the matching per-(provider, model)
     *          sampling profile when one is applying to the active
     *          conversation, OR empty string when no profile applies
     *          OR when the user has overridden the workaround
     *          per-conversation via the advanced sampling settings.
     */
    QString samplingProfileWarning() const;

    /**
     * @brief Reader for the hasAppRecommendedSampling Q_PROPERTY.
     * @returns true iff the sampling-profile registry has a profile
     *          for the active conv's (provider, model) pair.
     */
    bool hasAppRecommendedSampling() const;

    /**
     * @brief Reader for the useAppRecommendedSampling Q_PROPERTY
     *        ("Use app-recommended sampling" toggle).
     * @returns true iff the matching profile force-applies for the
     *          active conversation.
     */
    bool useAppRecommendedSampling() const;

    /**
     * @brief Setter for the useAppRecommendedSampling Q_PROPERTY.
     *        Persists to the active conv's
     *        `llm_config.force_app_sampling`.
     * @param on true to force-apply the app-recommended profile.
     */
    void setUseAppRecommendedSampling(bool on);

    /**
     * @brief Reader for the toolsInSystemPrompt Q_PROPERTY
     *        (prose-tool-list flag).
     * @returns true iff the full prose tool list is embedded in the
     *          system prompt for the active conversation.
     */
    bool toolsInSystemPrompt() const;

    /**
     * @brief Setter for the toolsInSystemPrompt Q_PROPERTY. Persists
     *        on the active conversation's llm_config.
     * @param on true to embed the full prose tool list; false sends
     *           the one-line protocol stub instead.
     */
    void setToolsInSystemPrompt(bool on);

    /**
     * @brief Reader for the dynamicCompactEnabled Q_PROPERTY
     *        (per-conversation compaction gate).
     * @returns true iff dynamic compaction may summarize this
     *          conversation's dropped history.
     */
    bool dynamicCompactEnabled() const;

    /**
     * @brief Reader for the implicitTaskCompletion Q_PROPERTY
     *        (per-conversation gate).
     * @returns true iff a task that goes quiet after a tool call may
     *          auto-complete in the active conversation.
     */
    bool implicitTaskCompletion() const;

    /**
     * @brief Setter for the implicitTaskCompletion Q_PROPERTY. Persists on
     *        the active conversation's llm_config.
     * @param on true to allow implicit (automatic) task completion.
     */
    void setImplicitTaskCompletion(bool on);

    /**
     * @brief Setter for the dynamicCompactEnabled Q_PROPERTY.
     *        Persists on the active conversation's llm_config.
     * @param on true to enable dynamic compaction.
     */
    void setDynamicCompactEnabled(bool on);

    /**
     * @brief Reader for the compactEveryTurns Q_PROPERTY.
     * @returns The conversation's proactive compaction cadence in
     *          assistant turns; 0 means the cadence path is off.
     */
    int compactEveryTurns() const;

    /**
     * @brief Setter for the compactEveryTurns Q_PROPERTY. Persists
     *        on the active conversation's llm_config.
     * @param turns New cadence in assistant turns; clamped to
     *              [0, 500]. 0 disables the cadence path.
     */
    void setCompactEveryTurns(int turns);

    /**
     * @brief Reader for the activeRagEnabled Q_PROPERTY.
     * @returns true iff RAG is enabled for the active conversation;
     *          false when no conversation is active.
     */
    bool activeRagEnabled() const;

    /**
     * @brief Setter for the activeRagEnabled Q_PROPERTY. Persists on the
     *        active conversation's llm_config.
     * @param enabled true to enable RAG for this conversation.
     */
    void setActiveRagEnabled(bool enabled);

    /**
     * @brief Reader for the activeAimEnabled Q_PROPERTY.
     * @returns true iff agent-memory recall is enabled for the active
     *          conversation; the struct default (true) when no conversation is
     *          active.
     */
    bool activeAimEnabled() const;

    /**
     * @brief Setter for the activeAimEnabled Q_PROPERTY. Persists on the active
     *        conversation's llm_config.
     * @param enabled true to inject agent-memory recall for this conversation.
     */
    void setActiveAimEnabled(bool enabled);

    /**
     * @brief Reader for the activeAcnEnabled Q_PROPERTY.
     * @returns the per-conversation team-memory flag (struct default true when no
     *          conversation is active).
     */
    bool activeAcnEnabled() const;

    /**
     * @brief Setter for the activeAcnEnabled Q_PROPERTY. Persists on the active
     *        conversation's llm_config.
     * @param enabled true to use team-memory recall for this conversation.
     */
    void setActiveAcnEnabled(bool enabled);

    /**
     * @brief Reader for the activeAcnAvailable Q_PROPERTY.
     * @returns true iff the active conversation is in a project/organization
     *          whose ACN master switch is on.
     */
    bool activeAcnAvailable() const;

    /**
     * @brief Reader for the activeMaxAutoRounds Q_PROPERTY.
     * @returns The conversation's autonomous-round cap; 6 default, 0 =
     *          unbounded; 6 when no conversation is active.
     */
    int activeMaxAutoRounds() const;

    /**
     * @brief Setter for the activeMaxAutoRounds Q_PROPERTY. Persists on the
     *        active conversation's llm_config. Clamped to [0, 50] (0 =
     *        unbounded / run until stagnation).
     * @param rounds New autonomous-round cap.
     */
    void setActiveMaxAutoRounds(int rounds);

    // Advanced sampling readers + writers. Each writes through to the
    // active conv's llm_config and emits
    // activeConversationSettingsChanged on commit. Setting -1 restores
    // "use model default".

    /**
     * @brief Reader for the activeTopK Q_PROPERTY.
     * @returns The conv's top-k override; -1 means "model default".
     */
    double activeTopK() const;

    /**
     * @brief Setter for the activeTopK Q_PROPERTY.
     * @param v New top-k value; -1 restores the model default.
     */
    void setActiveTopK(double v);

    /**
     * @brief Reader for the activeTopP Q_PROPERTY.
     * @returns The conv's top-p override; -1 means "model default".
     */
    double activeTopP() const;

    /**
     * @brief Setter for the activeTopP Q_PROPERTY.
     * @param v New top-p value; -1 restores the model default.
     */
    void setActiveTopP(double v);

    /**
     * @brief Reader for the activeRepeatPenalty Q_PROPERTY.
     * @returns The conv's repeat-penalty override; -1 means
     *          "model default".
     */
    double activeRepeatPenalty() const;

    /**
     * @brief Setter for the activeRepeatPenalty Q_PROPERTY.
     * @param v New repeat penalty; -1 restores the model default.
     */
    void setActiveRepeatPenalty(double v);

    /**
     * @brief Reader for the activePresencePenalty Q_PROPERTY.
     * @returns The conv's presence-penalty override; -1 means
     *          "model default".
     */
    double activePresencePenalty() const;

    /**
     * @brief Setter for the activePresencePenalty Q_PROPERTY.
     * @param v New presence penalty; -1 restores the model default.
     */
    void setActivePresencePenalty(double v);

    /**
     * @brief Reader for the activeFrequencyPenalty Q_PROPERTY.
     * @returns The conv's frequency-penalty override; -1 means
     *          "model default".
     */
    double activeFrequencyPenalty() const;

    /**
     * @brief Setter for the activeFrequencyPenalty Q_PROPERTY.
     * @param v New frequency penalty; -1 restores the model default.
     */
    void setActiveFrequencyPenalty(double v);

    /**
     * @brief Reader for the toolsEnabled Q_PROPERTY.
     * @returns true iff tools are offered to the LLM in this
     *          session.
     */
    bool toolsEnabled() const;

    /**
     * @brief Setter for the toolsEnabled Q_PROPERTY. Persists on
     *        the active conversation's llm_config and emits
     *        toolsEnabledChanged.
     * @param enabled true to expose tools to the LLM.
     */
    void setToolsEnabled(bool enabled);

  signals:
    /** Emitted on ModelRouter provider list refresh. */
    void availableProvidersChanged();

    /** Emitted on ModelRouter active provider/model change. */
    void activeProviderChanged();

    /**
     * @brief Emitted when any per-conversation setting changes.
     *        Fired on active-conv switch and on conversationUpdated
     *        for the cached active id.
     */
    void activeConversationSettingsChanged();

    /**
     * @brief Forwarded from ModelRouter::modelsRefreshed so QML
     *        model selectors can update without holding a
     *        ModelRouter reference.
     * @param providerId Provider id whose model list refreshed.
     * @param models     Full list of model names.
     */
    void modelsRefreshed(const QString& providerId, const QStringList& models);

    /**
     * @brief R/W toolsEnabled NOTIFY. Also picked up by
     *        ChatController via signal subscription to keep its
     *        sendMessage cache fresh.
     */
    void toolsEnabledChanged();

    /**
     * @brief Internal coordination signal for ChatController.
     *        Carries the canonical pattern name string so the
     *        consumer doesn't need to depend on this header for the
     *        AgentPattern enum.
     * @param patternName Canonical pattern name (`"direct"` /
     *                    `"react"` / etc.).
     */
    void agentPatternChanged(const QString& patternName);

    /**
     * @brief Internal coordination signal with the same purpose as
     *        agentPatternChanged for the "ask before each tool"
     *        toggle.
     * @param require true iff confirmation is required per tool
     *                call.
     */
    void requireConfirmationChanged(bool require);

    /** Error channel. QML binds this alongside the other singletons'
     *  errorOccurred signals on the central error banner. */
    void errorOccurred(const QString& message);

  private:
    /**
     * @brief Re-read the active conversation's agent settings
     *        (agent_pattern, tools_enabled, require_confirmation)
     *        from its `llm_config` JSON and emit the granular
     *        *Changed signals if the cached value flipped. Called
     *        from setActiveConversationId after the cache id
     *        changes, and from the conversationUpdated handler when
     *        the active conv's row is mutated. Empty active conv
     *        resolves to the global defaults (`"direct"`, true,
     *        false).
     */
    void refreshFromActiveConv();

    ConversationService& m_convSvc;
    ModelRouter& m_router;
    SettingsService* m_settings = nullptr;  // non-owning; optional

    // Cached canonical state for the QML surface. These are a thin
    // cache of the ACTIVE CONV's stored values (NOT a global
    // setting). Refreshed by refreshFromActiveConv on active-conv
    // switch and on conversationUpdated for the active id.
    QString m_activeConvId;
    bool m_toolsEnabled = true;
    QString m_agentPatternName = QStringLiteral("direct");
    bool m_requireConfirmation = false;

    int m_pendingThinking = -1;
};

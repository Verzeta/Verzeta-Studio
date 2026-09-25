// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file llm-config.h
 * @brief Data model for per-conversation LLM configuration.
 *        Stores provider selection, model name, and generation parameters.
 * @layer Data Access
 * @dependencies Qt6::Core
 */

#pragma once

#include <QtGlobal>

#include <algorithm>
#include <QJsonObject>
#include <QString>

/**
 * @brief Configuration settings for an LLM request.
 *
 * Stored as JSON in the conversations.llm_config column and as part
 * of message metadata. Passed to ILLMProvider::request() as part of LlmRequest.
 *
 * Note: this struct also carries per-conversation override flags that
 * aren't strictly LLM request parameters (e.g. the heartbeat
 * auto-surface gate). They are typed fields here so the toJson/fromJson
 * round-trip done by ConversationService::updateLlmConfig preserves
 * them. Any new per-conv override should be added here, NOT as a
 * sibling JSON blob.
 */
struct LlmConfig {
    /**
     * @brief Conservative default input context window in tokens.
     *
     * Used when no model-aware resolution is available (the provider
     * reports UNKNOWN and the user has not overridden the value). 8192
     * is safe for virtually every modern local model; Ollama otherwise
     * defaults to a brutal 2048 which silently truncates long prompts.
     * This constant is ALSO the sentinel that RequestBuilder uses to
     * decide whether `contextWindow` is a user override (any other
     * value) or the untouched default eligible for model-aware
     * resolution.
     */
    static constexpr int kDefaultContextWindow = 8192;

    /**
     * @brief Lower clamp for a model-resolved context window.
     *
     * A provider-reported window below this is widened to it so the
     * history budgeter never operates on an implausibly tiny window
     * (e.g. a server that reports a 2048 default for a model that
     * actually supports far more). Matches the historical safe floor.
     */
    static constexpr int kSaneFloorCtx = 8192;

    /**
     * @brief Upper clamp for a model-resolved context window.
     *
     * Resolved windows are capped here. A 131072-token model is honoured
     * only up to this ceiling because the num_ctx the provider then requests
     * costs real memory + prefill latency on the host: the model's own
     * weights PLUS a large KV cache must fit, and a small (e.g. 4B) model on
     * a modest box slows markedly as the window grows. 16384 is the measured
     * balanced point: roughly double the legacy 8192 (so long group chats
     * keep far more history) without the ~2x per-turn slowdown a 32k window
     * imposed on a 4B model serving from a single local Ollama. A user who
     * explicitly wants more sets it per-conversation (honoured verbatim,
     * uncapped). NOTE: this stays at or below RequestBuilder's auto-grow
     * limit of kMaxAutoContextWindow = 32768. That auto-grow only fires to
     * fit a single oversized turn, whereas this governs steady-state sizing.
     */
    static constexpr int kSaneCeilingCtx = 16384;

    /**
     * @brief Upper clamp for turns with a LARGE fixed prompt overhead.
     *
     * Group chats (and tool/canvas/skill/heartbeat-heavy turns) carry a big
     * fixed cost (layered system prompt + multi-member roster + tool schemas
     * + summary ≈ 9.5k tokens), so a 16384 window leaves only ~6.8k for actual
     * history (right back at the starved baseline). These turns get a higher
     * ceiling so real history survives: 24576 − ~9.5k ≈ 14.5k history budget,
     * ~2x the messages, without the full ~2x per-turn slowdown a 32768 window
     * imposes on a small model. The lighter `kSaneCeilingCtx` (16384) applies
     * to plain 1:1 turns whose overhead is small. A per-conversation user
     * override still wins verbatim over either.
     */
    static constexpr int kSaneCeilingCtxHeavy = 24576;

    QString providerId;       ///< "ollama" | "openai" | "anthropic" | "gemini" | "llamacpp"
    QString modelName;        ///< e.g. "gpt-4o", "claude-3-5-sonnet-20241022", "llama3:8b"
    double temperature = -1;  ///< Sampling temperature [0.0, 2.0]. -1 = use model default.
    int maxTokens = -1;       ///< Output cap. -1 = "Auto": defer to the
                              ///  model / provider default (Ollama
                              ///  num_predict=-1, OpenAI/Gemini omit the
                              ///  field, Anthropic substitutes 4096 as
                              ///  the API requires an explicit value).
                              /// History budgeting no
                              ///  longer reads this; a fixed output
                              ///  reservation governs that arithmetic.
    int contextWindow =
        kDefaultContextWindow;     ///< Input context window in tokens.
                                   ///  Used by Ollama's num_ctx option. Ollama otherwise
                                   ///  defaults to a brutal 2048 which silently truncates
                                   ///  long system prompts. The default is safe for
                                   ///  virtually every modern local model; raise in
                                   ///  settings for long docs. RequestBuilder treats a
                                   ///  value equal to kDefaultContextWindow as "not
                                   ///  overridden" and may replace it with a model-aware
                                   ///  resolution from the provider.
    bool stream = true;            ///< Whether to use streaming SSE response
    bool thinkingMode = false;     ///< Enable extended thinking (Anthropic claude-3-7 only)
    double topK = -1;              ///< Top-K sampling. -1 = use model default.
    double topP = -1;              ///< Top-P (nucleus) sampling. -1 = use model default.
    double repeatPenalty = -1;     ///< Repetition penalty. -1 = use model default.
    double presencePenalty = -1;   ///< Presence penalty. -1 = use model default
                                   ///  (omit from request). Practical override
                                   ///  range when set: 0.0..2.0.
    double frequencyPenalty = -1;  ///< Frequency penalty. -1 = use model default
                                   ///  (omit from request). Practical override
                                   ///  range when set: 0.0..2.0.

    /**
     * @brief When true, the per-(provider, model) sampling profile
     *        registry FORCES its prescribed values onto this config at
     *        request-build time, overriding any user-set sampling
     *        fields. Defaults to true so freshly-created conversations
     *        with a known-problematic (provider, model) automatically
     *        get the workaround applied.  Users can opt out by flipping
     *        the conversation-settings toggle; the registry then
     *        becomes additive (fills `-1` slots only) for that conv.
     *
     *        Stored per-conversation in `llm_config` JSON so the choice
     *        is sticky across restarts.
     */
    bool forceAppSampling = true;

    /**
     * @brief Per-conversation gate that allows
     *        heartbeat subagent reports to be auto-surfaced (Tier 2)
     *        as actual chat posts without user prompting. Default OFF.
     *        Both this gate AND the per-config heartbeat_configs.enabled
     *        must be true for Tier 2 to fire. The gate is gate-only:
     *        flipping it on does NOT enable any heartbeat by itself.
     *
     *        While the gate is off, the Tier-2 review path marks each
     *        report as skipped by the gate and posts nothing.
     */
    bool allowHeartbeatAutoSurface = false;

    /**
     * @brief Per-conversation per-day cap on Tier-2
     *        auto-surface posts. The intelligent default for a fresh
     *        conversation is computed from the union of enabled
     *        heartbeat configs in scope (see firesIn24h). Stored here
     *        only when the user accepts or overrides the default.
     *        Default 1 keeps a never-edited conversation safe even if
     *        the gate is ever flipped on.
     */
    int autoSurfaceMaxPerDay = 1;

    /**
     * @brief Per-conversation agent reasoning
     *        pattern. Canonical names: "direct" | "react" | "planner" |
     *        "router" | "multi_agent" | "memory". Empty / missing
     *        means "use the default" which is "direct". Promoted from
     *        a global in-memory field on AgentSettingsController so
     *        the choice persists across restart and so two clients
     *        viewing the same conversation see the same pattern.
     */
    QString agentPattern;

    /**
     * @brief Per-conversation tools toggle. Whether
     *        the LLM is allowed to call tools (search_web, write_file,
     *        memory tools, MCP tools, etc.) for THIS conversation.
     *        Default true matches the former global default. Was
     *        a global in-memory field on AgentSettingsController.
     */
    bool toolsEnabled = true;

    /**
     * @brief Whether the system prompt embeds the full
     *        "=== TOOL CALLING === Available tools: ..." text list.
     *        The tool schemas are ALWAYS sent on the structured
     *        `tools` channel regardless; this flag only controls the
     *        duplicated prose list, which measured ~3,000 tokens on a
     *        41-tool roster (~18% of a 16k context window). Default
     *        FALSE: the duplication starves conversation history on
     *        small windows, and tool_calls were verified to still
     *        fire with the section dropped (curl-replay regression +
     *        the group-chat stress gate's tool-health checks). The
     *        anti-inline-JSON protocol instruction is kept in BOTH
     *        modes. Turn ON per conversation only when a weak model
     *        demonstrably needs prose tool descriptions to comply.
     */
    bool toolsInSystemPrompt = false;

    /**
     * @brief Per-conversation dynamic-compaction
     *        gate. When true (default), long conversations whose
     *        history the budgeter is dropping get an LLM-generated
     *        summary injected at context head, with a visible
     *        "~Dynamic Compact Performed~" system receipt. Turning it
     *        off disables BOTH the auto-trigger and summary injection
     *        for this conversation; /compact (manual) still works.
     */
    bool dynamicCompactEnabled = true;

    /**
     * @brief Per-conversation gate for IMPLICIT task completion. When a
     *        task is active and a turn ends with reason=stop, zero content,
     *        and at least one tool call executed, the task gate can mark the
     *        plan Completed automatically (the model "went quiet" after a
     *        tool call). Default FALSE: implicit completion closed multi-part
     *        tasks prematurely (after the first file) and pre-empted the
     *        model's own complete_task, so a task is closed ONLY by an
     *        explicit complete_task / stop_task (or the user) unless this is
     *        turned on per-conversation in Conversation Settings.
     */
    bool implicitTaskCompletion = false;

    /**
     * @brief Proactive compaction cadence in ASSISTANT TURNS. When
     *        > 0, a summary is (re)generated once this many assistant
     *        replies have accumulated beyond what the current summary
     *        covers, BEFORE the budgeter starts dropping history.
     *        Rationale: multi-agent chats measurably degrade
     *        (attention dilution, agents losing the thread) after
     *        roughly 15-20 turns even when the tokens still fit; a
     *        fresh summary + verbatim tail keeps the model sharp.
     *        0 disables the cadence path; compaction then fires only
     *        on context pressure (fill/drop). Counted in assistant
     *        turns, not rows, because one group exchange produces
     *        many tool/system rows.
     */
    int compactEveryTurns = 20;

    /**
     * @brief Per-conversation "ask the user before
     *        each tool call" gate. Default false matches the
     *        former global default. Was a global in-memory field
     *        on AgentSettingsController.
     */
    bool requireConfirmation = false;

    /**
     * @brief Per-conversation RAG (Retrieval-Augmented Generation) toggle.
     *        When true, this conversation's turns embed the latest user
     *        text and inject the most relevant indexed passages
     *        ({conversation} ∪ {project} ∪ {global} scope) into the system
     *        prompt, and its messages are indexed into the corpus. Default
     *        false: RAG is opt-in per conversation. The embedding provider
     *        itself is configured globally on the Embeddings Providers page;
     *        if no provider responds, retrieval simply returns nothing
     *        (graceful degradation), so this flag is the only RAG switch.
     */
    bool ragEnabled = false;

    /**
     * @brief Whether agent memory (AIM) recall is injected pre-turn for this
     *        conversation. Default true: memory recall is on for everyone
     *        unless turned off here; it is NOT gated per-agent (saving is gated
     *        separately by the `remember` tool's whitelist). Each responder only
     *        ever recalls memories scoped to itself (`agent:\<id\>`) or, in an
     *        agentless chat, to the conversation, so "on for everyone" never
     *        leaks one agent's memory to another. With no embedder configured,
     *        recall degrades to lexical (FTS) search.
     */
    bool aimEnabled = true;

    /**
     * @brief Whether team memory (ACN) recall is injected pre-turn for this
     *        conversation. Default true, but ACN only applies at all when the
     *        conversation lives inside a project/organization whose folder-level
     *        ACN master switch (folders.acn_enabled) is on; this per-conversation
     *        flag lets a single chat in such a project opt out. Ignored entirely
     *        for non-project/org chats.
     */
    bool acnEnabled = true;

    /**
     * @brief Per-conversation cap on AUTONOMOUS cascade rounds per user
     *        message (group chats). After a round exhausts its turn caps with
     *        a valid next agent and real progress (a tool call, a retry, or a
     *        novel reply), the cascade starts a new round; this bounds how many
     *        such rounds run before the team pauses for the user. Default 6.
     *        0 = UNBOUNDED: keep going until the team stagnates (a round with
     *        no novel/tool progress stops it via the echo detector), subject
     *        to an absolute safety backstop so a pathological model can never
     *        loop forever. Larger = more autonomy before checking in.
     */
    int maxAutoRounds = 6;

    /**
     * @brief Serializes this config to a JSON object.
     * @return QJsonObject with all fields.
     */
    QJsonObject toJson() const;

    /**
     * @brief Deserializes a config from a JSON object.
     * @param json JSON object with config fields.
     * @return Populated LlmConfig struct. Missing fields use defaults.
     */
    static LlmConfig fromJson(const QJsonObject& json);

    /**
     * @brief Returns true if the provider and model are non-empty.
     * @return true if config has a valid provider and model.
     */
    bool isValid() const { return !providerId.isEmpty() && !modelName.isEmpty(); }
};

/**
 * @brief Resolves the effective input context window from a provider-reported
 *        raw window and the conversation's stored value.
 *
 * Pure, side-effect-free policy function so the clamp/override decision can be
 * unit-tested directly without constructing providers or touching the network.
 * RequestBuilder calls this with the value the active provider's
 * `contextWindowFor()` returned and the conversation's `cfg.contextWindow`.
 *
 * Policy (in order):
 *   1. User override honoured VERBATIM. When @p storedCtx differs from
 *      `LlmConfig::kDefaultContextWindow` the user set the context length
 *      explicitly in Conversation Settings. Their choice is returned EXACTLY
 *      as set; the model-aware resolution and the sane cap apply ONLY to the
 *      default/auto path, so this fix can never overwrite or shrink a value the
 *      user deliberately chose (including one above `kSaneCeilingCtx`).
 *   2. Model-aware resolution. When there is no user override and the provider
 *      reported a real window (@p rawCtx > 0), the raw value is clamped into
 *      [`kSaneFloorCtx`, @p ceilingCtx]. This is the only path that applies the
 *      sane cap (so e.g. a model advertising 1.3M resolves to the ceiling). The
 *      caller passes the role-aware ceiling: `kSaneCeilingCtxHeavy` for
 *      high-overhead turns (group / tools / canvas / skills / heartbeat),
 *      `kSaneCeilingCtx` for lightweight 1:1 turns.
 *   3. Fallback. No override and an UNKNOWN (0) raw window leaves the stored
 *      default in place.
 *
 * NOTE: this is a PER-TURN, in-memory resolution only. RequestBuilder applies
 * it to the request's `config.contextWindow`; it never writes back to the
 * conversation's persisted `llm_config`, so the user's stored setting is
 * untouched regardless of which branch fires.
 *
 * @param rawCtx        The provider's reported max context in tokens, or 0 when
 *                      unknown (cold cache / no source).
 * @param userStoredCtx The conversation's ORIGINAL stored `contextWindow` as
 *                      the user set it, captured BEFORE any internal floor
 *                      bump; used to detect a genuine user override.
 * @param currentCtx    The value the caller currently holds (may already
 *                      include an internal group/tools floor); used as the
 *                      no-override-and-unknown fallback so it is never shrunk.
 * @param ceilingCtx    The role-aware upper clamp for the model-aware path:
 *                      `kSaneCeilingCtxHeavy` for high-overhead turns,
 *                      `kSaneCeilingCtx` for lightweight ones.
 * @returns The context window RequestBuilder should use for this turn, always
 *          a positive token count.
 */
inline int effectiveContextWindow(int rawCtx, int userStoredCtx, int currentCtx, int ceilingCtx) {
    // 1. Explicit user choice — when the ORIGINAL stored value (as set in
    //    Conversation Settings, captured BEFORE any internal floor bump)
    //    differs from the default, honour it EXACTLY: never capped, never
    //    shrunk. `userStoredCtx` MUST be the pristine stored value, not a
    //    value an internal heuristic already raised, or that bump would be
    //    mistaken for a user override and suppress model-aware sizing.
    if (userStoredCtx != LlmConfig::kDefaultContextWindow) {
        return userStoredCtx;
    }
    // 2. No user override + provider reported a real window → model-aware,
    //    clamped to [floor, role-aware ceiling].
    if (rawCtx > 0) {
        return std::clamp(rawCtx, LlmConfig::kSaneFloorCtx, ceilingCtx);
    }
    // 3. Unknown window + no override → keep whatever the caller already has
    //    (e.g. an internal group/tools floor it applied); never shrink it.
    return currentCtx;
}

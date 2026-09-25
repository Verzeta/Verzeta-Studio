// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file openai-compat-provider.h
 * @brief LLM provider adapter for any OpenAI-API-compatible server:
 *        vLLM, LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI,
 *        SGLang, Oobabooga, or any future stack that respects the
 *        OpenAI `/v1/chat/completions` + `/v1/models` contract.
 *
 *        Each instance carries its own user-chosen identity (slug +
 *        display name), its own base URL, its own optional API key,
 *        and its own user-declared capability flags (streaming /
 *        tool calling / vision).  Multiple instances may exist
 *        concurrently: "LM Studio at home" on `localhost:1234/v1`
 *        and "vLLM on the lab box" on `lab.example:8000/v1` are
 *        distinct providers in the ModelRouter registry, each with
 *        its own HttpClient.
 *
 *        Identity model:
 *          - `slug`: stable post-creation identifier matching
 *            `[a-z0-9-]+`.  Never changes; the user deletes and
 *            re-adds to get a new slug.  The slug composes into
 *            `providerId()` as `"custom.\<slug\>"` which is the routing
 *            key in ModelRouter, in `conversations.llm_config.provider`,
 *            and in the per-member override columns on
 *            `project_members` and `conversation_members`.
 *          - `displayName`: user-facing string shown in the provider
 *            dropdown.  Editable at any time; survives rename without
 *            disrupting per-member routing.
 *
 *        Auth model:
 *          `requiresApiKey()` returns true iff EITHER the user has
 *          explicitly toggled the "Requires API Key" flag in the setup
 *          sheet OR `m_apiKey` is non-empty.  Default new instances
 *          have the flag off and no key set, so they dispatch through
 *          to keyless servers without the OpenAIProvider empty-key
 *          gate firing.  Either condition flipping on returns the
 *          fail-fast gate so a cleared-by-mistake key is surfaced as
 *          a clean error instead of a silent unauthenticated request.
 *
 *        Capability flags:
 *          Streaming / tool calling / vision are USER-DECLARED in the
 *          setup sheet, not auto-probed.  Defaults: streaming on,
 *          tool calling on, vision off.  Auto-probing was rejected
 *          as brittle, because different stacks advertise capabilities in
 *          incompatible ways.
 *
 *        License: LGPL-3.0-or-later.  Provider adapters are
 *        infrastructure rather than the differentiated moat, matching
 *        the OpenAI / Anthropic / DeepSeek / OpenRouter / Ollama /
 *        llama.cpp-Remote / Gemini provider classifications.
 * @layer API
 * @dependencies OpenAIProvider (API), HttpClient (Utility).  Does NOT
 *               depend on the in-process llama.cpp library; builds
 *               unconditionally regardless of VERZETA_HAS_LLAMACPP.
 */

#pragma once

#include "openai-provider.h"

#include <QNetworkAccessManager>
#include <QSet>

/**
 * @brief OpenAI-API-compatible client whose identity, base URL, API
 *        key, and capability flags are configured at runtime by the
 *        user via the CustomServerRegistry.
 *
 * Each configured custom server in the registry is backed by exactly
 * one instance of this class, owned by the registry and registered
 * with ModelRouter under the routing key `"custom.\<slug\>"`.  The
 * registry constructs the instance, calls the slug + display-name +
 * base-URL + capability setters, and hands it off to ModelRouter.
 *
 * Inherits SSE streaming, request-body assembly, tool-call parsing,
 * empty-stream safety net, and stale-request guards from
 * `OpenAIProvider` unchanged.  Overrides identity, capability
 * advertisement, and the requiresApiKey() contract.
 */
class OpenAICompatProvider : public OpenAIProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs an unconfigured custom-server instance.
     * @param http   Reference to a dedicated HttpClient instance.
     *               Each custom server gets its own
     *               HttpClient so a stuck server cannot poison the
     *               connection pool of siblings.
     * @param parent Optional Qt parent.
     *
     * The instance is unusable until `setSlug()` and `setBaseUrl()`
     * have been called.  The registry calls those plus the display
     * name + capability flags + (optional) API key + (optional)
     * requires-key flag during the add or hydrate flow.
     */
    explicit OpenAICompatProvider(HttpClient& http, QObject* parent = nullptr);

    /**
     * @brief Sets the routing slug, which is stable post-creation.
     * @param slug URL-safe identifier matching `[a-z0-9-]{2,32}`.
     *
     * The slug composes into `providerId()` as `"custom.\<slug\>"`.
     * The registry validates the slug shape before calling this;
     * passing an invalid slug here is undefined behaviour.
     * Conventionally called exactly once at instance creation.
     */
    void setSlug(const QString& slug);

    /**
     * @brief Returns the user-chosen routing slug.
     * @returns Slug string set by `setSlug()`, or empty if never set.
     */
    QString slug() const;

    /**
     * @brief Sets the user-facing display name.
     * @param name Free-form string shown in the UI provider dropdown.
     *
     * Editable at any time without disrupting per-member routing, because
     * the routing key is `providerId()` which derives from `slug()`,
     * not from this field.
     */
    void setDisplayName(const QString& name);

    /**
     * @brief Toggles the "Requires API Key" user flag.
     * @param enabled true to advertise the server as requiring an
     *                API key, so the empty-key fail-fast gate then
     *                fires even when the key field is empty.  false
     *                to leave the gate controlled solely by
     *                key-presence.
     */
    void setRequiresApiKeyFlag(bool enabled);

    /**
     * @brief Returns the current state of the "Requires API Key"
     *        user flag.
     * @returns true if the flag is on; false otherwise.
     */
    bool requiresApiKeyFlag() const;

    /**
     * @brief Sets the user-declared streaming support flag.
     * @param v true if the server supports SSE streaming.
     */
    void setSupportsStreamingFlag(bool v);

    /**
     * @brief Sets the user-declared tool-calling support flag.
     * @param v true if the server supports the OpenAI `tools` schema
     *          and emits `tool_calls` chunks in its SSE stream.
     */
    void setSupportsToolCallingFlag(bool v);

    /**
     * @brief Sets the user-declared vision (image-input) support flag.
     * @param v true if the server accepts image_url content parts.
     */
    void setSupportsVisionFlag(bool v);

    // -----------------------------------------------------------------------
    // ILLMProvider interface
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the routing identifier for ModelRouter dispatch.
     * @returns `"custom.\<slug\>"` when slug is set; empty string when
     *          unconfigured.
     */
    QString providerId() const override;

    /**
     * @brief Returns the user-chosen display name.
     * @returns Display name string, or empty if never set.
     */
    QString displayName() const override;

    /**
     * @brief Reports SSE streaming support per the user-declared flag.
     * @returns `supportsStreamingFlag()`.
     */
    bool supportsStreaming() const override;

    /**
     * @brief Reports tool-calling support per the user-declared flag.
     * @returns `supportsToolCallingFlag()`.
     */
    bool supportsToolCalling() const override;

    /**
     * @brief Reports vision-input support per the user-declared flag.
     * @returns `supportsVisionFlag()`.
     */
    bool supportsVision() const override;

    /**
     * @brief Reports whether the empty-API-key dispatch gate is on.
     * @returns true iff the user has toggled the "Requires API Key"
     *          flag OR `m_apiKey` is non-empty.
     */
    bool requiresApiKey() const override;

    /**
     * @brief Resolves a model's max context window via LM Studio's /api/v0.
     * @param model Model identifier as listed by the server.
     * @returns The cached `max_context_length` in tokens, or 0 when unknown.
     *
     * On a cache MISS this fires a single-flight async GET
     * `{root}/api/v0/models` (root = base URL with any trailing `/v1`
     * stripped) on a dedicated network manager. Each entry's
     * `max_context_length` is cached. A 404 / error (a generic
     * OpenAI-compatible server that is not LM Studio, whose only listing is
     * /v1/models with no context field) leaves the model UNKNOWN and this
     * returns 0. There is no published-family map for arbitrary custom
     * servers. Non-blocking; main-thread-safe.
     */
    int contextWindowFor(const QString& model) override;

    /**
     * @brief Rewrite an OpenAI `messages` array to satisfy strict chat
     *        templates, preserving all text content.
     *
     * Pure function (no instance state) so it is unit-testable directly.
     * Verzeta's multi-agent history contains shapes that lenient servers
     * (Ollama / OpenAI / OpenRouter) accept but strict server-side chat
     * templates (notably LM Studio's Qwen-family MLX templates) reject
     * with HTTP 400 "Invalid 'messages' in payload". Three rules, each a
     * NO-OP on an already-valid alternating conversation:
     *
     *  A. Only the FIRST message may be `system`. Any later `system`
     *     message is re-roled to `user` (text preserved). Verzeta injects
     *     ACTIVE TASK / ACTIVE CANVAS / cascade-status context as
     *     mid-conversation `system` turns; strict templates only allow a
     *     leading system turn.
     *  B. Consecutive `user` (or `system`) turns are merged into one,
     *     concatenating their content with a blank line. `assistant` and
     *     `tool` turns are NEVER merged, because they carry per-agent identity
     *     (the "(X said)" prefix) and tool_calls / tool_call_id structure.
     *  C. An `assistant` turn that carries `tool_calls` with empty-string
     *     content gets `content: null` (the OpenAI-spec shape; some
     *     validators reject `""`).
     *
     * @param in  The assembled OpenAI messages array.
     * @returns   The normalised array (identical to @p in when already
     *            valid).
     */
    static QJsonArray normalizeStrictMessages(const QJsonArray& in);

  protected:
    /**
     * @brief Adds `chat_template_kwargs: {"enable_thinking": bool}`
     *        to the request body driven by `req.config.thinkingMode`.
     * @param root  Mutable request body.
     * @param req   Full request being dispatched.
     *
     * vLLM, LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI,
     * SGLang and text-generation-webui all surface the request's
     * chat-template kwargs to the loaded model's HuggingFace
     * tokenizer chat template.  Reasoning-capable model templates
     * (Qwen 3 family, DeepSeek-R1, gpt-oss-thinking, etc.) read
     * `enable_thinking` as a Jinja kwarg; templates and servers
     * that do not reference the kwarg silently ignore it.  No
     * model gating is required; the kwarg is universally safe to
     * dispatch.
     */
    void applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const override;

    /**
     * @brief Applies `normalizeStrictMessages` to the assembled array so
     *        custom OpenAI-compatible servers with strict chat templates
     *        accept Verzeta's multi-agent history.
     * @param messages  Mutable assembled OpenAI messages array.
     */
    void normalizeMessages(QJsonArray& messages) const override;

  private:
    QString m_slug;
    QString m_displayName;
    bool m_requiresApiKeyFlag = false;
    bool m_supportsStreamingFlag = true;
    bool m_supportsToolCallingFlag = true;
    bool m_supportsVisionFlag = false;

    /**
     * @brief Models with an /api/v0/models warm query currently in flight.
     *        Single-flight guard. Entries cleared when the reply lands.
     */
    QSet<QString> m_ctxInFlight;

    /**
     * @brief Dedicated network manager for the /api/v0/models warm query,
     *        kept off the shared streaming HttpClient. Parented (RAII).
     */
    QNetworkAccessManager m_ctxNam;

    /**
     * @brief Fires the single-flight async /api/v0/models probe.
     * @param model The model that triggered the miss (single-flight key).
     * @sideeffects Caches every entry's `max_context_length` on reply.
     */
    void warmContextWindows(const QString& model);
};

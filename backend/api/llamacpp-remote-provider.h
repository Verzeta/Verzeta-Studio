// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file llamacpp-remote-provider.h
 * @brief LLM provider adapter for an externally-running llama.cpp server.
 *
 *        NOT to be confused with the in-process LlamaCppProvider
 *        (backend/api/llamacpp-provider.h), which links the llama.cpp
 *        library directly. This adapter is a pure HTTP client and does
 *        NOT include <llama.h>; it talks to a `llama.cpp server` binary
 *        running outside this process over its OpenAI-compatible Chat
 *        Completions endpoint.
 *
 *        Default base URL: http://localhost:8080/v1 (the typical
 *        llama-server default). User-configurable via SettingsService.
 *
 *        Tool calling: many shipped llama.cpp server builds do NOT
 *        enable the `tools` slot (the support exists in upstream master
 *        but is feature-gated at build time). The default is
 *        `supportsToolCalling() = false`; a setter is exposed so
 *        SettingsService can flip it on for users whose server build
 *        does support tools. There is no live probe.
 *
 *        Auth: llama.cpp server is auth-less by default. The
 *        Authorization header is sent only when the user has configured
 *        an API key (e.g. when fronting the server with a reverse
 *        proxy that requires bearer auth).
 * @layer API
 * @dependencies OpenAIProvider (API), HttpClient (Utility). Does NOT
 *               depend on the in-process llama.cpp library; safe to
 *               build unconditionally (no VERZETA_HAS_LLAMACPP guard).
 */


#pragma once

#include "openai-provider.h"

#include <QHash>
#include <QNetworkAccessManager>

/**
 * @brief OpenAI-compatible client targeting a local-network llama.cpp
 *        server.
 *
 * Builds unconditionally regardless of the VERZETA_HAS_LLAMACPP build
 * flag because it does not link in-process llama.cpp.
 */
class LlamaCppRemoteProvider : public OpenAIProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the llama.cpp-remote provider.
     * @param http   Reference to a shared HttpClient instance.
     * @param parent Optional Qt parent.
     */
    explicit LlamaCppRemoteProvider(HttpClient& http, QObject* parent = nullptr);

    /**
     * @brief Returns the provider id used by ModelRouter for dispatch.
     * @returns "llamacpp_remote".
     */
    QString providerId() const override;

    /**
     * @brief Human-readable name shown in the UI provider selector.
     * @returns "llama.cpp (Remote)".
     */
    QString displayName() const override;

    /**
     * @brief Reports tool-calling support for the configured server.
     * @returns The value last set by setSupportsToolCalling(), or false
     *          by default, since most shipped llama.cpp server builds do not
     *          enable the `tools` slot.
     *
     * llama.cpp server's `/v1/models` endpoint typically exposes only
     * whichever GGUF was passed on the server CLI. There is no live
     * capability probe; SettingsService is the source of truth.
     */
    bool supportsToolCalling() const override;

    /**
     * @brief SettingsService-driven toggle for tool-calling support.
     * @param enabled  true to advertise tool calling and include the
     *                 `tools` array in outgoing requests; false to
     *                 omit it.
     */
    void setSupportsToolCalling(bool enabled);

    /**
     * @brief Treats llama.cpp server as keyless by default.
     * @returns true iff a non-empty API key has been configured;
     *          false otherwise.
     *
     * The base llama.cpp server binary is auth-less; users only
     * configure an API key when they have fronted the server with a
     * reverse proxy that requires bearer auth.  Key-presence is the
     * trigger that flips the `OpenAIProvider::sendRequest` empty-key
     * gate back on, providing fail-fast behaviour for the proxied
     * case while permitting the typical local-server case to dispatch
     * without an API key at all.
     */
    bool requiresApiKey() const override;

    /**
     * @brief Resolves the server's context size via the llama.cpp /props
     *        endpoint.
     * @param model Model identifier (llama.cpp server is mono-model; the value
     *              is the same for any model name and cached under @p model).
     * @returns The cached `default_generation_settings.n_ctx`, or 0 when not
     *          yet known.
     *
     * On a cache MISS this fires a single-flight async GET `{root}/props`
     * (root = base URL with any trailing `/v1` stripped) on a dedicated
     * network manager and caches `default_generation_settings.n_ctx`. Returns
     * 0 on the cold first call. Non-blocking; main-thread-safe.
     */
    int contextWindowFor(const QString& model) override;

  private:
    bool m_toolCallingEnabled = false;

    /**
     * @brief Cached server context size (tokens), keyed by the model name the
     *        first probe was issued for. 0 / absent means unresolved.
     */
    QHash<QString, int> m_propsCtxCache;

    /** @brief True while a /props probe is in flight (single-flight guard). */
    bool m_propsInFlight = false;

    /**
     * @brief Dedicated network manager for the /props warm query, kept off the
     *        shared streaming HttpClient. Parented (RAII).
     */
    QNetworkAccessManager m_propsNam;

    /**
     * @brief Fires the single-flight async GET /props probe.
     * @param model The model the cached value is stored under.
     */
    void warmServerContext(const QString& model);

  protected:
    /**
     * @brief Omits the Authorization header entirely when no API key
     *        is configured (the typical local-server case). When a key
     *        IS set (reverse-proxy auth scenario) the base header set
     *        is returned unchanged.
     * @returns Map of header name to value.
     */
    QMap<QString, QString> buildHeaders() const override;

    /**
     * @brief Adds `chat_template_kwargs: {"enable_thinking": bool}`
     *        driven by `req.config.thinkingMode`.
     * @param root  Mutable request body.
     * @param req   Full request being dispatched.
     *
     * The llama.cpp server applies the loaded model's HuggingFace
     * tokenizer chat template, which for reasoning-capable models
     * (Qwen 3 family, DeepSeek-R1, gpt-oss-thinking, etc.) reads
     * `enable_thinking` as a Jinja kwarg.  Templates that do not
     * reference the kwarg silently ignore it, so this is safe to
     * send unconditionally.
     */
    void applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const override;
};

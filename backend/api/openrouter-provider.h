// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file openrouter-provider.h
 * @brief LLM provider adapter for OpenRouter (https://openrouter.ai).
 *
 *        OpenRouter exposes an OpenAI-compatible REST surface; this
 *        adapter is a thin subclass of OpenAIProvider that overrides
 *        the base URL, the provider id, and the per-request app-
 *        attribution headers (HTTP-Referer + X-Title) used by
 *        OpenRouter's dashboard and ranked routing. The wire format,
 *        SSE parser, and request body builder are all inherited.
 * @layer API
 * @dependencies OpenAIProvider (API), HttpClient (Utility).
 */


#pragma once

#include "openai-provider.h"

/**
 * @brief OpenAI-compatible client targeting api.openrouter.ai.
 *
 * Tool calling is enabled at the API-surface level; per-model support
 * is decided by OpenRouter. The default model list is empty until
 * refreshModels() populates it from GET /api/v1/models.
 */
class OpenRouterProvider : public OpenAIProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the OpenRouter provider.
     * @param http   Reference to a shared HttpClient instance.
     * @param parent Optional Qt parent.
     */
    explicit OpenRouterProvider(HttpClient& http, QObject* parent = nullptr);

    /**
     * @brief Returns the provider id used by ModelRouter for dispatch.
     * @returns "openrouter".
     */
    QString providerId() const override;

    /**
     * @brief Human-readable name shown in the UI provider selector.
     * @returns "OpenRouter".
     */
    QString displayName() const override;

  protected:
    /**
     * @brief Returns Authorization plus OpenRouter's HTTP-Referer + X-Title
     *        app-attribution headers.
     * @returns Header map suitable for the HttpClient call.
     *
     * The two attribution headers identify the calling app in OpenRouter's
     * dashboard and ranked-routing telemetry. The API key is only ever
     * carried in the Authorization header, never echoed elsewhere.
     */
    QMap<QString, QString> buildHeaders() const override;

    /**
     * @brief Embedded-family heuristic fallback for OpenRouter models.
     * @param model OpenRouter model slug (e.g. "meta-llama/llama-3-70b").
     * @returns A heuristic context window in tokens, or 0 when the family is
     *          unrecognised.
     *
     * Pure, synchronous, no network. Used ONLY as a fallback. The live cache
     * populated from `data[].context_length` during refreshModels() is the
     * primary source and is consulted first by the inherited
     * `contextWindowFor()`. This heuristic merely gives a sensible value for a
     * model the user selects before the catalog has been refreshed.
     */
    int publishedContextWindow(const QString& model) const override;

    /**
     * @brief Adds OpenRouter's standardised `reasoning: {effort: ...}`
     *        block driven by `req.config.thinkingMode`.
     * @param root  Mutable request body.
     * @param req   Full request being dispatched.
     *
     * OpenRouter exposes a provider-agnostic reasoning hint that it
     * routes to whichever upstream serves the model (OpenAI o-series,
     * Anthropic extended thinking, Qwen / DeepSeek-R1 self-hosted,
     * etc.).  Sent on every request regardless of upstream; models
     * that do not honour reasoning silently ignore the field.
     * `"high"` and `"low"` mirror the OpenAI o-series effort values
     * for cross-provider consistency; OpenRouter normalises them per
     * upstream.
     */
    void applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const override;
};

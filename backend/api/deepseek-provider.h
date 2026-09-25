// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file deepseek-provider.h
 * @brief LLM provider adapter for DeepSeek (https://api.deepseek.com).
 *
 *        DeepSeek exposes an OpenAI-compatible Chat Completions API;
 *        this adapter is a thin subclass of OpenAIProvider that
 *        overrides the base URL, the provider id and the thinking
 *        parameter, and seeds the default model list ("deepseek-flash",
 *        "deepseek-v4-pro"); the live list comes from GET /models.
 *        Tool calling is enabled, since DeepSeek's Chat Completions
 *        surface accepts the `tools` field.
 * @layer API
 * @dependencies OpenAIProvider (API), HttpClient (Utility).
 */


#pragma once

#include "openai-provider.h"

/**
 * @brief OpenAI-compatible client targeting api.deepseek.com.
 */
class DeepSeekProvider : public OpenAIProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the DeepSeek provider.
     * @param http   Reference to a shared HttpClient instance.
     * @param parent Optional Qt parent.
     */
    explicit DeepSeekProvider(HttpClient& http, QObject* parent = nullptr);

    /**
     * @brief Returns the provider id used by ModelRouter for dispatch.
     * @returns "deepseek".
     */
    QString providerId() const override;

    /**
     * @brief Human-readable name shown in the UI provider selector.
     * @returns "DeepSeek".
     */
    QString displayName() const override;

  protected:
    /**
     * @brief Sets DeepSeek's thinking parameter for the request.
     * @param root Request body being built; receives the `thinking` field.
     * @param req  The request, for its thinking toggle.
     */
    void applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const override;

    /**
     * @brief Published context window for DeepSeek models.
     * @param model Model identifier (unused, because DeepSeek's published models all
     *              share the same window).
     * @returns 131072 for every DeepSeek model.
     *
     * Pure, synchronous, no network. DeepSeek does not expose a per-model
     * context field on its OpenAI-compatible /v1/models, so the published
     * value is the only source.
     */
    int publishedContextWindow(const QString& model) const override;
};

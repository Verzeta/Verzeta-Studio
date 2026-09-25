// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file openrouter-provider.cpp
 * @brief OpenRouter adapter implementation (subclass of OpenAIProvider).
 *
 *        Overrides base URL, clears the default model list (refreshed
 *        on demand via GET /api/v1/models), and appends the
 *        OpenRouter app-attribution headers (HTTP-Referer + X-Title)
 *        to every request. The inherited OpenAIProvider handles
 *        wire format, SSE parsing, and request body assembly.
 * @layer API
 * @dependencies OpenAIProvider (API).
 */


#include "openrouter-provider.h"

OpenRouterProvider::OpenRouterProvider(HttpClient& http, QObject* parent)
    : OpenAIProvider(http, parent) {
    // OpenRouter API root.
    setBaseUrl(QStringLiteral("https://openrouter.ai/api/v1"));
    // OpenRouter's catalog is huge (300+ models) and changes
    // frequently. Start empty and let refreshModels() populate via
    // GET /v1/models once the user provides a key.
    m_models.clear();
}

QString OpenRouterProvider::providerId() const {
    return QStringLiteral("openrouter");
}

QString OpenRouterProvider::displayName() const {
    return QStringLiteral("OpenRouter");
}

QMap<QString, QString> OpenRouterProvider::buildHeaders() const {
    auto headers = OpenAIProvider::buildHeaders();
    // OpenRouter app-attribution headers per their docs. Static strings
    // — no per-request variability so the user agent is unambiguous in
    // OpenRouter's dashboard + ranked-routing telemetry.
    headers[QStringLiteral("HTTP-Referer")] = QStringLiteral("https://verzeta.studio");
    headers[QStringLiteral("X-Title")] = QStringLiteral("Verzeta Studio");
    return headers;
}

int OpenRouterProvider::publishedContextWindow(const QString& model) const {
    // Fallback heuristic only — the live cache (data[].context_length captured
    // during refreshModels) is the authoritative source and is checked first
    // by OpenAIProvider::contextWindowFor. This keeps a freshly-selected model
    // sane before the catalog has been fetched.
    const QString m = model.toLower();
    if (m.contains(QStringLiteral("llama-3")) || m.contains(QStringLiteral("llama3"))) {
        return 131072;
    }
    if (m.contains(QStringLiteral("qwen"))) {
        return 32768;
    }
    if (m.contains(QStringLiteral("claude"))) {
        return 200000;
    }
    if (m.contains(QStringLiteral("gemini"))) {
        return 1000000;
    }
    if (m.contains(QStringLiteral("deepseek"))) {
        return 131072;
    }
    if (m.contains(QStringLiteral("gpt-4o")) || m.contains(QStringLiteral("gpt-4.1")) ||
        m.contains(QStringLiteral("gpt-4-turbo"))) {
        return 128000;
    }
    // Unknown OpenRouter family — report unknown so the default applies.
    return 0;
}

void OpenRouterProvider::applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const {
    // OpenRouter's `reasoning` object is the standardised, provider-
    // agnostic hint.  The effort string maps cleanly across upstreams:
    // OpenAI's o-series consumes it as `reasoning_effort`, Anthropic's
    // extended-thinking models honour it as a thinking-budget hint,
    // and self-hosted reasoning models routed through OpenRouter
    // (Qwen / DeepSeek-R1 / etc.) get the corresponding chat-template
    // kwargs injected by OpenRouter's normalisation layer.
    // Any effort value switches reasoning on, so with thinking off the
    // field is left out and the model keeps its own default.
    if (!req.config.thinkingMode)
        return;
    root[QStringLiteral("reasoning")] =
        QJsonObject{{QStringLiteral("effort"), QStringLiteral("high")}};
}

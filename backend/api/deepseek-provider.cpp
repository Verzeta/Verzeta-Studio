// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file deepseek-provider.cpp
 * @brief DeepSeek adapter implementation (subclass of OpenAIProvider).
 *
 *        Overrides base URL and seeds the default model list. The
 *        inherited OpenAIProvider handles the wire format, SSE
 *        parsing, and request body assembly.
 * @layer API
 * @dependencies OpenAIProvider (API).
 */


#include "deepseek-provider.h"

DeepSeekProvider::DeepSeekProvider(HttpClient& http, QObject* parent)
    : OpenAIProvider(http, parent) {
    setBaseUrl(QStringLiteral("https://api.deepseek.com/v1"));
    m_models = {
        QStringLiteral("deepseek-flash"),
        QStringLiteral("deepseek-v4-pro"),
    };
}

void DeepSeekProvider::applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const {
    // DeepSeek thinks by default, and in a request with tools it rejects
    // earlier turns whose reasoning_content is not sent back. The app does
    // not keep that reasoning, so thinking is enabled only when the
    // conversation asks for it and the request carries no tools.
    const bool think = req.config.thinkingMode && !root.contains(QStringLiteral("tools"));
    root[QStringLiteral("thinking")] = QJsonObject{
        {QStringLiteral("type"), think ? QStringLiteral("enabled") : QStringLiteral("disabled")}};
}

QString DeepSeekProvider::providerId() const {
    return QStringLiteral("deepseek");
}

QString DeepSeekProvider::displayName() const {
    return QStringLiteral("DeepSeek");
}

int DeepSeekProvider::publishedContextWindow(const QString& model) const {
    Q_UNUSED(model);
    // All current DeepSeek models expose a 131072-token context window.
    return 131072;
}

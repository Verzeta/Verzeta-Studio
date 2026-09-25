// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file classifier-provider-factory.cpp
 * @brief Implementation of the RAGP classifier provider factory. See
 *        classifier-provider-factory.h for the architectural contract.
 * @layer Service
 * @dependencies Qt6::Core, all built-in ILLMProvider implementations,
 *               HttpClient, SettingsService.
 */

#include "classifier-provider-factory.h"

#include "../../api/anthropic-provider.h"
#include "../../api/deepseek-provider.h"
#include "../../api/gemini-provider.h"
#include "../../api/llamacpp-remote-provider.h"
#include "../../api/ollama-provider.h"
#include "../../api/openai-compat-provider.h"
#include "../../api/openai-provider.h"
#include "../../api/openrouter-provider.h"
#include "../../services/settings-service.h"
#include "../../utils/http-client.h"
#include "../../utils/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

namespace Ragp {

namespace {

/**
 * @brief Looks up the custom-server row for `slug` in the persisted
 *        `custom_servers` settings blob.
 * @param settings Settings source.
 * @param slug     The `\<slug\>` part of a `custom.\<slug\>` provider id.
 * @param outBaseUrl     Receives the row's baseUrl on hit.
 * @param outRequiresKey Receives the row's requiresApiKeyFlag on hit.
 * @returns true iff a row with that slug exists.
 */
bool findCustomServerRow(SettingsService& settings,
                         const QString& slug,
                         QString* outBaseUrl,
                         bool* outRequiresKey) {
    const QString blob = settings.customServersBlob();
    if (blob.trimmed().isEmpty())
        return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return false;
    }
    const QJsonArray arr = doc.array();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("slug")).toString() == slug) {
            if (outBaseUrl) {
                *outBaseUrl = o.value(QStringLiteral("baseUrl")).toString();
            }
            if (outRequiresKey) {
                *outRequiresKey = o.value(QStringLiteral("requiresApiKeyFlag")).toBool(false);
            }
            return true;
        }
    }
    return false;
}

/** @brief Compose an unavailability reason and return false. */
bool unavailable(QString* whyNot, const QString& reason) {
    if (whyNot)
        *whyNot = reason;
    return false;
}

}  // namespace

bool classifierProviderAvailable(const QString& providerId,
                                 SettingsService& settings,
                                 QString* whyNot) {
    const QString id = providerId.toLower();

    if (id.isEmpty()) {
        return unavailable(whyNot, QStringLiteral("no active provider configured"));
    }
    if (id == QStringLiteral("ollama")) {
        if (settings.ollamaBaseUrl().trimmed().isEmpty()) {
            return unavailable(whyNot, QStringLiteral("ollama: base URL not configured"));
        }
        return true;
    }
    if (id == QStringLiteral("openai") || id == QStringLiteral("anthropic") ||
        id == QStringLiteral("gemini") || id == QStringLiteral("openrouter") ||
        id == QStringLiteral("deepseek")) {
        if (settings.apiKey(id).isEmpty()) {
            return unavailable(whyNot, QStringLiteral("%1: no API key configured").arg(id));
        }
        return true;
    }
    if (id == QStringLiteral("llamacpp_remote")) {
        // llama.cpp server is auth-less by default and ships a default
        // base URL; an explicit empty override would still resolve to
        // the provider's built-in default. Always constructible.
        return true;
    }
    if (id == QStringLiteral("llamacpp")) {
        // In-process embedded llama.cpp. Constructing a second
        // instance doubles model RAM (a 7B Q4_K_M is ~4 GiB) and
        // doubles GGML_ASSERT abort() exposure (B-5). The designed
        // local classifier path is the dedicated small-GGUF
        // local-llama RAGP backend.
        return unavailable(
            whyNot,
            QStringLiteral("the local llama.cpp model cannot also run background tasks, "
                           "because that would load it twice. Choose the RAGP local model "
                           "or a remote provider"));
    }
    if (id.startsWith(QStringLiteral("custom."))) {
        const QString slug = providerId.mid(7);
        QString baseUrl;
        bool requiresKey = false;
        if (!findCustomServerRow(settings, slug, &baseUrl, &requiresKey)) {
            return unavailable(
                whyNot, QStringLiteral("custom server '%1' not found in settings").arg(slug));
        }
        if (baseUrl.trimmed().isEmpty()) {
            return unavailable(whyNot,
                               QStringLiteral("custom server '%1' has no base URL").arg(slug));
        }
        if (requiresKey && settings.apiKey(QStringLiteral("custom.") + slug).isEmpty()) {
            return unavailable(whyNot,
                               QStringLiteral("custom server '%1' requires an API key "
                                              "that is not configured")
                                   .arg(slug));
        }
        return true;
    }

    return unavailable(
        whyNot, QStringLiteral("provider '%1' is not supported by the classifier").arg(providerId));
}

ClassifierProvider makeClassifierProvider(const QString& providerId,
                                          SettingsService& settings,
                                          QObject* parent,
                                          QString* whyNot) {
    ClassifierProvider out;
    out.providerId = providerId;

    QString reason;
    if (!classifierProviderAvailable(providerId, settings, &reason)) {
        if (whyNot)
            *whyNot = reason;
        qCInfo(verzetaUi).noquote() << "RAGP classifier provider unavailable:" << reason;
        return out;
    }

    out.http = std::make_unique<HttpClient>(parent);
    const QString id = providerId.toLower();

    if (id == QStringLiteral("ollama")) {
        auto p = std::make_unique<OllamaProvider>(*out.http, parent);
        p->setBaseUrl(settings.ollamaBaseUrl());
        out.provider = std::move(p);
    } else if (id == QStringLiteral("openai")) {
        auto p = std::make_unique<OpenAIProvider>(*out.http, parent);
        p->setApiKey(settings.apiKey(QStringLiteral("openai")));
        out.provider = std::move(p);
    } else if (id == QStringLiteral("anthropic")) {
        auto p = std::make_unique<AnthropicProvider>(*out.http, parent);
        p->setApiKey(settings.apiKey(QStringLiteral("anthropic")));
        out.provider = std::move(p);
    } else if (id == QStringLiteral("gemini")) {
        auto p = std::make_unique<GeminiProvider>(*out.http, parent);
        p->setApiKey(settings.apiKey(QStringLiteral("gemini")));
        out.provider = std::move(p);
    } else if (id == QStringLiteral("openrouter")) {
        auto p = std::make_unique<OpenRouterProvider>(*out.http, parent);
        p->setApiKey(settings.apiKey(QStringLiteral("openrouter")));
        const QString baseOverride = settings.providerBaseUrl(QStringLiteral("openrouter"));
        if (!baseOverride.isEmpty())
            p->setBaseUrl(baseOverride);
        out.provider = std::move(p);
    } else if (id == QStringLiteral("deepseek")) {
        auto p = std::make_unique<DeepSeekProvider>(*out.http, parent);
        p->setApiKey(settings.apiKey(QStringLiteral("deepseek")));
        const QString baseOverride = settings.providerBaseUrl(QStringLiteral("deepseek"));
        if (!baseOverride.isEmpty())
            p->setBaseUrl(baseOverride);
        out.provider = std::move(p);
    } else if (id == QStringLiteral("llamacpp_remote")) {
        auto p = std::make_unique<LlamaCppRemoteProvider>(*out.http, parent);
        p->setApiKey(settings.apiKey(QStringLiteral("llamacpp_remote")));
        const QString baseOverride = settings.providerBaseUrl(QStringLiteral("llamacpp_remote"));
        if (!baseOverride.isEmpty())
            p->setBaseUrl(baseOverride);
        out.provider = std::move(p);
    } else if (id.startsWith(QStringLiteral("custom."))) {
        const QString slug = providerId.mid(7);
        QString baseUrl;
        bool requiresKey = false;
        findCustomServerRow(settings, slug, &baseUrl, &requiresKey);
        auto p = std::make_unique<OpenAICompatProvider>(*out.http, parent);
        p->setSlug(slug);
        p->setBaseUrl(baseUrl);
        p->setRequiresApiKeyFlag(requiresKey);
        if (requiresKey) {
            p->setApiKey(settings.apiKey(QStringLiteral("custom.") + slug));
        }
        out.provider = std::move(p);
    }

    if (!out.provider) {
        // classifierProviderAvailable returned true but no branch
        // matched — internal inconsistency; surface loudly.
        out.http.reset();
        const QString msg =
            QStringLiteral("internal: no construction branch for '%1'").arg(providerId);
        if (whyNot)
            *whyNot = msg;
        qCWarning(verzetaUi).noquote() << "RAGP classifier factory:" << msg;
        return out;
    }

    qCInfo(verzetaUi).noquote() << "RAGP classifier provider constructed for" << providerId;
    return out;
}

}  // namespace Ragp

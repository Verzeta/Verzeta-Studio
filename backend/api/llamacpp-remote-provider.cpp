// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file llamacpp-remote-provider.cpp
 * @brief llama.cpp-remote adapter implementation. Pure HTTP client
 *        over the OpenAI-compatible Chat Completions surface exposed
 *        by an external `llama.cpp server` process; no in-process
 *        llama.cpp linkage.
 *
 *        Overrides the base URL, seeds a synthetic single-model entry
 *        ("current"), and omits the Authorization header when no API
 *        key is configured. The inherited OpenAIProvider handles the
 *        wire format, SSE parsing, and request body assembly.
 * @layer API
 * @dependencies OpenAIProvider (API).
 */


#include "llamacpp-remote-provider.h"

#include "../utils/logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

LlamaCppRemoteProvider::LlamaCppRemoteProvider(HttpClient& http, QObject* parent)
    : OpenAIProvider(http, parent), m_propsNam(this) {
    setBaseUrl(QStringLiteral("http://localhost:8080/v1"));
    // llama.cpp server is mono-model — whichever GGUF was passed to
    // the server CLI is the only "model" exposed. We seed a single
    // synthetic entry; the user picks it via the standard model
    // selector. refreshModels() inherited from OpenAIProvider will
    // call GET /v1/models which most llama.cpp builds also honour,
    // returning the loaded model's display name.
    m_models = {QStringLiteral("current")};
}

QString LlamaCppRemoteProvider::providerId() const {
    return QStringLiteral("llamacpp_remote");
}

QString LlamaCppRemoteProvider::displayName() const {
    return QStringLiteral("llama.cpp (Remote)");
}

bool LlamaCppRemoteProvider::supportsToolCalling() const {
    return m_toolCallingEnabled;
}

void LlamaCppRemoteProvider::setSupportsToolCalling(bool enabled) {
    m_toolCallingEnabled = enabled;
}

QMap<QString, QString> LlamaCppRemoteProvider::buildHeaders() const {
    QMap<QString, QString> headers;
    // Authorization is only meaningful when the user has fronted the
    // llama.cpp server with a reverse-proxy that requires bearer auth.
    // The plain-server case is auth-less; emitting an empty Bearer
    // header would have llama.cpp respond 400 on some builds.
    if (!m_apiKey.isEmpty()) {
        headers[QStringLiteral("Authorization")] = QStringLiteral("Bearer ") + m_apiKey;
    }
    return headers;
}

bool LlamaCppRemoteProvider::requiresApiKey() const {
    // llama.cpp server is keyless by contract.  An empty m_apiKey means
    // the user is talking to a plain local server (no auth needed) and
    // the dispatch must proceed without the OpenAIProvider::sendRequest
    // empty-key short-circuit firing.  A non-empty m_apiKey means the
    // user has fronted the server with a reverse proxy that requires
    // bearer auth; in that case the gate becomes a useful fail-fast.
    return !m_apiKey.isEmpty();
}

int LlamaCppRemoteProvider::contextWindowFor(const QString& model) {
    const auto it = m_propsCtxCache.constFind(model);
    if (it != m_propsCtxCache.constEnd()) {
        return it.value();
    }
    warmServerContext(model);
    return 0;  // cold first call — no published map for a llama.cpp server
}

void LlamaCppRemoteProvider::warmServerContext(const QString& model) {
    if (m_propsInFlight) {
        return;  // single-flight — one /props probe at a time per server
    }
    // /props lives at the server root; strip the OpenAI `/v1` surface suffix.
    QString root = m_baseUrl;
    while (root.endsWith(QLatin1Char('/'))) {
        root.chop(1);
    }
    if (root.endsWith(QStringLiteral("/v1"))) {
        root.chop(3);
    }
    if (root.isEmpty()) {
        return;
    }
    m_propsInFlight = true;

    QNetworkRequest request{QUrl(root + QStringLiteral("/props"))};
    QNetworkReply* reply = m_propsNam.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, model]() {
        reply->deleteLater();
        m_propsInFlight = false;
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            return;
        }
        const int nCtx = doc.object()
                             .value(QStringLiteral("default_generation_settings"))
                             .toObject()
                             .value(QStringLiteral("n_ctx"))
                             .toInt(0);
        if (nCtx > 0) {
            m_propsCtxCache.insert(model, nCtx);
            qCDebug(verzetaLlm) << "llama.cpp-remote resolved n_ctx =" << nCtx << "for model"
                                << model;
        }
    });
}

void LlamaCppRemoteProvider::applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const {
    // llama.cpp server applies the loaded model's HuggingFace
    // tokenizer chat template via its `--jinja` path.  Reasoning-
    // capable templates (Qwen 3 family, DeepSeek-R1, gpt-oss-thinking)
    // read `enable_thinking` as a Jinja kwarg; templates that do not
    // reference the kwarg silently ignore the field, so the call is
    // safe to dispatch unconditionally.
    QJsonObject kwargs;
    kwargs[QStringLiteral("enable_thinking")] = req.config.thinkingMode;
    root[QStringLiteral("chat_template_kwargs")] = kwargs;
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file openai-compat-provider.cpp
 * @brief OpenAI-API-compatible custom server adapter implementation.
 *        Identity (slug + display name), base URL, API key, and the
 *        four user-declared flags (requires-key + streaming + tool
 *        calling + vision) are configured at runtime; SSE streaming,
 *        request-body assembly, and tool-call parsing inherit
 *        unchanged from OpenAIProvider.
 * @layer API
 * @dependencies OpenAIProvider (API).
 */

#include "openai-compat-provider.h"

#include "../utils/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

OpenAICompatProvider::OpenAICompatProvider(HttpClient& http, QObject* parent)
    : OpenAIProvider(http, parent), m_ctxNam(this) {
    // No baseUrl seeded — the registry calls setBaseUrl during the
    // add or hydrate flow.  No model list seeded — the registry calls
    // refreshModels at save time so the model dropdown is populated
    // by the time the user opens it.
    m_models.clear();
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

void OpenAICompatProvider::setSlug(const QString& slug) {
    m_slug = slug;
}

QString OpenAICompatProvider::slug() const {
    return m_slug;
}

void OpenAICompatProvider::setDisplayName(const QString& name) {
    m_displayName = name;
}

QString OpenAICompatProvider::providerId() const {
    if (m_slug.isEmpty()) {
        return QString();
    }
    return QStringLiteral("custom.") + m_slug;
}

QString OpenAICompatProvider::displayName() const {
    return m_displayName;
}

// ---------------------------------------------------------------------------
// Context-window resolution (LM Studio /api/v0/models)
// ---------------------------------------------------------------------------

int OpenAICompatProvider::contextWindowFor(const QString& model) {
    if (model.isEmpty()) {
        return 0;
    }
    const auto it = m_ctxCache.constFind(model);
    if (it != m_ctxCache.constEnd()) {
        return it.value();
    }
    // Miss — warm via /api/v0/models (LM Studio). No published map for an
    // arbitrary custom server, so report unknown now.
    warmContextWindows(model);
    return 0;
}

void OpenAICompatProvider::warmContextWindows(const QString& model) {
    if (model.isEmpty() || m_ctxInFlight.contains(model)) {
        return;  // single-flight per triggering model
    }
    // Derive the server root: the base URL with any trailing "/v1" stripped,
    // since /api/v0/models lives at the root (the OpenAI surface is /v1).
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
    m_ctxInFlight.insert(model);

    QNetworkRequest request{QUrl(root + QStringLiteral("/api/v0/models"))};
    QNetworkReply* reply = m_ctxNam.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, model]() {
        reply->deleteLater();
        m_ctxInFlight.remove(model);
        if (reply->error() != QNetworkReply::NoError) {
            // 404 / connection error → not an LM Studio server (or down).
            // Leave the model UNKNOWN; the default window applies.
            return;
        }
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            return;
        }
        const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
        for (const QJsonValue& v : data) {
            const QJsonObject obj = v.toObject();
            const QString id = obj.value(QStringLiteral("id")).toString();
            const int ctx = obj.value(QStringLiteral("max_context_length")).toInt(0);
            if (!id.isEmpty() && ctx > 0) {
                m_ctxCache.insert(id, ctx);
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Capability + auth flags
// ---------------------------------------------------------------------------

void OpenAICompatProvider::setRequiresApiKeyFlag(bool enabled) {
    m_requiresApiKeyFlag = enabled;
}

bool OpenAICompatProvider::requiresApiKeyFlag() const {
    return m_requiresApiKeyFlag;
}

void OpenAICompatProvider::setSupportsStreamingFlag(bool v) {
    m_supportsStreamingFlag = v;
}

void OpenAICompatProvider::setSupportsToolCallingFlag(bool v) {
    m_supportsToolCallingFlag = v;
}

void OpenAICompatProvider::setSupportsVisionFlag(bool v) {
    m_supportsVisionFlag = v;
}

bool OpenAICompatProvider::supportsStreaming() const {
    return m_supportsStreamingFlag;
}

bool OpenAICompatProvider::supportsToolCalling() const {
    return m_supportsToolCallingFlag;
}

bool OpenAICompatProvider::supportsVision() const {
    return m_supportsVisionFlag;
}

bool OpenAICompatProvider::requiresApiKey() const {
    // Flag OR key-presence flips the OpenAIProvider::sendRequest
    // empty-key gate on.  Default instances — flag off and empty
    // key — dispatch keyless through to the underlying server.
    return m_requiresApiKeyFlag || !m_apiKey.isEmpty();
}

void OpenAICompatProvider::applyReasoningConfig(QJsonObject& root, const LlmRequest& req) const {
    // The OpenAI-compatible self-host family (vLLM, LM Studio, Jan,
    // Llamafile, TabbyAPI, KoboldCpp, LocalAI, SGLang,
    // text-generation-webui) forwards `chat_template_kwargs` to the
    // loaded model's HuggingFace tokenizer chat template.  Reasoning-
    // capable templates read `enable_thinking` as a Jinja kwarg;
    // templates that do not reference the kwarg silently ignore the
    // field.  No model gate is needed — the kwarg is universally
    // safe across the OpenAICompat family.
    QJsonObject kwargs;
    kwargs[QStringLiteral("enable_thinking")] = req.config.thinkingMode;
    root[QStringLiteral("chat_template_kwargs")] = kwargs;
}

void OpenAICompatProvider::normalizeMessages(QJsonArray& messages) const {
    messages = normalizeStrictMessages(messages);
}

QJsonArray OpenAICompatProvider::normalizeStrictMessages(const QJsonArray& in) {
    QJsonArray out;
    for (const QJsonValue& v : in) {
        QJsonObject m = v.toObject();

        // Rule A — only the FIRST message may be `system`; any later
        // `system` turn is re-roled to `user` (text preserved verbatim).
        if (m.value(QStringLiteral("role")).toString() == QStringLiteral("system") &&
            !out.isEmpty()) {
            m[QStringLiteral("role")] = QStringLiteral("user");
        }

        // Rule C — `assistant` carrying `tool_calls` with empty-string
        // content → `content: null` (some strict validators reject "").
        if (m.value(QStringLiteral("role")).toString() == QStringLiteral("assistant") &&
            m.contains(QStringLiteral("tool_calls"))) {
            const QJsonValue c = m.value(QStringLiteral("content"));
            if (c.isString() && c.toString().isEmpty()) {
                m[QStringLiteral("content")] = QJsonValue(QJsonValue::Null);
            }
        }

        // Rule B — merge into the previous turn when both are the same
        // role and that role is user/system (after Rule A, system only
        // ever leads, so this mainly coalesces consecutive user turns).
        // `assistant` / `tool` turns are NEVER merged: they carry per-agent
        // identity (the "(X said)" prefix) and tool_calls / tool_call_id
        // pairing structure that strict templates depend on.
        if (!out.isEmpty()) {
            QJsonObject prev = out.last().toObject();
            const QString role = m.value(QStringLiteral("role")).toString();
            const QString prevRole = prev.value(QStringLiteral("role")).toString();
            const bool mergeable =
                role == prevRole &&
                (role == QStringLiteral("user") || role == QStringLiteral("system")) &&
                !m.contains(QStringLiteral("tool_calls")) &&
                !m.contains(QStringLiteral("tool_call_id")) &&
                !prev.contains(QStringLiteral("tool_calls")) &&
                !prev.contains(QStringLiteral("tool_call_id")) &&
                m.value(QStringLiteral("content")).isString() &&
                prev.value(QStringLiteral("content")).isString();
            if (mergeable) {
                prev[QStringLiteral("content")] = prev.value(QStringLiteral("content")).toString() +
                                                  QStringLiteral("\n\n") +
                                                  m.value(QStringLiteral("content")).toString();
                out.replace(out.size() - 1, prev);
                continue;
            }
        }
        out.append(m);
    }
    return out;
}

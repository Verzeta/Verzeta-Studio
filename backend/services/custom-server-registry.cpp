// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file custom-server-registry.cpp
 * @brief Implementation of the custom-OpenAI-API-compatible-server
 *        catalogue: hydrate from settings, register instances with
 *        ModelRouter, persist on mutation, fire model-list refreshes
 *        so the dropdown is always populated.
 * @layer Service
 * @dependencies OpenAICompatProvider, HttpClient, ModelRouter,
 *               SettingsService, Qt6::Core.
 */

#include "custom-server-registry.h"

#include "../api/openai-compat-provider.h"
#include "../utils/http-client.h"
#include "model-router.h"
#include "settings-service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QUrl>

Q_LOGGING_CATEGORY(verzetaCustomServers, "verzeta.custom-servers", QtInfoMsg)

namespace {

/**
 * @brief The persistence key constant. Keeps the magic string in
 *        one place and matches `SettingsService::customServersBlob`.
 */
constexpr auto kPersistKey = "custom_servers";

/**
 * @brief Slug regex per the contract documented on `setSlug`.
 *        Lowercase alphanumeric + hyphen, must start with a letter,
 *        length [2, 32].
 */
QRegularExpression slugPattern() {
    return QRegularExpression(QStringLiteral("^[a-z][a-z0-9-]{1,31}$"));
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CustomServerRegistry::CustomServerRegistry(ModelRouter& router,
                                           SettingsService& settings,
                                           QObject* parent)
    : QObject(parent), m_router(router), m_settings(settings) {
    hydrate();
}

CustomServerRegistry::~CustomServerRegistry() {
    // Unregister every provider from ModelRouter BEFORE the per-row
    // HttpClient unique_ptrs are destroyed.  Reverse-iteration order
    // protects against any internal Router state that walks
    // providers in registration order.
    for (auto it = m_rows.rbegin(); it != m_rows.rend(); ++it) {
        m_router.unregisterProvider(providerIdFor(it->slug));
    }
}

// ---------------------------------------------------------------------------
// Hydrate / persist
// ---------------------------------------------------------------------------

void CustomServerRegistry::hydrate() {
    const QString blob = m_settings.customServersBlob();
    if (blob.isEmpty()) {
        return;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        qCWarning(verzetaCustomServers)
            << "Failed to parse stored custom-servers blob:" << err.errorString();
        return;
    }
    const QJsonArray arr = doc.array();
    m_rows.reserve(static_cast<size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        InstanceRow row;
        row.slug = o.value(QStringLiteral("slug")).toString();
        row.displayName = o.value(QStringLiteral("displayName")).toString();
        row.baseUrl = o.value(QStringLiteral("baseUrl")).toString();
        row.requiresApiKeyFlag = o.value(QStringLiteral("requiresApiKeyFlag")).toBool(false);
        row.supportsStreaming = o.value(QStringLiteral("supportsStreaming")).toBool(true);
        row.supportsToolCalling = o.value(QStringLiteral("supportsToolCalling")).toBool(true);
        row.supportsVision = o.value(QStringLiteral("supportsVision")).toBool(false);
        if (!isValidSlug(row.slug) || row.baseUrl.isEmpty()) {
            qCWarning(verzetaCustomServers) << "Skipping invalid persisted row: slug='" << row.slug
                                            << "' baseUrl='" << row.baseUrl << "'";
            continue;
        }
        const QString apiKey = m_settings.apiKey(providerIdFor(row.slug));
        m_rows.push_back(std::move(row));
        InstanceRow& placed = m_rows.back();
        if (!buildAndRegister(placed, apiKey)) {
            m_rows.pop_back();
            continue;
        }
        // Hydration of a known-configured row triggers an immediate
        // refresh so the model dropdown is never empty by the time
        // the user opens it (the "model list never empty" rule).
        if (auto* instance = placed.instance.data()) {
            instance->refreshModels();
        }
    }
}

void CustomServerRegistry::persist() {
    QJsonArray arr;
    for (const InstanceRow& row : m_rows) {
        QJsonObject o;
        o.insert(QStringLiteral("slug"), row.slug);
        o.insert(QStringLiteral("displayName"), row.displayName);
        o.insert(QStringLiteral("baseUrl"), row.baseUrl);
        o.insert(QStringLiteral("requiresApiKeyFlag"), row.requiresApiKeyFlag);
        o.insert(QStringLiteral("supportsStreaming"), row.supportsStreaming);
        o.insert(QStringLiteral("supportsToolCalling"), row.supportsToolCalling);
        o.insert(QStringLiteral("supportsVision"), row.supportsVision);
        arr.append(o);
    }
    const QJsonDocument doc(arr);
    m_settings.setCustomServersBlob(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

// ---------------------------------------------------------------------------
// Slug helpers
// ---------------------------------------------------------------------------

QString CustomServerRegistry::providerIdFor(const QString& slug) const {
    return QStringLiteral("custom.") + slug;
}

bool CustomServerRegistry::isValidSlug(const QString& slug) {
    return slugPattern().match(slug).hasMatch();
}

QString CustomServerRegistry::generateSlug(const QString& fromDisplayName) const {
    // 1. lowercase + replace any non-[a-z0-9-] with -, collapse
    //    consecutive hyphens, trim leading/trailing hyphens.
    QString base = fromDisplayName.toLower();
    QString cleaned;
    cleaned.reserve(base.size());
    bool prevHyphen = false;
    for (QChar c : base) {
        if (c.isLetterOrNumber() && c.isLower()) {
            cleaned.append(c);
            prevHyphen = false;
        } else if (c.isDigit()) {
            cleaned.append(c);
            prevHyphen = false;
        } else {
            if (!prevHyphen && !cleaned.isEmpty()) {
                cleaned.append(QChar(u'-'));
                prevHyphen = true;
            }
        }
    }
    while (cleaned.endsWith(QChar(u'-'))) {
        cleaned.chop(1);
    }
    // 2. Ensure starts with a letter — otherwise prepend a fallback.
    while (!cleaned.isEmpty() && !cleaned.front().isLetter()) {
        cleaned.remove(0, 1);
    }
    // 3. Length clamp [2, 32]; if too short, fall back.
    if (cleaned.size() < 2) {
        cleaned = QStringLiteral("custom-server");
    }
    if (cleaned.size() > 32) {
        cleaned.resize(32);
    }
    // 4. Collision avoidance: append -2, -3, ... until unique.
    QString candidate = cleaned;
    int suffix = 2;
    while (rowBySlug(candidate) != nullptr) {
        candidate = cleaned + QStringLiteral("-") + QString::number(suffix);
        if (candidate.size() > 32) {
            // Shrink the base portion to keep within the cap.
            const int budget = 32 - QString::number(suffix).size() - 1;
            candidate = cleaned.left(budget) + QStringLiteral("-") + QString::number(suffix);
        }
        ++suffix;
        if (suffix > 9999) {
            // Defensive — practical install limit; refuse rather
            // than loop forever.
            return QString();
        }
    }
    return candidate;
}

CustomServerRegistry::InstanceRow* CustomServerRegistry::rowBySlug(const QString& slug) {
    for (InstanceRow& row : m_rows) {
        if (row.slug == slug) {
            return &row;
        }
    }
    return nullptr;
}

const CustomServerRegistry::InstanceRow*
CustomServerRegistry::rowBySlug(const QString& slug) const {
    for (const InstanceRow& row : m_rows) {
        if (row.slug == slug) {
            return &row;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Instance lifecycle
// ---------------------------------------------------------------------------

bool CustomServerRegistry::buildAndRegister(InstanceRow& row, const QString& apiKey) {
    row.http = std::make_unique<HttpClient>();
    auto provider = std::make_unique<OpenAICompatProvider>(*row.http);
    provider->setSlug(row.slug);
    provider->setDisplayName(row.displayName);
    provider->setBaseUrl(row.baseUrl);
    provider->setSupportsStreamingFlag(row.supportsStreaming);
    provider->setSupportsToolCallingFlag(row.supportsToolCalling);
    provider->setSupportsVisionFlag(row.supportsVision);
    provider->setRequiresApiKeyFlag(row.requiresApiKeyFlag);
    if (!apiKey.isEmpty()) {
        provider->setApiKey(apiKey);
    }
    row.instance = QPointer<OpenAICompatProvider>(provider.get());
    m_router.registerProvider(std::move(provider));
    // ModelRouter rejects duplicate ids by logging and discarding the
    // pointer; detect that via QPointer.
    if (!row.instance) {
        row.http.reset();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// QML reads
// ---------------------------------------------------------------------------

QVariantList CustomServerRegistry::list() const {
    QVariantList out;
    out.reserve(static_cast<int>(m_rows.size()));
    for (const InstanceRow& row : m_rows) {
        out.append(rowToVariantMap(row));
    }
    return out;
}

QVariantMap CustomServerRegistry::byslug(const QString& slug) const {
    const InstanceRow* row = rowBySlug(slug);
    if (!row) {
        return {};
    }
    return rowToVariantMap(*row);
}

QVariantMap CustomServerRegistry::rowToVariantMap(const InstanceRow& row) const {
    QVariantMap out;
    out.insert(QStringLiteral("slug"), row.slug);
    out.insert(QStringLiteral("displayName"), row.displayName);
    out.insert(QStringLiteral("baseUrl"), row.baseUrl);
    out.insert(QStringLiteral("requiresApiKeyFlag"), row.requiresApiKeyFlag);
    out.insert(QStringLiteral("hasApiKey"), m_settings.hasApiKey(providerIdFor(row.slug)));
    out.insert(QStringLiteral("supportsStreaming"), row.supportsStreaming);
    out.insert(QStringLiteral("supportsToolCalling"), row.supportsToolCalling);
    out.insert(QStringLiteral("supportsVision"), row.supportsVision);
    int modelCount = 0;
    if (auto* instance = row.instance.data()) {
        modelCount = instance->availableModels().size();
    }
    out.insert(QStringLiteral("modelCount"), modelCount);
    out.insert(QStringLiteral("lastRefreshStatus"), row.lastRefreshStatus);
    return out;
}

// ---------------------------------------------------------------------------
// QML writes
// ---------------------------------------------------------------------------

QString CustomServerRegistry::normalizeBaseUrl(const QString& raw) {
    const QUrl u(raw.trimmed());
    if (!u.isValid() || u.scheme().isEmpty() || u.host().isEmpty()) {
        // Not a parseable URL — fall back to a trimmed, trailing-slash-
        // stripped, lowercased compare so two identical raw strings still
        // dedup.
        QString s = raw.trimmed();
        while (s.endsWith(QLatin1Char('/')))
            s.chop(1);
        return s.toLower();
    }
    QString path = u.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    QString norm = u.scheme().toLower() + QStringLiteral("://") + u.host().toLower();
    if (u.port() != -1) {
        norm += QLatin1Char(':') + QString::number(u.port());
    }
    norm += path;  // path/query kept case-sensitive; servers may differ on it
    return norm;
}

QString CustomServerRegistry::slugForBaseUrl(const QString& baseUrl) const {
    const QString norm = normalizeBaseUrl(baseUrl);
    if (norm.isEmpty())
        return QString();
    for (const InstanceRow& row : m_rows) {
        if (normalizeBaseUrl(row.baseUrl) == norm)
            return row.slug;
    }
    return QString();
}

QString CustomServerRegistry::add(const QString& displayName,
                                  const QString& baseUrl,
                                  bool requiresApiKeyFlag,
                                  const QString& apiKey,
                                  bool supportsStreaming,
                                  bool supportsToolCalling,
                                  bool supportsVision) {
    if (displayName.trimmed().isEmpty()) {
        qCWarning(verzetaCustomServers) << "add: refusing empty displayName";
        return QString();
    }
    const QUrl probe(baseUrl);
    if (!probe.isValid() || probe.scheme().isEmpty() || probe.host().isEmpty()) {
        qCWarning(verzetaCustomServers) << "add: refusing invalid baseUrl:" << baseUrl;
        return QString();
    }
    // Refuse a duplicate base URL — silently creating a second provider for
    // the same endpoint (with a different display name -> different slug)
    // is the bug; the setup sheet checks slugForBaseUrl first to warn, this
    // guard backstops any other caller.
    const QString existingSlug = slugForBaseUrl(baseUrl);
    if (!existingSlug.isEmpty()) {
        qCWarning(verzetaCustomServers) << "add: refusing duplicate baseUrl" << baseUrl
                                        << "— already configured as" << existingSlug;
        return QString();
    }

    InstanceRow row;
    row.slug = generateSlug(displayName);
    if (row.slug.isEmpty()) {
        return QString();
    }
    row.displayName = displayName.trimmed();
    row.baseUrl = baseUrl.trimmed();
    row.requiresApiKeyFlag = requiresApiKeyFlag;
    row.supportsStreaming = supportsStreaming;
    row.supportsToolCalling = supportsToolCalling;
    row.supportsVision = supportsVision;

    m_rows.push_back(std::move(row));
    InstanceRow& placed = m_rows.back();
    if (!buildAndRegister(placed, apiKey)) {
        m_rows.pop_back();
        return QString();
    }
    // Persist the API key BEFORE persisting the catalogue so a
    // crash between the two does not leave a row with no key.
    if (!apiKey.isEmpty()) {
        m_settings.setApiKey(providerIdFor(placed.slug), apiKey);
    }
    persist();
    if (auto* instance = placed.instance.data()) {
        instance->refreshModels();
    }
    emit serversChanged();
    return placed.slug;
}

bool CustomServerRegistry::update(const QString& slug,
                                  const QString& displayName,
                                  const QString& baseUrl,
                                  bool requiresApiKeyFlag,
                                  const QString& apiKey,
                                  bool supportsStreaming,
                                  bool supportsToolCalling,
                                  bool supportsVision) {
    InstanceRow* row = rowBySlug(slug);
    if (!row) {
        return false;
    }
    const QUrl probe(baseUrl);
    if (!probe.isValid() || probe.scheme().isEmpty() || probe.host().isEmpty()) {
        qCWarning(verzetaCustomServers) << "update: refusing invalid baseUrl:" << baseUrl;
        return false;
    }
    // Refuse editing this server's URL to one that ANOTHER configured
    // server already uses (self is allowed — keeping its own URL).
    const QString dupSlug = slugForBaseUrl(baseUrl);
    if (!dupSlug.isEmpty() && dupSlug != slug) {
        qCWarning(verzetaCustomServers) << "update: refusing baseUrl collision with" << dupSlug;
        return false;
    }
    const bool urlChanged = (row->baseUrl != baseUrl);
    const QString priorApiKey = m_settings.apiKey(providerIdFor(slug));
    const bool keyChanged = (priorApiKey != apiKey);

    row->displayName = displayName.trimmed();
    row->baseUrl = baseUrl.trimmed();
    row->requiresApiKeyFlag = requiresApiKeyFlag;
    row->supportsStreaming = supportsStreaming;
    row->supportsToolCalling = supportsToolCalling;
    row->supportsVision = supportsVision;

    if (auto* instance = row->instance.data()) {
        instance->setDisplayName(row->displayName);
        instance->setBaseUrl(row->baseUrl);
        instance->setSupportsStreamingFlag(row->supportsStreaming);
        instance->setSupportsToolCallingFlag(row->supportsToolCalling);
        instance->setSupportsVisionFlag(row->supportsVision);
        instance->setRequiresApiKeyFlag(row->requiresApiKeyFlag);
        instance->setApiKey(apiKey);
    }
    if (keyChanged) {
        m_settings.setApiKey(providerIdFor(slug), apiKey);
    }
    persist();
    if (urlChanged || keyChanged) {
        if (auto* instance = row->instance.data()) {
            instance->refreshModels();
        }
    }
    emit serversChanged();
    return true;
}

bool CustomServerRegistry::remove(const QString& slug) {
    InstanceRow* row = rowBySlug(slug);
    if (!row) {
        return false;
    }
    const QString id = providerIdFor(slug);
    // 1. Unregister provider — destroys the OpenAICompatProvider
    //    instance via ModelRouter's unique_ptr.
    m_router.unregisterProvider(id);
    // 2. Clear the API key from the keychain.
    m_settings.setApiKey(id, QString());
    // 3. Erase row — destroys HttpClient AFTER provider is gone.
    const QString slugCopy = slug;
    m_rows.erase(std::remove_if(m_rows.begin(),
                                m_rows.end(),
                                [&slugCopy](const InstanceRow& r) { return r.slug == slugCopy; }),
                 m_rows.end());
    persist();
    emit serversChanged();
    return true;
}

void CustomServerRegistry::refreshModels(const QString& slug) {
    InstanceRow* row = rowBySlug(slug);
    if (!row) {
        return;
    }
    if (auto* instance = row->instance.data()) {
        instance->refreshModels();
    }
}

// ---------------------------------------------------------------------------
// testConnection
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief Composes `<base>/models` with no double slash regardless of
 *        whether base ends with `/`.
 */
QString modelsUrlFor(const QString& base) {
    if (base.endsWith(QChar(u'/'))) {
        return base + QStringLiteral("models");
    }
    return base + QStringLiteral("/models");
}

/**
 * @brief Parse OpenAI `/v1/models` response into a flat id list.
 *        Returns an empty list when the payload is not the expected
 *        `{"data":[{"id":...}]}` shape.
 */
QStringList parseModelIds(const QByteArray& body) {
    QStringList out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return out;
    }
    const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
    for (const QJsonValue& v : data) {
        const QString id = v.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            out.append(id);
        }
    }
    return out;
}

}  // namespace

void CustomServerRegistry::testConnection(const QString& correlationToken,
                                          const QString& baseUrl,
                                          bool requiresKey,
                                          const QString& apiKey) {
    // Fresh HttpClient parented to the registry so it is auto-cleaned
    // if the registry destructs before the response arrives.  The
    // client deletes itself via deleteLater() in both the success and
    // failure branches.
    auto* http = new HttpClient(this);
    const QString token = correlationToken;
    const bool keySent = !apiKey.isEmpty() || requiresKey;

    QObject::connect(
        http,
        &HttpClient::responseReceived,
        this,
        [this, http, token, keySent](int statusCode, const QByteArray& body) {
            QString verdictKey;
            QString human;
            QStringList models;
            bool ok = false;

            if (statusCode == 200) {
                models = parseModelIds(body);
                if (models.isEmpty()) {
                    verdictKey = QStringLiteral("no_models_loaded");
                    human = QStringLiteral("Reachable, but no models are loaded. Load a model "
                                           "on the server first, for example in LM Studio's Chat "
                                           "tab, or start vLLM with `--model <id>`.");
                } else {
                    ok = true;
                    verdictKey = QStringLiteral("ok");
                    QString preview = models.mid(0, 3).join(QStringLiteral(", "));
                    if (models.size() > 3) {
                        preview += QStringLiteral(" (+%1 more)").arg(models.size() - 3);
                    }
                    human =
                        QStringLiteral("OK. %1 model(s) found: %2").arg(models.size()).arg(preview);
                }
            } else if (statusCode == 401) {
                if (keySent) {
                    verdictKey = QStringLiteral("auth_rejected");
                    human = QStringLiteral(
                        "The server rejected the API key. Check that it is correct.");
                } else {
                    verdictKey = QStringLiteral("auth_required");
                    human = QStringLiteral("The server requires an API key. Turn on "
                                           "'Server requires an API key' and enter one.");
                }
            } else if (statusCode == 404) {
                verdictKey = QStringLiteral("path_not_found");
                human = QStringLiteral("Reachable, but /v1/models was not found. The base URL "
                                       "probably needs `/v1` at the end, for example "
                                       "`http://localhost:1234/v1` for LM Studio.");
            } else if (statusCode >= 500) {
                verdictKey = QStringLiteral("server_error");
                human = QStringLiteral("The server is reachable but returned an error (HTTP %1). "
                                       "Check the server logs.")
                            .arg(statusCode);
            } else {
                verdictKey = QStringLiteral("unexpected_status");
                human = QStringLiteral("Reachable, but returned an unexpected HTTP %1.")
                            .arg(statusCode);
            }
            emit connectionTestResult(token, ok, verdictKey, human, models);
            http->deleteLater();
        });

    QObject::connect(
        http,
        &HttpClient::errorOccurred,
        this,
        [this, http, token, keySent](const QString& message) {
            // HttpClient routes every 4xx / 5xx response through
            // errorOccurred with the format "HTTP <status> — <body>"
            // (or "HTTP <status>" when the body is empty).  Extract
            // the status and reuse the same verdict classifier the
            // 200-path uses.  When the message has no HTTP-status
            // prefix it is a real transport failure (TCP refused,
            // DNS, timeout) — the unreachable verdict.
            static const QRegularExpression kStatusRe(QStringLiteral("^HTTP\\s+(\\d+)"));
            const QRegularExpressionMatch m = kStatusRe.match(message);
            QString verdictKey;
            QString human;
            if (!m.hasMatch()) {
                verdictKey = QStringLiteral("unreachable");
                human = QStringLiteral("Cannot reach the server. Check that the URL is correct "
                                       "and the server is running. (%1)")
                            .arg(message);
            } else {
                const int statusCode = m.captured(1).toInt();
                if (statusCode == 401) {
                    if (keySent) {
                        verdictKey = QStringLiteral("auth_rejected");
                        human = QStringLiteral(
                            "The server rejected the API key. Check that it is correct.");
                    } else {
                        verdictKey = QStringLiteral("auth_required");
                        human = QStringLiteral("The server requires an API key. Turn on "
                                               "'Server requires an API key' and enter one.");
                    }
                } else if (statusCode == 404) {
                    verdictKey = QStringLiteral("path_not_found");
                    human = QStringLiteral("Reachable, but /v1/models was not found. The base "
                                           "URL probably needs `/v1` at the end, for example "
                                           "`http://localhost:1234/v1` for LM Studio.");
                } else if (statusCode >= 500) {
                    verdictKey = QStringLiteral("server_error");
                    human =
                        QStringLiteral("The server is reachable but returned an error (HTTP %1). "
                                       "Check the server logs.")
                            .arg(statusCode);
                } else {
                    verdictKey = QStringLiteral("unexpected_status");
                    human = QStringLiteral("Reachable, but returned an unexpected HTTP %1.")
                                .arg(statusCode);
                }
            }
            emit connectionTestResult(token, false, verdictKey, human, QStringList());
            http->deleteLater();
        });

    QMap<QString, QString> headers;
    if (!apiKey.isEmpty()) {
        headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer ") + apiKey);
    }
    http->get(QUrl(modelsUrlFor(baseUrl)), headers);
}

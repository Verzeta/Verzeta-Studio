// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file image-provider-registry.cpp
 * @brief Implementation of the image-generation-provider catalogue:
 *        hydrate from settings (or migrate from the legacy image-gen
 *        settings on first run), upsert / remove / list, track the
 *        active provider, and resolve the active typed config.
 * @layer Service
 * @dependencies SettingsService, Qt6::Core.
 */

#include "image-provider-registry.h"

#include "../utils/thread-discipline.h"
#include "settings-service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QUrl>

Q_LOGGING_CATEGORY(verzetaImageProviders, "verzeta.image-providers", QtInfoMsg)

namespace {

/**
 * @brief Slug regex: lowercase alphanumeric + hyphen, must start with a
 *        letter, length [2, 32].
 */
QRegularExpression slugPattern() {
    return QRegularExpression(QStringLiteral("^[a-z][a-z0-9-]{1,31}$"));
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ImageProviderRegistry::ImageProviderRegistry(SettingsService& settings, QObject* parent)
    : QObject(parent), m_settings(settings) {
    hydrate();
}

// ---------------------------------------------------------------------------
// Hydrate / persist
// ---------------------------------------------------------------------------

void ImageProviderRegistry::hydrate() {
    const QString blob = m_settings.imageProvidersBlob();
    if (blob.isEmpty()) {
        // First run after this upgrade — seed from the legacy per-backend
        // image-gen settings so no existing configuration is lost.
        migrateFromLegacySettings();
        return;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        qCWarning(verzetaImageProviders)
            << "Failed to parse stored image-providers blob:" << err.errorString();
        return;
    }
    const QJsonArray arr = doc.array();
    m_rows.reserve(static_cast<size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        ProviderRow row;
        row.id = o.value(QStringLiteral("id")).toString();
        row.displayName = o.value(QStringLiteral("displayName")).toString();
        row.endpointShape = o.value(QStringLiteral("endpointShape")).toString();
        row.baseUrl = o.value(QStringLiteral("baseUrl")).toString();
        row.model = o.value(QStringLiteral("model")).toString();
        row.requiresApiKey = o.value(QStringLiteral("requiresApiKey")).toBool(false);
        row.authHeaderName =
            o.value(QStringLiteral("authHeaderName")).toString(QStringLiteral("Authorization"));
        row.authValuePrefix =
            o.value(QStringLiteral("authValuePrefix")).toString(QStringLiteral("Bearer "));
        row.authQueryParam = o.value(QStringLiteral("authQueryParam")).toString();
        row.authBodyField =
            o.value(QStringLiteral("authBodyField")).toString(QStringLiteral("api_key"));
        // Migrate rows persisted before authLocation existed: a non-empty
        // query param means query auth, otherwise header auth.
        row.authLocation = o.value(QStringLiteral("authLocation"))
                               .toString(row.authQueryParam.isEmpty() ? QStringLiteral("header")
                                                                      : QStringLiteral("query"));
        row.sdPath = o.value(QStringLiteral("sdPath")).toString();
        row.size = o.value(QStringLiteral("size")).toString(QStringLiteral("1024x1024"));
        row.quality = o.value(QStringLiteral("quality")).toString(QStringLiteral("standard"));
        // Default image-only for rows persisted before this field existed,
        // so an existing chat-image provider starts working without the
        // user re-saving it.
        row.outputModalities =
            o.value(QStringLiteral("outputModalities")).toString(QStringLiteral("image"));
        row.extraParams = o.value(QStringLiteral("extraParams")).toObject();
        if (!isValidSlug(row.id) || !isValidEndpointShape(row.endpointShape)) {
            qCWarning(verzetaImageProviders) << "Skipping invalid persisted row: id='" << row.id
                                             << "' endpointShape='" << row.endpointShape << "'";
            continue;
        }
        m_rows.push_back(std::move(row));
    }
}

void ImageProviderRegistry::persist() {
    QJsonArray arr;
    for (const ProviderRow& row : m_rows) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), row.id);
        o.insert(QStringLiteral("displayName"), row.displayName);
        o.insert(QStringLiteral("endpointShape"), row.endpointShape);
        o.insert(QStringLiteral("baseUrl"), row.baseUrl);
        o.insert(QStringLiteral("model"), row.model);
        o.insert(QStringLiteral("requiresApiKey"), row.requiresApiKey);
        o.insert(QStringLiteral("authLocation"), row.authLocation);
        o.insert(QStringLiteral("authHeaderName"), row.authHeaderName);
        o.insert(QStringLiteral("authValuePrefix"), row.authValuePrefix);
        o.insert(QStringLiteral("authQueryParam"), row.authQueryParam);
        o.insert(QStringLiteral("authBodyField"), row.authBodyField);
        o.insert(QStringLiteral("sdPath"), row.sdPath);
        o.insert(QStringLiteral("size"), row.size);
        o.insert(QStringLiteral("quality"), row.quality);
        o.insert(QStringLiteral("outputModalities"), row.outputModalities);
        o.insert(QStringLiteral("extraParams"), row.extraParams);
        arr.append(o);
    }
    const QJsonDocument doc(arr);
    m_settings.setImageProvidersBlob(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

// ---------------------------------------------------------------------------
// Legacy-settings migration (first run only — blob empty)
// ---------------------------------------------------------------------------

void ImageProviderRegistry::migrateFromLegacySettings() {
    // Seed rows from the existing per-backend image-gen settings ONLY
    // when the blob is empty.  Each seeded row copies its key (if any)
    // into the new keychain id `image.<id>`.
    const QString openAiKey = m_settings.apiKey(QStringLiteral("openai"));
    if (!openAiKey.isEmpty()) {
        ProviderRow row;
        row.id = QStringLiteral("openai-dalle");
        row.displayName = QStringLiteral("OpenAI DALL·E");
        row.endpointShape = QStringLiteral("openai_images");
        row.baseUrl = QStringLiteral("https://api.openai.com/v1");
        row.model = QStringLiteral("dall-e-3");
        row.requiresApiKey = true;
        m_rows.push_back(row);
        m_settings.setApiKey(keyIdFor(row.id), openAiKey);
    }

    const QString compatUrl = m_settings.imageGenOpenAICompatUrl();
    if (!compatUrl.isEmpty()) {
        ProviderRow row;
        row.id = QStringLiteral("openai-compatible");
        row.displayName = QStringLiteral("OpenAI-compatible server");
        row.endpointShape = QStringLiteral("openai_images");
        row.baseUrl = compatUrl;
        m_rows.push_back(row);
        const QString key = m_settings.imageGenOpenAICompatKey();
        if (!key.isEmpty()) {
            m_settings.setApiKey(keyIdFor(row.id), key);
        }
    }

    const QString a1111Url = m_settings.imageGenA1111Url();
    if (!a1111Url.isEmpty()) {
        ProviderRow row;
        row.id = QStringLiteral("automatic1111");
        row.displayName = QStringLiteral("Automatic1111 / SD WebUI");
        row.endpointShape = QStringLiteral("a1111");
        row.baseUrl = a1111Url;
        m_rows.push_back(row);
        const QString key = m_settings.imageGenA1111Key();
        if (!key.isEmpty()) {
            m_settings.setApiKey(keyIdFor(row.id), key);
        }
    }

    const QString sdPath = m_settings.imageGenLocalSdPath();
    if (!sdPath.isEmpty()) {
        ProviderRow row;
        row.id = QStringLiteral("local-sd");
        row.displayName = QStringLiteral("Local Stable Diffusion");
        row.endpointShape = QStringLiteral("local_cli");
        row.sdPath = sdPath;
        m_rows.push_back(row);
    }

    if (m_rows.empty()) {
        return;  // Nothing to migrate — leave the blob untouched.
    }

    // Map the OLD active-backend value to the NEW id and keep it active.
    const QString legacyActive = m_settings.imageGenActiveBackend();
    QString mappedActive;
    if (legacyActive == QStringLiteral("openai_dalle")) {
        mappedActive = QStringLiteral("openai-dalle");
    } else if (legacyActive == QStringLiteral("remote_openai_compat")) {
        mappedActive = QStringLiteral("openai-compatible");
    } else if (legacyActive == QStringLiteral("remote_a1111")) {
        mappedActive = QStringLiteral("automatic1111");
    } else if (legacyActive == QStringLiteral("local_cli")) {
        mappedActive = QStringLiteral("local-sd");
    }
    if (!mappedActive.isEmpty() && rowBySlug(mappedActive) != nullptr) {
        m_settings.setImageGenActiveBackend(mappedActive);
    }

    persist();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString ImageProviderRegistry::keyIdFor(const QString& id) const {
    return QStringLiteral("image.") + id;
}

bool ImageProviderRegistry::isValidEndpointShape(const QString& shape) {
    return shape == QStringLiteral("openai_images") || shape == QStringLiteral("a1111") ||
           shape == QStringLiteral("local_cli") || shape == QStringLiteral("openai_chat_image");
}

bool ImageProviderRegistry::isValidSlug(const QString& slug) {
    return slugPattern().match(slug).hasMatch();
}

QString ImageProviderRegistry::generateSlug(const QString& fromDisplayName) const {
    // 1. lowercase + replace any non-[a-z0-9] run with a single hyphen,
    //    trim trailing hyphens.
    QString cleaned;
    cleaned.reserve(fromDisplayName.size());
    bool prevHyphen = false;
    for (QChar c : fromDisplayName.toLower()) {
        if (c.isLetterOrNumber() && c.unicode() < 128) {
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
    // 2. Ensure starts with a letter.
    while (!cleaned.isEmpty() && !cleaned.front().isLetter()) {
        cleaned.remove(0, 1);
    }
    // 3. Length clamp [2, 32]; fall back when too short.
    if (cleaned.size() < 2) {
        cleaned = QStringLiteral("image-provider");
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
            const int budget = 32 - QString::number(suffix).size() - 1;
            candidate = cleaned.left(budget) + QStringLiteral("-") + QString::number(suffix);
        }
        ++suffix;
        if (suffix > 9999) {
            return QString();  // Defensive — refuse rather than loop forever.
        }
    }
    return candidate;
}

ImageProviderRegistry::ProviderRow* ImageProviderRegistry::rowBySlug(const QString& id) {
    for (ProviderRow& row : m_rows) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

const ImageProviderRegistry::ProviderRow*
ImageProviderRegistry::rowBySlug(const QString& id) const {
    for (const ProviderRow& row : m_rows) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

QString ImageProviderRegistry::normalizeBaseUrl(const QString& raw) {
    const QUrl u(raw.trimmed());
    if (!u.isValid() || u.scheme().isEmpty() || u.host().isEmpty()) {
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
    norm += path;
    return norm;
}

// ---------------------------------------------------------------------------
// QML reads
// ---------------------------------------------------------------------------

QVariantList ImageProviderRegistry::list() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    out.reserve(static_cast<int>(m_rows.size()));
    for (const ProviderRow& row : m_rows) {
        out.append(rowToVariantMap(row));
    }
    return out;
}

QVariantMap ImageProviderRegistry::byId(const QString& id) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const ProviderRow* row = rowBySlug(id);
    if (!row) {
        return {};
    }
    return rowToVariantMap(*row);
}

QString ImageProviderRegistry::slugForBaseUrl(const QString& baseUrl) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString norm = normalizeBaseUrl(baseUrl);
    if (norm.isEmpty())
        return QString();
    for (const ProviderRow& row : m_rows) {
        if (!row.baseUrl.isEmpty() && normalizeBaseUrl(row.baseUrl) == norm) {
            return row.id;
        }
    }
    return QString();
}

QVariantMap ImageProviderRegistry::rowToVariantMap(const ProviderRow& row) const {
    QVariantMap out;
    out.insert(QStringLiteral("id"), row.id);
    out.insert(QStringLiteral("displayName"), row.displayName);
    out.insert(QStringLiteral("endpointShape"), row.endpointShape);
    out.insert(QStringLiteral("baseUrl"), row.baseUrl);
    out.insert(QStringLiteral("model"), row.model);
    out.insert(QStringLiteral("requiresApiKey"), row.requiresApiKey);
    out.insert(QStringLiteral("hasApiKey"), m_settings.hasApiKey(keyIdFor(row.id)));
    out.insert(QStringLiteral("authLocation"), row.authLocation);
    out.insert(QStringLiteral("authHeaderName"), row.authHeaderName);
    out.insert(QStringLiteral("authValuePrefix"), row.authValuePrefix);
    out.insert(QStringLiteral("authQueryParam"), row.authQueryParam);
    out.insert(QStringLiteral("authBodyField"), row.authBodyField);
    out.insert(QStringLiteral("sdPath"), row.sdPath);
    out.insert(QStringLiteral("size"), row.size);
    out.insert(QStringLiteral("quality"), row.quality);
    out.insert(QStringLiteral("outputModalities"), row.outputModalities);
    out.insert(QStringLiteral("extraParams"), row.extraParams.toVariantMap());
    return out;
}

// ---------------------------------------------------------------------------
// QML writes
// ---------------------------------------------------------------------------

QString ImageProviderRegistry::upsert(const QVariantMap& cfg) {
    VERZETA_ASSERT_MAIN_THREAD();

    const QString displayName = cfg.value(QStringLiteral("displayName")).toString().trimmed();
    if (displayName.isEmpty()) {
        qCWarning(verzetaImageProviders) << "upsert: refusing empty displayName";
        return QString();
    }
    const QString endpointShape = cfg.value(QStringLiteral("endpointShape")).toString();
    if (!isValidEndpointShape(endpointShape)) {
        qCWarning(verzetaImageProviders)
            << "upsert: refusing invalid endpointShape:" << endpointShape;
        return QString();
    }

    const QString requestedId = cfg.value(QStringLiteral("id")).toString();
    const bool isUpdate = !requestedId.isEmpty() && rowBySlug(requestedId) != nullptr;

    QString id;
    ProviderRow* row = nullptr;
    if (isUpdate) {
        id = requestedId;
        row = rowBySlug(id);
    } else {
        id = generateSlug(displayName);
        if (id.isEmpty()) {
            qCWarning(verzetaImageProviders) << "upsert: could not generate a unique id";
            return QString();
        }
        m_rows.push_back(ProviderRow{});
        row = &m_rows.back();
        row->id = id;
    }

    // Apply recognised fields, defaulting from the row's current value so
    // an UPDATE that omits a key keeps the prior value.
    auto str = [&cfg](const char* key, const QString& fallback) {
        const QString k = QString::fromLatin1(key);
        return cfg.contains(k) ? cfg.value(k).toString() : fallback;
    };
    row->displayName = displayName;
    row->endpointShape = endpointShape;
    row->baseUrl = str("baseUrl", row->baseUrl).trimmed();
    row->model = str("model", row->model);
    row->requiresApiKey = cfg.contains(QStringLiteral("requiresApiKey"))
                              ? cfg.value(QStringLiteral("requiresApiKey")).toBool()
                              : row->requiresApiKey;
    row->authLocation = str("authLocation", row->authLocation);
    row->authHeaderName = str("authHeaderName", row->authHeaderName);
    row->authValuePrefix = str("authValuePrefix", row->authValuePrefix);
    row->authQueryParam = str("authQueryParam", row->authQueryParam);
    row->authBodyField = str("authBodyField", row->authBodyField);
    row->sdPath = str("sdPath", row->sdPath);
    row->size = str("size", row->size);
    row->quality = str("quality", row->quality);
    row->outputModalities = str("outputModalities", row->outputModalities);
    if (cfg.contains(QStringLiteral("extraParams"))) {
        row->extraParams =
            QJsonObject::fromVariantMap(cfg.value(QStringLiteral("extraParams")).toMap());
    }

    // API key — keychain only.  Absent on update => leave untouched;
    // present (even empty) => write/clear.
    if (cfg.contains(QStringLiteral("apiKey"))) {
        m_settings.setApiKey(keyIdFor(id), cfg.value(QStringLiteral("apiKey")).toString());
    }

    persist();
    emit providersChanged();
    return id;
}

bool ImageProviderRegistry::remove(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    ProviderRow* row = rowBySlug(id);
    if (!row) {
        return false;
    }
    // Clear the keychain key for this provider.
    m_settings.setApiKey(keyIdFor(id), QString());

    const bool wasActive = (activeProviderId() == id);

    const QString idCopy = id;
    m_rows.erase(std::remove_if(m_rows.begin(),
                                m_rows.end(),
                                [&idCopy](const ProviderRow& r) { return r.id == idCopy; }),
                 m_rows.end());

    if (wasActive) {
        // Clear via the shared setting; emit the dedicated signal below.
        // Use a non-null empty string — a null QString() binds as SQL
        // NULL and the settings.value column is NOT NULL.
        m_settings.setImageGenActiveBackend(QString::fromLatin1(""));
    }
    persist();
    emit providersChanged();
    if (wasActive) {
        emit activeProviderChanged();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Active selection
// ---------------------------------------------------------------------------

QString ImageProviderRegistry::activeProviderId() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_settings.imageGenActiveBackend();
}

void ImageProviderRegistry::setActiveProviderId(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_settings.imageGenActiveBackend() == id) {
        return;
    }
    // A null QString() binds as SQL NULL and the settings.value column is
    // NOT NULL — coerce an empty/null id to a non-null empty string so a
    // "clear the selection" call persists cleanly.
    m_settings.setImageGenActiveBackend(id.isEmpty() ? QString::fromLatin1("") : id);
    emit activeProviderChanged();
}

ActiveImageConfig ImageProviderRegistry::activeConfig() const {
    VERZETA_ASSERT_MAIN_THREAD();
    ActiveImageConfig cfg;
    const QString id = m_settings.imageGenActiveBackend();
    if (id.isEmpty()) {
        return cfg;
    }
    const ProviderRow* row = rowBySlug(id);
    if (!row) {
        return cfg;
    }
    cfg.id = row->id;
    cfg.displayName = row->displayName;
    cfg.endpointShape = row->endpointShape;
    cfg.baseUrl = row->baseUrl;
    cfg.model = row->model;
    cfg.apiKey = m_settings.apiKey(keyIdFor(row->id));
    cfg.authLocation = row->authLocation;
    cfg.authHeaderName = row->authHeaderName;
    cfg.authValuePrefix = row->authValuePrefix;
    cfg.authQueryParam = row->authQueryParam;
    cfg.authBodyField = row->authBodyField;
    cfg.sdPath = row->sdPath;
    cfg.size = row->size;
    cfg.quality = row->quality;
    cfg.outputModalities = row->outputModalities;
    cfg.extraParams = row->extraParams;
    cfg.valid = true;
    return cfg;
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file image-provider-registry.h
 * @brief Owns the runtime catalogue of user-configured image-generation
 *        providers (OpenAI Images / DALL·E, OpenAI-compatible image
 *        servers such as SD.Next, Forge in OpenAI mode and ComfyUI proxies,
 *        Automatic1111 / SD WebUI, local Stable-Diffusion CLIs, and
 *        chat-style image models) under a single fully-customisable,
 *        persisted registry.
 *
 *        Responsibilities:
 *          - Hydrate at construction from
 *            `SettingsService::imageProvidersBlob` (JSON array under the
 *            settings key `image_providers`).  When that blob is empty,
 *            seed rows from the older per-backend image-gen settings so
 *            no existing configuration is lost (see
 *            `migrateFromLegacySettings`).
 *          - Expose upsert / remove / list / byId Q_INVOKABLE methods to
 *            QML for the image-provider setup UI.
 *          - Track the active provider id, stored through the EXISTING
 *            `SettingsService::imageGenActiveBackend()` accessor so the
 *            selection survives across restarts without a new settings
 *            key.
 *          - On every mutation: persist the catalogue back to settings
 *            and emit `providersChanged()`.
 *          - Provide a typed, non-QML `activeConfig()` getter that
 *            ImageService uses to dispatch generation.
 *
 *        Persistence shape: a JSON array, one object per provider. The
 *        API key is never embedded; it is stored separately through
 *        `SettingsService::setApiKey("image.\<id\>", key)`:
 *        ```
 *        [
 *          {
 *            "id":              "openai-dalle",
 *            "displayName":     "OpenAI DALL·E",
 *            "endpointShape":   "openai_images",
 *            "baseUrl":         "https://api.openai.com/v1",
 *            "model":           "dall-e-3",
 *            "requiresApiKey":  true,
 *            "authHeaderName":  "Authorization",
 *            "authValuePrefix": "Bearer ",
 *            "authQueryParam":  "",
 *            "sdPath":          "",
 *            "size":            "1024x1024",
 *            "quality":         "standard",
 *            "extraParams":     {}
 *          },
 *          ...
 *        ]
 *        ```
 *
 *        Threading: strictly main-thread.  Every public method begins
 *        with `VERZETA_ASSERT_MAIN_THREAD()`.
 * @layer Service
 * @dependencies Qt6::Core; SettingsService (non-owning reference).
 */

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

class SettingsService;

/**
 * @brief Resolved, typed configuration for the active image-generation
 *        provider.
 *
 * Produced by `ImageProviderRegistry::activeConfig()`.  Unlike the
 * QML-facing `list()` / `byId()` maps, this struct DOES carry the
 * API key (read through SettingsService) so ImageService can dispatch
 * a generation request without re-reading settings.
 * `valid` is false when no provider is active or the active id resolves
 * to no row.
 */
struct ActiveImageConfig {
    QString id;           ///< Provider id (slug).
    QString displayName;  ///< Name shown in the UI.
    /// Request format: "openai_images", "a1111", "local_cli" or
    /// "openai_chat_image".
    QString endpointShape;
    QString baseUrl;           ///< Server base URL; unused by "local_cli".
    QString model;             ///< Model name sent with the request.
    QString apiKey;            ///< API key; empty for keyless servers.
    QString authLocation;      ///< "header" | "query" | "body".
    QString authHeaderName;    ///< Header name when authLocation == "header".
    QString authValuePrefix;   ///< Text put before the key, such as "Bearer ".
    QString authQueryParam;    ///< Query parameter when authLocation == "query".
    QString authBodyField;     ///< JSON key when authLocation == "body".
    QString sdPath;            ///< Executable path for "local_cli".
    QString size;              ///< Default image size, such as "1024x1024".
    QString quality;           ///< Default quality setting.
    QString outputModalities;  ///< "image" (image-only) | "image_text".
    QJsonObject extraParams;   ///< Extra provider-specific request parameters.
    bool valid = false;        ///< False when no provider is active or its id is unknown.
};

/**
 * @brief Catalogue + active-selection controller for user-configured
 *        image-generation providers.
 *
 * Constructed once at AppController init time.  Hydrates its catalogue
 * from `SettingsService::imageProvidersBlob` (or seeds from the legacy
 * image-gen settings when that blob is empty), then serves the QML
 * setup UI and ImageService.  All mutations go
 * through this class so persistence, the active-id selection, and signal
 * fan-out stay coherent.
 */
class ImageProviderRegistry : public QObject {
    Q_OBJECT
    /// The configured image providers, as a bindable property so QML
    /// refreshes automatically when the catalogue changes. Same shape as
    /// `list()`.
    Q_PROPERTY(QVariantList providers READ list NOTIFY providersChanged)
    /// The id of the currently-active image provider, persisted via the
    /// existing image-gen active-backend setting.
    Q_PROPERTY(QString activeProviderId READ activeProviderId WRITE setActiveProviderId NOTIFY
                   activeProviderChanged)

  public:
    /**
     * @brief Constructs the registry and hydrates from persistence.
     * @param settings Non-owning reference to SettingsService for
     *                 catalogue blob persistence, the active-id setting,
     *                 and per-provider API key storage.
     * @param parent   Optional Qt parent.
     *
     * After construction the catalogue is fully hydrated.  When the
     * stored blob is empty, the legacy per-backend image-gen settings
     * are migrated into rows (first run after upgrade).
     */
    explicit ImageProviderRegistry(SettingsService& settings, QObject* parent = nullptr);

    // -----------------------------------------------------------------------
    // QML reads
    // -----------------------------------------------------------------------

    /**
     * @brief Lists every configured image provider.
     * @returns QVariantList of maps with keys: id, displayName,
     *          endpointShape, baseUrl, model, requiresApiKey, hasApiKey,
     *          authHeaderName, authValuePrefix, authQueryParam, sdPath,
     *          size, quality, extraParams.  API key values are NEVER
     *          returned; only the boolean `hasApiKey`.
     */
    Q_INVOKABLE QVariantList list() const;

    /**
     * @brief Returns one provider's metadata by id.
     * @param id Provider id returned by an earlier `upsert()` call or
     *           read from `list()`.
     * @returns Same shape as a single entry in `list()`; empty map when
     *          the id is unknown.
     */
    Q_INVOKABLE QVariantMap byId(const QString& id) const;

    /**
     * @brief Returns the id of an already-configured provider whose base
     *        URL matches @p baseUrl (normalised: trimmed, lowercase
     *        scheme+host, trailing slash dropped), or empty if none.
     * @param baseUrl Candidate base URL.
     * @returns Existing id for that URL, or empty QString.
     */
    Q_INVOKABLE QString slugForBaseUrl(const QString& baseUrl) const;

    // -----------------------------------------------------------------------
    // QML writes
    // -----------------------------------------------------------------------

    /**
     * @brief Adds a new provider or updates an existing one in place.
     * @param cfg Provider fields as a map.  Recognised keys: id,
     *            displayName, endpointShape, baseUrl, model,
     *            requiresApiKey, authHeaderName, authValuePrefix,
     *            authQueryParam, sdPath, size, quality, extraParams, and
     *            the write-only "apiKey".
     *
     *            When `id` is empty a new row is ADDED and an id slug is
     *            generated from `displayName`.  When `id` matches an
     *            existing row that row is UPDATED in place.
     *
     *            The "apiKey" value is stored through SettingsService,
     *            never in the blob.  On update: if "apiKey" is ABSENT from the map the
     *            existing key is left untouched; if present and empty the
     *            key is cleared.
     * @returns The provider id (slug) on success; empty string on
     *          failure (empty displayName, or an unrecognised
     *          endpointShape).
     */
    Q_INVOKABLE QString upsert(const QVariantMap& cfg);

    /**
     * @brief Removes a configured provider.
     * @param id Existing provider id.
     * @returns true on success; false when the id is unknown.
     *
     * Clears the provider's stored API key, drops the row, clears the
     * active selection when it pointed at this provider, persists, and
     * emits `providersChanged` (plus `activeProviderChanged` when the
     * active selection was cleared).
     */
    Q_INVOKABLE bool remove(const QString& id);

    /**
     * @brief Returns the active provider id.
     * @returns The persisted active id (via the image-gen active-backend
     *          setting), or empty when no provider is active.
     */
    QString activeProviderId() const;

    /**
     * @brief Sets the active provider id.
     * @param id Provider id to activate, or empty to clear the
     *           selection.  Stored via the existing image-gen
     *           active-backend setting.
     *
     * Emits `activeProviderChanged` when the value changes.
     */
    void setActiveProviderId(const QString& id);

    /**
     * @brief Resolves the active provider into a typed config with its
     *        decrypted API key.
     * @returns A populated ActiveImageConfig with `valid == true` when a
     *          provider is active and resolves to a row; otherwise a
     *          default-constructed config with `valid == false`.
     */
    ActiveImageConfig activeConfig() const;

  signals:
    /**
     * @brief Emitted after every upsert / remove that lands.  QML
     *        bindings on `list()` recompute on receipt.
     */
    void providersChanged();

    /**
     * @brief Emitted whenever the active provider id changes.
     */
    void activeProviderChanged();

  private:
    /**
     * @brief Per-provider persisted row. The API key is NOT stored here;
     *        SettingsService stores it under the id `image.\<id\>`
     *        through CryptoUtils, obfuscated rather than encrypted.
     */
    struct ProviderRow {
        QString id;
        QString displayName;
        QString endpointShape;
        QString baseUrl;
        QString model;
        bool requiresApiKey = false;
        // Where the credential is placed on the request:
        //   "header" — authHeaderName: authValuePrefix + key (default)
        //   "query"  — ?authQueryParam=key
        //   "body"   — {authBodyField: key} merged into the JSON payload
        // Migrated from the legacy fields on hydrate (a non-empty
        // authQueryParam → "query", otherwise "header").
        QString authLocation = QStringLiteral("header");
        QString authHeaderName = QStringLiteral("Authorization");
        QString authValuePrefix = QStringLiteral("Bearer ");
        QString authQueryParam;
        QString authBodyField = QStringLiteral("api_key");
        QString sdPath;
        QString size = QStringLiteral("1024x1024");
        QString quality = QStringLiteral("standard");
        // Output modalities requested from chat-image providers. Default
        // "image" (image-only) — the compatible choice for dedicated
        // image-generation models (Flux, grok-imagine, …). "image_text"
        // is for dual-output models (e.g. Gemini) that also return text.
        // Hardcoding "image,text" previously broke every image-only model
        // with OpenRouter's "No endpoints found that support the requested
        // output modalities: image, text".
        QString outputModalities = QStringLiteral("image");
        QJsonObject extraParams;
    };

    SettingsService& m_settings;
    std::vector<ProviderRow> m_rows;

    void hydrate();
    void persist();
    void migrateFromLegacySettings();
    /// API key storage id for a row: `image.<id>`.
    QString keyIdFor(const QString& id) const;
    /// Reports whether @p shape is one of the four recognised shapes.
    static bool isValidEndpointShape(const QString& shape);
    QString generateSlug(const QString& fromDisplayName) const;
    static bool isValidSlug(const QString& slug);
    /// Canonical form of a base URL for duplicate detection: trimmed,
    /// lowercase scheme + host, trailing slash dropped (path/port kept).
    static QString normalizeBaseUrl(const QString& raw);
    ProviderRow* rowBySlug(const QString& id);
    const ProviderRow* rowBySlug(const QString& id) const;
    QVariantMap rowToVariantMap(const ProviderRow& row) const;
};

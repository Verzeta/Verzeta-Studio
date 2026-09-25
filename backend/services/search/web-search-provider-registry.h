// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file web-search-provider-registry.h
 * @brief QML/wire-facing controller for web-search backend selection.
 *        Reads the active provider + per-provider key/base-URL from
 *        SettingsService and pushes the resolved WebSearchConfig into
 *        WebSearchService. Single owner of the settings→config apply path
 *        (mirrors how ImageProviderRegistry drives image generation).
 * @layer Service (Search subsystem)
 * @dependencies WebSearchService, SettingsService.
 *
 * Main-thread only (Q_INVOKABLE surface for QML + the wire bridge). API
 * keys are HOST-ONLY: stored via SettingsService::setApiKey("search.\<id\>")
 * in the OS keychain; the remote wire surface exposes
 * list/active/set-active/status, never the raw key.
 */
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

class SettingsService;

namespace Search {

class WebSearchService;

/**
 * @brief Selection + credential controller for web-search backends.
 */
class WebSearchProviderRegistry : public QObject {
    Q_OBJECT
    /// Catalogue for QML; re-reads on providersChanged (active/key/url edits).
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
  public:
    /**
     * @brief Construct and push the persisted active config into the service.
     * @param service The WebSearchService whose active backend this drives.
     * @param settings Persistence for active id, per-provider base URL, and
     *                 (via the keychain) API keys.
     * @param parent Optional QObject parent.
     */
    WebSearchProviderRegistry(WebSearchService& service,
                              SettingsService& settings,
                              QObject* parent = nullptr);

    /**
     * @brief Catalogue of backends for the UI/wire.
     * @returns List of maps {id, displayName, requiresApiKey, hasKey,
     *          active, baseUrl}.
     */
    Q_INVOKABLE QVariantList providers() const;

    /** @brief Active backend id. @returns The persisted active provider id. */
    Q_INVOKABLE QString activeProviderId() const;

    /**
     * @brief Make a backend active and re-apply the config.
     * @param providerId Provider id to activate.
     */
    Q_INVOKABLE void setActiveProvider(const QString& providerId);

    /**
     * @brief Whether a host-side API key is stored for a provider.
     * @param providerId Provider id.
     * @returns true if a non-empty key is in the keychain.
     */
    Q_INVOKABLE bool hasApiKey(const QString& providerId) const;

    /**
     * @brief Store/replace a provider's API key (host-only) and re-apply.
     * @param providerId Provider id.
     * @param key API key (empty to clear).
     */
    Q_INVOKABLE void setApiKey(const QString& providerId, const QString& key);

    /**
     * @brief Get a provider's base-URL override.
     * @param providerId Provider id.
     * @returns The override, or empty for the built-in default.
     */
    Q_INVOKABLE QString baseUrl(const QString& providerId) const;

    /**
     * @brief Set a provider's base-URL override (e.g. self-hosted SearXNG)
     *        and re-apply.
     * @param providerId Provider id.
     * @param url Base URL (empty to clear).
     */
    Q_INVOKABLE void setBaseUrl(const QString& providerId, const QString& url);

    /**
     * @brief The Custom (JSON) provider's request/response-mapping config.
     * @returns A map (url/method/auth/queryParam/resultsPath/title…Key/…),
     *          empty when unconfigured. The API key is NOT included (it is
     *          host-only; query hasApiKey("custom") for its presence).
     */
    Q_INVOKABLE QVariantMap customConfig() const;

    /**
     * @brief Persist the Custom (JSON) provider config and re-apply.
     * @param config Map of the request/response-mapping fields.
     */
    Q_INVOKABLE void setCustomConfig(const QVariantMap& config);

    /**
     * @brief Run a live connectivity/credential test against a provider and
     *        report the result via testResult(). Non-blocking: the network
     *        round-trip runs on a worker thread (QtConcurrent); the result
     *        is delivered on the main thread.
     * @param providerId Provider to test (uses its stored key / base URL).
     */
    Q_INVOKABLE void testProvider(const QString& providerId);

    /**
     * @brief Re-read settings + keychain and push the resolved config into
     *        the WebSearchService. Called at construction and after every
     *        mutation; also safe for AppController to call at startup.
     */
    void applyActiveConfig();

  signals:
    /** @brief Emitted when the provider list / active selection changes. */
    void providersChanged();

    /**
     * @brief Result of a testProvider() run.
     * @param providerId The provider that was tested.
     * @param ok True iff the provider returned results.
     * @param message Human-readable outcome ("Connected, 3 result(s).", an
     *                HTTP/credential error, etc.) for display in the UI.
     */
    void testResult(const QString& providerId, bool ok, const QString& message);

  private:
    WebSearchService& m_service;
    SettingsService& m_settings;
};

}  // namespace Search

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file web-search-service.h
 * @brief Front door for web search. Selects the active backend, falls
 *        back on failure, and always returns a graceful, model-safe
 *        response. Backs the single `search_web` tool.
 * @layer Service (Search subsystem)
 * @dependencies iweb-search-provider.h, the concrete providers, Qt6::Core.
 *
 * Threading: owned by AppController on the main thread. The provider map
 * is built once in the constructor and never mutated, so it is read
 * lock-free. The active WebSearchConfig is mutable (the main thread sets
 * it from settings) and is therefore guarded by a QMutex. search(), which
 * runs on a QtConcurrent worker thread (the search_web tool is
 * worker-resident), copies the config under the lock, releases it, then
 * performs network I/O. No signals are emitted from search().
 */
#pragma once

#include "iweb-search-provider.h"
#include "web-search-types.h"

#include <map>
#include <memory>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <vector>

namespace Search {

/**
 * @brief Selects and runs the active web-search backend with fallback.
 */
class WebSearchService : public QObject {
    Q_OBJECT
  public:
    /**
     * @brief Construct with the built-in (no-key) providers registered and
     *        a default active configuration (ddg-html primary).
     * @param parent Optional QObject parent (AppController).
     */
    explicit WebSearchService(QObject* parent = nullptr);
    ~WebSearchService() override;

    /**
     * @brief Replace the active configuration (provider id, base url, key,
     *        limits). Called on the main thread from settings.
     * @param cfg The new resolved configuration.
     */
    void setActiveConfig(const WebSearchConfig& cfg);

    /**
     * @brief Snapshot the active configuration.
     * @returns A copy of the current WebSearchConfig.
     */
    WebSearchConfig activeConfig() const;

    /**
     * @brief List the registered backends in stable display order.
     * @returns Descriptors (id / displayName / requiresApiKey) for the
     *          selection UI + wire surface. Read-only; the provider set is
     *          fixed at construction.
     */
    QList<WebSearchProviderInfo> providers() const;

    /**
     * @brief Run ONE specific provider directly, with NO fallback. Used by
     *        the settings "Test" button to report that provider's true
     *        result (status + errorReason) rather than a fallback's.
     * @param cfg Config whose providerId selects the backend (apiKey /
     *            baseUrl / limits honoured).
     * @param query Test query.
     * @returns The provider's raw WebSearchResponse (Unavailable with an
     *          errorReason if the id is unknown). Safe on a worker thread.
     */
    WebSearchResponse probe(const WebSearchConfig& cfg, const QString& query) const;

    /**
     * @brief Run a web search through the active backend, falling back to
     *        the Instant Answer backend on empty/unavailable, and always
     *        returning a model-safe response.
     * @param query The search text.
     * @returns A fully-formed WebSearchResponse. Never throws; on failure
     *          `status` is NoResults/Unavailable and `note` instructs the
     *          model to proceed with its own knowledge / ask the user and
     *          not fabricate. Safe to call on a worker thread.
     */
    WebSearchResponse search(const QString& query) const;

  private:
    /**
     * @brief Look up a registered provider by id (non-owning).
     * @param id Provider id.
     * @returns Pointer to the provider, or nullptr if not registered.
     */
    const IWebSearchProvider* providerFor(const QString& id) const;

    std::map<QString, std::unique_ptr<IWebSearchProvider>> m_providers;
    std::vector<QString> m_order;  ///< Registration order for providers().
    mutable QMutex m_configMutex;
    WebSearchConfig m_config;  ///< Guarded by m_configMutex.
};

}  // namespace Search

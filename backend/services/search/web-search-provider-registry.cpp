// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file web-search-provider-registry.cpp
 * @brief Implementation of Search::WebSearchProviderRegistry.
 * @layer Service (Search subsystem)
 * @dependencies WebSearchService, SettingsService.
 */

#include "web-search-provider-registry.h"

#include "../settings-service.h"
#include "web-search-service.h"
#include "web-search-types.h"

#include <QtConcurrent>

#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>

namespace Search {

namespace {
// Keychain namespace for per-provider keys (host-only).
QString keyService(const QString& providerId) {
    return QStringLiteral("search.") + providerId;
}
const QString kCustomId = QStringLiteral("custom");
}  // namespace

WebSearchProviderRegistry::WebSearchProviderRegistry(WebSearchService& service,
                                                     SettingsService& settings,
                                                     QObject* parent)
    : QObject(parent), m_service(service), m_settings(settings) {
    applyActiveConfig();
}

QVariantMap WebSearchProviderRegistry::customConfig() const {
    const QString json = m_settings.webSearchCustomConfig();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object().toVariantMap();
}

void WebSearchProviderRegistry::setCustomConfig(const QVariantMap& config) {
    const QByteArray json =
        QJsonDocument(QJsonObject::fromVariantMap(config)).toJson(QJsonDocument::Compact);
    m_settings.setWebSearchCustomConfig(QString::fromUtf8(json));
    applyActiveConfig();
    emit providersChanged();
}

void WebSearchProviderRegistry::applyActiveConfig() {
    const QString id = m_settings.webSearchActiveProvider();
    WebSearchConfig cfg;
    cfg.providerId = id;
    cfg.apiKey = m_settings.apiKey(keyService(id));
    cfg.baseUrl = m_settings.webSearchBaseUrl(id);
    if (id == kCustomId)
        cfg.extra = customConfig();
    m_service.setActiveConfig(cfg);
}

QVariantList WebSearchProviderRegistry::providers() const {
    const QString active = activeProviderId();
    QVariantList out;
    const QList<WebSearchProviderInfo> infos = m_service.providers();
    for (const WebSearchProviderInfo& p : infos) {
        QVariantMap m;
        m[QStringLiteral("id")] = p.id;
        m[QStringLiteral("displayName")] = p.displayName;
        m[QStringLiteral("requiresApiKey")] = p.requiresApiKey;
        m[QStringLiteral("hasKey")] = m_settings.hasApiKey(keyService(p.id));
        m[QStringLiteral("active")] = (p.id == active);
        m[QStringLiteral("baseUrl")] = m_settings.webSearchBaseUrl(p.id);
        out.append(m);
    }
    return out;
}

QString WebSearchProviderRegistry::activeProviderId() const {
    return m_settings.webSearchActiveProvider();
}

void WebSearchProviderRegistry::setActiveProvider(const QString& providerId) {
    m_settings.setWebSearchActiveProvider(providerId);
    applyActiveConfig();
    emit providersChanged();
}

bool WebSearchProviderRegistry::hasApiKey(const QString& providerId) const {
    return m_settings.hasApiKey(keyService(providerId));
}

void WebSearchProviderRegistry::setApiKey(const QString& providerId, const QString& key) {
    // Trim — pasted keys routinely carry a trailing newline/space that
    // silently breaks auth. API keys never contain surrounding whitespace.
    m_settings.setApiKey(keyService(providerId), key.trimmed());
    applyActiveConfig();
    emit providersChanged();
}

QString WebSearchProviderRegistry::baseUrl(const QString& providerId) const {
    return m_settings.webSearchBaseUrl(providerId);
}

void WebSearchProviderRegistry::setBaseUrl(const QString& providerId, const QString& url) {
    // Trim — a stray space/newline in a base URL breaks the request.
    m_settings.setWebSearchBaseUrl(providerId, url.trimmed());
    applyActiveConfig();
    emit providersChanged();
}

void WebSearchProviderRegistry::testProvider(const QString& providerId) {
    // Build the provider's resolved config on the main thread (settings +
    // keychain reads), then run the network round-trip off-thread so the UI
    // never blocks. The provider's search() is worker-thread safe (local
    // QNAM per call). Result is delivered on the main thread via the watcher.
    WebSearchConfig cfg;
    cfg.providerId = providerId;
    cfg.apiKey = m_settings.apiKey(keyService(providerId));
    cfg.baseUrl = m_settings.webSearchBaseUrl(providerId);
    cfg.maxResults = 3;
    if (providerId == kCustomId)
        cfg.extra = customConfig();

    WebSearchService* svc = &m_service;
    const QString query = QStringLiteral("verzeta web search connectivity test");

    auto* watcher = new QFutureWatcher<WebSearchResponse>(this);
    connect(
        watcher, &QFutureWatcher<WebSearchResponse>::finished, this, [this, watcher, providerId]() {
            const WebSearchResponse r = watcher->result();
            watcher->deleteLater();
            bool ok = false;
            QString msg;
            switch (r.status) {
                case WebSearchStatus::Ok:
                    ok = true;
                    msg = tr("Connected, %1 result(s).").arg(r.results.size());
                    break;
                case WebSearchStatus::NoResults:
                    msg = tr("Reached the provider, but it returned no results "
                             "for the test query.");
                    break;
                case WebSearchStatus::Unavailable:
                    msg = r.errorReason.isEmpty() ? tr("Provider unavailable.") : r.errorReason;
                    break;
            }
            emit testResult(providerId, ok, msg);
        });
    watcher->setFuture(QtConcurrent::run([svc, cfg, query]() { return svc->probe(cfg, query); }));
}

}  // namespace Search

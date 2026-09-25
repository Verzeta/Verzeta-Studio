// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file web-search-service.cpp
 * @brief Implementation of Search::WebSearchService.
 * @layer Service (Search subsystem)
 * @dependencies the concrete providers, Qt6::Core, utils/logger.h.
 */

#include "web-search-service.h"

#include "../../utils/logger.h"
#include "custom-json-provider.h"
#include "ddg-html-provider.h"
#include "ddg-instant-answer-provider.h"
#include "exa-provider.h"
#include "langsearch-provider.h"
#include "searxng-provider.h"
#include "tavily-provider.h"

#include <QMutexLocker>
#include <QStringList>

namespace Search {

namespace {
const QString kDdgHtmlProviderId = QStringLiteral("ddg-html");
const QString kFallbackProviderId = QStringLiteral("ddg-instant");
}  // namespace

WebSearchService::WebSearchService(QObject* parent) : QObject(parent) {
    auto add = [this](std::unique_ptr<IWebSearchProvider> p) {
        const QString id = p->id();
        m_order.push_back(id);
        m_providers.emplace(id, std::move(p));
    };
    add(std::make_unique<DdgHtmlProvider>());
    add(std::make_unique<DdgInstantAnswerProvider>());
    // Keyed providers — usable once the user supplies a key (host-side).
    // Registered so they are selectable; search() returns Unavailable when
    // no key is configured.
    add(std::make_unique<TavilyProvider>());
    add(std::make_unique<ExaProvider>());
    add(std::make_unique<LangSearchProvider>());
    add(std::make_unique<SearxngProvider>());
    // User-configurable JSON endpoint (reads its request/response mapping
    // from WebSearchConfig::extra, populated by the registry).
    add(std::make_unique<CustomJsonProvider>());

    // Default active backend: real web results, no key.
    m_config.providerId = QStringLiteral("ddg-html");

    qCInfo(verzetaTools) << "WebSearchService initialized; default provider" << m_config.providerId;
}

WebSearchService::~WebSearchService() = default;

void WebSearchService::setActiveConfig(const WebSearchConfig& cfg) {
    QMutexLocker lock(&m_configMutex);
    m_config = cfg;
}

WebSearchConfig WebSearchService::activeConfig() const {
    QMutexLocker lock(&m_configMutex);
    return m_config;
}

const IWebSearchProvider* WebSearchService::providerFor(const QString& id) const {
    const auto it = m_providers.find(id);
    return it == m_providers.end() ? nullptr : it->second.get();
}

WebSearchResponse WebSearchService::probe(const WebSearchConfig& cfg, const QString& query) const {
    const IWebSearchProvider* p = providerFor(cfg.providerId);
    if (!p) {
        WebSearchResponse r;
        r.query = query;
        r.providerId = cfg.providerId;
        r.status = WebSearchStatus::Unavailable;
        r.errorReason = QStringLiteral("unknown provider id");
        return r;
    }
    const QString q = query.trimmed().isEmpty() ? QStringLiteral("verzeta web search test") : query;
    return p->search(cfg, q);
}

QList<WebSearchProviderInfo> WebSearchService::providers() const {
    QList<WebSearchProviderInfo> out;
    out.reserve(static_cast<int>(m_order.size()));
    for (const QString& id : m_order) {
        const auto it = m_providers.find(id);
        if (it == m_providers.end())
            continue;
        const IWebSearchProvider* p = it->second.get();
        out.append({p->id(), p->displayName(), p->requiresApiKey()});
    }
    return out;
}

WebSearchResponse WebSearchService::search(const QString& query) const {
    WebSearchResponse resp;
    resp.query = query;

    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("empty query");
        resp.note = QStringLiteral("No query was provided to the web search.");
        return resp;
    }

    // Snapshot config under the lock, then release before network I/O.
    WebSearchConfig cfg;
    {
        QMutexLocker lock(&m_configMutex);
        cfg = m_config;
    }

    const IWebSearchProvider* primary = providerFor(cfg.providerId);
    if (!primary) {
        // Misconfiguration: fall back to the first registered provider.
        primary = providerFor(QStringLiteral("ddg-html"));
    }

    if (primary) {
        resp = primary->search(cfg, trimmed);
    } else {
        resp.status = WebSearchStatus::Unavailable;
        resp.errorReason = QStringLiteral("no web-search provider registered");
    }

    // Fallback chain: if the primary produced nothing usable (no key /
    // rate-limited / unreachable), try the no-key DuckDuckGo backends in
    // order — real SERP scrape first, then Instant Answers — so a keyed
    // provider without a key still yields real results rather than nothing.
    // The fallbacks need no credentials, so the active provider's key /
    // base URL are cleared before delegating.
    if (resp.status != WebSearchStatus::Ok) {
        const QStringList fallbackChain{kDdgHtmlProviderId, kFallbackProviderId};
        for (const QString& fbId : fallbackChain) {
            if (fbId == cfg.providerId)
                continue;  // already tried as primary
            const IWebSearchProvider* fb = providerFor(fbId);
            if (!fb)
                continue;
            WebSearchConfig fbCfg = cfg;
            fbCfg.providerId = fbId;
            fbCfg.apiKey.clear();
            fbCfg.baseUrl.clear();
            const WebSearchResponse fbResp = fb->search(fbCfg, trimmed);
            if (fbResp.status == WebSearchStatus::Ok) {
                resp = fbResp;
                break;
            }
        }
    }

    // Graceful, model-safe framing. A failed/empty search must never read
    // as a hard error that makes the model abandon the task.
    switch (resp.status) {
        case WebSearchStatus::Ok:
            break;
        case WebSearchStatus::NoResults:
            resp.note =
                QStringLiteral("The web search ran but found no usable results. Proceed using "
                               "your own knowledge, or ask the user to narrow the query — do "
                               "NOT fabricate sources, links, or facts.");
            break;
        case WebSearchStatus::Unavailable:
            resp.note =
                QStringLiteral("Web search is temporarily unavailable. Proceed using your own "
                               "knowledge, or tell the user search could not be reached — do "
                               "NOT fabricate sources, links, or facts.");
            break;
    }

    qCDebug(verzetaTools).nospace()
        << "WebSearch: query=\"" << trimmed << "\" provider=" << resp.providerId
        << " status=" << static_cast<int>(resp.status) << " results=" << resp.results.size();

    return resp;
}

}  // namespace Search

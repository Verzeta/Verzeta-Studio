// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-service.h
 * @brief RAGP orchestrator. Combines Tier 1 (rule classifier),
 *        Tier 2 (cache), and Tier 3 (LLM backend) into a single
 *        classification pipeline.
 * @layer Service
 * @dependencies Qt6::Core
 *
 * Pipeline:
 *   1. Tier 1 (rules) classifies. If confidence == 1.0, return.
 *   2. Tier 2 (cache) is checked. If hit, return cached classification.
 *   3. Tier 3 (backend) is invoked. Result is cached and returned.
 *
 * The service emits the `userMentioned` signal when the classification
 * detects ESCALATION_TO_USER (the \@owner notification pathway).
 */

#pragma once

#include "iragp-backend.h"
#include "ragp-cache.h"
#include "ragp-types.h"

#include <memory>
#include <QFuture>
#include <QObject>

namespace Ragp {

/**
 * @brief RAGP three-tier pipeline orchestrator. Combines the rule
 *        classifier, the in-memory cache, and a swappable LLM
 *        backend into a single classify() entry point.
 */
class Service : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the service.
     * @param backend Owned Tier 3 backend. Must not be null;
     *                callers supply StubBackend / RemoteBackend /
     *                LocalLlamaBackend as appropriate.
     * @param parent  Qt parent.
     */
    explicit Service(std::unique_ptr<IRagpBackend> backend, QObject* parent = nullptr);

    ~Service() override;

    /**
     * @brief Classify an agent response through the full pipeline.
     * @param req Request envelope.
     * @returns A QFuture<Classification> that always resolves (never
     *          hangs). When Tier 1 or Tier 2 is conclusive, the
     *          future is already-ready on return. When Tier 3 is
     *          invoked, the future resolves when the backend call
     *          completes or times out.
     */
    QFuture<Classification> classify(const Request& req);

    /**
     * @brief Active backend name for logging.
     * @returns Canonical name of the current Tier 3 backend.
     */
    QString backendName() const;

    /**
     * @brief One-shot text completion on the configured backend's provider.
     *
     * Delegates to the active Tier-3 backend's oneShotCompleteAsync, i.e. uses the
     * SAME provider RAGP is configured with (internal GGUF or remote), with
     * no separate setting. For callers that need a single LLM answer (e.g.
     * confirming a file-creation intent) without @-mention classification.
     *
     * @param prompt Full user-role prompt.
     * @returns QFuture<QString> with the raw completion, empty on failure.
     */
    QFuture<QString> oneShotCompleteAsync(const QString& prompt);

    /**
     * @brief Clear the Tier 2 cache. For tests or manual override.
     */
    void clearCache();

    /**
     * @brief Replace the Tier 3 backend. Useful at runtime once a
     *        better backend becomes available (e.g. after
     *        SettingsService is wired and we can construct the
     *        remote backend). The previous backend is deleted. Also
     *        clears the cache because cached classifications from
     *        the old backend may have different semantics than the
     *        new one.
     * @param backend Owned replacement backend; must not be null.
     */
    void setBackend(std::unique_ptr<IRagpBackend> backend);

  private:
    std::unique_ptr<IRagpBackend> m_backend;
    Cache m_cache;
};

}  // namespace Ragp

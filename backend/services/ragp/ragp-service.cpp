// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-service.cpp
 * @brief RAGP orchestrator implementation. Drives the
 *        Tier 1 → Tier 2 → Tier 3 pipeline asynchronously.
 * @layer Service
 * @dependencies Qt6::Core
 */

#include "ragp-service.h"

#include "../../utils/logger.h"
#include "ragp-rule-classifier.h"

#include <memory>
#include <QDateTime>
#include <QFuture>
#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QPromise>

namespace Ragp {

Service::Service(std::unique_ptr<IRagpBackend> backend, QObject* parent)
    : QObject(parent), m_backend(std::move(backend)) {
    Q_ASSERT_X(m_backend != nullptr, "Ragp::Service", "backend must not be null");
}

Service::~Service() = default;

QFuture<Classification> Service::classify(const Request& req) {
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();

    // Tier 1: rule-based fast-path.
    Classification ruleResult = RuleClassifier::classify(req);

    auto stampLatency = [startMs](Classification c) -> Classification {
        c.latencyMs = QDateTime::currentMSecsSinceEpoch() - startMs;
        return c;
    };

    if (ruleResult.confidence >= 1.0 && !req.wantsPendingActionVerdict) {
        return QtFuture::makeReadyValueFuture(stampLatency(ruleResult));
    }

    // Tier 2: cache lookup. Bypassed for verdict-wanting requests —
    // a cached entry was produced under a DIFFERENT executed-tools
    // context, so its pending_action cannot be trusted.
    Classification cached;
    if (!req.wantsPendingActionVerdict && m_cache.lookup(req.content, req.rosterAliases, cached)) {
        qCDebug(verzetaUi) << "RAGP cache hit:"
                           << "targets=" << cached.targets.size();
        // Verdicts flow only when explicitly requested (and never from
        // the cache) — clear anything a stored entry carries.
        cached.selfPendingAction = false;
        cached.pendingActionHint.clear();
        cached.pendingDelegateAlias.clear();
        return QtFuture::makeReadyValueFuture(stampLatency(cached));
    }

    // Tier 3: backend (async). We chain a continuation that applies
    // the failure-fallback policy and caches successes.
    if (!m_backend->isAvailable()) {
        qCWarning(verzetaUi) << "RAGP: backend" << m_backend->backendName()
                             << "unavailable — returning Tier 1 result";
        return QtFuture::makeReadyValueFuture(stampLatency(ruleResult));
    }

    // Snapshot values needed by the continuation. Can't capture `this`
    // directly because Service may outlive the call but we want
    // weak-ref semantics (if Service dies, the continuation still
    // resolves — the promise is safe via shared_ptr).
    QFuture<Classification> backendFuture = m_backend->classifyAsync(req);

    auto promise = std::make_shared<QPromise<Classification>>();
    promise->start();

    const QString contentCopy = req.content;
    const QStringList rosterCopy = req.rosterAliases;
    const QString authorCopy = req.authorAlias;
    const bool wantsVerdict = req.wantsPendingActionVerdict;
    const QString backendName = m_backend->backendName();
    // Capture a raw pointer is safe because:
    //   - We only touch m_cache, which is a value member of Service
    //   - The continuation context is `this`, so if Service dies
    //     before the backend resolves, Qt disconnects the
    //     continuation → we never dereference `this`.
    //   - The shared_ptr<QPromise> is held by the continuation and
    //     outlives Service if needed; its destructor cancels the
    //     future if we never resolved it.
    Service* self = this;

    // Use QFutureWatcher for the continuation instead of .then() to
    // get explicit QObject-context-based disconnect on Service
    // destruction.
    auto* watcher = new QFutureWatcher<Classification>(this);

    QObject::connect(
        watcher,
        &QFutureWatcher<Classification>::finished,
        this,
        [self,
         watcher,
         promise,
         startMs,
         contentCopy,
         rosterCopy,
         authorCopy,
         backendName,
         ruleResult,
         wantsVerdict]() {
            watcher->deleteLater();

            Classification backendResult = watcher->result();

            const auto aliasMatches = [](const QString& a, const QString& b) {
                const QString na = QString(a).replace(QLatin1Char('_'), QLatin1Char(' ')).trimmed();
                const QString nb = QString(b).replace(QLatin1Char('_'), QLatin1Char(' ')).trimmed();
                return na.compare(nb, Qt::CaseInsensitive) == 0;
            };
            const int rawTargetCount = backendResult.targets.size();
            int droppedFabricated = 0;
            int droppedAuthor = 0;
            {
                QList<Target> sanitized;
                for (const Target& t : backendResult.targets) {
                    // The author never routes to themselves — dropped,
                    // but this is a MEANINGFUL answer shape (a model
                    // often names the author when the author kept the
                    // work: consistent with pending_action who=self),
                    // NOT evidence of garbage. Only aliases outside
                    // the roster count as fabrication.
                    if (aliasMatches(t.alias, authorCopy)) {
                        ++droppedAuthor;
                        continue;
                    }
                    bool inRoster = false;
                    for (const QString& r : rosterCopy) {
                        if (aliasMatches(t.alias, r)) {
                            inRoster = true;
                            break;
                        }
                    }
                    if (inRoster) {
                        sanitized.append(t);
                    } else {
                        ++droppedFabricated;
                    }
                }
                if (droppedFabricated > 0) {
                    qCWarning(verzetaUi) << "RAGP: dropped" << droppedFabricated
                                         << "backend target(s) naming aliases that do"
                                         << "not exist in this conversation's roster"
                                         << "(model echoed prompt-example names)";
                }
                if (droppedAuthor > 0) {
                    qCInfo(verzetaUi) << "RAGP: dropped" << droppedAuthor
                                      << "self-target(s) (the author never routes to"
                                      << "themselves; the rest of the answer is kept)";
                }
                backendResult.targets = sanitized;
            }

            // DELEGATE-ALIAS validation — the handoff arm gets the
            // same structural guards as targets: a fabricated alias
            // is cleared (never dispatched), and the AUTHOR named as
            // their own delegate is really a self-kept verdict (the
            // model was told who="self" for that shape) — convert
            // instead of discarding the information.
            if (!backendResult.pendingDelegateAlias.isEmpty()) {
                const QString& delegate = backendResult.pendingDelegateAlias;
                if (aliasMatches(delegate, authorCopy)) {
                    qCDebug(verzetaUi) << "RAGP: delegate verdict names the author (" << delegate
                                       << ") — treating as who=self";
                    backendResult.selfPendingAction = true;
                    backendResult.pendingDelegateAlias.clear();
                } else {
                    bool inRoster = false;
                    for (const QString& r : rosterCopy) {
                        if (aliasMatches(delegate, r)) {
                            inRoster = true;
                            break;
                        }
                    }
                    if (!inRoster) {
                        qCWarning(verzetaUi) << "RAGP: dropped delegate verdict naming" << delegate
                                             << "— alias does not exist in this"
                                             << "conversation's roster";
                        backendResult.pendingDelegateAlias.clear();
                    }
                }
            }

            const bool backendFailed =
                (backendResult.targets.isEmpty() && backendResult.confidence < 0.1) ||
                (droppedFabricated > 0 && backendResult.targets.isEmpty());

            const bool backendUnsure = backendResult.confidence < 0.5 &&
                                       (!ruleResult.targets.isEmpty() || ruleResult.routeToUser);

            Classification finalResult;
            if (backendFailed) {
                qCWarning(verzetaUi) << "RAGP: backend returned empty/failed result "
                                        "— falling back to Tier 1 rule targets ("
                                     << ruleResult.targets.size() << "alias(es))";
                finalResult = ruleResult;
                finalResult.source = QStringLiteral("rule-fallback-after:%1").arg(backendName);
                // TARGETS fall back to the rules; the pending-action
                // VERDICT does not exist anywhere else — when it was
                // explicitly requested, carry the backend's verdict
                // through the fallback instead of destroying it.
                // Discarding it froze a live chat ("I am calling
                // write_file now!" + one echoed example alias →
                // fabrication fallback → verdict gone → no nudge);
                // keeping it costs at most one budget-bounded nudge,
                // and dispatch-first means it only fires when the
                // rule targets dispatched nobody.
                if (wantsVerdict) {
                    finalResult.selfPendingAction = backendResult.selfPendingAction;
                    finalResult.pendingActionHint = backendResult.pendingActionHint;
                    finalResult.pendingDelegateAlias = backendResult.pendingDelegateAlias;
                }
            } else if (backendUnsure) {
                qCWarning(verzetaUi)
                    << "RAGP: backend unsure (confidence" << backendResult.confidence
                    << "< 0.5) while Tier-1 rules hold structural"
                    << "targets — preferring the rule result (" << ruleResult.targets.size()
                    << "alias(es))";
                finalResult = ruleResult;
                finalResult.source =
                    QStringLiteral("rule-preferred-after-unsure:%1").arg(backendName);
                // Not cached: the backend answer was unsure, and the
                // rule result is recomputed structurally each turn.
                // The requested verdict rides through here too (same
                // rationale as the failed branch above).
                if (wantsVerdict) {
                    finalResult.selfPendingAction = backendResult.selfPendingAction;
                    finalResult.pendingActionHint = backendResult.pendingActionHint;
                    finalResult.pendingDelegateAlias = backendResult.pendingDelegateAlias;
                }
            } else {
                if (self) {
                    // Cached entries never carry a pending-action
                    // verdict (context-dependent — see the Tier-2
                    // bypass above).
                    Classification toCache = backendResult;
                    toCache.selfPendingAction = false;
                    toCache.pendingActionHint.clear();
                    toCache.pendingDelegateAlias.clear();
                    self->m_cache.store(contentCopy, rosterCopy, toCache);
                }
                finalResult = backendResult;
                if (finalResult.source.isEmpty()) {
                    finalResult.source = QStringLiteral("backend:%1").arg(backendName);
                }
                // Tier-3 (LLM) backends never set routeToUser — they
                // classify teammate-routing only. Tier-1's @user / @owner
                // detection is structural and must NOT be overwritten by
                // a Tier-3 result that doesn't carry the flag.
                if (ruleResult.routeToUser && !finalResult.routeToUser) {
                    finalResult.routeToUser = true;
                }
                // Verdicts flow only when explicitly requested — a
                // volunteered pending_action on a non-requesting call
                // lacks the executed-tools context and is dropped.
                if (!wantsVerdict) {
                    finalResult.selfPendingAction = false;
                    finalResult.pendingActionHint.clear();
                    finalResult.pendingDelegateAlias.clear();
                }
            }

            finalResult.latencyMs = QDateTime::currentMSecsSinceEpoch() - startMs;
            promise->addResult(finalResult);
            promise->finish();
        });

    watcher->setFuture(backendFuture);

    return promise->future();
}

QString Service::backendName() const {
    return m_backend ? m_backend->backendName() : QStringLiteral("(none)");
}

QFuture<QString> Service::oneShotCompleteAsync(const QString& prompt) {
    if (!m_backend) {
        QPromise<QString> p;
        p.start();
        p.addResult(QString());
        p.finish();
        return p.future();
    }
    return m_backend->oneShotCompleteAsync(prompt);
}

void Service::clearCache() {
    m_cache.clear();
}

void Service::setBackend(std::unique_ptr<IRagpBackend> backend) {
    Q_ASSERT_X(backend != nullptr, "Ragp::Service::setBackend", "backend must not be null");
    m_backend = std::move(backend);
    m_cache.clear();
}

}  // namespace Ragp

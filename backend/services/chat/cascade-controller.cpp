// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file cascade-controller.cpp
 * @brief Implementation of Chat::CascadeController. Contains the
 *        RAGP classification routing (applyClassification), per-turn
 *        dispatch (tryDispatchNext), backend configuration +
 *        deferred auto-fallback, and cascade-end state clear.
 *
 *        Cross-collaborator concerns that stay on ChatController
 *        (invoked from its onCascadeComplete slot):
 *          - active-plan declared-status handling
 *          - active-plan implicit completion
 *          - streaming reset, tool-dispatcher reset, inflight id
 *            clear, request-id clear, isGenerating flag.
 *
 *        See cascade-controller.h for the full contract.
 *
 * @layer Service (Chat subsystem)
 * @dependencies ConversationService, MembershipService, ModelRouter,
 *               SettingsService, Ragp::Service (owned), Ragp types,
 *               QFutureWatcher, QTimer, QPointer,
 *               NotificationManager (for user-mention preview),
 *               utils/logger.h, utils/thread-discipline.h.
 */

#include "cascade-controller.h"

#include "../../models/conversation.h"
#include "../../models/llm-config.h"
#include "../../models/member.h"
#include "../../services/conversation-service.h"
#include "../../services/membership-service.h"
#include "../../services/model-router.h"
#include "../../services/ragp/local-llama-backend.h"
#include "../../services/ragp/ragp-backend-selector.h"
#include "../../services/ragp/ragp-service.h"
#include "../../services/ragp/remote-ragp-backend.h"
#include "../../services/ragp/stub-ragp-backend.h"
#include "../../services/settings-service.h"
#include "../../utils/logger.h"
#include "../../utils/notification-manager.h"
#include "../../utils/thread-discipline.h"

#include <QTimer>

#include <QFutureWatcher>
#include <QRegularExpression>

namespace {
inline bool streamTraceEnabled() {
    static const bool s_enabled = qEnvironmentVariableIntValue("VERZETA_STREAM_TRACE") > 0;
    return s_enabled;
}

/**
 * @brief Resolve a classifier-produced alias against the conversation
 *        roster (case-insensitive, underscore/space-tolerant, because models
 *        underscore-join multi-word aliases).
 * @param members The conversation's member list.
 * @param target  Alias to resolve.
 * @returns The matching member, or nullptr.
 */
const Member* findMemberByAliasIn(const QList<Member>& members, const QString& target) {
    for (const Member& m : members) {
        const QString normalized = m.alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
        if (normalized.compare(target, Qt::CaseInsensitive) == 0 ||
            m.alias.compare(target, Qt::CaseInsensitive) == 0) {
            return &m;
        }
    }
    return nullptr;
}

/**
 * @brief One-line alias/intent rendering of a classification's targets
 *        for the routing logs. A silent dead-end proved that logging
 *        only the target COUNT hides WHY nothing dispatched (the
 *        intent gate is invisible without the intent).
 * @param result Classification whose targets are rendered.
 * @returns " [alias/INTENT, …]", or an empty string with no targets.
 */
QString targetsDetail(const Ragp::Classification& result) {
    if (result.targets.isEmpty())
        return QString();
    QStringList parts;
    parts.reserve(result.targets.size());
    for (const Ragp::Target& t : result.targets) {
        parts.append(t.alias + QLatin1Char('/') + Ragp::intentToString(t.intent));
    }
    return QStringLiteral(" [") + parts.join(QStringLiteral(", ")) + QLatin1Char(']');
}

/**
 * @brief Compact single-line summary of a classify verdict for the
 *        default-visible (Info) log: the routing/pending OUTCOME the
 *        deferred-action nudge is gated on, without the verbose per-field
 *        dump (which stays at Debug).
 * @param responderAlias The alias whose reply was classified.
 * @param result         The classification whose verdict to summarise.
 * @returns A one-line summary: alias, target count, pending-self, delegate,
 *          route-to-user and the resolving source tier.
 */
QString classifyVerdictSummary(const QString& responderAlias, const Ragp::Classification& result) {
    return QStringLiteral("classify @%1 -> targets:%2 pendingSelf:%3 "
                          "delegate:%4 routeUser:%5 (%6)")
        .arg(responderAlias.isEmpty() ? QStringLiteral("?") : responderAlias)
        .arg(result.targets.size())
        .arg(result.selfPendingAction ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(result.pendingDelegateAlias.isEmpty() ? QStringLiteral("-")
                                                   : result.pendingDelegateAlias)
        .arg(result.routeToUser ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(result.source);
}
}  // namespace


namespace Chat {

CascadeController::CascadeController(ConversationService& convSvc,
                                     ModelRouter& router,
                                     QObject* parent)
    : QObject(parent)
    , m_convSvc(convSvc)
    , m_router(router)
    ,
    // Initial backend matches ChatController's pre-extraction default
    // Initial backend matches the pre-configuration default:
    // StubBackend always returns UNKNOWN so Tier 1
    // rules + cache drive routing until AppController calls
    // configureBackend() with real settings.
    m_ragpService(std::make_unique<Ragp::Service>(std::make_unique<Ragp::StubBackend>(), this)) {}

CascadeController::~CascadeController() = default;

void CascadeController::setMembershipService(MembershipService* membership) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_membership = membership;
}

// ---------------------------------------------------------------------------
// Responder identity
// ---------------------------------------------------------------------------

void CascadeController::setCurrentResponder(const QString& alias, const QString& agentId) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_responderAlias = alias;
    m_responderAgentId = agentId;
}

QString CascadeController::currentResponderAlias() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_responderAlias;
}

QString CascadeController::currentResponderAgentId() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_responderAgentId;
}

// ---------------------------------------------------------------------------
// Cascade queue
// ---------------------------------------------------------------------------

bool CascadeController::enqueueTarget(const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (alias.isEmpty())
        return false;
    const QString key = alias.toLower();
    // Self-skip.
    if (alias.compare(m_responderAlias, Qt::CaseInsensitive) == 0) {
        return false;
    }
    // Dedup against immediate queue.
    if (m_queue.contains(alias))
        return false;
    if (m_memberTurns.value(key, 0) >= kMaxTurnsPerMember) {
        m_capRefusedAlias = alias;
        return false;
    }
    m_queue.append(alias);
    return true;
}

bool CascadeController::enqueueDeferredTarget(const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (alias.isEmpty())
        return false;
    const QString key = alias.toLower();
    // Self-skip — current responder doesn't deserve a deferred slot.
    if (alias.compare(m_responderAlias, Qt::CaseInsensitive) == 0) {
        return false;
    }
    // Dedup against BOTH queues — an alias the classifier already
    // routed doesn't need a deferred slot too. The dispatcher would
    // dedupe at pop time, but skipping the append keeps the deferred
    // queue tidy and avoids stale-state confusion in tests.
    if (m_queue.contains(alias))
        return false;
    if (m_deferredQueue.contains(alias))
        return false;
    // Per-member cap defends here too (refusal recorded for round
    // continuation, same as enqueueTarget).
    if (m_memberTurns.value(key, 0) >= kMaxTurnsPerMember) {
        m_capRefusedAlias = alias;
        return false;
    }
    m_deferredQueue.append(alias);
    return true;
}

int CascadeController::queueSize() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_queue.size() + m_deferredQueue.size();
}

bool CascadeController::isQueueEmpty() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_queue.isEmpty() && m_deferredQueue.isEmpty();
}

bool CascadeController::yieldToQueuedAfterToolBatch(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Same body as the private `tryDispatchNext`; exposed as the
    // public yield path used by ChatController::onToolBatchCompleted
    // when `request_turn` fired in the just-finished tool batch. See
    // the header for the failure history that motivated this entry.
    return tryDispatchNext(convId);
}

void CascadeController::forceCascadeComplete(const CascadeRouteInputs& inputs) {
    VERZETA_ASSERT_MAIN_THREAD();
    emitCascadeCompleteAndClear(inputs);
}

int CascadeController::cascadeIterations() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_iterations;
}

int CascadeController::memberTurnCount(const QString& alias) const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_memberTurns.value(alias.toLower(), 0);
}

// ---------------------------------------------------------------------------
// RAGP backend management
// ---------------------------------------------------------------------------

QString CascadeController::ragpBackendName() const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_ragpService)
        return QString();
    return m_ragpService->backendName();
}

Ragp::Service* CascadeController::ragpService() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_ragpService.get();
}

// The deferred-auto-fallback QTimer::singleShot pattern is load-
// bearing. Without the defer, a LocalLlamaBackend::loadFailed
// emission would trigger setBackend(remote) synchronously — which
// destructs the local backend WHILE STILL UNWINDING its own signal
// emission, and that reliably crashes. The single-shot posts the
// backend swap to the next main-thread tick so the emission fully
// unwinds first. See file header for the QPointer<SettingsService>
// race guard.
void CascadeController::configureBackend(SettingsService& settings) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_ragpService) {
        qCWarning(verzetaUi) << "CascadeController::configureBackend: m_ragpService null";
        return;
    }

    // Resolve the local-model filename to an absolute path exactly
    // once here, then let the pure selector decide. Tests call the
    // selector directly with the same primitive inputs.
    const QString filename = settings.ragpDefaultModelFilename();
    const QString resolvedPath = filename.isEmpty() ? QString{} : settings.ragpModelPath(filename);
    const QString spec = Ragp::chooseRagpBackendSpec(settings.ragpLocalEnabled(), resolvedPath);

    if (spec.startsWith(QStringLiteral("local:"))) {
        const QString modelPath = spec.mid(QStringLiteral("local:").size());
        // Bridge backend: generation runs in the verzeta-inference
        // sidecar (the single llama.cpp integration); available in
        // EVERY build. A null host degrades through the existing
        // loadFailed → RemoteBackend auto-fallback below.
        auto local =
            std::make_unique<Ragp::LocalLlamaBackend>(modelPath, m_inferenceHost, /*parent=*/this);

        // Deferred auto-fallback: LocalLlamaBackend::loadFailed fires
        // from the worker thread (queued into the main thread). We
        // MUST NOT destroy the sender (the local backend) inside its
        // own signal-emission chain; QTimer::singleShot(0, this, ...)
        // defers the swap to the next main-thread tick so the
        // emission fully unwinds first. QPointer<SettingsService>
        // guards the settings pointer in case AppController destroyed
        // settings between the emission and the deferred run.
        QPointer<SettingsService> settingsGuard(&settings);
        connect(local.get(),
                &Ragp::LocalLlamaBackend::loadFailed,
                this,
                [this, settingsGuard](const QString& reason) {
                    qCWarning(verzetaUi) << "CascadeController: local RAGP load failed —" << reason
                                         << "| auto-falling back to RemoteBackend (deferred)";
                    QTimer::singleShot(0, this, [this, settingsGuard]() {
                        if (!m_ragpService)
                            return;
                        if (!settingsGuard) {
                            qCWarning(verzetaUi) << "CascadeController: settings destroyed "
                                                    "before fallback; leaving current "
                                                    "backend as-is";
                            return;
                        }
                        m_ragpService->setBackend(
                            std::make_unique<Ragp::RemoteBackend>(m_router, *settingsGuard, this));
                        const QString name = m_ragpService->backendName();
                        qCInfo(verzetaUi)
                            << "CascadeController: RAGP auto-fallback complete (" << name << ")";
                        emit ragpBackendChanged(name);
                    });
                });

        m_ragpService->setBackend(std::move(local));
        const QString name = m_ragpService->backendName();
        qCInfo(verzetaUi) << "CascadeController: RAGP local backend configured (" << name
                          << ", path:" << modelPath << ")";
        emit ragpBackendChanged(name);
        return;
    }

    // Default / fallback path: Remote.
    m_ragpService->setBackend(std::make_unique<Ragp::RemoteBackend>(m_router, settings, this));
    const QString name = m_ragpService->backendName();
    qCInfo(verzetaUi) << "CascadeController: RAGP remote backend configured (" << name << ")";
    emit ragpBackendChanged(name);
}

// ---------------------------------------------------------------------------
// Turn-state lifecycle
// ---------------------------------------------------------------------------

void CascadeController::resetTurnState() {
    VERZETA_ASSERT_MAIN_THREAD();
    m_queue.clear();
    m_deferredQueue.clear();
    m_memberTurns.clear();
    m_iterations = 0;
    m_responderAlias.clear();
    m_responderAgentId.clear();

    m_recentContentFingerprints.clear();
    m_memberRetryCount.clear();
    // Round-continuation state is per-user-message.
    m_capRefusedAlias.clear();
    m_roundToolFires = 0;
    m_roundRetries = 0;
    m_roundNovelTurns = 0;
    m_round = 1;
}

void CascadeController::clearCrossCascadeFingerprints() {
    VERZETA_ASSERT_MAIN_THREAD();
    m_priorCascadeFingerprints.clear();
}


QSet<QString> CascadeController::contentFingerprint(const QString& content) {
    static const QRegularExpression kAtMention(QStringLiteral("@\\w+"));
    static const QRegularExpression kNonWord(QStringLiteral("[^a-z0-9 ]"));
    static const QRegularExpression kSpaceRun(QStringLiteral("\\s+"));

    QString normalised = content.toLower();
    normalised.replace(kAtMention, QString());
    normalised.replace(kNonWord, QStringLiteral(" "));
    normalised.replace(kSpaceRun, QStringLiteral(" "));

    const QStringList words = normalised.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QSet<QString> set;
    for (const QString& w : words) {
        // Drop short tokens (a/the/is/of/...) so they don't dominate
        // the similarity score for short replies.
        if (w.length() >= 3)
            set.insert(w);
    }
    return set;
}

double CascadeController::jaccardSimilarity(const QSet<QString>& a, const QSet<QString>& b) {
    // Either side empty → no comparable signal, treat as dissimilar.
    // (The opposite convention "empty vs empty = identical" trips the
    // saturation detector on every short-content turn like "@Bob 1"
    // whose post-strip token set is empty.)
    if (a.isEmpty() || b.isEmpty())
        return 0.0;
    QSet<QString> intersection = a;
    intersection.intersect(b);
    QSet<QString> uni = a;
    uni.unite(b);
    return static_cast<double>(intersection.size()) / static_cast<double>(uni.size());
}

bool CascadeController::isCascadeSaturated(const QSet<QString>& candidate) const {
    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    const bool kTrace = streamTraceEnabled();
    if (kTrace) {
        QStringList candTokens(candidate.values());
        candTokens.sort();
        qCInfo(verzetaUi).noquote() << "TRACE: CASCADE-FP candidate tokens=" << candidate.size()
                                    << " threshold=" << kCascadeNoveltyThreshold
                                    << " withinWindow=" << m_recentContentFingerprints.size()
                                    << " priorWindow=" << m_priorCascadeFingerprints.size();
        qCInfo(verzetaUi).noquote() << "TRACE: CASCADE-CANDIDATE-TOKENS\n"
                                    << candTokens.join(QLatin1Char(' '));
    }
    // === END STREAM-TRACE DEBUG ===

    int idx = 0;
    for (const QSet<QString>& past : m_recentContentFingerprints) {
        const double sim = jaccardSimilarity(candidate, past);
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        if (kTrace) {
            qCInfo(verzetaUi).noquote()
                << "TRACE: CASCADE-CMP within[" << idx << "]"
                << " jaccard=" << QString::number(sim, 'f', 3) << " pastTokens=" << past.size()
                << " saturated?=" << (sim >= kCascadeNoveltyThreshold);
        }
        ++idx;
        // === END STREAM-TRACE DEBUG ===
        if (sim >= kCascadeNoveltyThreshold) {
            return true;
        }
    }
    idx = 0;
    for (const QSet<QString>& past : m_priorCascadeFingerprints) {
        const double sim = jaccardSimilarity(candidate, past);
        // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
        if (kTrace) {
            qCInfo(verzetaUi).noquote()
                << "TRACE: CASCADE-CMP prior[" << idx << "]"
                << " jaccard=" << QString::number(sim, 'f', 3) << " pastTokens=" << past.size()
                << " saturated?=" << (sim >= kCascadeNoveltyThreshold);
        }
        ++idx;
        // === END STREAM-TRACE DEBUG ===
        if (sim >= kCascadeNoveltyThreshold) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// routeOrFinalize — the main cascade entry point
//
// Body migrated from onRequestFinished cpp:1850-1998. Flow:
//   1. Determine isGroupChat (via conversation metadata + membership).
//   2. Update the current responder's per-member turn counter (cap
//      guard on future requeues).
//   3. If not group chat OR empty content OR no RAGP service →
//      attempt one tryDispatchNext; if dispatched emit
//      memberTurnStarted (already done), else emit cascadeComplete
//      and clear state.
//   4. Else → classify via Ragp::Service. Tier 1/2 resolves
//      synchronously; Tier 3 defers via QFutureWatcher. Both paths
//      call applyClassification + tryDispatchNext, then either
//      return (dispatched) or emit cascadeComplete.
// ---------------------------------------------------------------------------
void CascadeController::setVoiceTurnGate(IVoiceTurnGate* gate) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_voiceGate = gate;
    if (!gate && m_voiceHoldActive) {
        // The gate is going away with a turn held: replay it now rather
        // than strand the cascade.
        releaseVoiceHold(m_voiceHeldInputs.responderMsgId);
    }
}

void CascadeController::releaseVoiceHold(const QString& msgId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_voiceHoldActive)
        return;
    if (!msgId.isEmpty() && msgId != m_voiceHeldInputs.responderMsgId) {
        return;  // A different message finished; keep waiting.
    }
    if (m_voiceHoldDeadline)
        m_voiceHoldDeadline->stop();
    const CascadeRouteInputs held = m_voiceHeldInputs;
    m_voiceHoldActive = false;
    m_voiceHeldInputs = {};
    // A turn is held AT MOST ONCE. Replaying it re-enters routeOrFinalize,
    // and without this the gate would hold the same reply again and spin
    // forever on the deadline instead of advancing the cascade.
    if (!held.responderMsgId.isEmpty()) {
        m_voiceReleasedMsgIds.insert(held.responderMsgId);
        while (m_voiceReleasedMsgIds.size() > 64) {
            m_voiceReleasedMsgIds.erase(m_voiceReleasedMsgIds.begin());
        }
    }
    // Replays the identical call the hold intercepted.
    routeOrFinalize(held);
}

void CascadeController::routeOrFinalize(const CascadeRouteInputs& inputs) {
    VERZETA_ASSERT_MAIN_THREAD();

    // Voice pacing gate. m_voiceGate is NULL unless a voice call is live,
    // so text mode evaluates one null pointer and falls straight through
    // to the unchanged body below. When a call IS live, the whole call is
    // replayed verbatim once the reply has finished being spoken, so no
    // downstream logic (dispatch, finalize, retries) differs — it simply
    // happens at speech speed instead of text speed.
    if (m_voiceGate && !m_voiceHoldActive &&
        !m_voiceReleasedMsgIds.contains(inputs.responderMsgId) &&
        m_voiceGate->shouldHoldTurn(inputs.convId, inputs.responderMsgId)) {
        m_voiceHeldInputs = inputs;
        m_voiceHoldActive = true;
        if (!m_voiceHoldDeadline) {
            m_voiceHoldDeadline = new QTimer(this);
            m_voiceHoldDeadline->setSingleShot(true);
            connect(m_voiceHoldDeadline, &QTimer::timeout, this, [this]() {
                if (!m_voiceHoldActive)
                    return;
                qCWarning(verzetaUi) << "Cascade: voice hold released by deadline for"
                                     << m_voiceHeldInputs.responderMsgId;
                releaseVoiceHold(m_voiceHeldInputs.responderMsgId);
            });
        }
        // Bounded safety net: a release that never arrives (a daemon that
        // died mid-sentence) must not wedge the cascade. Scaled to the
        // reply length because long replies legitimately take longer to
        // speak. Cancelled by the real release.
        const int budgetMs = qMin(120000, 10000 + inputs.content.size() * 80);
        m_voiceHoldDeadline->start(budgetMs);
        return;
    }

    const auto convOpt = m_convSvc.getConversation(inputs.convId);
    const bool isGroupChat = convOpt.has_value() && convOpt->isGroup && m_membership;

    // Record this agent's turn count — ALWAYS in group chats, regardless
    // of content. Per-member cap defends against runaway loops while
    // still allowing back-and-forth.
    if (isGroupChat && !m_responderAlias.isEmpty()) {
        const QString key = m_responderAlias.toLower();
        m_memberTurns[key] = m_memberTurns.value(key, 0) + 1;
        if (inputs.content.trimmed().isEmpty()) {
            qCWarning(verzetaUi) << "Agent @" << m_responderAlias
                                 << "returned empty content — skipping parse, continuing cascade";
        }
    }

    // Echo / empty-reply detection with internal retry recovery.
    //
    // Saturation check — detect when the responder's reply is a
    // near-verbatim copy of a recent reply in this cascade
    // (Jaccard >= kCascadeNoveltyThreshold). The classifier itself
    // cannot see this — each classify call examines one reply in
    // isolation — so this conversation-level check is the safety
    // net for cross-agent voice contamination.
    //
    // Empty replies are treated as a separate failure class: the
    // model produced no content at all (often a stop-token reflex
    // when the prior context confused it). Both bad-reply classes
    // route through the same recovery path.
    //
    // Recovery: instead of ending the cascade on first failure
    // (which would silently drop every subsequent member), give the
    // SAME responder up to kMaxEchoRetries attempts. Each retry
    // deletes the bad message, increments a per-alias retry
    // counter, decrements the member-turn count (so the failed
    // attempt does NOT consume the per-member cap budget), and
    // emits echoDetectedRequestRetry — ChatController re-opens the
    // streaming placeholder for the same alias and dispatches with
    // a progressively stronger anti-echo nudge in BuildRequestInputs.
    // The fingerprint of a bad reply is NOT added to the recent
    // window — keeping garbage there would bias all future checks.
    //
    // After kMaxEchoRetries failures: emit echoMaxRetriesExhausted
    // so ChatController can either delete the empty row or mark
    // the non-empty row "echo-failed" for UI rendering, then
    // continue the cascade normally to the next queued member.
    QSet<QString> currentFp;
    bool saturated = false;
    if (isGroupChat && !inputs.content.trimmed().isEmpty()) {
        currentFp = contentFingerprint(inputs.content);
        saturated = isCascadeSaturated(currentFp);
    }
    const bool isEmptyReply = inputs.content.trimmed().isEmpty();
    const bool isImpersonation = isGroupChat && !inputs.impersonatedAlias.isEmpty();
    const bool needsRetryOrExhaust = saturated || isEmptyReply || isImpersonation;

    // Retry-counter key. Group members key by alias; a 1:1 direct chat has
    // no alias, so it shares a single sentinel bucket. (saturated only
    // arises in group, where the alias is always set, so the sentinel is
    // used only for the 1:1 empty-reply class.)
    const QString retryKey =
        m_responderAlias.isEmpty() ? QStringLiteral("__direct__") : m_responderAlias.toLower();

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    if (streamTraceEnabled()) {
        qCInfo(verzetaUi) << "TRACE: CASCADE-DECISION responder=" << m_responderAlias
                          << " responderMsgId=" << inputs.responderMsgId.left(8)
                          << " isGroupChat=" << isGroupChat
                          << " contentChars=" << inputs.content.size()
                          << " trimmedChars=" << inputs.content.trimmed().size()
                          << " saturated=" << saturated << " isEmptyReply=" << isEmptyReply
                          << " isImpersonation=" << isImpersonation
                          << " needsRetryOrExhaust=" << needsRetryOrExhaust
                          << " retriesUsed=" << m_memberRetryCount.value(retryKey, 0)
                          << " maxRetries=" << kMaxEchoRetries;
    }
    // === END STREAM-TRACE DEBUG ===

    if (needsRetryOrExhaust) {
        const QString key = retryKey;
        const int retriesUsed = m_memberRetryCount.value(key, 0);

        if (retriesUsed < kMaxEchoRetries) {
            // Retry path. Refund the failed turn (so the per-member
            // cap budget is preserved across retries) and increment
            // the retry counter. Do NOT add the bad fingerprint to
            // the saturation window.
            m_memberRetryCount[key] = retriesUsed + 1;
            ++m_roundRetries;  // Progress signal for round continuation.
            // Member-turn refund is group-only bookkeeping; a 1:1 direct
            // chat (empty alias) has no per-member turn budget to refund.
            if (!m_responderAlias.isEmpty() && m_memberTurns.value(key, 0) > 0) {
                m_memberTurns[key] = m_memberTurns.value(key) - 1;
            }
            const QString retryReason = isImpersonation ? QStringLiteral("impersonation")
                                        : saturated     ? QStringLiteral("echo")
                                                        : QStringLiteral("empty");
            qCWarning(verzetaUi) << "Cascade bad-reply detected for @" << m_responderAlias
                                 << "— retry" << (retriesUsed + 1) << "/" << kMaxEchoRetries
                                 << "(reason:" << retryReason << ")";
            emit echoDetectedRequestRetry(inputs.convId,
                                          inputs.responderMsgId,
                                          m_responderAlias,
                                          m_responderAgentId,
                                          retriesUsed + 1,
                                          retryReason);
            return;  // ChatController drives the retry; cascade
                     // iteration index does NOT advance for retries.
        }

        // Retries exhausted. The responder failed to produce an
        // original / non-empty reply across kMaxEchoRetries attempts.
        const bool wasEmpty = inputs.content.trimmed().isEmpty();
        qCWarning(verzetaUi) << "Cascade echo retries exhausted for @" << m_responderAlias
                             << "— wasEmpty:" << wasEmpty << "msgId:" << inputs.responderMsgId;
        emit echoMaxRetriesExhausted(inputs.convId, inputs.responderMsgId, wasEmpty);
        m_memberRetryCount.remove(key);
        // Fall through to the normal "continue cascade" path so the
        // next queued / cohort member gets its turn. The bad
        // fingerprint is NOT added to the recent window because it
        // is a known-bad reply we are intentionally retaining for
        // UI transparency, not a legitimate cascade contribution.
        if (tryDispatchNext(inputs.convId))
            return;
        emitCascadeCompleteAndClear(inputs);
        return;
    }

    // Successful (non-echo, non-empty) reply path. Add fingerprint
    // to the recent window for future saturation checks, and reset
    // the retry counter for this responder so the next time they
    // hit the saturation/empty path they get a fresh budget.
    if (!currentFp.isEmpty()) {
        m_recentContentFingerprints.append(currentFp);
        while (m_recentContentFingerprints.size() > kCascadeFingerprintWindow) {
            m_recentContentFingerprints.removeFirst();
        }
        // This turn produced accepted, non-echoed content → it counts as
        // real progress for the round-continuation gate, so a purely
        // conversational collaboration round keeps going instead of pausing.
        ++m_roundNovelTurns;
    }
    // Reset the retry budget on a successful reply — keyed the same way as
    // the retry path (sentinel for 1:1, alias for group).
    m_memberRetryCount.remove(retryKey);

    static const QRegularExpression kHasMentionRx(QStringLiteral("@\\w+"));
    const bool hasAnyMention = kHasMentionRx.match(inputs.content).hasMatch();

    const bool shouldClassify = isGroupChat && !inputs.content.trimmed().isEmpty() &&
                                m_ragpService &&
                                (hasAnyMention || inputs.hasStructuralActionSignals);

    if (shouldClassify) {
        const bool isFirstResponder = (m_iterations == 0);

        const QList<Member> members = m_membership->conversationMembers(inputs.convId);

        Ragp::Request ragpReq;
        ragpReq.content = inputs.content;
        ragpReq.authorAlias = m_responderAlias;
        ragpReq.executedToolsSummary = inputs.executedToolsSummary;
        // The pending-action verdict is REQUESTED only when structural
        // tokens are present — that request forces Tier 3 (rules/cache
        // cannot answer it; see Ragp::Service::classify).
        ragpReq.wantsPendingActionVerdict = inputs.hasStructuralActionSignals;
        for (const Member& m : members) {
            ragpReq.rosterAliases.append(m.alias);
        }

        QFuture<Ragp::Classification> ragpFuture = m_ragpService->classify(ragpReq);

        const quint64 requestIdAtDispatch = inputs.requestId;
        const CascadeRouteInputs inputsCopy = inputs;
        const QString asyncCapturedContent = inputs.content;

        // Hot-path: Tier 1/2 already resolved → run synchronously.
        if (ragpFuture.isFinished()) {
            const Ragp::Classification result = ragpFuture.result();
            qCDebug(verzetaUi) << "RAGP classify (sync) —"
                               << "agent:" << m_responderAlias
                               << "| targets:" << result.targets.size()
                               << qPrintable(targetsDetail(result))
                               << "| routeToUser:" << result.routeToUser
                               << "| confidence:" << result.confidence
                               << "| source:" << result.source << "| latencyMs:" << result.latencyMs
                               << "| pendingSelf:" << result.selfPendingAction
                               << "| pendingDelegate:" << result.pendingDelegateAlias
                               << "| first responder:" << isFirstResponder;
            qCInfo(verzetaUi).noquote() << classifyVerdictSummary(m_responderAlias, result);

            // Capture the speaker BEFORE dispatch re-seats the responder —
            // the two-winners handling below re-queues them.
            const QString speakerAlias = m_responderAlias;
            applyClassification(
                result, members, isFirstResponder, asyncCapturedContent, inputs.convId);
            if (tryDispatchNext(inputs.convId)) {
                if (shouldRequestDeferredContinuation(result, inputs) &&
                    enqueueTarget(speakerAlias)) {
                    qCDebug(verzetaUi)
                        << "Self-pending work queued behind the routed"
                        << "target — @" << speakerAlias
                        << "gets a follow-up turn | pending:" << result.pendingActionHint.left(80);
                }
                return;  // New turn owns the lifecycle.
            }
            // Handoff arm: the verdict's delegate dispatches only when
            // no classified target did (same precedence-by-call-order
            // as the self-pending nudge below).
            if (maybeDispatchDelegate(result, members, inputs.convId)) {
                return;  // Delegate turn owns the lifecycle.
            }
            if (shouldRequestDeferredContinuation(result, inputs)) {
                emit deferredContinuationRequested(
                    inputs.convId, m_responderAlias, m_responderAgentId, result.pendingActionHint);
                return;  // Consumer dispatches the continuation turn.
            }
            noteUndispatchedTargets(result, inputs.convId);
            emitCascadeCompleteAndClear(inputs);
            return;
        }

        // Async path — defer completion to a QFutureWatcher.
        // `this` context means Qt auto-disconnects if CascadeController
        // is destroyed, at which point we drop the cascade (the turn
        // is effectively abandoned because CascadeController is gone).
        auto* watcher = new QFutureWatcher<Ragp::Classification>(this);
        QObject::connect(
            watcher,
            &QFutureWatcher<Ragp::Classification>::finished,
            this,
            [this,
             watcher,
             members,
             isFirstResponder,
             requestIdAtDispatch,
             inputsCopy,
             asyncCapturedContent]() {
                watcher->deleteLater();

                Q_UNUSED(requestIdAtDispatch);

                Ragp::Classification result;
                if (watcher->future().isCanceled()) {
                    // Service destroyed mid-flight. Treat as empty.
                    result.source = QStringLiteral("canceled");
                    result.confidence = 0.0;
                } else {
                    result = watcher->result();
                }

                qCDebug(verzetaUi)
                    << "RAGP classify (async) —"
                    << "agent:" << m_responderAlias << "| targets:" << result.targets.size()
                    << qPrintable(targetsDetail(result)) << "| routeToUser:" << result.routeToUser
                    << "| confidence:" << result.confidence << "| source:" << result.source
                    << "| latencyMs:" << result.latencyMs
                    << "| pendingSelf:" << result.selfPendingAction
                    << "| pendingDelegate:" << result.pendingDelegateAlias
                    << "| first responder:" << isFirstResponder;
                qCInfo(verzetaUi).noquote() << classifyVerdictSummary(m_responderAlias, result);

                const QString speakerAlias = m_responderAlias;
                applyClassification(
                    result, members, isFirstResponder, asyncCapturedContent, inputsCopy.convId);

                // PRECEDENCE: real routing first; the nudge only when
                // nobody actually dispatched (see the sync arm).
                if (tryDispatchNext(inputsCopy.convId)) {
                    // Two-winners handling — see the sync arm.
                    if (shouldRequestDeferredContinuation(result, inputsCopy) &&
                        enqueueTarget(speakerAlias)) {
                        qCDebug(verzetaUi) << "Self-pending work queued behind the"
                                           << "routed target — @" << speakerAlias
                                           << "gets a follow-up turn | pending:"
                                           << result.pendingActionHint.left(80);
                    }
                    return;  // New turn owns the lifecycle.
                }
                // Handoff arm (see the sync arm).
                if (maybeDispatchDelegate(result, members, inputsCopy.convId)) {
                    return;  // Delegate turn owns the lifecycle.
                }
                if (shouldRequestDeferredContinuation(result, inputsCopy)) {
                    emit deferredContinuationRequested(inputsCopy.convId,
                                                       m_responderAlias,
                                                       m_responderAgentId,
                                                       result.pendingActionHint);
                    return;  // Consumer dispatches the continuation turn.
                }
                noteUndispatchedTargets(result, inputsCopy.convId);
                emitCascadeCompleteAndClear(inputsCopy);
            });

        watcher->setFuture(ragpFuture);
        return;  // Defer completion to the watcher callback.
    }

    // No classification needed (1:1 chat, empty content, or
    // RAGP service absent). Try a dispatch first in case the
    // cascade queue has pre-seeded aliases from sendMessage's
    // multi-mention parse.
    if (tryDispatchNext(inputs.convId)) {
        return;  // New turn owns the lifecycle.
    }

    emitCascadeCompleteAndClear(inputs);
}

// ---------------------------------------------------------------------------
// applyClassification — body verbatim from
// ChatController::applyRagpClassification, minus the
// m_streaming->content() reads (those now come from capturedContent).
bool CascadeController::shouldRequestDeferredContinuation(const Ragp::Classification& result,
                                                          const CascadeRouteInputs& inputs) const {
    // NOTE: no classified-targets veto here. Routing precedence is
    // enforced by CALL ORDER — routeOrFinalize tries tryDispatchNext
    // FIRST and only consults this predicate when nobody actually
    // dispatched. Vetoing on the mere existence of classified targets
    // (the original shape) froze conversations whenever the intent
    // gate / self-skip / caps dropped every target: no route AND no
    // nudge.
    if (!result.selfPendingAction)
        return false;
    if (result.confidence < 0.5)
        return false;  // unsure verdict
    if (inputs.deferredBudgetExhausted)
        return false;  // per-alias budget
    if (m_responderAlias.isEmpty())
        return false;  // group-only path
    return true;
}

// ---------------------------------------------------------------------------
void CascadeController::applyClassification(const Ragp::Classification& result,
                                            const QList<Member>& members,
                                            bool isFirstResponder,
                                            const QString& capturedContent,
                                            const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();


    // Roster resolution lives in the file-scope findMemberByAliasIn —
    // ONE implementation shared with maybeDispatchDelegate (a local
    // duplicate of shared logic is exactly the defect class that hid
    // the queueMember cap-refusal bug).

    // Intent-dependent queueing policy:
    //   - BROADCAST_REQUEST must NOT re-invite already-spoken
    //     members; otherwise an agent who says "everyone share X"
    //     triggers an endless round-robin.
    //   - DIRECT intents MAY re-queue up to kMaxTurnsPerMember so
    //     natural chain-handoff patterns ("A → B → A follow-up")
    //     still work.
    // Runaway loops are bounded at enqueueTarget() → kMaxTurnsPerMember
    // and tryDispatchNext() → kMaxAgentCascade.
    Q_UNUSED(isFirstResponder);

    // Confidence floor — classifier results below this threshold are
    // treated as too uncertain to drive routing. Tier 1 emits
    // confidence=1.0 on conclusive results (no @-mentions or all
    // mentions classified by structural rules) and 0.0 when any
    // mention falls through to UNKNOWN; Tier 3 emits its own value.
    // The user-mention notification path (routeToUser) below is NOT
    // gated on this floor — the @owner / @user channel stays
    // reserved for explicit human escalation regardless of how
    // uncertain the classifier was about the @-mention's intent.
    constexpr double kMinCascadeConfidence = 0.5;
    const bool llmFailedFallback =
        result.source.startsWith(QStringLiteral("rule-fallback-after:")) ||
        result.source.startsWith(QStringLiteral("rule-preferred-after-unsure:"));
    if (llmFailedFallback && !result.targets.isEmpty()) {
        qCDebug(verzetaUi) << "RAGP: LLM classify failed (" << result.source << ") — cascading to"
                           << result.targets.size()
                           << "explicit @-mention target(s) via Tier-1 fallback";
    }
    if (result.confidence >= kMinCascadeConfidence || llmFailedFallback) {
        for (const Ragp::Target& t : result.targets) {
            // Only cascade on intents the classifier explicitly
            // identified as routing. UNKNOWN means "could not
            // decide" — DEFAULT is to NOT cascade. Treating UNKNOWN
            // as a routing target is the inversion that defeated
            // RAGP: virtually every realistic @-mention falls
            // through Tier 1's structural rules and arrives at
            // Tier 3 as UNKNOWN-prone, and a small classifier model
            // routinely returns UNKNOWN under uncertainty.
            //
            // EXCEPTION: on a collision-caused fallback the intent is
            // UNKNOWN only because the LLM was prevented from running;
            // an explicit @-mention is still a reasonable routing target,
            // so bypass the intent gate (but keep the loop-bounding
            // broadcast-suppression below).
            const bool cascade = llmFailedFallback || Ragp::shouldCascade(t.intent);
            if (!cascade)
                continue;

            const Member* m = findMemberByAliasIn(members, t.alias);
            if (!m)
                continue;

            if (t.intent == Ragp::Intent::BROADCAST_REQUEST) {
                const QString key = m->alias.toLower();
                if (m_memberTurns.value(key, 0) >= 1) {
                    qCDebug(verzetaUi) << "RAGP: suppressing BROADCAST re-invite of" << m->alias
                                       << "(already responded once)";
                    continue;
                }
            }

            enqueueTarget(m->alias);
        }
    } else {
        qCDebug(verzetaUi) << "RAGP: confidence" << result.confidence << "< floor"
                           << kMinCascadeConfidence << "— suppressing cascade dispatch ("
                           << result.targets.size() << "target(s) ignored)";
        // Visible-suppression surface. Only fire when actual targets
        // were dropped — a confidently-empty classification (no
        // mentions, or all mentions non-routing) is normal flow, not
        // a stall. Subscribers: ChatController forwards to QML for a
        // conversation badge; diagnostics read the source tag so
        // "backend was down" and "LLM was genuinely unsure" are
        // distinguishable post-hoc.
        if (!result.targets.isEmpty()) {
            QStringList suppressed;
            suppressed.reserve(result.targets.size());
            for (const Ragp::Target& t : result.targets) {
                suppressed.append(t.alias);
            }
            emit mentionRoutingSuppressed(
                convId, m_responderAlias, suppressed, result.confidence, result.source);
        }
    }

    // User-mention notification applies regardless of responder
    // position — any agent pinging @owner must notify the human.
    if (result.routeToUser) {
        const QString title =
            QStringLiteral("@%1 needs your input")
                .arg(m_responderAlias.isEmpty() ? QStringLiteral("Agent") : m_responderAlias);
        QString preview = capturedContent.trimmed();
        preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (preview.length() > 220)
            preview = preview.left(220) + QStringLiteral("…");

        NotificationManager::instance().notify(title, preview);
        emit userMentionedInGroup(convId, m_responderAlias, capturedContent);
        qCDebug(verzetaUi) << "Agent" << m_responderAlias << "@mentioned the user in group chat";
    }
}

// ---------------------------------------------------------------------------
bool CascadeController::maybeDispatchDelegate(const Ragp::Classification& result,
                                              const QList<Member>& members,
                                              const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (result.pendingDelegateAlias.isEmpty())
        return false;
    // Unsure verdicts never dispatch (same floor the self-pending
    // nudge applies — the prompt's own "< 0.5 if unsure" contract).
    if (result.confidence < 0.5)
        return false;

    const Member* m = findMemberByAliasIn(members, result.pendingDelegateAlias);
    if (!m) {
        // Ragp::Service already roster-validated the alias; failing
        // here means the roster changed between classify and apply.
        qCWarning(verzetaUi) << "RAGP: delegate verdict @" << result.pendingDelegateAlias
                             << "no longer resolves against the conversation roster —"
                             << "handoff skipped";
        return false;
    }

    // enqueueTarget is THE enqueue: self-skip, dedup, and the
    // per-member cap WITH m_capRefusedAlias recording — a cap-refused
    // delegate flows into the visible round-continuation / pause
    // machinery instead of dying silently.
    if (!enqueueTarget(m->alias))
        return false;
    if (!tryDispatchNext(convId)) {
        // Iteration cap: the delegate stays queued and
        // emitCascadeCompleteAndClear's cap handling surfaces it.
        return false;
    }
    qCDebug(verzetaUi) << "RAGP: LLM-decided handoff — dispatching delegate @" << m->alias
                       << "for pending work:" << result.pendingActionHint
                       << "(no classified target dispatched)";
    return true;
}

// ---------------------------------------------------------------------------
void CascadeController::noteUndispatchedTargets(const Ragp::Classification& result,
                                                const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (result.targets.isEmpty())
        return;
    // A cap-refusal or still-queued work is owned by the round
    // machinery (visible round-continue / ⏸) — don't double-report.
    if (!m_capRefusedAlias.isEmpty())
        return;
    if (!m_queue.isEmpty() || !m_deferredQueue.isEmpty())
        return;

    QStringList aliases;
    aliases.reserve(result.targets.size());
    for (const Ragp::Target& t : result.targets) {
        aliases.append(t.alias);
    }
    qCDebug(verzetaUi) << "RAGP: classified" << result.targets.size()
                       << "target(s) but dispatched none — every mention carried a"
                       << "non-routing intent, was the responder, or was unresolvable:"
                       << qPrintable(targetsDetail(result)) << "| confidence:" << result.confidence
                       << "| source:" << result.source;
    emit mentionRoutingSuppressed(
        convId, m_responderAlias, aliases, result.confidence, result.source);
}

// ---------------------------------------------------------------------------
// tryDispatchNext — body migrated from
// ChatController::dispatchNextCascadeMember. The
// QTimer::singleShot(0, this, [...]) deferred buildAndSendRequest
// that used to live at the end has been moved to ChatController's
// onCascadeMemberTurnStarted slot (because buildAndSendRequest is a
// ChatController member).
// ---------------------------------------------------------------------------
bool CascadeController::tryDispatchNext(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();

    const auto convOpt = m_convSvc.getConversation(convId);
    const bool isGroupChat = convOpt.has_value() && convOpt->isGroup && m_membership;

    if (!isGroupChat || m_iterations >= kMaxAgentCascade) {
        return false;
    }

    // Two-tier drain: m_queue (immediate, classifier-driven) FIRST,
    // then m_deferredQueue (user-pre-seeded mentions whose work
    // semantically depends on the immediate sub-cascade settling).
    if (m_queue.isEmpty() && m_deferredQueue.isEmpty()) {
        return false;
    }
    QStringList* sourceQueue = m_queue.isEmpty() ? &m_deferredQueue : &m_queue;

    // Skip any queued aliases that are no longer valid members.
    Member next;
    while (!sourceQueue->isEmpty() && !next.isValid()) {
        const QString candidateAlias = sourceQueue->takeFirst();
        next = m_membership->findConversationMemberByAlias(convId, candidateAlias);
        if (!next.isValid()) {
            qCWarning(verzetaUi) << "Cascade: skipping unknown alias" << candidateAlias;
        }
        // If the chosen source emptied during the skip-walk and we
        // started on the immediate queue, fall over to the deferred
        // queue so a deferred-only state still finds something to
        // dispatch.
        if (sourceQueue->isEmpty() && sourceQueue == &m_queue && !m_deferredQueue.isEmpty() &&
            !next.isValid()) {
            sourceQueue = &m_deferredQueue;
        }
    }

    if (!next.isValid())
        return false;

    ++m_iterations;
    qCDebug(verzetaUi) << "Agent cascade: turn" << m_iterations << "→ @" << next.alias
                       << "(agent_id:" << next.agentId << ")";

    m_responderAlias = next.alias;
    m_responderAgentId = next.agentId;

    // ChatController's slot handles the rest: streaming begin,
    // tool-dispatcher reset, deferred buildAndSendRequest. We do NOT
    // call buildAndSendRequest here — that would be a direct
    // cross-collaborator call and violate the star-topology rule.
    emit memberTurnStarted(next.alias, next.agentId);
    return true;
}

// ---------------------------------------------------------------------------
// emitCascadeCompleteAndClear — wraps the cap-hit warning + cascade
// state clear (queue, member turns, iterations, responder) and emits
// cascadeComplete with the caller's passthrough fields. ChatController's
// onCascadeComplete slot then runs the active-plan transitions +
// turn-end cleanup.
// ---------------------------------------------------------------------------
void CascadeController::emitCascadeCompleteAndClear(const CascadeRouteInputs& inputs) {
    VERZETA_ASSERT_MAIN_THREAD();

    const auto convOpt = m_convSvc.getConversation(inputs.convId);
    const bool isGroupChat = convOpt.has_value() && convOpt->isGroup && m_membership;

    bool pauseNoted = false;
    const bool workWaiting =
        !m_capRefusedAlias.isEmpty() || !m_queue.isEmpty() || !m_deferredQueue.isEmpty();
    if (isGroupChat && workWaiting && inputs.finishReason != QStringLiteral("tool_iteration_cap") &&
        inputs.finishReason != QStringLiteral("tool_chain_echo")) {
        // Progress = the round did real work. Tool fires and corrective
        // retries count, AND so does genuinely NOVEL conversational content
        // (a turn whose reply was NOT flagged as a near-verbatim echo by the
        // saturation detector). Without the novelty signal a legitimate
        // discussion round — agents collaborating via @-mentions with no tool
        // calls — read as "no progress" and the cascade paused after the caps,
        // forcing the user to nudge every few turns. The echo/saturation
        // detector remains the runaway guard: a round that only repeats prior
        // content produces no novel turns → no progress → it still pauses.
        const bool progress = m_roundToolFires > 0 || m_roundRetries > 0 || m_roundNovelTurns > 0;
        QString nextAlias = m_capRefusedAlias;
        if (nextAlias.isEmpty() && !m_queue.isEmpty()) {
            nextAlias = m_queue.first();
        }
        if (nextAlias.isEmpty() && !m_deferredQueue.isEmpty()) {
            nextAlias = m_deferredQueue.first();
        }
        // Per-conversation autonomous-round ceiling. Default kMaxAutoRounds
        // (6); 0 in llm_config = UNBOUNDED ("run until the team stagnates") —
        // continuation is then gated only by `progress` (a no-progress round
        // still pauses via the echo detector), with kAutoRoundsHardCeiling as
        // an absolute backstop. An explicit value is clamped to the backstop.
        const int cfgRounds = convOpt.has_value()
                                  ? LlmConfig::fromJson(convOpt->llmConfig).maxAutoRounds
                                  : kMaxAutoRounds;
        const int effectiveMaxRounds =
            (cfgRounds <= 0) ? kAutoRoundsHardCeiling : qMin(cfgRounds, kAutoRoundsHardCeiling);
        if (progress && m_round < effectiveMaxRounds) {
            ++m_round;
            qCDebug(verzetaUi) << "Cascade round" << (m_round - 1)
                               << "hit caps with work waiting (@" << nextAlias
                               << ") and made progress (tools:" << m_roundToolFires
                               << "retries:" << m_roundRetries << "novel:" << m_roundNovelTurns
                               << ") — continuing as round" << m_round;
            // Fresh round budgets; anti-echo windows intentionally
            // survive (they are content protection, not turn budget).
            m_memberTurns.clear();
            m_iterations = 0;
            m_capRefusedAlias.clear();
            m_roundToolFires = 0;
            m_roundRetries = 0;
            m_roundNovelTurns = 0;
            m_queue.clear();
            m_deferredQueue.clear();
            // Resolve + enqueue via the membership service directly
            // (the applyClassification helpers are call-local lambdas).
            const Member member =
                m_membership->findConversationMemberByAlias(inputs.convId, nextAlias);
            if (member.isValid() && enqueueTarget(member.alias) && tryDispatchNext(inputs.convId)) {
                emit roundContinued(inputs.convId, m_round, nextAlias);
                return;  // Round continues — NOT complete.
            }
            // Dispatch failed (member gone, etc.) — fall through to
            // a visible pause rather than a silent end.
        }
        qCDebug(verzetaUi) << "Cascade pausing at caps — refused:" << nextAlias
                           << "progress:" << progress << "round:" << m_round;
        emit cascadePausedAtCap(inputs.convId,
                                nextAlias,
                                progress ? QStringLiteral("rounds_exhausted")
                                         : QStringLiteral("no_progress"));
        pauseNoted = true;
    }

    if (isGroupChat && m_iterations >= kMaxAgentCascade &&
        (!m_queue.isEmpty() || !m_deferredQueue.isEmpty())) {
        qCWarning(verzetaUi) << "Agent cascade reached max iterations (" << kMaxAgentCascade
                             << "); dropping remaining immediate:" << m_queue
                             << "deferred:" << m_deferredQueue;
    }

    // Capture the final round's dispatched-turn count BEFORE the clear
    // — the completion slot uses it for the visible round-settled note.
    const int roundTurns = m_iterations;

    // Clear cascade-internal state BEFORE emitting completion so the
    // slot (and any further controller operations during completion)
    // see a clean slate.
    m_queue.clear();
    m_deferredQueue.clear();
    m_memberTurns.clear();
    m_iterations = 0;
    m_responderAlias.clear();
    m_responderAgentId.clear();

    for (const QSet<QString>& fp : m_recentContentFingerprints) {
        m_priorCascadeFingerprints.append(fp);
    }
    while (m_priorCascadeFingerprints.size() > kCrossCascadeFingerprintMaxEntries) {
        m_priorCascadeFingerprints.removeFirst();
    }

    m_recentContentFingerprints.clear();
    m_memberRetryCount.clear();
    m_capRefusedAlias.clear();
    m_roundToolFires = 0;
    m_roundRetries = 0;
    m_roundNovelTurns = 0;
    m_round = 1;

    emit cascadeComplete(inputs.declaredTaskStatus,
                         inputs.finishReason,
                         inputs.totalTokens,
                         inputs.elapsedMs,
                         roundTurns,
                         pauseNoted);
}

}  // namespace Chat

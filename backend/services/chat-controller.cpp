// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file chat-controller.cpp
 * @brief Implementation of the ChatController coordinator. Owns a map of
 *        Chat::ConversationRun engines keyed by conversation id (one per
 *        conversation that has become active or holds a turn) and
 *        delegates the entire QML-facing surface to the ACTIVE
 *        conversation's run, re-emitting that run's change signals onto
 *        its own identically-named facade signals. A small set of
 *        non-per-turn, conversation-scoped reads (artifact path, scope
 *        label, last assistant message, conversation config) stay
 *        resident here because they read services directly and never
 *        touch per-turn state.
 * @layer Service
 * @dependencies Chat::ConversationRun, ModelRouter, ConversationService,
 *               MessageService, ExportService, MessageListModel,
 *               MarkdownConverter, Qt6::Core.
 *
 * The send → stream → tools → cascade → finalize pipeline and all
 * per-turn state live inside each Chat::ConversationRun
 * (backend/services/chat/conversation-run.*). For the
 * single-active-conversation case behaviour is byte-for-byte preserved:
 * there is exactly one run, the wiring is identical, and the facade
 * simply forwards. Additional runs appear only when a turn is left
 * running on a conversation the user has switched away from.
 */


#include "chat-controller.h"

#include "../models/conversation.h"
#include "../models/llm-config.h"
#include "../models/message-list-model.h"
#include "../models/message.h"
#include "../services/chat/conversation-run.h"
#include "../services/chat/streaming-manager.h"
#include "../services/conversation-service.h"
#include "../services/file-service.h"
#include "../services/message-service.h"
#include "../utils/logger.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ChatController::ChatController(ModelRouter& router,
                               ConversationService& convSvc,
                               MessageService& msgSvc,
                               ExportService& exportSvc,
                               QObject* parent)
    : QObject(parent)
    , m_router(router)
    , m_convSvc(convSvc)
    , m_msgSvc(msgSvc)
    , m_exportSvc(exportSvc)
    , m_msgModel(new MessageListModel(msgSvc, this)) {
    // Construct the initial run and point the facade at it. This run has
    // no conversation yet; it adopts one on the first switchConversation /
    // sendMessage. It is created in the constructor (not lazily) because
    // AppController wires external consumers — TaskController task tools,
    // HeartbeatSubagentService — to cascadeInternal() / streamingInternal()
    // at startup, before any conversation is selected, and those accessors
    // must return a valid run from the moment construction completes.
    Chat::ConversationRun* initial = createRun(QString());
    repointFacade(initial, QString());

    // When a conversation's metadata changes elsewhere (rename from
    // another path, model change), refresh the OWNING run's cached title.
    // Re-emit the chat-header-facing facade signals only when the change
    // touches the active conversation (the header label, model selector,
    // and settings panel bind to the active conversation). NOT a list
    // refresh — a per-field binding update.
    connect(&m_convSvc, &ConversationService::conversationUpdated, this, [this](const QString& id) {
        Chat::ConversationRun* r = runFor(id, /*createIfMissing=*/false);
        if (r && r->onConversationUpdated(id) && id == m_activeConvId) {
            emit activeConversationChanged();
            emit activeConversationSettingsChanged();
        }
    });

    qCInfo(verzetaUi) << "ChatController initialized";
}

ChatController::~ChatController() {
    // Tear down facade forwarding before the runs (children) are
    // destroyed by the QObject parent chain. The unique_ptrs in m_runs
    // destroy each run; runs reference shared services only via non-owning
    // pointers, so they are safe to destroy before those services (which
    // AppController owns and outlive this controller).
    for (const QMetaObject::Connection& c : m_facadeConns) {
        QObject::disconnect(c);
    }
    m_facadeConns.clear();
    qCInfo(verzetaUi) << "ChatController destroyed";
}

// ---------------------------------------------------------------------------
// Run map: creation, resolution, facade re-pointing
// ---------------------------------------------------------------------------

Chat::ConversationRun* ChatController::createRun(const QString& convId) {
    auto run =
        std::make_unique<Chat::ConversationRun>(m_router, m_convSvc, m_msgSvc, m_msgModel, this);
    Chat::ConversationRun* r = run.get();

    // Attach every currently-known service so this run is wired exactly
    // like the active run already is. Order mirrors the set*() forwarders.
    if (m_services.toolService)
        r->setToolService(m_services.toolService);
    if (m_services.ragService)
        r->setRagService(m_services.ragService);
    if (m_services.memoryRetriever)
        r->setMemoryRetriever(m_services.memoryRetriever);
    if (m_services.fileService)
        r->setFileService(m_services.fileService);
    if (m_services.agentRegistry)
        r->setAgentRegistry(m_services.agentRegistry);
    if (m_services.membershipService)
        r->setMembershipService(m_services.membershipService);
    if (m_services.planService)
        r->setPlanService(m_services.planService);
    if (m_services.taskRunner)
        r->setTaskRunner(m_services.taskRunner);
    if (m_services.taskObserver)
        r->setTaskObserver(m_services.taskObserver);
    if (m_services.skillService)
        r->setSkillService(m_services.skillService);
    if (m_services.heartbeatConfigService)
        r->setHeartbeatConfigService(m_services.heartbeatConfigService);
    if (m_services.pollService)
        r->setPollService(m_services.pollService);
    if (m_services.auditService)
        r->setAuditService(m_services.auditService);
    if (m_services.sessionRouter)
        r->setSessionRouter(m_services.sessionRouter);
    if (m_services.taskGate)
        r->setTaskGateService(m_services.taskGate);
    if (m_services.slashService)
        r->setSlashCommandService(m_services.slashService);
    if (m_services.canvasSvc)
        r->setCanvasService(m_services.canvasSvc);
    if (m_services.agentService)
        r->setAgentService(m_services.agentService);
    if (m_services.summarizer)
        r->setSummarizer(m_services.summarizer);
    if (m_services.providerScheduler)
        r->setProviderScheduler(m_services.providerScheduler);
    if (m_services.inferenceSidecarHost)
        r->setInferenceSidecarHost(m_services.inferenceSidecarHost);
    if (m_services.ragpSettings)
        r->configureRagpBackend(m_services.ragpSettings);

    // Re-emit this run's stream-finalize so consumers that
    // need "any foreground stream finalized" (HeartbeatSubagentService's
    // Tier-2 drain pump) get every run's finalize through one stable
    // ChatController signal rather than a startup-pinned StreamingManager.
    if (Chat::StreamingManager* sm = r->streamingInternal()) {
        connect(
            sm, &Chat::StreamingManager::streamFinalized, this, &ChatController::streamFinalized);
    }

    connect(r,
            &Chat::ConversationRun::isGeneratingChanged,
            this,
            &ChatController::generatingConversationsChanged);

    m_runs.emplace(convId, std::move(run));
    return r;
}

Chat::ConversationRun* ChatController::runFor(const QString& convId, bool createIfMissing) {
    if (convId.isEmpty())
        return nullptr;
    const auto it = m_runs.find(convId);
    if (it != m_runs.end())
        return it->second.get();
    if (!createIfMissing)
        return nullptr;
    return createRun(convId);
}

Chat::ConversationRun* ChatController::inflightOrActiveRun() const {
    // Dispatch is serialized this phase: at most one run is generating.
    // Prefer it so collaborator accessors resolve to the run actually
    // driving the turn even when the foreground is elsewhere.
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        if (run && run->isAnyConvGenerating()) {
            return run.get();
        }
    }
    return m_activeRun;
}

void ChatController::connectFacade(Chat::ConversationRun* run) {
    if (!run)
        return;

    // Direct facade re-emit (identically-named signal → signal). Each
    // connection is recorded so repointFacade can disconnect them when
    // the active run changes.
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::isGeneratingChanged,
                                 this,
                                 &ChatController::isGeneratingChanged));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::contextFillPercentChanged,
                                 this,
                                 &ChatController::contextFillPercentChanged));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::compactionCadenceChanged,
                                 this,
                                 &ChatController::compactionCadenceChanged));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::isCompactingChanged,
                                 this,
                                 &ChatController::isCompactingChanged));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::summaryDeferredPendingChanged,
                                 this,
                                 &ChatController::compactionWaitingForIdleChanged));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::queuedUserTextChanged,
                                 this,
                                 &ChatController::queuedUserTextChanged));
    m_facadeConns.append(
        connect(run, &Chat::ConversationRun::statsChanged, this, &ChatController::statsChanged));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::toolsEnabledChanged,
                                 this,
                                 &ChatController::toolsEnabledChanged));
    m_facadeConns.append(
        connect(run, &Chat::ConversationRun::errorOccurred, this, &ChatController::errorOccurred));
    m_facadeConns.append(connect(
        run, &Chat::ConversationRun::userMessageQueued, this, &ChatController::userMessageQueued));
    m_facadeConns.append(connect(
        run, &Chat::ConversationRun::toolCallStarted, this, &ChatController::toolCallStarted));
    m_facadeConns.append(connect(
        run, &Chat::ConversationRun::toolCallCompleted, this, &ChatController::toolCallCompleted));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::userMentionedInGroup,
                                 this,
                                 &ChatController::userMentionedInGroup));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::mentionRoutingSuppressed,
                                 this,
                                 &ChatController::mentionRoutingSuppressed));
    m_facadeConns.append(connect(run,
                                 &Chat::ConversationRun::ragpBackendChanged,
                                 this,
                                 &ChatController::ragpBackendChanged));

    // The run's activeConversationChanged drives BOTH facade signals
    // (the pre-extraction code always emitted them as a pair). A run can
    // emit this for a reason other than a user-initiated switch (e.g. it
    // autocreated its conversation on first send, or its conversation was
    // deleted); when it autocreates, m_activeConvId must follow.
    m_facadeConns.append(
        connect(run, &Chat::ConversationRun::activeConversationChanged, this, [this, run]() {
            // Keep the coordinator's key in sync when the active run adopts
            // a new conversation id under the same object (first-send
            // autocreate of "New Chat") so runFor() still resolves it.
            if (run == m_activeRun) {
                rekeyActiveRunIfNeeded();
            }
            emit activeConversationChanged();
            emit activeConversationSettingsChanged();
        }));
}

void ChatController::repointFacade(Chat::ConversationRun* run, const QString& convId) {
    // Tear down the previous active run's facade forwarding so exactly
    // one run drives the facade at a time.
    for (const QMetaObject::Connection& c : m_facadeConns) {
        QObject::disconnect(c);
    }
    m_facadeConns.clear();

    m_activeRun = run;
    m_activeConvId = convId;

    if (run) {
        connectFacade(run);
    }

    // Re-emit every facade change signal so QML rebinds to the newly-
    // active run's state (or to idle/empty defaults when run is nullptr).
    emit activeConversationChanged();
    emit activeConversationSettingsChanged();
    emit isGeneratingChanged();
    emit contextFillPercentChanged();
    emit compactionCadenceChanged();
    emit isCompactingChanged();
    emit compactionWaitingForIdleChanged();
    emit queuedUserTextChanged();
    emit statsChanged();
}

// ---------------------------------------------------------------------------
// Q_INVOKABLE turn entry points — delegate to the run.
// ---------------------------------------------------------------------------

void ChatController::sendMessage(const QString& text, const QVariantList& attachments) {
    Q_UNUSED(attachments)
    Chat::ConversationRun* r = ensureActiveRun();
    if (!r)
        return;
    r->sendMessage(text);
}

void ChatController::sendMessageWithAttachments(const QString& text,
                                                const QStringList& attachmentPaths) {
    Chat::ConversationRun* r = ensureActiveRun();
    if (!r)
        return;
    r->sendMessageWithAttachments(text, attachmentPaths);
}

void ChatController::stopGeneration() {
    if (m_activeRun)
        m_activeRun->stopGeneration();
}

void ChatController::retryLastMessage() {
    if (m_activeRun)
        m_activeRun->retryLastMessage();
}

void ChatController::switchConversation(const QString& id) {
    if (id.isEmpty()) {
        // Clearing the selection: keep any background run alive (it may be
        // mid-stream) but detach the facade.
        repointFacade(nullptr, QString());
        return;
    }
    if (id == m_activeConvId && m_activeRun) {
        return;  // already foreground — no-op, matches prior behaviour
    }

    // Is there already a (background) run for the target conversation —
    // e.g. a turn the user started there and then switched away from?
    // Reactivate it rather than spinning up a duplicate.
    if (Chat::ConversationRun* existing = runFor(id, /*create=*/false)) {
        // Re-key the active run's map slot if it adopted a new id while we
        // weren't looking (defensive — normally already keyed).
        rekeyActiveRunIfNeeded();
        // Load the conversation into the run before repointing so the run's
        // own activeConversationChanged is not double-forwarded; repoint
        // then emits the rebind once. (no-op if already on this conv.)
        existing->switchConversation(id);
        repointFacade(existing, id);
        return;
    }

    // No run exists for the target. The active run drives the foreground;
    // if it is IDLE we reuse it (a single object switching conversations —
    // exactly the pre-coordinator behaviour), re-keying its map slot to the
    // new id. If it is GENERATING, we must NOT disturb its in-flight turn:
    // leave it as a background run bound to its current conversation and
    // create a fresh run for the target.
    if (m_activeRun && !m_activeRun->isAnyConvGenerating()) {
        // Reuse the same run object — a single engine switching its own
        // conversation, exactly the pre-coordinator behaviour. The active
        // run's facade forwarding is already wired; we do NOT repoint (that
        // would disconnect/reconnect and double-emit). The run emits its
        // own activeConversationChanged, which the live facade forwarding
        // re-emits ONCE and re-keys the map (rekeyActiveRunIfNeeded in the
        // connectFacade lambda).
        m_activeConvId = id;  // pre-set so the forward lambda re-keys to id
        rekeyRun(m_activeRun->activeConvId(), id, m_activeRun);
        m_activeRun->switchConversation(id);
        // Keep the field authoritative even if the run short-circuited
        // (e.g. id already equalled its internal active conv).
        m_activeConvId = m_activeRun->activeConvId();
    } else {
        // Active run is mid-stream: leave it as a background run on its own
        // conversation and bring up a fresh engine for the target. Load the
        // target conversation into the new run BEFORE repointing so the
        // run's own activeConversationChanged is not yet forwarded (the
        // facade is still attached to the previous run); repointFacade then
        // emits the rebind exactly once.
        Chat::ConversationRun* target = createRun(id);
        target->switchConversation(id);
        repointFacade(target, id);
    }
}

void ChatController::compactNow() {
    if (m_activeRun)
        m_activeRun->compactNow();
}

Chat::ConversationRun* ChatController::ensureActiveRun() {
    if (m_activeRun)
        return m_activeRun;
    // Defensive: the constructor always installs an initial active run, so
    // this only fires after a clearing switchConversation("") with no
    // background run to reactivate. Recreate an empty active run.
    Chat::ConversationRun* r = createRun(QString());
    repointFacade(r, QString());
    return r;
}

void ChatController::rekeyRun(const QString& oldKey,
                              const QString& newKey,
                              Chat::ConversationRun* run) {
    if (oldKey == newKey)
        return;
    auto node = m_runs.find(oldKey);
    if (node == m_runs.end() || node->second.get() != run)
        return;
    std::unique_ptr<Chat::ConversationRun> moved = std::move(node->second);
    m_runs.erase(node);
    m_runs.emplace(newKey, std::move(moved));
}

void ChatController::rekeyActiveRunIfNeeded() {
    if (!m_activeRun)
        return;
    const QString runConv = m_activeRun->activeConvId();
    if (runConv != m_activeConvId) {
        rekeyRun(m_activeConvId, runConv, m_activeRun);
        m_activeConvId = runConv;
    }
}

// ---------------------------------------------------------------------------
// Property accessors — delegate to the run.
// ---------------------------------------------------------------------------

QString ChatController::activeConversationId() const {
    return m_activeRun ? m_activeRun->activeConvId() : QString();
}

QString ChatController::activeConversationTitle() const {
    return m_activeRun ? m_activeRun->activeConvTitle() : QString();
}

void ChatController::setActiveConversationId(const QString& id) {
    switchConversation(id);
}

bool ChatController::isGenerating() const {
    return m_activeRun ? m_activeRun->isGenerating() : false;
}

MessageListModel* ChatController::messages() const {
    return m_msgModel;
}

QString ChatController::queuedUserText() const {
    return m_activeRun ? m_activeRun->queuedUserText() : QString();
}

int ChatController::contextFillPercent() const {
    return m_activeRun ? m_activeRun->contextFillPercent() : 0;
}

int ChatController::compactionTurnsUsed() const {
    return m_activeRun ? m_activeRun->compactionTurnsUsed() : 0;
}

int ChatController::compactionTurnsTotal() const {
    return m_activeRun ? m_activeRun->compactionTurnsTotal() : 0;
}

bool ChatController::isCompacting() const {
    return m_activeRun ? m_activeRun->isCompacting() : false;
}

bool ChatController::compactionWaitingForIdle() const {
    return m_activeRun ? m_activeRun->summaryDeferredPending() : false;
}

int ChatController::totalTokens() const {
    return m_activeRun ? m_activeRun->totalTokens() : 0;
}
double ChatController::estimatedCostUsd() const {
    return m_activeRun ? m_activeRun->estimatedCostUsd() : 0.0;
}
int ChatController::lastResponseTimeMs() const {
    return m_activeRun ? m_activeRun->lastResponseTimeMs() : 0;
}
int ChatController::messageCount() const {
    return m_msgModel ? m_msgModel->count() : 0;
}

int ChatController::runCountForTest() const {
    return static_cast<int>(m_runs.size());
}

bool ChatController::isConversationGenerating(const QString& convId) const {
    if (convId.isEmpty())
        return false;
    // Background-aware aggregate: a turn may be in flight on a run keyed
    // by `convId` (the common case), but switchConversation can leave a
    // generating run bound to its conversation while a fresh run takes
    // the foreground for a different convId. Scan every run and ask each
    // whether its in-flight turn targets `convId`. hasInflightStreamFor
    // already gates on the run's own m_isGenerating, so an idle run never
    // matches.
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        if (run && run->hasInflightStreamFor(convId))
            return true;
    }
    return false;
}

bool ChatController::hasInflightStreamFor(const QString& convId) const {
    Chat::ConversationRun* r = const_cast<ChatController*>(this)->runFor(convId, /*create=*/false);
    return r ? r->hasInflightStreamFor(convId) : false;
}

bool ChatController::isAnyConvGenerating() const {
    // True when ANY run on this instance holds the (serialized) slot —
    // SessionRouter needs the unmasked truth for cross-instance gating.
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        if (run && run->isAnyConvGenerating())
            return true;
    }
    return false;
}

QString ChatController::activeTaskPlanId() const {
    return m_activeRun ? m_activeRun->activeTaskPlanId() : QString();
}

// ---------------------------------------------------------------------------
// External-service coordination slots — route to the OWNING run.
//
// Conversation-scoped events go to the run that owns the named
// conversation (creating it when the event implies the conversation is
// becoming live, e.g. a task start / sub-agent reaction). Settings-mirror
// slots and "drain" pumps fan out to every run so a background run's
// per-turn caches and queued work stay correct. With a single active
// conversation every fan-out is a single delegation — byte-identical.
// ---------------------------------------------------------------------------

void ChatController::onExternalAgentPatternChanged(const QString& patternName) {
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        run->onExternalAgentPatternChanged(patternName);
    }
}

void ChatController::onExternalRequireConfirmationChanged(bool require) {
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        run->onExternalRequireConfirmationChanged(require);
    }
}

void ChatController::onExternalToolsEnabledChanged(bool enabled) {
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        run->onExternalToolsEnabledChanged(enabled);
    }
}

void ChatController::drainQueuedSendIfPossible() {
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        run->drainQueuedSendIfPossible();
    }
}

void ChatController::onSlotAvailable() {
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        run->onSlotAvailable();
    }
}

void ChatController::onActiveCanvasMetadataRefresh(const QString& filename,
                                                   const QString& language,
                                                   int revision,
                                                   int lineCount,
                                                   qint64 byteSize) {
    // Canvas metadata follows the active conversation (the editor binds to
    // the foreground chat).
    if (m_activeRun) {
        m_activeRun->onActiveCanvasMetadataRefresh(
            filename, language, revision, lineCount, byteSize);
    }
}

void ChatController::onExternalTaskStarted(const QString& planId,
                                           const QString& convId,
                                           const QString& ownerAlias,
                                           const QString& ownerAgentId) {
    Chat::ConversationRun* r = runFor(convId, /*createIfMissing=*/true);
    if (r)
        r->onExternalTaskStarted(planId, convId, ownerAlias, ownerAgentId);
}

void ChatController::onExternalPlanStopped(const QString& planId) {
    // The stopped plan is anchored to whichever run holds it; the shared
    // TaskGateService makes the active run authoritative, but a background
    // run may be mid-stream on that plan — fan out so the right one stops.
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        run->onExternalPlanStopped(planId);
    }
}

void ChatController::onExternalAllPlansStoppedInConversation(const QString& convId) {
    Chat::ConversationRun* r = runFor(convId, /*createIfMissing=*/false);
    if (r)
        r->onExternalAllPlansStoppedInConversation(convId);
}

void ChatController::onExternalConversationAboutToBeDeleted(const QString& id) {
    Chat::ConversationRun* r = runFor(id, /*createIfMissing=*/false);
    if (r)
        r->onExternalConversationAboutToBeDeleted(id);
}

void ChatController::onExternalConversationDeleted(const QString& id) {
    Chat::ConversationRun* r = runFor(id, /*createIfMissing=*/false);
    if (r) {
        r->onExternalConversationDeleted(id);
    }
    // Reap the deleted conversation's run. If it was the active run, the
    // run cleared its own view state and detached the message model; drop
    // the facade so getters return idle defaults.
    const bool wasActive = (id == m_activeConvId);
    if (wasActive) {
        repointFacade(nullptr, QString());
    }
    m_runs.erase(id);  // unique_ptr destroys the run (a child QObject)
}

void ChatController::enqueueTaskDispatch(const TaskDispatchRequest& req) {
    Chat::ConversationRun* r = runFor(req.convId, /*createIfMissing=*/false);
    if (!r)
        r = m_activeRun;  // legacy queue is drained-and-dropped anyway
    if (r)
        r->enqueueTaskDispatch(req);
}

void ChatController::drainTaskDispatchQueue() {
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        run->drainTaskDispatchQueue();
    }
}

void ChatController::dispatchSubagentReaction(const QString& convId,
                                              const QString& requesterAlias) {
    Chat::ConversationRun* r = runFor(convId, /*createIfMissing=*/true);
    if (r)
        r->dispatchSubagentReaction(convId, requesterAlias);
}

void ChatController::drainPendingSubagentReactions() {
    for (const auto& kv : m_runs) {
        const std::unique_ptr<Chat::ConversationRun>& run = kv.second;
        run->drainPendingSubagentReactions();
    }
}

void ChatController::onTaskArtifactReady(const QString& convId,
                                         const QString& agentId,
                                         const QString& alias,
                                         const QString& artifactContent,
                                         const QString& summary,
                                         const QString& stepId,
                                         const QString& planId) {
    Chat::ConversationRun* r = runFor(convId, /*createIfMissing=*/true);
    if (r) {
        r->onTaskArtifactReady(convId, agentId, alias, artifactContent, summary, stepId, planId);
    }
}

void ChatController::onImageReadyForFollowUp(const QString& convId,
                                             const QString& alias,
                                             const QString& agentId,
                                             const QString& imagePath) {
    // createIfMissing: the completion may target a conversation whose
    // run was never activated this session — the resume guards inside
    // the run decide whether a dispatch is appropriate.
    Chat::ConversationRun* r = runFor(convId, /*createIfMissing=*/true);
    if (r) {
        r->resumeAfterImageGenerated(convId, alias, agentId, imagePath);
    }
}

// ---------------------------------------------------------------------------
// Service attachment — store in m_services (so lazily-created runs inherit
// it) AND forward to every existing run. At startup only the initial run
// exists, so each forward is a single call — byte-identical to before.
// ---------------------------------------------------------------------------

void ChatController::setToolService(ToolService* toolService) {
    m_services.toolService = toolService;
    for (const auto& kv : m_runs)
        kv.second->setToolService(toolService);
    qCInfo(verzetaUi) << "ChatController: ToolService" << (toolService ? "attached" : "detached");
}

void ChatController::setRagService(RagService* ragService) {
    m_services.ragService = ragService;
    for (const auto& kv : m_runs)
        kv.second->setRagService(ragService);
    qCInfo(verzetaUi) << "ChatController: RagService" << (ragService ? "attached" : "detached");
}

void ChatController::setMemoryRetriever(Chat::MemoryRetriever* retriever) {
    m_services.memoryRetriever = retriever;
    for (const auto& kv : m_runs)
        kv.second->setMemoryRetriever(retriever);
    qCInfo(verzetaUi) << "ChatController: MemoryRetriever" << (retriever ? "attached" : "detached");
}

void ChatController::setFileService(FileService* fileService) {
    m_fileService = fileService;  // resident copy for activeArtifactsPath
    m_services.fileService = fileService;
    for (const auto& kv : m_runs)
        kv.second->setFileService(fileService);
}

void ChatController::setAgentRegistry(AgentRegistry* registry) {
    if (m_msgModel) {
        m_msgModel->setAgentRegistry(registry);
    }
    m_services.agentRegistry = registry;
    for (const auto& kv : m_runs)
        kv.second->setAgentRegistry(registry);
}

void ChatController::setMembershipService(MembershipService* svc) {
    m_services.membershipService = svc;
    for (const auto& kv : m_runs)
        kv.second->setMembershipService(svc);
}

void ChatController::setPlanService(PlanService* svc) {
    m_services.planService = svc;
    for (const auto& kv : m_runs)
        kv.second->setPlanService(svc);
}

void ChatController::setTaskRunner(TaskRunner* runner) {
    m_services.taskRunner = runner;
    for (const auto& kv : m_runs)
        kv.second->setTaskRunner(runner);
}

void ChatController::setTaskObserver(TaskObserver* observer) {
    m_services.taskObserver = observer;
    for (const auto& kv : m_runs)
        kv.second->setTaskObserver(observer);
}

void ChatController::setSkillService(SkillService* svc) {
    m_services.skillService = svc;
    for (const auto& kv : m_runs)
        kv.second->setSkillService(svc);
}

void ChatController::setHeartbeatConfigService(HeartbeatConfigService* svc) {
    m_services.heartbeatConfigService = svc;
    for (const auto& kv : m_runs)
        kv.second->setHeartbeatConfigService(svc);
}

void ChatController::setPollService(PollService* svc) {
    m_services.pollService = svc;
    for (const auto& kv : m_runs)
        kv.second->setPollService(svc);
}

void ChatController::setAuditService(AuditService* svc) {
    m_services.auditService = svc;
    for (const auto& kv : m_runs)
        kv.second->setAuditService(svc);
}

void ChatController::setSessionRouter(Verzeta::Session::SessionRouter* router) {
    m_services.sessionRouter = router;
    for (const auto& kv : m_runs)
        kv.second->setSessionRouter(router);
}

void ChatController::setTaskGateService(TaskGateService* svc) {
    m_services.taskGate = svc;
    for (const auto& kv : m_runs)
        kv.second->setTaskGateService(svc);
    qCInfo(verzetaUi) << "ChatController: TaskGateService" << (svc ? "attached" : "detached");
}

void ChatController::setSlashCommandService(SlashCommandService* svc) {
    m_services.slashService = svc;
    for (const auto& kv : m_runs)
        kv.second->setSlashCommandService(svc);
    qCInfo(verzetaUi) << "ChatController: SlashCommandService" << (svc ? "attached" : "detached");
}

void ChatController::setCanvasService(CanvasService* svc) {
    m_services.canvasSvc = svc;
    for (const auto& kv : m_runs)
        kv.second->setCanvasService(svc);
    qCInfo(verzetaUi) << "ChatController: CanvasService" << (svc ? "attached" : "detached");
}

void ChatController::setAgentService(AgentService* agentService) {
    m_services.agentService = agentService;
    for (const auto& kv : m_runs)
        kv.second->setAgentService(agentService);
    qCInfo(verzetaUi) << "ChatController: AgentService" << (agentService ? "attached" : "detached");
}

void ChatController::setSummarizer(ConversationSummarizer* summarizer) {
    m_services.summarizer = summarizer;
    for (const auto& kv : m_runs)
        kv.second->setSummarizer(summarizer);
}

void ChatController::setProviderScheduler(Chat::ProviderScheduler* scheduler) {
    m_services.providerScheduler = scheduler;
    for (const auto& kv : m_runs)
        kv.second->setProviderScheduler(scheduler);
    qCInfo(verzetaUi) << "ChatController: ProviderScheduler"
                      << (scheduler ? "attached" : "detached");
}

void ChatController::setInferenceSidecarHost(Verzeta::Infer::InferenceSidecarHost* host) {
    m_services.inferenceSidecarHost = host;
    for (const auto& kv : m_runs)
        kv.second->setInferenceSidecarHost(host);
}

// ---------------------------------------------------------------------------
// RAGP backend — delegate to every run (each CascadeController owns its own
// Ragp::Service). Store the settings so lazily-created runs configure too.
// ---------------------------------------------------------------------------

void ChatController::configureRagpBackend(SettingsService* settings) {
    m_services.ragpSettings = settings;
    for (const auto& kv : m_runs)
        kv.second->configureRagpBackend(settings);
}

QString ChatController::ragpBackendName() const {
    return m_activeRun ? m_activeRun->ragpBackendName() : QString();
}

Ragp::Service* ChatController::ragpService() const {
    return m_activeRun ? m_activeRun->ragpService() : nullptr;
}

// ---------------------------------------------------------------------------
// Internal-collaborator accessors (AppController wiring + tests).
//
// These resolve to the run currently driving a turn (or the active run
// when idle) at CALL time. External consumers — TaskController task tools
// and HeartbeatSubagentService — now hold a resolver that calls back into
// these accessors per invocation (rather than a value captured once at
// startup), so a task tool / heartbeat firing on a backgrounded run while
// another conversation is foreground resolves the RIGHT run's cascade.
// ---------------------------------------------------------------------------

Chat::CascadeController* ChatController::cascadeInternal() const {
    Chat::ConversationRun* r = inflightOrActiveRun();
    return r ? r->cascadeInternal() : nullptr;
}

Chat::StreamingManager* ChatController::streamingInternal() const {
    Chat::ConversationRun* r = inflightOrActiveRun();
    return r ? r->streamingInternal() : nullptr;
}

Chat::ActionIntentConfirmer* ChatController::intentConfirmerInternal() const {
    Chat::ConversationRun* r = inflightOrActiveRun();
    return r ? r->intentConfirmerInternal() : nullptr;
}

QList<LlmMessage> ChatController::assembleLlmHistory(const QList<Message>& dbMessages,
                                                     bool isGroupChat,
                                                     const QString& excludeMsgId) {
    if (!m_activeRun)
        return {};
    return m_activeRun->assembleLlmHistory(dbMessages, isGroupChat, excludeMsgId);
}

// ---------------------------------------------------------------------------
// Conversation-scoped reads that stay resident (no per-turn state).
// ---------------------------------------------------------------------------

QString ChatController::lastAssistantMessage() const {
    const QString convId = activeConversationId();
    if (convId.isEmpty())
        return {};
    const QList<Message> msgs = m_msgSvc.getRecentMessages(convId, 30);
    for (int i = msgs.size() - 1; i >= 0; --i) {
        if (msgs[i].role == QStringLiteral("assistant")) {
            return msgs[i].content;
        }
    }
    return {};
}

QString ChatController::activeArtifactsPath() const {
    const QString convId = activeConversationId();
    if (convId.isEmpty() || !m_fileService)
        return {};

    // Project-scoped directory when the conversation lives inside a
    // project/organization folder; otherwise the per-conversation dir.
    const QList<Folder> chain =
        const_cast<ConversationService&>(m_convSvc).folderChainForConversation(convId);
    for (const Folder& f : chain) {
        if (f.isProject()) {
            m_fileService->setActiveProjectContext(f.id, f.name);
            return m_fileService->activeProjectDir();
        }
    }

    m_fileService->setActiveConversation(convId);
    return m_fileService->activeProjectDir();
}

void ChatController::openActiveArtifactsFolder() {
    const QString path = activeArtifactsPath();
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool ChatController::openPathExternally(const QString& absolutePath) {
    if (absolutePath.isEmpty())
        return false;
    const QFileInfo info(absolutePath);
    // Only hand existing regular files to the OS — this is a file
    // opener, not a URL launcher; refusing directories/URIs keeps the
    // surface narrow.
    if (!info.exists() || !info.isFile()) {
        qCWarning(verzetaUi) << "ChatController::openPathExternally: not an existing file:"
                             << absolutePath;
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
}

QString ChatController::activeConversationScopeLabel() const {
    const QString convId = activeConversationId();
    if (convId.isEmpty())
        return {};
    const auto convOpt = m_convSvc.getConversation(convId);
    if (!convOpt.has_value())
        return {};

    const QString folderId = convOpt->folderId;
    if (folderId.isEmpty()) {
        return convOpt->isGroup ? QStringLiteral("standalone group chat (no folder)")
                                : QStringLiteral("standalone chat (no folder)");
    }

    const QList<Folder> chain = m_convSvc.folderChainForConversation(convId);
    if (chain.isEmpty())
        return QStringLiteral("in an unknown folder");

    QStringList parts;
    for (const Folder& f : chain) {
        QString kind;
        if (f.folderType == QStringLiteral("project"))
            kind = QStringLiteral("project");
        else if (f.folderType == QStringLiteral("organization"))
            kind = QStringLiteral("organization");
        else
            kind = QStringLiteral("folder");
        parts.append(QStringLiteral("%1: %2").arg(kind, f.name));
    }
    return QStringLiteral("in ") + parts.join(QStringLiteral(" → "));
}

QString ChatController::activeConversationFolderId() const {
    const QString convId = activeConversationId();
    if (convId.isEmpty())
        return {};
    const auto convOpt = m_convSvc.getConversation(convId);
    return convOpt.has_value() ? convOpt->folderId : QString();
}

LlmConfig ChatController::activeConvConfig() const {
    const QString convId = activeConversationId();
    if (convId.isEmpty()) {
        return LlmConfig{};
    }
    const auto conv = m_convSvc.getConversation(convId);
    if (!conv.has_value()) {
        return LlmConfig{};
    }
    LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
    if (cfg.temperature == 0.0 && cfg.maxTokens == 0) {
        cfg.temperature = 0.7;
        cfg.maxTokens = 4096;
        cfg.stream = true;
        cfg.thinkingMode = false;
    }
    return cfg;
}

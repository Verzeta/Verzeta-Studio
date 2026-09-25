// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file conversation-run.cpp
 * @brief Implementation of ConversationRun, the per-turn chat engine
 *        extracted out of ChatController. Owns the send → stream →
 *        tools → cascade → finalize lifecycle and all per-turn state.
 * @layer Service (Chat subsystem)
 * @dependencies See conversation-run.h.
 *
 * Behaviour note: every method body here was relocated verbatim from
 * the former ChatController implementation, with member references and
 * facade-signal emissions remapped onto this class. The wiring is
 * identical (no Qt::UniqueConnection-with-lambda is introduced), so
 * single-chat behaviour is byte-for-byte unchanged.
 *
 * Security: tool loops capped at Chat::ToolDispatcher::kMaxToolIterations
 * per user message; the LLM cannot recurse indefinitely on tool_calls.
 */

#include "conversation-run.h"

#include "../../api/llm-interface.h"
#include "../../models/activity-event.h"
#include "../../models/conversation.h"
#include "../../models/llm-config.h"
#include "../../models/member.h"
#include "../../models/message-list-model.h"
#include "../../models/message.h"
#include "../../models/tool-call.h"
#include "../../utils/contention-ledger.h"
#include "../../utils/http-client.h"
#include "../../utils/logger.h"
#include "../../utils/markdown-utils.h"
#include "../../utils/thread-discipline.h"
#include "../agent-registry.h"
#include "../agent-service.h"
#include "../audit-service.h"
#include "../canvas-service.h"
#include "../conversation-service.h"
#include "../conversation-summarizer.h"
#include "../file-service.h"
#include "../membership-service.h"
#include "../message-service.h"
#include "../model-router.h"
#include "../plan-service.h"
#include "../rag-service.h"
#include "../ragp/ragp-service.h"
#include "../session/session-router.h"
#include "../settings-service.h"
#include "../skill-service.h"
#include "../slash-command-service.h"
#include "../task-gate-service.h"
#include "../task-observer.h"
#include "../task-runner.h"
#include "../tool-service.h"
#include "action-intent-confirmer.h"
#include "cascade-controller.h"
#include "content-sanitizer.h"
#include "memory-retriever.h"
#include "provider-scheduler.h"
#include "request-builder.h"
#include "streaming-manager.h"
#include "tool-dispatcher.h"

#include <QTimer>

#include <algorithm>
#include <memory>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace {
// === STREAM-TRACE DEBUG GROUP ===
// Env-gated diagnostics (VERZETA_STREAM_TRACE=1) for the truncation /
// repetition investigation arcs. Zero-cost when the variable is unset.
inline bool streamTraceEnabled() {
    static const bool s_enabled = qEnvironmentVariableIntValue("VERZETA_STREAM_TRACE") > 0;
    return s_enabled;
}

/// Group deferred-action continuation budget, per alias per user round
/// (shared by the classify-verdict slot and the pre-routing snapshot).
constexpr int kMaxGroupDeferredPerAlias = 2;

/// Async-image follow-up budget per user round: how many idle-resume
/// turns completed image generations may dispatch before the next user
/// message. Bounds the image→turn→image loop a stubborn model could
/// otherwise sustain indefinitely.
constexpr int kMaxAsyncArtifactContinuations = 2;
}  // namespace

namespace Chat {


quint64 ConversationRun::nextGlobalRequestId() {
    // Single process-wide source shared with the agent path (see header).
    return ModelRouter::nextRequestId();
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ConversationRun::ConversationRun(ModelRouter& router,
                                 ConversationService& convSvc,
                                 MessageService& msgSvc,
                                 MessageListModel* msgModel,
                                 QObject* parent)
    : QObject(parent)
    , m_router(router)
    , m_convSvc(convSvc)
    , m_msgSvc(msgSvc)
    , m_msgModel(msgModel)
    , m_requestBuilder(std::make_unique<Chat::RequestBuilder>(convSvc, msgSvc, router, this))
    , m_streaming(std::make_unique<Chat::StreamingManager>(msgSvc, this))
    , m_toolDispatcher(std::make_unique<Chat::ToolDispatcher>(msgSvc, convSvc, this))
    , m_cascade(std::make_unique<Chat::CascadeController>(convSvc, router, this))
    , m_sanitizer(std::make_unique<Chat::ContentSanitizer>(this)) {
    // ModelRouter streaming relay.
    connect(&m_router, &ModelRouter::chunkReceived, this, &ConversationRun::onChunkReceived);
    connect(&m_router, &ModelRouter::requestFinished, this, &ConversationRun::onRequestFinished);
    connect(&m_router, &ModelRouter::requestError, this, &ConversationRun::onRequestError);

    // ToolDispatcher feedback.
    connect(m_toolDispatcher.get(),
            &Chat::ToolDispatcher::toolCallStarted,
            this,
            &ConversationRun::toolCallStarted);
    // Deferred-action fire-awareness: record every call executed in the
    // current turn-chain so the gate never re-nudges a responder for work
    // it already did (announced-vs-executed comparison + truthful confirm
    // context). Cleared on a fresh user send and on every new member turn.
    connect(
        m_toolDispatcher.get(),
        &Chat::ToolDispatcher::toolCallStarted,
        this,
        [this](const QString& toolName, const QString& argsJson) {
            m_chainExecutedToolNames.insert(toolName.toLower());
            const QJsonDocument doc = QJsonDocument::fromJson(argsJson.toUtf8());
            if (doc.isObject()) {
                const QJsonObject o = doc.object();
                for (const QString& key :
                     {QStringLiteral("filename"), QStringLiteral("path"), QStringLiteral("file")}) {
                    QString f = o.value(key).toString().toLower();
                    const int slash = f.lastIndexOf(QLatin1Char('/'));
                    if (slash >= 0)
                        f = f.mid(slash + 1);
                    if (!f.isEmpty() && f.contains(QLatin1Char('.'))) {
                        m_chainExecutedFiles.insert(f);
                    }
                }
            }
        });
    connect(m_toolDispatcher.get(),
            &Chat::ToolDispatcher::toolCallCompleted,
            this,
            &ConversationRun::toolCallCompleted);
    connect(m_toolDispatcher.get(),
            &Chat::ToolDispatcher::batchCompletedContinueTurn,
            this,
            &ConversationRun::onToolBatchCompleted);
    connect(m_toolDispatcher.get(),
            &Chat::ToolDispatcher::requestTurnTargetQueued,
            this,
            &ConversationRun::onToolDispatcherRequestTurnTarget);
    connect(m_toolDispatcher.get(),
            &Chat::ToolDispatcher::activeTaskAutoAnchored,
            this,
            &ConversationRun::onToolDispatcherActiveTaskAutoAnchored);
    connect(m_toolDispatcher.get(),
            &Chat::ToolDispatcher::batchFailed,
            this,
            &ConversationRun::onToolDispatcherBatchFailed);

    // CascadeController feedback.
    connect(m_cascade.get(),
            &Chat::CascadeController::memberTurnStarted,
            this,
            &ConversationRun::onCascadeMemberTurnStarted);
    connect(m_cascade.get(),
            &Chat::CascadeController::cascadeComplete,
            this,
            &ConversationRun::onCascadeComplete);
    connect(m_cascade.get(),
            &Chat::CascadeController::userMentionedInGroup,
            this,
            &ConversationRun::userMentionedInGroup);
    // Relay @user mentions onto the shared MessageService spine so wire
    // clients receive chat.user_mentioned; the local signal above keeps
    // driving the desktop banner.
    connect(m_cascade.get(),
            &Chat::CascadeController::userMentionedInGroup,
            this,
            [this](const QString& convId, const QString& alias, const QString& text) {
                m_msgSvc.notifyUserMentioned(convId, alias, text);
            });
    connect(m_cascade.get(),
            &Chat::CascadeController::mentionRoutingSuppressed,
            this,
            &ConversationRun::mentionRoutingSuppressed);
    // Round-continuation visibility: both outcomes of a cap-ended round
    // insert a first-class system message so the user (and the next LLM
    // turns) can SEE what happened instead of a silent stall.
    connect(m_cascade.get(),
            &Chat::CascadeController::roundContinued,
            this,
            [this](const QString& convId, int round, const QString& nextAlias) {
                Message m;
                m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                m.conversationId = convId;
                m.role = QStringLiteral("system");
                m.createdAt = QDateTime::currentDateTimeUtc();
                m.content = QStringLiteral("▶ Turn limits reached, continuing automatically "
                                           "(round %1/%2), next: @%3")
                                .arg(round)
                                .arg(Chat::CascadeController::kMaxAutoRounds)
                                .arg(nextAlias);
                m_msgSvc.addMessage(m);
            });
    connect(m_cascade.get(),
            &Chat::CascadeController::cascadePausedAtCap,
            this,
            [this](const QString& convId, const QString& refusedAlias, const QString& reason) {
                Message m;
                m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                m.conversationId = convId;
                m.role = QStringLiteral("system");
                m.createdAt = QDateTime::currentDateTimeUtc();
                m.content = reason == QStringLiteral("no_progress")
                                ? QStringLiteral("⏸ Agent turn limits reached with @%1 still "
                                                 "queued. This round ran no tools, so the team "
                                                 "is pausing to avoid a loop. "
                                                 "Reply with anything (for example \"continue\") "
                                                 "to start a new round.")
                                      .arg(refusedAlias)
                                : QStringLiteral("⏸ %1 automatic rounds completed; @%2 is "
                                                 "still queued. Reply to continue.")
                                      .arg(Chat::CascadeController::kMaxAutoRounds)
                                      .arg(refusedAlias);
                m_msgSvc.addMessage(m);
            });
    connect(m_cascade.get(),
            &Chat::CascadeController::ragpBackendChanged,
            this,
            &ConversationRun::ragpBackendChanged);
    connect(m_cascade.get(),
            &Chat::CascadeController::echoDetectedRequestRetry,
            this,
            &ConversationRun::onCascadeEchoDetectedRequestRetry);
    connect(m_cascade.get(),
            &Chat::CascadeController::echoMaxRetriesExhausted,
            this,
            &ConversationRun::onCascadeEchoMaxRetriesExhausted);
    connect(m_cascade.get(),
            &Chat::CascadeController::deferredContinuationRequested,
            this,
            &ConversationRun::onCascadeDeferredContinuationRequested);

    // Stable per-run identity for the ProviderScheduler's queue
    // membership + cancel(). Unique per ConversationRun instance for the
    // process lifetime (a deleted+recreated run for the same conversation
    // id gets a fresh waiter id, so a stale queued grant can never target
    // a different live run).
    m_schedulerWaiterId = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

ConversationRun::~ConversationRun() {
    // Last-resort cleanup: if this run is destroyed while holding or
    // waiting on a provider slot (e.g. ChatController torn down mid-turn),
    // free the scheduler state so the provider does not wedge and no
    // queued grant fires into a dead run.
    releaseProviderSlotIfHeld();
    cancelQueuedSchedulerWaiter();
}

// ---------------------------------------------------------------------------
// Optional-service attachment
// ---------------------------------------------------------------------------

void ConversationRun::setToolService(ToolService* svc) {
    m_toolService = svc;
    ensureOwnAgent();  // may now have all deps for the owned executor
}
void ConversationRun::setRagService(RagService* svc) {
    m_ragService = svc;
    // Pre-turn RAG recall now runs through MemoryRetriever (the unified
    // coordinator), not RagService::retrieveAsync directly — see
    // setMemoryRetriever(). RagService is still attached here for the agent
    // pattern (MemoryAugmented) + search consumers.
    ensureOwnAgent();  // may now have all deps for the owned executor
}
void ConversationRun::setMemoryRetriever(MemoryRetriever* r) {
    m_memoryRetriever = r;
    if (r) {
        // Signal-driven async continuation. PMF connect → UniqueConnection is
        // safe (no lambda) and idempotent across repeat setMemoryRetriever().
        connect(r,
                &MemoryRetriever::retrievalReady,
                this,
                &ConversationRun::onMemoryRetrievalReady,
                Qt::UniqueConnection);
    }
}
void ConversationRun::setFileService(FileService* svc) {
    m_fileService = svc;
}
void ConversationRun::setMembershipService(MembershipService* svc) {
    m_membershipService = svc;
    m_cascade->setMembershipService(svc);
}
void ConversationRun::setPlanService(PlanService* svc) {
    m_planService = svc;
}
void ConversationRun::setTaskRunner(TaskRunner* runner) {
    m_taskRunner = runner;
}
void ConversationRun::setTaskObserver(TaskObserver* observer) {
    m_taskObserver = observer;
}
void ConversationRun::setSkillService(SkillService* svc) {
    m_skillService = svc;
}
void ConversationRun::setHeartbeatConfigService(HeartbeatConfigService* svc) {
    m_heartbeatConfigService = svc;
}
void ConversationRun::setPollService(PollService* svc) {
    m_pollService = svc;
}
void ConversationRun::setSessionRouter(Verzeta::Session::SessionRouter* router) {
    m_sessionRouter = router;
}
void ConversationRun::setTaskGateService(TaskGateService* svc) {
    m_taskGate = svc;
}
void ConversationRun::setSlashCommandService(SlashCommandService* svc) {
    m_slashService = svc;
}
void ConversationRun::setCanvasService(CanvasService* svc) {
    m_canvasSvc = svc;
}

void ConversationRun::setAgentRegistry(AgentRegistry* registry) {
    m_agentRegistry = registry;
}

void ConversationRun::setAuditService(AuditService* svc) {
    m_auditService = svc;
    // Forward to the internally-owned ToolDispatcher so the audit
    // recipient is wired in ONE place — both the agent_turn record path
    // here AND ToolDispatcher's tool_invoked / file_written /
    // canvas_edited records share this pointer.
    if (m_toolDispatcher) {
        m_toolDispatcher->setAuditService(svc);
    }
}

void ConversationRun::setSummarizer(ConversationSummarizer* summarizer) {
    m_summarizer = summarizer;
    if (!m_summarizer)
        return;

    // Mirror the summarizer's single-flight busy state so the compact
    // gauge can animate while compaction is in flight.
    // NB: Qt::UniqueConnection is NOT permitted with a functor/lambda slot
    // (it requires a pointer-to-member) — setSummarizer is called once, so
    // a plain connection is correct here.
    connect(m_summarizer,
            &ConversationSummarizer::compactingChanged,
            this,
            [this](const QString& convId, bool busy) {
                m_summarizerBusy = busy;
                m_compactingConvId = busy ? convId : QString();
                emit isCompactingChanged();
            });

    // A conversation switch can move the foreground into / out of the
    // conversation being compacted — re-notify so the indicator follows.
    connect(this,
            &ConversationRun::activeConversationChanged,
            this,
            &ConversationRun::isCompactingChanged,
            Qt::UniqueConnection);
}

void ConversationRun::setProviderScheduler(Chat::ProviderScheduler* scheduler) {
    m_providerScheduler = scheduler;
}

void ConversationRun::setInferenceSidecarHost(Verzeta::Infer::InferenceSidecarHost* host) {
    if (m_cascade)
        m_cascade->setInferenceSidecarHost(host);
}

bool ConversationRun::ensureOwnAgent() {
    if (m_ownAgent) {
        // Already built — just (re)register with the facade if one is now
        // attached and we have not yet done so. registerRelaySource is
        // idempotent (ignores an already-registered source).
        if (m_agentService) {
            m_agentService->registerRelaySource(m_ownAgent.get());
        }
        return true;
    }
    // AgentService requires ModelRouter (always present), ToolService and
    // RagService. Prefer the run's own attached services; fall back to the
    // shared facade's services (it was built from the same app-level
    // ToolService / RagService). This keeps existing wiring working: in
    // production AppController attaches both to every run AND attaches the
    // facade; some tests attach only the facade. Both services are
    // stateless/shared (no per-turn state), so sourcing them from the
    // facade is equivalent to the pre-P4-4b shared executor.
    ToolService* tools = m_toolService;
    RagService* rag = m_ragService;
    if ((!tools || !rag) && m_agentService) {
        if (!tools)
            tools = &m_agentService->toolService();
        if (!rag)
            rag = &m_agentService->ragService();
    }
    if (!tools || !rag)
        return false;

    m_ownAgent = std::make_unique<AgentService>(m_router, *tools, *rag, this);
    AgentService* agent = m_ownAgent.get();

    // Forward agent chunks to the streaming message in the model.
    connect(agent, &AgentService::chunkReceived, this, [this](const LlmChunk& chunk) {
        if (!m_usingAgent || !m_streaming->isStreaming())
            return;
        if (!chunk.delta.isEmpty()) {
            m_streaming->appendChunk(chunk.delta);
        }
        // Mirror the foreground path's reasoning capture so an agent
        // turn that produces thinking lands the same disclosure row.
        m_streaming->appendThinkingChunk(chunk.thinkingDelta);
    });

    // Agent finished — finalize the streaming message.
    connect(agent, &AgentService::finished, this, [this](const QString& result) {
        if (!m_usingAgent)
            return;

        const qint64 elapsed = m_responseTimer.elapsed();

        if (!m_streaming->wasPersisted() && m_streaming->isStreaming()) {
            const QJsonObject metadata{
                {QStringLiteral("elapsed_ms"), elapsed},
                {QStringLiteral("agent_pattern"), static_cast<int>(m_agentPattern)},
            };
            m_streaming->rewrite(result);
            m_streaming->finalize(
                0, QStringLiteral("stop"), QString(), m_currentDispatchedModelName, metadata);
        }

        // Touch the request-origin conversation (m_inflightConvId), NOT
        // the UI-active one — the user may have switched chats while the
        // agent was running.
        if (!m_inflightConvId.isEmpty()) {
            m_convSvc.touchConversation(m_inflightConvId);
        }

        m_lastResponseTimeMs = static_cast<int>(elapsed);
        emit statsChanged();

        m_streaming->resetWithoutAbort();
        m_usingAgent = false;
        m_isGenerating = false;
        m_inflightConvId.clear();
        m_currentRequestId = 0;
        // Agent turn terminal — free the provider slot held for the
        // whole agent turn.
        releaseProviderSlotIfHeld();
        emit isGeneratingChanged();

        qCDebug(verzetaUi) << "Agent finished, elapsed:" << elapsed << "ms";
    });

    // Agent error — finalize with error.
    connect(agent, &AgentService::errorOccurred, this, [this](const QString& error) {
        if (!m_usingAgent)
            return;

        if (m_streaming->isStreaming()) {
            if (m_streaming->hasInMessageService()) {
                m_msgSvc.abortStreamingMessage(m_streaming->msgId());
            }
            m_streaming->resetWithoutAbort();
        }

        m_usingAgent = false;
        m_isGenerating = false;
        m_inflightConvId.clear();
        m_currentRequestId = 0;
        // Agent turn terminal (error) — free the provider slot.
        releaseProviderSlotIfHeld();
        emit isGeneratingChanged();
        emit errorOccurred(error);
    });

    // Relay the owned executor's UI-facing signals through the app-level
    // facade (if attached) so the QML AgentService property and the remote
    // bridge observe agent progress for this run.
    if (m_agentService) {
        m_agentService->registerRelaySource(agent);
    }
    return true;
}

void ConversationRun::setAgentService(AgentService* agentService) {
    // The facade is non-owning and shared by every run. When it changes,
    // move this run's owned executor's relay registration to the new
    // facade so progress continues to surface in QML / the remote bridge.
    if (m_agentService == agentService) {
        // No facade change; ensure the owned executor exists + is wired.
        ensureOwnAgent();
        return;
    }
    if (m_agentService && m_ownAgent) {
        m_agentService->unregisterRelaySource(m_ownAgent.get());
    }
    m_agentService = agentService;
    // Build (if deps ready) and register the owned executor with the new
    // facade. If deps are not ready yet, ensureOwnAgent() will register
    // once setToolService()/setRagService() complete the dependency set.
    ensureOwnAgent();
}

// ---------------------------------------------------------------------------
// State reads
// ---------------------------------------------------------------------------

bool ConversationRun::isGenerating() const {
    // Active-conv-aware: a cascade running for a different conv
    // (m_inflightConvId != m_activeConvId) should NOT show as
    // "generating" in this view. The raw m_isGenerating field stays the
    // global "is any cascade in flight" flag for queueing logic.
    return m_isGenerating && m_inflightConvId == m_activeConvId;
}

bool ConversationRun::isConversationGenerating(const QString& convId) const {
    return m_isGenerating && convId == m_activeConvId;
}

bool ConversationRun::hasInflightStreamFor(const QString& convId) const {
    if (!m_isGenerating)
        return false;
    if (convId.isEmpty())
        return false;
    return convId == m_inflightConvId;
}

bool ConversationRun::isCompacting() const {
    return m_summarizerBusy && m_compactingConvId == m_activeConvId;
}

QString ConversationRun::activeTaskPlanId() const {
    return m_taskGate ? m_taskGate->activePlanId() : QString();
}

Ragp::Service* ConversationRun::ragpService() const {
    return m_cascade->ragpService();
}

QString ConversationRun::ragpBackendName() const {
    return m_cascade->ragpBackendName();
}

void ConversationRun::configureRagpBackend(SettingsService* settings) {
    if (!settings) {
        qCWarning(verzetaUi) << "ConversationRun::configureRagpBackend: settings is null "
                                "— keeping current backend";
        return;
    }
    m_cascade->configureBackend(*settings);
    // Keep a live pointer so the tool-iteration budget is read per turn
    // (the value can change from the settings UI mid-session).
    m_settings = settings;
    m_toolDispatcher->setMaxToolIterations(settings->toolIterationCap());
}

QList<LlmMessage> ConversationRun::assembleLlmHistory(const QList<Message>& dbMessages,
                                                      bool isGroupChat,
                                                      const QString& excludeMsgId) {
    return m_requestBuilder->assembleHistory(dbMessages, isGroupChat, excludeMsgId);
}

void ConversationRun::setQueuedUserMessage(const QString& text, const QStringList& attachments) {
    if (m_queuedUserText == text && m_queuedUserAttachments == attachments) {
        return;
    }
    m_queuedUserText = text;
    m_queuedUserAttachments = attachments;
    emit queuedUserTextChanged();
}

// ---------------------------------------------------------------------------
// Active-conversation view state
// ---------------------------------------------------------------------------

void ConversationRun::switchConversation(const QString& id) {
    if (id == m_activeConvId) {
        // This run is already bound to `id` internally, but the SHARED
        // MessageListModel may be pointed at a DIFFERENT conversation: a
        // background run that streamed while the foreground was elsewhere
        // never moved off its own conversation, so when the coordinator
        // re-foregrounds it (ChatController::switchConversation's
        // existing-run branch) the model must still be repointed here —
        // otherwise the visible view keeps showing the previous
        // conversation and this run's in-flight reply stays hidden. The
        // model setter is idempotent (no-op when already on `id`), so this
        // is a no-op for the single-conversation / already-foreground case
        // and only repoints when reactivating a background run.
        if (m_msgModel)
            m_msgModel->setActiveConversation(id);
        return;
    }

    const auto conv = m_convSvc.getConversation(id);
    if (!conv.has_value()) {
        emit errorOccurred(QStringLiteral("Conversation not found: ") + id);
        return;
    }

    m_cascade->clearCrossCascadeFingerprints();

    // Context-fill indicator is per-conversation (taken at request
    // build). Reset on switch.
    updateContextFillPercent(0);

    const bool wasGeneratingForActive = m_isGenerating && m_inflightConvId == m_activeConvId;

    m_activeConvId = id;
    m_activeConvTitle = conv->title;
    // Per-active-conv UX state — retry of "the last user message I typed
    // here" is conv-local. The streaming buffer is NOT cleared because
    // it belongs to the in-flight cascade on m_inflightConvId.
    m_lastUserText.clear();
    m_lastUserMsgId.clear();

    LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
    if (cfg.isValid()) {
        ILLMProvider* provider = m_router.providerForId(cfg.providerId);
        if (provider && provider->availableModels().contains(cfg.modelName)) {
            m_router.setActiveProvider(cfg.providerId, cfg.modelName);
        } else if (provider) {
            // The provider's live model list may not have arrived yet (it is
            // fetched at startup, and the last conversation opens before the
            // reply). Keep the saved model, and only report it as unavailable
            // if the next list from that provider really does not contain it.
            m_router.setActiveProvider(cfg.providerId, cfg.modelName);
            auto conn = std::make_shared<QMetaObject::Connection>();
            const QString convId = id;
            const QString providerId = cfg.providerId;
            const QString modelName = cfg.modelName;
            *conn = connect(&m_router,
                            &ModelRouter::modelsRefreshed,
                            this,
                            [this, conn, convId, providerId, modelName](const QString& refreshedId,
                                                                        const QStringList& models) {
                                if (refreshedId != providerId)
                                    return;
                                QObject::disconnect(*conn);
                                if (m_activeConvId != convId || models.contains(modelName))
                                    return;
                                emit errorOccurred(
                                    QStringLiteral("Model \"%1\" is no longer available on %2. "
                                                   "Please select a different model to continue.")
                                        .arg(modelName, providerId));
                            });
        } else if (m_router.activeModelName().isEmpty()) {
            m_router.setActiveProvider(cfg.providerId, QString());
            emit errorOccurred(QStringLiteral("Model \"%1\" is no longer available on %2. "
                                              "Please select a different model to continue.")
                                   .arg(cfg.modelName, cfg.providerId));
        }
    } else if (!m_router.activeModelName().isEmpty()) {
        LlmConfig newCfg;
        newCfg.providerId = m_router.activeProviderId();
        newCfg.modelName = m_router.activeModelName();
        newCfg.stream = true;
        m_convSvc.updateLlmConfig(id, newCfg);
    }

    if (m_msgModel)
        m_msgModel->setActiveConversation(id);
    emit activeConversationChanged();

    const bool nowGeneratingForActive = m_isGenerating && m_inflightConvId == m_activeConvId;
    if (wasGeneratingForActive != nowGeneratingForActive) {
        emit isGeneratingChanged();
    }

    if (m_taskGate) {
        m_taskGate->onConversationSwitched(id);
    }
}

bool ConversationRun::onConversationUpdated(const QString& id) {
    if (id != m_activeConvId)
        return false;
    const auto conv = m_convSvc.getConversation(id);
    if (conv.has_value()) {
        m_activeConvTitle = conv->title;
    }
    return true;
}

// ---------------------------------------------------------------------------
// sendMessage
// ---------------------------------------------------------------------------

void ConversationRun::sendMessage(const QString& text) {
    if (text.trimmed().isEmpty()) {
        return;
    }

    // Handle slash commands before sending to LLM.
    if (m_slashService && m_slashService->tryHandle(text.trimmed())) {
        return;
    }

    // Per-conversation queue: a 2nd message typed into THIS conversation
    // while its own turn is in flight queues and preempts at the next
    // turn boundary. Cross-conversation / cross-session serialization is
    // NO LONGER gated here — the app-level ProviderScheduler serializes
    // per provider (different providers run in parallel) when a turn
    // actually dispatches, replacing the old global single-flight gate.
    if (m_isGenerating) {
        setQueuedUserMessage(text, {});
        m_pendingImages.clear();
        m_pendingFileContext.clear();
        emit userMessageQueued(text);
        return;
    }

    // Ensure we have an active conversation; create one if needed.
    if (m_activeConvId.isEmpty()) {
        const QString id = m_convSvc.createConversation(QStringLiteral("New Chat"));
        if (id.isEmpty()) {
            emit errorOccurred(QStringLiteral("Failed to create conversation"));
            return;
        }
        m_activeConvId = id;
        m_activeConvTitle = QStringLiteral("New Chat");

        if (!m_router.activeModelName().isEmpty()) {
            LlmConfig cfg;
            cfg.providerId = m_router.activeProviderId();
            cfg.modelName = m_router.activeModelName();
            cfg.stream = true;
            m_convSvc.updateLlmConfig(id, cfg);
        }

        emit activeConversationChanged();
    }

    // Snapshot the cascade-time agent settings cache from the
    // conversation we're about to cascade on.
    refreshAgentSettingsForConv(m_activeConvId);

    m_deferredContinuationCount = 0;
    m_groupDeferredContinuations.clear();
    m_asyncArtifactContinuations = 0;
    m_chainExecutedToolNames.clear();
    m_chainExecutedFiles.clear();

    // Group chat: parse @mention to route this message to a member.
    m_cascade->resetTurnState();
    m_yieldRequestedThisBatch = false;
    if (m_membershipService) {
        const auto convOpt = m_convSvc.getConversation(m_activeConvId);
        if (convOpt.has_value() && convOpt->isGroup) {
            Member responder;

            static const QRegularExpression kMentionRx(QStringLiteral("@(\\w+)"));
            const QString text_ = text;

            QStringList mentions;  // ordered, de-duplicated
            auto it = kMentionRx.globalMatch(text_);
            while (it.hasNext()) {
                const QString m = it.next().captured(1);
                if (!mentions.contains(m, Qt::CaseInsensitive)) {
                    mentions.append(m);
                }
            }

            bool isBroadcast = false;
            for (const QString& m : mentions) {
                if (m.compare(QStringLiteral("everyone"), Qt::CaseInsensitive) == 0 ||
                    m.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0 ||
                    m.compare(QStringLiteral("team"), Qt::CaseInsensitive) == 0) {
                    isBroadcast = true;
                    break;
                }
            }

            if (isBroadcast) {
                const QList<Member> members =
                    m_membershipService->conversationMembers(m_activeConvId);
                QList<Member> ordered;
                for (const Member& m : members) {
                    if (m.isCoordinator)
                        ordered.append(m);
                }
                for (const Member& m : members) {
                    if (!m.isCoordinator)
                        ordered.append(m);
                }
                if (!ordered.isEmpty()) {
                    responder = ordered.first();
                    for (int i = 1; i < ordered.size(); ++i) {
                        m_cascade->enqueueTarget(ordered[i].alias);
                    }
                    qCDebug(verzetaUi) << "Group broadcast (@all/@everyone) → queued"
                                       << ordered.size() << "members";
                }
            } else {
                QList<Member> matched;
                for (const QString& mention : mentions) {
                    if (mention.compare(QStringLiteral("owner"), Qt::CaseInsensitive) == 0)
                        continue;

                    const Member m =
                        m_membershipService->findConversationMemberByAlias(m_activeConvId, mention);
                    if (m.isValid())
                        matched.append(m);
                }

                if (!matched.isEmpty()) {
                    responder = matched.first();
                    for (int i = 1; i < matched.size(); ++i) {
                        m_cascade->enqueueDeferredTarget(matched[i].alias);
                    }
                    if (matched.size() > 1) {
                        qCDebug(verzetaUi) << "Multi-mention routing → primary:" << responder.alias
                                           << "| deferred queue size:" << m_cascade->queueSize();
                    }
                }
            }

            if (!responder.isValid()) {
                responder = m_membershipService->defaultResponderForConversation(m_activeConvId);
                if (responder.isValid() && !mentions.isEmpty()) {
                    qCWarning(verzetaUi) << "Group chat: @mentions" << mentions
                                         << "did not match any roster member — falling back to "
                                            "default responder:"
                                         << responder.alias;
                }
            }

            if (responder.isValid()) {
                m_cascade->setCurrentResponder(responder.alias, responder.agentId);
                qCDebug(verzetaUi) << "Group chat routing → @" << responder.alias
                                   << "(mentions parsed:" << mentions << ")";
            }
        }
    }

    // 1. Persist user message.
    const QString userMsgId = storeUserMessage(text);
    if (userMsgId.isEmpty()) {
        emit errorOccurred(QStringLiteral("Failed to store user message"));
        return;
    }
    m_lastUserMsgId = userMsgId;
    m_lastUserText = text;

    // 2. Create streaming assistant placeholder; reset tool loop state.
    if (m_cascade->currentResponderAgentId().isEmpty()) {
        const auto convOpt = m_convSvc.getConversation(m_activeConvId);
        if (convOpt.has_value() && !convOpt->primaryAgentId.isEmpty()) {
            m_cascade->setCurrentResponder(m_cascade->currentResponderAlias(),
                                           convOpt->primaryAgentId);
        }
    }

    m_toolDispatcher->resetState();
    m_streaming->begin(m_activeConvId,
                       QStringLiteral("assistant"),
                       m_cascade->currentResponderAgentId(),
                       m_cascade->currentResponderAlias());

    // 3. Start timer and set generating state.
    //
    // Anchor the in-flight conversation BEFORE flipping the generating
    // flag and emitting isGeneratingChanged. The streaming placeholder
    // above was opened for m_activeConvId, so this turn targets it; both
    // the Direct (buildAndSendRequest) and agent branches below would set
    // it again, but the Direct branch sets it only inside
    // buildAndSendRequest, which runs synchronously AFTER this emit. Any
    // observer that reacts to isGeneratingChanged (e.g. the sidebar's
    // per-conversation generating indicator, which queries
    // hasInflightStreamFor) must see a valid m_inflightConvId during the
    // emit, so set it here first.
    m_inflightConvId = m_activeConvId;
    m_responseTimer.start();
    m_isGenerating = true;
    emit isGeneratingChanged();

    // 4. Build and dispatch — delegate to agent if pattern != Direct.
    if (m_ownAgent && m_agentPattern != AgentPattern::Direct) {
        m_usingAgent = true;
        m_currentRequestId = nextGlobalRequestId();
        // Off-turn-path async unified recall; resume in onMemoryRetrievalReady →
        // dispatchAgentTurn (or dispatch now if no augmentation applies).
        if (maybeBeginMemoryRetrieve(PendingTurn::Agent)) {
            return;
        }
        dispatchAgentTurn();
    } else {
        buildAndSendRequest();
    }
}

void ConversationRun::dispatchAgentTurn() {
    Chat::BuildRequestInputs inputs = makeBuildInputs();

    const Chat::BuildResult br = m_requestBuilder->buildRequest(inputs);
    m_pendingRagContext.clear();  // consumed into br.request
    updateContextFillPercent(br.contextFillPercent);
    updateCompactionCadence(br.compactionTurnsUsed, br.compactionTurnsTotal);

    if (br.clearActiveTaskPlanId && m_taskGate) {
        m_taskGate->clearActivePlan();
    }

    if (!br.success) {
        m_usingAgent = false;
        emit errorOccurred(br.errorReason);
        onRequestError(m_currentRequestId, br.errorReason);
        return;
    }

    noteSummaryDecision(br);

    if (br.consumedAttachments) {
        m_pendingImages.clear();
        m_pendingFileContext.clear();
    }

    AgentConfig agentCfg;
    agentCfg.pattern = m_agentPattern;
    agentCfg.maxIterations = 10;
    agentCfg.requireConfirmation = m_requireConfirmation;
    agentCfg.callerAgentId = m_cascade->currentResponderAgentId();
    agentCfg.callerAgentAlias = m_cascade->currentResponderAlias();
    agentCfg.conversationId = m_inflightConvId;  // scope MemoryAugmented RAG (G2d)
    if (m_toolService) {
        agentCfg.enabledTools = QStringList{};  // all tools
    }

    // Capture the dispatched provider/model so the agent terminal
    // lambdas (finished/error) release the right slot.
    m_currentDispatchedModelName = br.request.config.modelName;
    m_currentDispatchedProviderId = br.request.config.providerId;

    ledgerBeginTurn(br.request.config.providerId, br.request.config.modelName);
    if (!m_providerScheduler) {
        m_schedulerHeldProviderId = br.request.config.providerId;
        ContentionLedger::markDispatched(m_ledgerTurnId);
        m_ownAgent->execute(br.request, agentCfg);
    } else {
        QPointer<ConversationRun> self(this);
        const LlmRequest agentReq = br.request;
        const AgentConfig cfg = agentCfg;
        const quint64 dispatchRequestId = m_currentRequestId;
        const bool granted = m_providerScheduler->acquire(
            br.request.config.providerId,
            m_schedulerWaiterId,
            [self, agentReq, cfg, dispatchRequestId]() {
                if (!self)
                    return;  // run destroyed while queued
                if (self->m_currentRequestId != dispatchRequestId || !self->m_isGenerating ||
                    !self->m_usingAgent || !self->m_ownAgent) {
                    self->m_schedulerHeldProviderId = agentReq.config.providerId;
                    self->releaseProviderSlotIfHeld();
                    return;
                }
                self->m_schedulerQueuedProviderId.clear();
                self->m_schedulerHeldProviderId = agentReq.config.providerId;
                ContentionLedger::markDispatched(self->m_ledgerTurnId);
                self->m_ownAgent->execute(agentReq, cfg);
            });
        if (granted) {
            m_schedulerHeldProviderId = br.request.config.providerId;
            ContentionLedger::markDispatched(m_ledgerTurnId);
            m_ownAgent->execute(br.request, agentCfg);
        } else {
            m_schedulerQueuedProviderId = br.request.config.providerId;
        }
    }
}

void ConversationRun::sendMessageWithAttachments(const QString& text,
                                                 const QStringList& attachmentPaths) {
    // Per-conversation queue only (see sendMessage). The ProviderScheduler
    // handles cross-conversation / cross-session per-provider
    // serialization at dispatch time.
    if (m_isGenerating) {
        setQueuedUserMessage(text, attachmentPaths);
        m_pendingImages.clear();
        m_pendingFileContext.clear();
        emit userMessageQueued(text);
        return;
    }
    if (!m_fileService || attachmentPaths.isEmpty()) {
        sendMessage(text);
        return;
    }

    QString displayText = text;
    QString llmContext;

    for (const QString& path : attachmentPaths) {
        const QFileInfo fi(path);
        const QString mime = m_fileService->mimeType(path);

        if (mime.startsWith(QStringLiteral("image/"))) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QByteArray raw = f.readAll();
            f.close();
            if (raw.isEmpty())
                continue;

            LlmImageData img;
            img.base64 = raw.toBase64();
            img.mimeType = mime;
            m_pendingImages.append(img);

            displayText += QStringLiteral("\n📎 %1").arg(fi.fileName());
        } else {
            const QByteArray raw = m_fileService->readFileContent(path, 64 * 1024);
            if (!raw.isEmpty()) {
                llmContext += QStringLiteral("\n\n[File: %1]\n```\n%2\n```")
                                  .arg(fi.fileName(), QString::fromUtf8(raw));
            }
            displayText += QStringLiteral("\n📎 %1").arg(fi.fileName());
        }
    }

    if (displayText.trimmed().isEmpty() && !m_pendingImages.isEmpty()) {
        displayText = QStringLiteral("Please describe this image.");
    }

    m_pendingFileContext = llmContext;

    sendMessage(displayText);
}

// ---------------------------------------------------------------------------
// stopGeneration / retryLastMessage / compactNow
// ---------------------------------------------------------------------------

void ConversationRun::stopGeneration() {
    // Cancel any in-flight async RAG pre-fetch: clearing the id makes a late
    // retrievalReady fail the identity match, so a cancelled turn never
    // resumes. (Signal-driven; the worker embed completes harmlessly.)
    m_pendingQueryId = 0;
    m_pendingTurn = PendingTurn::None;
    m_pendingRagContext.clear();
    if (!m_isGenerating) {
        setQueuedUserMessage(QString(), {});
        m_cascade->clearCrossCascadeFingerprints();
        return;
    }
    m_userCancelledInFlight = true;
    setQueuedUserMessage(QString(), {});

    // Free any scheduler state held/queued by this turn. cancel handles
    // the "queued but not yet dispatched" case; release handles the
    // "dispatched, holding the slot" case. Both are idempotent.
    cancelQueuedSchedulerWaiter();
    releaseProviderSlotIfHeld();

    if (m_usingAgent && m_ownAgent && m_ownAgent->isRunning()) {
        m_ownAgent->cancel();
        return;
    }
    // cancel ONLY this run's in-flight request so a
    // concurrent turn on another conversation/provider is untouched.
    if (m_currentRequestId != 0) {
        m_router.cancelRequest(m_currentRequestId);
    }

    if (m_streaming->isStreaming()) {
        if (!m_streaming->content().isEmpty()) {
            m_streaming->rewrite(m_streaming->content());
            m_streaming->finalize(0,
                                  QStringLiteral("user_interrupted"),
                                  QString(),
                                  m_currentDispatchedModelName,
                                  QJsonObject{});
        } else {
            m_streaming->abort(QStringLiteral("user_interrupted"));
        }
    }

    m_cascade->resetTurnState();
    m_cascade->clearCrossCascadeFingerprints();
    m_inflightConvId.clear();
    m_currentRequestId = 0;
    m_toolDispatcher->resetState();

    m_isGenerating = false;
    emit isGeneratingChanged();
}

void ConversationRun::retryLastMessage() {
    if (m_isGenerating || m_lastUserText.isEmpty()) {
        return;
    }

    if (m_streaming->isStreaming()) {
        if (m_streaming->wasPersisted()) {
            m_msgSvc.deleteMessage(m_streaming->msgId());
        } else if (m_streaming->hasInMessageService()) {
            m_msgSvc.abortStreamingMessage(m_streaming->msgId());
        }
    }

    if (!m_lastUserMsgId.isEmpty()) {
        m_msgSvc.deleteMessage(m_lastUserMsgId);
    }

    const QString text = m_lastUserText;
    m_lastUserText.clear();
    m_lastUserMsgId.clear();
    m_streaming->resetWithoutAbort();
    sendMessage(text);
}

void ConversationRun::compactNow() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_summarizer || m_activeConvId.isEmpty()) {
        qCDebug(verzetaUi) << "compactNow: no summarizer/conversation";
        return;
    }
    const QString convId = m_activeConvId;
    m_summarizer->invalidate(convId, QStringLiteral("user_regenerated"));
    m_summarizer->generateAsync(convId, QStringLiteral("manual"));
}

// ---------------------------------------------------------------------------
// External-service coordination
// ---------------------------------------------------------------------------

void ConversationRun::onExternalAgentPatternChanged(const QString& patternName) {
    if (m_isGenerating) {
        qCDebug(verzetaUi) << "ConversationRun: agent pattern signal" << patternName
                           << "deferred — cascade in flight; will refresh from "
                              "inflight conv at next send";
        return;
    }
    if (patternName == QStringLiteral("react")) {
        m_agentPattern = AgentPattern::ReAct;
    } else if (patternName == QStringLiteral("planner")) {
        m_agentPattern = AgentPattern::PlannerExecutor;
    } else if (patternName == QStringLiteral("router")) {
        m_agentPattern = AgentPattern::Router;
    } else if (patternName == QStringLiteral("multi_agent")) {
        m_agentPattern = AgentPattern::MultiAgent;
    } else if (patternName == QStringLiteral("memory")) {
        m_agentPattern = AgentPattern::MemoryAugmented;
    } else {
        m_agentPattern = AgentPattern::Direct;
    }
    qCDebug(verzetaUi) << "ConversationRun: agent pattern mirror updated to" << patternName;
}

void ConversationRun::onExternalRequireConfirmationChanged(bool require) {
    if (m_isGenerating)
        return;
    m_requireConfirmation = require;
}

void ConversationRun::onExternalToolsEnabledChanged(bool enabled) {
    if (m_toolsEnabled == enabled)
        return;
    if (m_isGenerating)
        return;
    m_toolsEnabled = enabled;
    emit toolsEnabledChanged();
}

void ConversationRun::refreshAgentSettingsForConv(const QString& convId) {
    if (convId.isEmpty())
        return;

    const auto conv = m_convSvc.getConversation(convId);
    if (!conv.has_value())
        return;

    const LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);

    const QString resolvedPattern =
        cfg.agentPattern.isEmpty() ? QStringLiteral("direct") : cfg.agentPattern;

    if (resolvedPattern == QStringLiteral("react")) {
        m_agentPattern = AgentPattern::ReAct;
    } else if (resolvedPattern == QStringLiteral("planner")) {
        m_agentPattern = AgentPattern::PlannerExecutor;
    } else if (resolvedPattern == QStringLiteral("router")) {
        m_agentPattern = AgentPattern::Router;
    } else if (resolvedPattern == QStringLiteral("multi_agent")) {
        m_agentPattern = AgentPattern::MultiAgent;
    } else if (resolvedPattern == QStringLiteral("memory")) {
        m_agentPattern = AgentPattern::MemoryAugmented;
    } else {
        m_agentPattern = AgentPattern::Direct;
    }

    m_toolsEnabled = cfg.toolsEnabled;
    m_requireConfirmation = cfg.requireConfirmation;
}

void ConversationRun::onActiveCanvasMetadataRefresh(const QString& filename,
                                                    const QString& language,
                                                    int revision,
                                                    int lineCount,
                                                    qint64 byteSize) {
    m_activeCanvasFilename = filename;
    m_activeCanvasLanguage = language;
    m_activeCanvasRevision = revision;
    m_activeCanvasLineCount = lineCount;
    m_activeCanvasByteSize = byteSize;
}

void ConversationRun::onExternalTaskStarted(const QString& planId,
                                            const QString& convId,
                                            const QString& ownerAlias,
                                            const QString& ownerAgentId) {
    if (convId != m_activeConvId)
        return;
    if (m_isGenerating)
        return;
    if (!m_taskGate)
        return;

    m_taskGate->setActivePlanId(planId);
    m_cascade->setCurrentResponder(ownerAlias, ownerAgentId);
    m_toolDispatcher->resetState();
    m_streaming->begin(m_activeConvId, QStringLiteral("assistant"), ownerAgentId, ownerAlias);
    m_responseTimer.start();
    m_isGenerating = true;
    emit isGeneratingChanged();
    buildAndSendRequest();
}

void ConversationRun::onExternalPlanStopped(const QString& planId) {
    if (!m_taskGate)
        return;
    const bool wasActivePlan = (m_taskGate->activePlanId() == planId);
    if (wasActivePlan && m_isGenerating) {
        stopGeneration();
        m_cascade->resetTurnState();
    }
    if (wasActivePlan) {
        m_taskGate->clearActivePlan();
    }
}

void ConversationRun::onExternalAllPlansStoppedInConversation(const QString& convId) {
    if (m_isGenerating && convId == m_inflightConvId) {
        stopGeneration();
        m_cascade->resetTurnState();
    }
    if (convId == m_activeConvId && m_taskGate) {
        m_taskGate->clearActivePlan();
    }
}

void ConversationRun::onExternalConversationAboutToBeDeleted(const QString& id) {
    if (m_isGenerating && (id == m_activeConvId || id == m_inflightConvId)) {
        stopGeneration();
    }
}

void ConversationRun::onExternalConversationDeleted(const QString& id) {
    if (id != m_activeConvId)
        return;
    m_activeConvId.clear();
    m_activeConvTitle.clear();
    m_lastUserText.clear();
    m_lastUserMsgId.clear();
    m_streaming->resetWithoutAbort();
    if (m_msgModel)
        m_msgModel->setActiveConversation(QString());
    emit activeConversationChanged();
}

// ---------------------------------------------------------------------------
// ModelRouter streaming relay
// ---------------------------------------------------------------------------

void ConversationRun::onChunkReceived(quint64 requestId, const LlmChunk& chunk) {
    if (requestId == 0 || requestId != m_currentRequestId) {
        return;
    }
    if (!m_ledgerFirstTokSeen && m_ledgerTurnId != 0) {
        m_ledgerFirstTokSeen = true;
        ContentionLedger::markFirstToken(m_ledgerTurnId);
    }
    // Agent-path ownership: AgentService forwards chunks via its own
    // signal wired in setAgentService; if this slot ALSO appended, every
    // token would be written twice.
    if (m_usingAgent) {
        return;
    }
    if (!m_streaming->isStreaming()) {
        return;
    }
    if (!chunk.delta.isEmpty()) {
        m_streaming->appendChunk(chunk.delta);
    }
    m_streaming->appendThinkingChunk(chunk.thinkingDelta);
    if (!chunk.toolCallJson.isEmpty()) {
        m_toolDispatcher->appendPendingCall(chunk.toolCallJson);
    }
}

void ConversationRun::onRequestFinished(quint64 requestId,
                                        const QString& finishReasonIn,
                                        int totalTokens) {
    if (requestId == 0 || requestId != m_currentRequestId) {
        qCDebug(verzetaUi) << "onRequestFinished: dropping stale signal for request" << requestId
                           << "(current:" << m_currentRequestId << ")";
        return;
    }
    ContentionLedger::end(m_ledgerTurnId, true, finishReasonIn);
    if (m_usingAgent) {
        return;
    }

    // The provider request for THIS turn has terminated — free its
    // scheduler slot now so another conversation queued on the same
    // provider can dispatch. A continuation turn (tool batch / next
    // cascade member / retry) re-acquires the slot via its own
    // buildAndSendRequest → dispatchThroughScheduler. Released BEFORE the
    // early-return below so a no-stream finish still frees the slot.
    releaseProviderSlotIfHeld();

    if (!m_streaming->isStreaming()) {
        qCWarning(verzetaUi) << "onRequestFinished: no active streaming message "
                                "(finishReason:"
                             << finishReasonIn << ")";
        return;
    }

    QString finishReason = finishReasonIn;

    const qint64 elapsedMs = m_responseTimer.elapsed();

    qCDebug(verzetaUi) << "Response finished —"
                       << "responder:"
                       << (m_cascade->currentResponderAlias().isEmpty()
                               ? QStringLiteral("-")
                               : m_cascade->currentResponderAlias())
                       << "| content chars:" << m_streaming->content().length()
                       << "| tokens:" << totalTokens << "| reason:" << finishReason
                       << "| elapsed:" << elapsedMs << "ms";

    const bool userCancel = m_userCancelledInFlight;
    m_userCancelledInFlight = false;

    if (userCancel && m_taskGate && m_taskGate->hasActivePlan()) {
        m_taskGate->handleUserCancel();
        if (m_streaming->isStreaming() && m_streaming->hasInMessageService()) {
            m_msgSvc.abortStreamingMessage(m_streaming->msgId());
        }
        m_streaming->resetWithoutAbort();
        m_toolDispatcher->resetState();
        m_cascade->resetTurnState();
        m_isGenerating = false;
        emit isGeneratingChanged();
        return;
    }

    if (finishReason == QStringLiteral("stop") && m_streaming->content().trimmed().isEmpty()) {
        const QString misrouted = m_streaming->thinkingContent().trimmed();
        if (!misrouted.isEmpty()) {
            qCWarning(verzetaUi) << "Thinking-misroute recovery: promoting" << misrouted.size()
                                 << "chars from the thinking channel to content for @"
                                 << (m_cascade->currentResponderAlias().isEmpty()
                                         ? QStringLiteral("assistant")
                                         : m_cascade->currentResponderAlias())
                                 << "— the reply was fully captured, only misrouted; without"
                                 << "promotion it would be treated as empty and delete-retried";
            m_streaming->rewrite(misrouted);
            m_streaming->clearThinking();
        }
    }

    QString declaredTaskStatus;
    QString impersonatedAlias;
    if (!m_streaming->content().isEmpty()) {
        const QString anchorId = m_taskGate ? m_taskGate->activePlanId() : QString();
        Chat::SanitizeInputs sanIn;
        sanIn.content = m_streaming->content();
        sanIn.responderAlias = m_cascade->currentResponderAlias();
        sanIn.activeTaskPlanSet = !anchorId.isEmpty();
        sanIn.activePlanIdPrefix = anchorId.left(8);
        if (m_membershipService && !m_inflightConvId.isEmpty()) {
            const auto convOpt = m_convSvc.getConversation(m_inflightConvId);
            if (convOpt.has_value() && convOpt->isGroup) {
                sanIn.isGroupChat = true;
                const QList<Member> members =
                    m_membershipService->conversationMembers(m_inflightConvId);
                for (const Member& m : members) {
                    if (!m.alias.isEmpty()) {
                        sanIn.rosterAliases.append(m.alias);
                    }
                }
            }
        }
        if (streamTraceEnabled()) {
            const QString sid = m_streaming->msgId().left(8);
            qCInfo(verzetaUi).noquote()
                << "TRACE: PRE-SANITIZE msgId=" << sid << " responder=" << sanIn.responderAlias
                << " isGroupChat=" << sanIn.isGroupChat
                << " rosterSize=" << sanIn.rosterAliases.size()
                << " activeTaskPlanSet=" << sanIn.activeTaskPlanSet
                << " contentChars=" << sanIn.content.size()
                << " thinkingChars=" << m_streaming->thinkingContent().size();
            qCInfo(verzetaUi).noquote()
                << "TRACE: PRE-SANITIZE-CONTENT-BEGIN msgId=" << sid << "\n"
                << sanIn.content << "\nTRACE: PRE-SANITIZE-CONTENT-END msgId=" << sid;
        }
        const Chat::SanitizeResult sanResult = m_sanitizer->sanitize(sanIn);
        if (streamTraceEnabled()) {
            const QString sid = m_streaming->msgId().left(8);
            qCInfo(verzetaUi).noquote()
                << "TRACE: POST-SANITIZE msgId=" << sid
                << " charsStripped=" << sanResult.charsStripped
                << " sanitisedChars=" << sanResult.sanitisedContent.size() << " declaredTaskStatus="
                << (sanResult.declaredTaskStatus.isEmpty() ? QStringLiteral("(none)")
                                                           : sanResult.declaredTaskStatus);
            qCInfo(verzetaUi).noquote()
                << "TRACE: POST-SANITIZE-CONTENT-BEGIN msgId=" << sid << "\n"
                << sanResult.sanitisedContent << "\nTRACE: POST-SANITIZE-CONTENT-END msgId=" << sid;
        }
        if (sanResult.sanitisedContent != m_streaming->content()) {
            m_streaming->rewrite(sanResult.sanitisedContent);
        }
        declaredTaskStatus = sanResult.declaredTaskStatus;
        impersonatedAlias = sanResult.impersonatedAlias;
    }

    const QString html;

    const QString trimmedContent = m_streaming->content().trimmed();
    const bool haveThinkingToRender = !m_streaming->thinkingContent().trimmed().isEmpty();

    bool isGroupCascadeYield = false;
    if (trimmedContent.isEmpty() && !m_cascade->isQueueEmpty()) {
        const auto convOpt = m_convSvc.getConversation(m_inflightConvId);
        if (convOpt.has_value() && convOpt->isGroup) {
            isGroupCascadeYield = true;
        }
    }

    const bool skipEmptyPersist =
        trimmedContent.isEmpty() && !haveThinkingToRender &&
        finishReason != QStringLiteral("tool_calls") &&
        (finishReason == QStringLiteral("stop") || finishReason == QStringLiteral("length") ||
         finishReason == QStringLiteral("error") || isGroupCascadeYield);

    if (skipEmptyPersist && m_streaming->isStreaming() && m_streaming->hasInMessageService()) {
        m_msgSvc.abortStreamingMessage(m_streaming->msgId());
    }

    const QString parentAssistantMsgIdForToolBatch = m_streaming->msgId();
    const QString contentAtFinishForDownstream = m_streaming->content();

    if (!m_streaming->wasPersisted() && !skipEmptyPersist) {
        const QJsonObject metadata{
            {QStringLiteral("elapsed_ms"), elapsedMs},
            {QStringLiteral("finish_reason"), finishReason},
        };

        m_streaming->rewrite(m_streaming->content());
        const bool persisted = m_streaming->finalize(
            totalTokens, finishReason, html, m_currentDispatchedModelName, metadata);

        if (!persisted) {
            qCWarning(verzetaUi) << "onRequestFinished: finalizeStreamingMessage failed"
                                 << "— placeholder aborted, skipping tool dispatch";
            finishReason = QStringLiteral("error");
        }

        if (m_taskGate) {
            m_taskGate->recordTextArtifactIfApplicable(
                finishReason, contentAtFinishForDownstream, m_cascade->currentResponderAlias());
        }
    }

    // Sync the tool-iteration budget from settings each turn so a change
    // in the settings UI takes effect live (0 = unlimited).
    if (m_settings) {
        m_toolDispatcher->setMaxToolIterations(m_settings->toolIterationCap());
    }

    const bool toolChainEcho = finishReason == QStringLiteral("tool_calls") &&
                               m_toolDispatcher->pendingCount() > 0 &&
                               m_toolDispatcher->continuationIsEcho(contentAtFinishForDownstream);

    if (finishReason == QStringLiteral("tool_calls") && !toolChainEcho &&
        m_streaming->wasPersisted() && m_toolService && m_toolDispatcher->pendingCount() > 0 &&
        !m_toolDispatcher->iterationCapReached(m_toolDispatcher->iterationCount()) &&
        !m_toolDispatcher->failureCapReached()) {
        Chat::ToolBatchInputs inputs;
        inputs.parentAssistantMsgId = parentAssistantMsgIdForToolBatch;
        inputs.inflightConvId = m_inflightConvId;
        inputs.requestId = m_currentRequestId;
        inputs.responseMemberAlias = m_cascade->currentResponderAlias();
        inputs.responseMemberAgentId = m_cascade->currentResponderAgentId();
        inputs.activeTaskPlanId = m_taskGate ? m_taskGate->activePlanId() : QString();
        inputs.lastUserText = m_lastUserText;
        inputs.toolService = m_toolService;
        inputs.planService = m_planService;
        inputs.taskRunner = m_taskRunner;
        inputs.taskObserver = m_taskObserver;
        inputs.fileService = m_fileService;
        inputs.membershipService = m_membershipService;

        m_streaming->resetWithoutAbort();
        m_toolDispatcher->enqueueBatch(inputs);
        return;  // Continuation turn kicked off by onToolBatchCompleted.
    }

    const bool failureCap = m_toolDispatcher->failureCapReached();
    const bool budgetCap =
        m_toolDispatcher->iterationCapReached(m_toolDispatcher->iterationCount());
    if ((failureCap || budgetCap || toolChainEcho) &&
        finishReason == QStringLiteral("tool_calls")) {
        const int dropped = m_toolDispatcher->pendingCount();
        // Precedence: a run of FAILURES is the real runaway (a stuck loop of
        // failing tools) and is reported first; then the narration echo;
        // then the plain budget ceiling (legitimate work that ran long).
        QString capReason;
        if (failureCap) {
            capReason = QStringLiteral("tool_failure_cap");
            qCWarning(verzetaUi) << "ConversationRun:" << m_toolDispatcher->consecutiveFailures()
                                 << "consecutive tool failures — force-finalizing cascade,"
                                 << "dropping" << dropped << "unprocessed tool calls";
        } else if (toolChainEcho) {
            capReason = QStringLiteral("tool_chain_echo");
            qCWarning(verzetaUi) << "ConversationRun: tool-chain narration echo —"
                                 << "force-finalizing cascade — dropping" << dropped
                                 << "unprocessed tool calls from the echoed response";
        } else {
            capReason = QStringLiteral("tool_iteration_cap");
            qCWarning(verzetaUi) << "ConversationRun: tool-iteration budget reached ("
                                 << m_toolDispatcher->maxToolIterations()
                                 << "), force-finalizing cascade — dropping" << dropped
                                 << "unprocessed tool calls from the over-cap response";
        }

        // The cap path force-completes the cascade with a non-"stop" finish,
        // so the natural "✔ Round settled" note does NOT fire — without a
        // note here the round ends INVISIBLY (a productive agent that hit
        // the budget mid-task looked like the chat just died). Post an
        // explicit, actionable row for the two USER-relevant cases; the
        // tool-chain-echo case is a narration loop (not real work) and keeps
        // its silent finalize.
        const int failures = m_toolDispatcher->consecutiveFailures();
        const int budget = m_toolDispatcher->maxToolIterations();
        m_toolDispatcher->resetState();
        if (failureCap || budgetCap) {
            const QString who = m_cascade->currentResponderAlias();
            const QString whoLabel =
                who.isEmpty() ? QStringLiteral("The assistant") : (QStringLiteral("@") + who);
            const QString note =
                failureCap
                    ? QStringLiteral("⚠ %1 had %2 tool calls fail in a row, so the run was "
                                     "stopped to avoid a loop. Check the errors above, then "
                                     "give direction or say \"try again\".")
                          .arg(whoLabel)
                          .arg(failures)
                    : QStringLiteral("⏸ %1 reached the limit of %2 tool steps per turn and "
                                     "paused, so some work may be unfinished. Say \"continue\" "
                                     "to let it keep going, or raise \"Tool steps per turn\" in "
                                     "Chat Settings.")
                          .arg(whoLabel)
                          .arg(budget);
            m_msgSvc.addInterveningSystemMessage(m_inflightConvId, note);
        }

        Chat::CascadeRouteInputs capInputs;
        capInputs.convId = m_inflightConvId;
        capInputs.content = contentAtFinishForDownstream;
        capInputs.requestId = m_currentRequestId;
        capInputs.responderMsgId = parentAssistantMsgIdForToolBatch;
        capInputs.declaredTaskStatus = declaredTaskStatus;
        capInputs.finishReason = capReason;
        capInputs.totalTokens = totalTokens;
        capInputs.elapsedMs = elapsedMs;

        if (m_auditService) {
            m_auditService->record(ActivityEvent::forAgentTurn(QString(),
                                                               m_inflightConvId,
                                                               QString::number(m_currentRequestId),
                                                               m_cascade->currentResponderAlias(),
                                                               m_cascade->currentResponderAgentId(),
                                                               QString(),
                                                               m_currentDispatchedProviderId,
                                                               m_currentDispatchedModelName,
                                                               capReason,
                                                               totalTokens,
                                                               elapsedMs));
        }

        m_cascade->forceCascadeComplete(capInputs);
        return;  // explicit exit.
    }

    if (m_streaming->isStreaming() && m_streaming->hasInMessageService()) {
        qCWarning(verzetaUi) << "onRequestFinished: safety-net abort of orphan streaming"
                             << "slot" << m_streaming->msgId();
        m_msgSvc.abortStreamingMessage(m_streaming->msgId());
    }
    m_convSvc.touchConversation(m_inflightConvId);

    // Auto-rename "New Chat" based on the user's first message.
    if (m_activeConvTitle == QStringLiteral("New Chat") && !m_lastUserText.isEmpty()) {
        QString title = m_lastUserText.left(50).trimmed();
        const int nl = title.indexOf(QLatin1Char('\n'));
        if (nl > 0)
            title.truncate(nl);
        if (title.length() >= 50)
            title += QStringLiteral("…");
        if (!title.isEmpty()) {
            m_convSvc.renameConversation(m_inflightConvId, title);
            m_activeConvTitle = title;
            emit activeConversationChanged();
        }
    }

    m_totalTokens += totalTokens;
    m_lastResponseTimeMs = static_cast<int>(elapsedMs);
    const QString provider = m_currentDispatchedProviderId;
    if (provider != QStringLiteral("ollama") && provider != QStringLiteral("llamacpp")) {
        m_estimatedCostUsd += totalTokens * 0.00001;  // $10/1M = $0.00001/token
    }
    emit statsChanged();

    Chat::CascadeRouteInputs cascadeInputs;
    cascadeInputs.convId = m_inflightConvId;
    cascadeInputs.content = contentAtFinishForDownstream;
    cascadeInputs.requestId = m_currentRequestId;
    cascadeInputs.responderMsgId = parentAssistantMsgIdForToolBatch;
    cascadeInputs.declaredTaskStatus = declaredTaskStatus;
    cascadeInputs.finishReason = finishReason;
    cascadeInputs.totalTokens = totalTokens;
    cascadeInputs.elapsedMs = elapsedMs;
    cascadeInputs.impersonatedAlias = impersonatedAlias;
    const bool is1to1 = m_cascade->currentResponderAlias().isEmpty();


    const QString responderAlias = m_cascade->currentResponderAlias();

    // Live tool-name snapshot for the structural gate — from the actual
    // registry, never a hardcoded list.
    QStringList registeredToolNames;
    if (m_toolService) {
        const QList<ToolSchema> schemas = m_toolService->availableTools();
        registeredToolNames.reserve(schemas.size());
        for (const ToolSchema& s : schemas) {
            registeredToolNames.append(s.name);
        }
    }

    // Truthful executed-tools context (group classify prompt + 1:1
    // confirmer prompt): partially-executed chains must never have their
    // done work judged as "still pending".
    QString executedSummary;
    if (!m_chainExecutedToolNames.isEmpty()) {
        executedSummary = QStringLiteral("tools: %1")
                              .arg(m_chainExecutedToolNames.values().join(QStringLiteral(", ")));
        if (!m_chainExecutedFiles.isEmpty()) {
            executedSummary += QStringLiteral("; files: %1")
                                   .arg(m_chainExecutedFiles.values().join(QStringLiteral(", ")));
        }
    }

    // Structural candidacy + fire-awareness (both modes). A misattributed
    // reply (impersonatedAlias set) is never a candidate — the cascade's
    // bounded retry owns it; confirming/nudging imposter content would
    // act on the WRONG voice.
    bool structuralCandidate = false;
    if (m_toolsEnabled && impersonatedAlias.isEmpty() &&
        finishReason != QStringLiteral("tool_calls") &&
        !contentAtFinishForDownstream.trimmed().isEmpty()) {
        structuralCandidate = !Chat::ToolDispatcher::narrationDeferredActionKind(
                                   contentAtFinishForDownstream, registeredToolNames)
                                   .isEmpty();
        // FIRE-AWARENESS: a reply announcing ONLY work already executed
        // in this turn-chain (the post-batch summary) must never be
        // re-nudged — tool fired successfully → no more nudges.
        if (structuralCandidate &&
            Chat::ToolDispatcher::announcesOnlyExecutedWork(contentAtFinishForDownstream,
                                                            registeredToolNames,
                                                            m_chainExecutedToolNames,
                                                            m_chainExecutedFiles)) {
            qCInfo(verzetaUi) << "Deferred-action: suppressed — the announced work is"
                              << "already covered by tools executed this turn ("
                              << m_chainExecutedToolNames.values().join(QStringLiteral(", "))
                              << ") — accepting the reply, no nudge";
            structuralCandidate = false;
        }
    }

    if (!is1to1) {
        // GROUP: hand the whole decision to the classify call (one LLM
        // call decides routing AND pending-action, in any language).
        cascadeInputs.executedToolsSummary = executedSummary;
        cascadeInputs.hasStructuralActionSignals = structuralCandidate;
        cascadeInputs.deferredBudgetExhausted =
            m_groupDeferredContinuations.value(responderAlias.toLower(), 0) >=
            kMaxGroupDeferredPerAlias;
        finishTurnRouting(cascadeInputs);
        return;
    }

    // At most ONE deferred-action nudge per user send. If the model does
    // not act on a single "go ahead and do it" cue, a second/third cue does
    // not help — it drives an apology loop (live-observed). A missed
    // narrate-and-stop just means the user types "continue"; a wrong nudge
    // derails the turn, so the asymmetric cost favors a single attempt.
    constexpr int kMaxDeferredContinuations = 1;  // 1:1, per user send
    const bool deferredCandidate =
        structuralCandidate && m_deferredContinuationCount < kMaxDeferredContinuations;

    if (deferredCandidate) {
        ensureIntentConfirmer();
        if (m_intentConfirmer && m_intentConfirmer->canConfirm()) {
            // Transient UI while the one-shot confirm is in flight; removed
            // the instant it resolves.
            const QString checkingMsgId = m_msgSvc.addInterveningSystemMessage(
                m_inflightConvId,
                QStringLiteral("⏳ The model described an action. Checking "
                               "whether it still needs to run a tool…"));
            const quint64 reqAtKick = m_currentRequestId;
            const QString contAgentId = m_cascade->currentResponderAgentId();
            using ConfirmResult = Chat::ActionIntentConfirmer::Result;
            auto* w = new QFutureWatcher<ConfirmResult>(this);
            connect(w,
                    &QFutureWatcher<ConfirmResult>::finished,
                    this,
                    [this, w, cascadeInputs, checkingMsgId, reqAtKick, contAgentId]() mutable {
                        w->deleteLater();
                        if (!checkingMsgId.isEmpty()) {
                            m_msgSvc.deleteMessage(checkingMsgId);
                        }
                        if (reqAtKick != m_currentRequestId) {
                            return;
                        }
                        const bool confirmed = w->future().resultCount() > 0 &&
                                               w->result() == ConfirmResult::Confirmed;
                        if (!confirmed) {
                            qCInfo(verzetaUi) << "Deferred-action: not confirmed (or LLM"
                                                 " unavailable) — accepting the reply";
                            finishTurnRouting(cascadeInputs);
                            return;
                        }
                        // CONFIRMED → NON-DESTRUCTIVE continuation. Keep the
                        // reply; insert a visible note; dispatch ONE more turn
                        // nudging the model to actually run the tool. The next
                        // reply re-enters onRequestFinished and is evaluated
                        // fresh (tool fire / larp-again / normal / empty).
                        ++m_deferredContinuationCount;
                        m_deferredContinuationPending = true;
                        m_currentEchoRetryAttempt = 0;  // not an empty/echo retry
                        m_currentRetryReason.clear();
                        m_msgSvc.addInterveningSystemMessage(
                            m_inflightConvId,
                            QStringLiteral("↻ The model described a file/tool action "
                                           "but didn't run it, so it is being asked to "
                                           "do it now (or say why it can't)."));
                        qCInfo(verzetaUi) << "Deferred-action (1:1) CONFIRMED — continuation"
                                          << "(reply kept, not deleted)";
                        m_streaming->begin(
                            m_inflightConvId, QStringLiteral("assistant"), contAgentId, QString());
                        QTimer::singleShot(0, this, [this]() { buildAndSendRequest(); });
                    });
            w->setFuture(m_intentConfirmer->confirmFileCreationIntentAsync(
                contentAtFinishForDownstream, executedSummary));
            return;  // tail runs from the watcher (continue or accept)
        }
        // No confirmer / provider → do NOT fire on tokens alone.
        qCInfo(verzetaUi) << "Deferred-action: no LLM confirmation available — not "
                             "firing (structural tokens alone are not trusted)";
    }

    finishTurnRouting(cascadeInputs);
}

void ConversationRun::finishTurnRouting(const Chat::CascadeRouteInputs& cascadeInputs) {
    if (m_auditService && cascadeInputs.finishReason != QStringLiteral("tool_calls")) {
        m_auditService->record(ActivityEvent::forAgentTurn(QString(),
                                                           m_inflightConvId,
                                                           QString::number(m_currentRequestId),
                                                           m_cascade->currentResponderAlias(),
                                                           m_cascade->currentResponderAgentId(),
                                                           QString(),
                                                           m_currentDispatchedProviderId,
                                                           m_currentDispatchedModelName,
                                                           cascadeInputs.finishReason,
                                                           cascadeInputs.totalTokens,
                                                           cascadeInputs.elapsedMs));
    }

    if (!m_queuedUserText.isEmpty() || !m_queuedUserAttachments.isEmpty()) {
        qCDebug(verzetaUi) << "ConversationRun: user message queued mid-cascade —"
                           << "finalizing at this turn boundary so the user can steer";
        Chat::CascadeRouteInputs preemptInputs = cascadeInputs;
        preemptInputs.finishReason = QStringLiteral("user_message_pending");
        m_cascade->forceCascadeComplete(preemptInputs);
        return;
    }

    m_cascade->routeOrFinalize(cascadeInputs);
}

void ConversationRun::ensureIntentConfirmer() {
    if (m_intentConfirmer) {
        return;
    }
    Ragp::Service* ragp = m_cascade ? m_cascade->ragpService() : nullptr;
    if (!ragp) {
        return;  // built lazily again once the cascade's RAGP service exists
    }
    m_intentConfirmer = std::make_unique<Chat::ActionIntentConfirmer>(ragp, this);
}

void ConversationRun::onRequestError(quint64 requestId, const QString& errorMessage) {
    if (requestId == 0 || requestId != m_currentRequestId) {
        qCDebug(verzetaUi) << "onRequestError: dropping stale error for request" << requestId
                           << "(current:" << m_currentRequestId << ")"
                           << "err:" << errorMessage;
        return;
    }
    ContentionLedger::end(m_ledgerTurnId, false, QStringLiteral("error"));
    if (m_usingAgent) {
        return;
    }
    qCWarning(verzetaUi) << "Request error:" << errorMessage;

    // Terminal path — free the provider slot.
    releaseProviderSlotIfHeld();

    if (m_streaming->isStreaming()) {
        if (m_streaming->hasInMessageService()) {
            m_msgSvc.abortStreamingMessage(m_streaming->msgId());
        }
        m_streaming->resetWithoutAbort();
    }

    m_cascade->resetTurnState();
    m_inflightConvId.clear();
    m_currentRequestId = 0;
    m_toolDispatcher->resetState();

    m_isGenerating = false;
    emit isGeneratingChanged();
    emit errorOccurred(errorMessage);
}

// ---------------------------------------------------------------------------
// CascadeController feedback
// ---------------------------------------------------------------------------

void ConversationRun::onCascadeMemberTurnStarted(const QString& alias, const QString& agentId) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (m_inflightConvId.isEmpty()) {
        qCWarning(verzetaUi) << "onCascadeMemberTurnStarted: empty m_inflightConvId — "
                                "stale cascade signal after stop/switch; ignoring";
        return;
    }

    m_toolDispatcher->resetState();
    m_currentEchoRetryAttempt = 0;
    m_currentRetryReason.clear();
    m_yieldRequestedThisBatch = false;
    // A new member turn is a fresh chain — its fire-awareness record
    // starts empty (one member's executed calls must not vouch for
    // another member's announcements).
    m_chainExecutedToolNames.clear();
    m_chainExecutedFiles.clear();
    m_streaming->begin(m_inflightConvId, QStringLiteral("assistant"), agentId, alias);
    QTimer::singleShot(0, this, [this]() { buildAndSendRequest(); });
}

void ConversationRun::onCascadeComplete(const QString& declaredTaskStatus,
                                        const QString& finishReason,
                                        int totalTokens,
                                        qint64 elapsedMs,
                                        int roundTurns,
                                        bool pauseNoted) {
    VERZETA_ASSERT_MAIN_THREAD();

    // Capture before m_inflightConvId is cleared below — used for the deferred
    // summary dispatch in the idle gap at the end of this handler.
    const QString completedConvId = m_inflightConvId;

    if (roundTurns >= 2 && !pauseNoted && finishReason == QStringLiteral("stop") &&
        m_queuedUserText.isEmpty() && !completedConvId.isEmpty()) {
        const auto convSettle = m_convSvc.getConversation(completedConvId);
        if (convSettle.has_value() && convSettle->isGroup) {
            m_msgSvc.addInterveningSystemMessage(
                completedConvId,
                QStringLiteral("✔ Round settled with no further handoffs "
                               "after %1 agent turn(s). The team "
                               "is waiting for you.")
                    .arg(roundTurns));
        }
    }

    if (m_taskGate) {
        m_taskGate->applyDeclaredStatus(declaredTaskStatus, m_inflightConvId);
        const auto convForTask = m_convSvc.getConversation(m_inflightConvId);
        if (convForTask.has_value() &&
            LlmConfig::fromJson(convForTask->llmConfig).implicitTaskCompletion) {
            m_taskGate->applyImplicitCompletion(declaredTaskStatus,
                                                finishReason,
                                                m_toolDispatcher->iterationCount(),
                                                m_inflightConvId);
        }
    }

    m_streaming->resetWithoutAbort();
    m_toolDispatcher->resetState();
    m_inflightConvId.clear();
    m_currentRequestId = 0;  // request finished cleanly

    m_isGenerating = false;
    emit isGeneratingChanged();

    qCDebug(verzetaUi) << "Request finished:" << finishReason << "tokens:" << totalTokens
                       << "elapsed:" << elapsedMs << "ms";

    if (!m_queuedUserText.isEmpty()) {
        const QString queued = m_queuedUserText;
        const QStringList queuedAttachments = m_queuedUserAttachments;
        setQueuedUserMessage(QString(), {});
        if (queuedAttachments.isEmpty()) {
            sendMessage(queued);
        } else {
            sendMessageWithAttachments(queued, queuedAttachments);
        }
        return;  // queued user message owns the next turn (NOT idle —
                 // m_summaryWantedThisCascade carries into that turn's cascade).
    }

    // Idle gap: the cascade has fully drained and no user message is queued, so
    // the provider is free. Dispatch any deferred compaction summary now — this
    // is the whole point of deferring it out of the build path (no concurrent
    // foreground turn to saturate the provider / time out RAGP).
    if (m_summaryWantedThisCascade) {
        setSummaryWantedThisCascade(false);
        if (m_summarizer && !completedConvId.isEmpty()) {
            m_summarizer->generateAsync(completedConvId, QStringLiteral("auto"));
        }
    }

    drainPendingSubagentReactions();
}

void ConversationRun::onCascadeEchoDetectedRequestRetry(const QString& convId,
                                                        const QString& badMsgId,
                                                        const QString& alias,
                                                        const QString& agentId,
                                                        int retryAttempt,
                                                        const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    Q_UNUSED(convId);  // currently always == m_inflightConvId

    qCWarning(verzetaUi) << "Echo/empty retry: deleting badMsgId" << badMsgId
                         << "and re-dispatching @" << alias << "attempt" << retryAttempt << "/"
                         << Chat::CascadeController::kMaxEchoRetries;

    if (!badMsgId.isEmpty()) {
        m_msgSvc.deleteMessage(badMsgId);
    }

    if (alias.isEmpty()) {
        m_msgSvc.addInterveningSystemMessage(
            m_inflightConvId,
            QStringLiteral("↻ The model returned an empty response, so it is being asked "
                           "to try again (attempt %1 of %2).")
                .arg(retryAttempt)
                .arg(Chat::CascadeController::kMaxEchoRetries));
    }

    m_toolDispatcher->resetState();
    m_streaming->resetWithoutAbort();
    m_currentEchoRetryAttempt = retryAttempt;
    m_currentRetryReason = reason;
    m_streaming->begin(m_inflightConvId, QStringLiteral("assistant"), agentId, alias);
    QTimer::singleShot(0, this, [this]() { buildAndSendRequest(); });
}

void ConversationRun::onCascadeEchoMaxRetriesExhausted(const QString& convId,
                                                       const QString& badMsgId,
                                                       bool wasEmpty) {
    VERZETA_ASSERT_MAIN_THREAD();
    Q_UNUSED(convId);

    qCWarning(verzetaUi) << "Echo retries exhausted: badMsgId" << badMsgId
                         << "wasEmpty:" << wasEmpty;

    m_currentEchoRetryAttempt = 0;
    m_currentRetryReason.clear();

    if (badMsgId.isEmpty())
        return;

    if (wasEmpty) {
        m_msgSvc.deleteMessage(badMsgId);
        return;
    }

    m_msgSvc.markMessageAsEchoFailed(badMsgId);
}

void ConversationRun::onCascadeDeferredContinuationRequested(const QString& convId,
                                                             const QString& alias,
                                                             const QString& agentId,
                                                             const QString& hint) {
    if (convId != m_inflightConvId) {
        qCDebug(verzetaUi) << "Deferred continuation dropped — conversation changed"
                           << "(was" << convId.left(8) << ")";
        return;  // turn state already reset; nothing to finalize
    }

    // Preemption discipline: a user message queued while classify was in
    // flight wins over the continuation — finalize the cascade at this
    // boundary so onCascadeComplete dispatches the queued message.
    if (!m_queuedUserText.isEmpty() || !m_queuedUserAttachments.isEmpty()) {
        qCDebug(verzetaUi) << "Deferred continuation superseded by a queued user message"
                           << "— finalizing the cascade instead";
        Chat::CascadeRouteInputs preemptInputs;
        preemptInputs.convId = convId;
        preemptInputs.finishReason = QStringLiteral("user_message_pending");
        m_cascade->forceCascadeComplete(preemptInputs);
        return;
    }

    // Per-alias budget re-check (the pre-classify snapshot can be stale
    // by one turn in a fast round; the budget is the real gate).
    const QString key = alias.toLower();
    if (m_groupDeferredContinuations.value(key, 0) >= kMaxGroupDeferredPerAlias) {
        qCInfo(verzetaUi) << "Deferred continuation budget exhausted for @" << alias
                          << "— accepting the reply, finalizing";
        Chat::CascadeRouteInputs doneInputs;
        doneInputs.convId = convId;
        doneInputs.finishReason = QStringLiteral("stop");
        m_cascade->forceCascadeComplete(doneInputs);
        return;
    }
    m_groupDeferredContinuations[key] = m_groupDeferredContinuations.value(key, 0) + 1;
    // A corrective continuation is round progress — the cascade round
    // must not pause mid-correction.
    m_cascade->noteCorrectiveContinuationThisRound();

    m_deferredContinuationPending = true;
    m_currentEchoRetryAttempt = 0;  // not an empty/echo retry
    m_currentRetryReason.clear();
    m_msgSvc.addInterveningSystemMessage(
        m_inflightConvId,
        QStringLiteral("↻ @%1 described a file/tool action but didn't "
                       "run it, so they are being asked to do it now "
                       "(or hand it off).")
            .arg(alias));
    qCInfo(verzetaUi) << "Deferred-action (group, classify verdict) — continuation for @" << alias
                      << "| pending:" << hint.left(80) << "(reply kept, not deleted)";
    m_streaming->begin(m_inflightConvId, QStringLiteral("assistant"), agentId, alias);
    QTimer::singleShot(0, this, [this]() { buildAndSendRequest(); });
}

void ConversationRun::resumeAfterImageGenerated(const QString& convId,
                                                const QString& alias,
                                                const QString& agentId,
                                                const QString& imagePath) {
    VERZETA_ASSERT_MAIN_THREAD();

    // The completion belongs to this run's conversation only.
    if (convId != m_activeConvId)
        return;
    if (agentId.isEmpty())
        return;  // user/QML-driven generation — no agent to resume

    // A live cascade sees the persisted image row in history on its own
    // next turn; resuming is only needed when the conversation went idle
    // before the asynchronous completion arrived (the stall this fixes).
    if (m_isGenerating) {
        qCDebug(verzetaUi) << "Image follow-up skipped — conversation is mid-turn; the"
                           << "image row is in history for the live cascade";
        return;
    }
    // A queued user message is about to own the next turn — it wins.
    if (!m_queuedUserText.isEmpty() || !m_queuedUserAttachments.isEmpty()) {
        qCDebug(verzetaUi) << "Image follow-up skipped — a queued user message takes"
                           << "the turn (the image row is in history)";
        return;
    }
    if (m_asyncArtifactContinuations >= kMaxAsyncArtifactContinuations) {
        qCDebug(verzetaUi) << "Image follow-up budget exhausted for this round ("
                           << kMaxAsyncArtifactContinuations
                           << ") — the image row stays in history, no dispatch";
        return;
    }
    ++m_asyncArtifactContinuations;

    // Thread the generated image into the follow-through message so the
    // requesting agent SEES its own output once (vision models). An
    // unreadable file degrades to text-only feedback, never a skip.
    QFile img(imagePath);
    if (img.open(QIODevice::ReadOnly)) {
        LlmImageData data;
        data.base64 = img.readAll().toBase64();
        data.mimeType = QStringLiteral("image/png");
        m_pendingImages = {data};
    }

    m_asyncArtifactPending = true;
    m_currentEchoRetryAttempt = 0;
    m_currentRetryReason.clear();
    m_cascade->setCurrentResponder(alias, agentId);
    qCDebug(verzetaUi) << "Image follow-up — resuming @"
                       << (alias.isEmpty() ? QStringLiteral("assistant") : alias)
                       << "after async image completion (" << imagePath << ")";

    m_streaming->begin(m_activeConvId, QStringLiteral("assistant"), agentId, alias);
    m_inflightConvId = m_activeConvId;
    m_responseTimer.start();
    m_isGenerating = true;
    emit isGeneratingChanged();
    QTimer::singleShot(0, this, [this]() { buildAndSendRequest(); });
}

// ---------------------------------------------------------------------------
// ToolDispatcher feedback
// ---------------------------------------------------------------------------

void ConversationRun::onToolBatchCompleted(const QString& convId,
                                           const QString& agentId,
                                           const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();

    m_cascade->noteToolFireThisRound();

    const bool yieldFlag = m_yieldRequestedThisBatch;
    m_yieldRequestedThisBatch = false;
    if (yieldFlag && !m_cascade->isQueueEmpty() && m_cascade->yieldToQueuedAfterToolBatch(convId)) {
        qCDebug(verzetaUi) << "ConversationRun: request_turn yield honoured — "
                              "skipping continuation for @"
                           << alias << "after tool batch completed";
        return;
    }

    m_currentEchoRetryAttempt = 0;
    m_currentRetryReason.clear();
    // Mark the next build as a post-tool continuation so RequestBuilder can
    // steer a 1:1 model to keep going (read-then-cleared in buildAndSendRequest).
    m_postToolContinuationPending = true;

    m_streaming->begin(convId, QStringLiteral("assistant"), agentId, alias);
    buildAndSendRequest();
}

void ConversationRun::onToolDispatcherRequestTurnTarget(const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();

    const bool added = m_cascade->enqueueTarget(alias);
    if (added) {
        m_yieldRequestedThisBatch = true;
        qCDebug(verzetaUi) << "request_turn: queued" << alias
                           << "→ cascade queue size:" << m_cascade->queueSize()
                           << "→ yield flag set for current batch";
    }
}

void ConversationRun::onToolDispatcherActiveTaskAutoAnchored(const QString& planId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_taskGate)
        m_taskGate->setActivePlanId(planId);
}

void ConversationRun::onToolDispatcherBatchFailed(const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    qCWarning(verzetaUi) << "ToolDispatcher batch failed (flow-control event):" << reason;
}

// ---------------------------------------------------------------------------
// Sub-agent reactions + task dispatch + slot draining
// ---------------------------------------------------------------------------

void ConversationRun::dispatchSubagentReaction(const QString& convId,
                                               const QString& requesterAlias) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return;

    // Queue only behind THIS run's own in-flight turn. Cross-run /
    // cross-session serialization is the ProviderScheduler's job at
    // dispatch time, so a reaction need not wait on unrelated sessions.
    if (m_isGenerating) {
        const auto pending = qMakePair(convId, requesterAlias);
        if (!m_pendingSubagentReactions.contains(pending)) {
            m_pendingSubagentReactions.append(pending);
        }
        qCDebug(verzetaUi) << "Sub-agent reaction: queued behind in-flight generation"
                           << "(queue size" << m_pendingSubagentReactions.size() << ")";
        return;
    }

    QString agentId;
    QString alias;
    if (m_membershipService && !requesterAlias.isEmpty()) {
        const Member m = m_membershipService->findConversationMemberByAlias(convId, requesterAlias);
        if (m.isValid()) {
            agentId = m.agentId;
            alias = m.alias;
        }
    }
    if (agentId.isEmpty()) {
        const auto convOpt = m_convSvc.getConversation(convId);
        if (!convOpt.has_value())
            return;  // conversation deleted
        agentId = convOpt->primaryAgentId;
    }

    qCDebug(verzetaUi) << "Sub-agent reaction: dispatching @"
                       << (alias.isEmpty() ? QStringLiteral("(primary)") : alias) << "in conv"
                       << convId.left(8);

    m_inflightConvId = convId;
    m_currentRequestId = nextGlobalRequestId();
    m_cascade->setCurrentResponder(alias, agentId);
    m_toolDispatcher->resetState();
    m_streaming->begin(convId, QStringLiteral("assistant"), agentId, alias);
    m_responseTimer.start();
    m_isGenerating = true;
    emit isGeneratingChanged();
    QTimer::singleShot(0, this, [this]() { buildAndSendRequest(); });
}

void ConversationRun::drainPendingSubagentReactions() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_pendingSubagentReactions.isEmpty() || m_isGenerating)
        return;
    const auto next = m_pendingSubagentReactions.takeFirst();
    QTimer::singleShot(
        0, this, [this, next]() { dispatchSubagentReaction(next.first, next.second); });
}

void ConversationRun::drainTaskDispatchQueue() {
    if (m_taskDispatchQueue.isEmpty())
        return;
    qCWarning(verzetaUi) << "drainTaskDispatchQueue called with" << m_taskDispatchQueue.size()
                         << "entries — the task-dispatch path should not enqueue here. Dropping.";
    m_taskDispatchQueue.clear();
}

void ConversationRun::enqueueTaskDispatch(const TaskDispatchRequest& req) {
    TaskDispatchRequest resolved = req;
    if (resolved.agentId.isEmpty() && m_membershipService && !resolved.convId.isEmpty() &&
        !resolved.ownerAlias.isEmpty()) {
        const Member m = m_membershipService->findConversationMemberByAlias(resolved.convId,
                                                                            resolved.ownerAlias);
        if (m.isValid())
            resolved.agentId = m.agentId;
    }

    m_taskDispatchQueue.append(resolved);
    qCDebug(verzetaUi) << "Task dispatch queued:" << resolved.ownerAlias << "step"
                       << resolved.stepId << "reason" << resolved.reason << "(queue size"
                       << m_taskDispatchQueue.size() << ")";

    if (!m_isGenerating) {
        drainTaskDispatchQueue();
    }
}

void ConversationRun::drainQueuedSendIfPossible() {
    if (m_queuedUserText.isEmpty())
        return;
    if (m_isGenerating)
        return;
    // No cross-session gate: the per-conversation queued send drains as
    // soon as THIS run is idle; the ProviderScheduler serializes the
    // resulting dispatch per provider.
    const QString text = m_queuedUserText;
    const QStringList atts = m_queuedUserAttachments;
    setQueuedUserMessage(QString(), {});
    if (atts.isEmpty()) {
        sendMessage(text);
    } else {
        sendMessageWithAttachments(text, atts);
    }
}

void ConversationRun::onSlotAvailable() {
    drainQueuedSendIfPossible();
    drainPendingSubagentReactions();
}

// ---------------------------------------------------------------------------
// Task artifact posting
// ---------------------------------------------------------------------------

void ConversationRun::onTaskArtifactReady(const QString& convId,
                                          const QString& agentId,
                                          const QString& alias,
                                          const QString& artifactContent,
                                          const QString& summary,
                                          const QString& stepId,
                                          const QString& planId) {
    if (convId.isEmpty() || artifactContent.isEmpty())
        return;

    Message msg;
    msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.conversationId = convId;
    msg.role = QStringLiteral("assistant");
    msg.content = artifactContent;
    msg.contentHtml = MarkdownConverter::instance().toHtml(artifactContent);
    msg.createdAt = QDateTime::currentDateTimeUtc();
    msg.tokenCount = 0;
    msg.finishReason = QStringLiteral("artifact");
    msg.modelUsed = m_currentDispatchedModelName;
    msg.agentId = agentId;
    msg.memberAlias = alias;
    msg.metadata = QJsonObject{
        {QStringLiteral("plan_artifact"), true},
        {QStringLiteral("step_id"), stepId},
        {QStringLiteral("plan_id"), planId},
        {QStringLiteral("summary"), summary},
    };
    m_msgSvc.addMessage(msg);
    qCDebug(verzetaUi) << "Artifact posted as assistant msg —"
                       << "alias:" << alias << "chars:" << artifactContent.length();

    if (m_canvasSvc && !artifactContent.isEmpty()) {
        QString filename;
        const QString trimSummary = summary.trimmed();
        if (!trimSummary.isEmpty()) {
            static const QRegularExpression rx(QStringLiteral("([\\w./_-]+\\.[a-zA-Z0-9]{1,8})"));
            const auto match = rx.match(trimSummary.left(120));
            if (match.hasMatch()) {
                filename = QFileInfo(match.captured(1)).fileName();
            }
        }
        if (filename.isEmpty()) {
            filename = QStringLiteral("artifact-%1.md").arg(stepId.left(8));
        }
        m_canvasSvc->tryAutoPromote(convId, filename, artifactContent);
    }
}

// ---------------------------------------------------------------------------
// Request build + dispatch
// ---------------------------------------------------------------------------

Chat::BuildRequestInputs ConversationRun::makeBuildInputs() {
    Chat::BuildRequestInputs inputs;
    inputs.agentRegistry = m_agentRegistry;
    inputs.fileService = m_fileService;
    inputs.membershipService = m_membershipService;
    inputs.planService = m_planService;
    inputs.ragService = m_ragService;
    inputs.skillService = m_skillService;
    inputs.toolService = m_toolService;
    inputs.taskObserver = m_taskObserver;
    inputs.heartbeatConfigService = m_heartbeatConfigService;
    inputs.pollService = m_pollService;
    if (m_skillService) {
        const auto resolved = m_skillService->resolveForConversation(m_activeConvId);
        inputs.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        inputs.skillsExposeOnly = resolved.exposeOnly;
    }
    inputs.activeConvId = m_activeConvId;
    inputs.inflightConvId = m_inflightConvId;
    inputs.requestId = m_currentRequestId;
    inputs.streamingMsgId = m_streaming->msgId();
    inputs.cascadeIterations = m_cascade->cascadeIterations();
    inputs.responseMemberAgentId = m_cascade->currentResponderAgentId();
    inputs.responseMemberAlias = m_cascade->currentResponderAlias();
    inputs.activeTaskPlanId = m_taskGate ? m_taskGate->activePlanId() : QString();
    inputs.lastUserText = m_lastUserText;
    inputs.ragContext = m_pendingRagContext;  // pre-fetched async (G2)
    inputs.pendingFileContext = m_pendingFileContext;
    inputs.pendingImages = m_pendingImages;
    inputs.toolsEnabled = m_toolsEnabled;
    inputs.maxAgentCascade = Chat::CascadeController::kMaxAgentCascade;
    inputs.echoRetryAttempt = m_currentEchoRetryAttempt;
    inputs.retryReason = m_currentRetryReason;
    inputs.isPostToolContinuation = m_postToolContinuationPending;
    m_postToolContinuationPending = false;
    inputs.isDeferredActionContinuation = m_deferredContinuationPending;
    m_deferredContinuationPending = false;
    // Read-then-clear: only the idle-resume turn dispatched after an
    // async image generation completed sees this true.
    inputs.isAsyncArtifactContinuation = m_asyncArtifactPending;
    m_asyncArtifactPending = false;
    inputs.summarizer = m_summarizer;
    inputs.canvasFilename = m_activeCanvasFilename;
    inputs.canvasLanguage = m_activeCanvasLanguage;
    inputs.canvasRevision = m_activeCanvasRevision;
    inputs.canvasLineCount = m_activeCanvasLineCount;
    inputs.canvasByteSize = m_activeCanvasByteSize;
    return inputs;
}

void ConversationRun::buildAndSendRequest() {
    if (m_inflightConvId.isEmpty()) {
        m_inflightConvId = m_activeConvId;
    }
    m_currentRequestId = nextGlobalRequestId();

    // Off-turn-path async unified recall (signal-driven). If it starts, the turn
    // resumes in onMemoryRetrievalReady → buildAndDispatchDirect; otherwise
    // dispatch now, un-augmented. buildRequest never embeds/blocks the main thread.
    if (maybeBeginMemoryRetrieve(PendingTurn::Direct)) {
        return;
    }
    buildAndDispatchDirect();
}

void ConversationRun::buildAndDispatchDirect() {
    Chat::BuildRequestInputs inputs = makeBuildInputs();

    const Chat::BuildResult br = m_requestBuilder->buildRequest(inputs);

    updateContextFillPercent(br.contextFillPercent);
    updateCompactionCadence(br.compactionTurnsUsed, br.compactionTurnsTotal);

    if (br.clearActiveTaskPlanId && m_taskGate) {
        m_taskGate->clearActivePlan();
    }

    if (!br.success) {
        emit errorOccurred(br.errorReason);
        onRequestError(m_currentRequestId, br.errorReason);
        return;
    }

    noteSummaryDecision(br);

    if (br.consumedAttachments) {
        m_pendingImages.clear();
        m_pendingFileContext.clear();
    }

    // Capture the actual dispatched provider/model BEFORE route() —
    // RequestBuilder's per-agent/per-member override substitution writes
    // into br.request.config; ModelRouter::route() dispatches by that
    // providerId without swapping m_active.
    m_currentDispatchedModelName = br.request.config.modelName;
    m_currentDispatchedProviderId = br.request.config.providerId;

    dispatchThroughScheduler(br.request);
    m_pendingRagContext.clear();  // consumed for this turn
}

bool ConversationRun::maybeBeginMemoryRetrieve(PendingTurn turn) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_memoryRetriever || m_lastUserText.isEmpty()) {
        return false;
    }
    // Unified pre-turn recall: the coordinator consults every source's OWN gate
    // (RAG llm_config.rag_enabled / AIM llm_config.aim_enabled / ACN later),
    // embeds the query ONCE, and returns the merged injected context. beginRetrieve
    // returns 0 when nothing is enabled (or empty query) → dispatch un-augmented.
    Chat::MemoryContext ctx;
    ctx.conversationId = m_inflightConvId;
    ctx.responderAgentId = m_cascade->currentResponderAgentId();
    ctx.queryText = m_lastUserText;
    // The MemoryAugmented agent pattern means "always augment with memory" — it
    // forces the coordinator's sources ON (overriding the per-conversation
    // toggles) and no longer retrieves separately, so there is ONE retrieval.
    ctx.forceMemory = (m_agentPattern == AgentPattern::MemoryAugmented);
    const quint64 rid = m_memoryRetriever->beginRetrieve(ctx);
    if (rid == 0) {
        return false;  // nothing to retrieve → dispatch un-augmented now
    }
    m_pendingQueryId = rid;
    m_pendingTurn = turn;
    m_pendingRagContext.clear();
    return true;
}

void ConversationRun::onMemoryRetrievalReady(quint64 requestId, const QString& injectedContext) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Identity match — ignore stale/cancelled results (stop, conversation
    // switch, or a newer turn that replaced this one). Not timing-based.
    if (requestId == 0 || requestId != m_pendingQueryId) {
        return;
    }
    const PendingTurn turn = m_pendingTurn;
    m_pendingQueryId = 0;
    m_pendingTurn = PendingTurn::None;
    m_pendingRagContext = injectedContext;  // already merged + formatted

    if (turn == PendingTurn::Direct) {
        buildAndDispatchDirect();
    } else if (turn == PendingTurn::Agent) {
        dispatchAgentTurn();
    }
}

void ConversationRun::ledgerBeginTurn(const QString& providerId, const QString& model) {
    if (m_ledgerTurnId != 0) {
        ContentionLedger::end(m_ledgerTurnId, false, QStringLiteral("superseded"));
    }
    m_ledgerTurnId = ContentionLedger::begin(ContentionLedger::CallClass::Turn, providerId, model);
    m_ledgerFirstTokSeen = false;
}

void ConversationRun::dispatchThroughScheduler(const LlmRequest& request) {
    VERZETA_ASSERT_MAIN_THREAD();

    const QString providerId = request.config.providerId;
    ledgerBeginTurn(providerId, request.config.modelName);

    // No scheduler attached → legacy immediate dispatch (single-instance
    // tests that never wired one).
    if (!m_providerScheduler) {
        doRoute(request);
        return;
    }

    // A run drives exactly one provider request at a time. If we somehow
    // still hold a slot here it is a stale hold from an earlier turn that
    // did not release — free it before acquiring again so we never leak a
    // second slot. (Belt-and-suspenders; the terminal paths release.)
    if (!m_schedulerHeldProviderId.isEmpty()) {
        qCWarning(verzetaUi) << "ConversationRun: dispatching a new turn while still"
                                " holding provider slot"
                             << m_schedulerHeldProviderId << "— releasing the stale hold";
        releaseProviderSlotIfHeld();
    }

    // Guard the deferred grant against this run being destroyed between
    // enqueue and the grant firing.
    QPointer<ConversationRun> self(this);
    const quint64 dispatchRequestId = m_currentRequestId;
    const bool granted = m_providerScheduler->acquire(
        providerId, m_schedulerWaiterId, [self, request, dispatchRequestId]() {
            if (!self) {
                // Run destroyed while queued — the scheduler already took
                // the slot on our behalf in release(); hand it straight
                // back so the provider does not wedge.
                return;
            }
            // If the run moved on to a different request (stop + resend,
            // retry) the queued grant is stale: release the slot the
            // scheduler reserved for us instead of dispatching an old
            // request.
            if (self->m_currentRequestId != dispatchRequestId || !self->m_isGenerating) {
                self->m_schedulerHeldProviderId =
                    request.config.providerId;  // mark so release is exact
                self->releaseProviderSlotIfHeld();
                return;
            }
            self->m_schedulerQueuedProviderId.clear();
            self->doRoute(request);
        });

    if (granted) {
        // Slot taken immediately — dispatch now.
        doRoute(request);
    } else {
        // Parked; the onGranted callback above will dispatch when free.
        m_schedulerQueuedProviderId = providerId;
    }
}

void ConversationRun::doRoute(const LlmRequest& request) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Single recording point: from here on this run holds the provider's
    // slot and is responsible for exactly one matching release on the
    // turn's terminal path.
    m_schedulerHeldProviderId = request.config.providerId;
    m_schedulerQueuedProviderId.clear();
    ContentionLedger::markDispatched(m_ledgerTurnId);
    m_router.route(request);
}

void ConversationRun::releaseProviderSlotIfHeld() {
    VERZETA_ASSERT_MAIN_THREAD();
    // Catch-all for terminal paths that never reached finished/error
    // (user stop, teardown, stale grant). Idempotent: a normal finish
    // already ended the entry and this becomes a no-op.
    ContentionLedger::end(m_ledgerTurnId, false, QStringLiteral("released"));
    if (m_schedulerHeldProviderId.isEmpty()) {
        return;  // not holding — idempotent no-op.
    }
    const QString providerId = m_schedulerHeldProviderId;
    // Clear FIRST so a re-entrant terminal path (release → grant →
    // dispatch → terminal) cannot double-release this same hold.
    m_schedulerHeldProviderId.clear();
    if (m_providerScheduler) {
        m_providerScheduler->release(providerId);
    }
}

void ConversationRun::cancelQueuedSchedulerWaiter() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_schedulerQueuedProviderId.isEmpty()) {
        return;
    }
    m_schedulerQueuedProviderId.clear();
    if (m_providerScheduler) {
        m_providerScheduler->cancel(m_schedulerWaiterId);
    }
}

QString ConversationRun::storeUserMessage(const QString& text) {
    Message msg;
    msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.conversationId = m_activeConvId;
    msg.role = QStringLiteral("user");
    msg.content = text;
    msg.createdAt = QDateTime::currentDateTimeUtc();
    msg.tokenCount = 0;

    const QString savedId = m_msgSvc.addMessage(msg);
    if (savedId.isEmpty()) {
        qCWarning(verzetaUi) << "Failed to persist user message";
        return {};
    }
    return savedId;
}

void ConversationRun::updateContextFillPercent(int percent) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (percent != m_contextFillPercent) {
        m_contextFillPercent = percent;
        emit contextFillPercentChanged();
    }
    // Relay real measurements onto the shared MessageService spine so
    // wire clients receive chat.context_fill.changed for their gauge.
    if (percent > 0 && !m_inflightConvId.isEmpty()) {
        m_msgSvc.notifyContextFillMeasured(m_inflightConvId, percent);
    }
}

void ConversationRun::noteSummaryDecision(const Chat::BuildResult& br) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!br.shouldSummarize || !m_summarizer) {
        return;
    }
    // Ceiling: when the window is already near-full, a long cascade could overflow
    // before it ends, so summarise NOW even though it briefly contends with the
    // provider — correctness (not losing context) wins over the latency blip. The
    // summarizer guards against re-entrant generation for the same conversation.
    constexpr int kSummaryForceFillPercent = 88;
    if (br.contextFillPercent >= kSummaryForceFillPercent && !m_inflightConvId.isEmpty()) {
        m_summarizer->generateAsync(m_inflightConvId, QStringLiteral("auto"));
        setSummaryWantedThisCascade(false);
        return;
    }
    // Otherwise defer to the idle gap after the cascade completes so the summary
    // never runs concurrently with the foreground turn.
    setSummaryWantedThisCascade(true);
}

void ConversationRun::setSummaryWantedThisCascade(bool wanted) {
    if (m_summaryWantedThisCascade == wanted) {
        return;
    }
    m_summaryWantedThisCascade = wanted;
    emit summaryDeferredPendingChanged();
}

void ConversationRun::updateCompactionCadence(int used, int total) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (used != m_compactionTurnsUsed || total != m_compactionTurnsTotal) {
        m_compactionTurnsUsed = used;
        m_compactionTurnsTotal = total;
        emit compactionCadenceChanged();
    }
}

}  // namespace Chat

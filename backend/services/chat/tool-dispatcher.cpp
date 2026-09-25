// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file tool-dispatcher.cpp
 * @brief Implementation of Chat::ToolDispatcher. Cross-collaborator
 *        effects (cascade-queue append, active-task auto-anchor)
 *        flow back to ChatController via signals instead of direct
 *        state mutations (star-topology rule; see the tool-dispatcher.h
 *        contract).
 * @layer Service (Chat subsystem)
 * @dependencies MessageService, ConversationService, ToolService,
 *               PlanService, TaskRunner, TaskObserver, FileService,
 *               MembershipService (latter six via ToolBatchInputs),
 *               QtConcurrent, QFutureWatcher,
 *               utils/logger.h, utils/thread-discipline.h.
 *
 * Behavioural contract: every public method is main-thread-only and
 * asserts that invariant via VERZETA_ASSERT_MAIN_THREAD(). The
 * tool handler runs on a QtConcurrent worker thread for tools that
 * do not touch SQLite or ChatController state. Residency is decided
 * per-tool by consulting `ToolService::runsOnMainThread(name)`;
 * SQLite-touching tools run inline on the main thread.
 * All follow-up work (DB writes, signal emits, tail-call to
 * dispatchNext) happens on the main thread inside the
 * QFutureWatcher::finished slot.
 */

#include "tool-dispatcher.h"

#include "../../models/activity-event.h"
#include "../../models/agent-plan.h"
#include "../../models/conversation.h"
#include "../../models/member.h"
#include "../../models/message.h"
#include "../../models/tool-call.h"
#include "../../services/audit-service.h"
#include "../../services/conversation-service.h"
#include "../../services/file-service.h"
#include "../../services/membership-service.h"
#include "../../services/message-service.h"
#include "../../services/plan-service.h"
#include "../../services/task-observer.h"
#include "../../services/task-runner.h"
#include "../../services/tool-service.h"
#include "../../utils/logger.h"
#include "../../utils/thread-discipline.h"
#include "cascade-controller.h"

#include <QtConcurrent/QtConcurrent>
#include <QTimer>

#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace Chat {

ToolDispatcher::ToolDispatcher(MessageService& msgSvc,
                               ConversationService& convSvc,
                               QObject* parent)
    : QObject(parent), m_msgSvc(msgSvc), m_convSvc(convSvc) {}

void ToolDispatcher::setAuditService(AuditService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_auditService = svc;
}

ToolDispatcher::~ToolDispatcher() = default;

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool ToolDispatcher::hasPendingBatch() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return !m_pendingCalls.isEmpty() || !m_batchParentMsgId.isEmpty();
}

int ToolDispatcher::pendingCount() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_pendingCalls.size();
}

int ToolDispatcher::iterationCount() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_iterations;
}

QString ToolDispatcher::currentBatchParentMsgId() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_batchParentMsgId;
}

// ---------------------------------------------------------------------------
// State mutations
// ---------------------------------------------------------------------------

void ToolDispatcher::appendPendingCall(const QJsonObject& call) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (call.isEmpty()) {
        qCWarning(verzetaUi) << "ToolDispatcher::appendPendingCall: empty call object ignored";
        return;
    }
    m_pendingCalls.append(call);
}

void ToolDispatcher::enqueueBatch(const ToolBatchInputs& inputs) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (m_pendingCalls.isEmpty()) {
        qCWarning(verzetaUi) << "ToolDispatcher::enqueueBatch: no pending calls — "
                                "nothing to dispatch";
        emit batchFailed(QStringLiteral("No pending tool calls when batch was enqueued"));
        return;
    }
    if (inputs.parentAssistantMsgId.isEmpty()) {
        qCWarning(verzetaUi) << "ToolDispatcher::enqueueBatch: empty parent assistant "
                                "msg id — aborting batch";
        m_pendingCalls.clear();
        emit batchFailed(QStringLiteral("Missing parent assistant row id for tool batch"));
        return;
    }
    if (!inputs.toolService) {
        qCWarning(verzetaUi) << "ToolDispatcher::enqueueBatch: no ToolService attached "
                                "— aborting batch";
        m_pendingCalls.clear();
        emit batchFailed(QStringLiteral("ToolService not attached when batch was enqueued"));
        return;
    }

    m_batchInputs = inputs;
    m_batchParentMsgId = inputs.parentAssistantMsgId;
    m_batchRequestId = inputs.requestId;
    // m_iterations counter carries over ACROSS batches within a
    // single user turn — ChatController resets it to 0 at turn end
    // (or on retry, cancel, conversation switch) via resetState().
    // We don't reset it here because a multi-batch turn (tool call
    // → tool call → tool call across continuation turns) must still
    // honour the global kMaxToolIterations cap.

    dispatchNext();
}

namespace {

/**
 * @brief Curated file-extension alternation for the structural
 *        filename detector. Extensions are language-neutral technical
 *        tokens (like tool names), so the gate works for narration in
 *        ANY language. This is the deliberate replacement for the retired
 *        English action-verb lists. Kept tight enough
 *        that prose artifacts ("e.g", "i.e", domains like
 *        "example.com") do not read as filenames.
 */
const char* kFileExtAlternation = "md|txt|rst|py|ipynb|js|mjs|ts|jsx|tsx|html|htm|css|scss|json|"
                                  "yaml|yml|sh|bash|zsh|bat|ps1|cpp|cxx|cc|c|h|hpp|hh|rs|go|java|"
                                  "kt|kts|qml|cs|rb|php|pl|lua|swift|sql|csv|tsv|xml|toml|ini|cfg|"
                                  "conf|properties|gradle|cmake|dockerfile|tex|bib|pdf|png|jpg|"
                                  "jpeg|svg|gif|webp|ico|mp3|wav|mp4|zip|tar|gz|log";

/** @brief Compiled filename detector (word chars/paths + curated extension). */
const QRegularExpression& fileTokenRx() {
    static const QRegularExpression rx(
        QStringLiteral("\\b([\\w/-]+\\.(?:%1))\\b").arg(QLatin1String(kFileExtAlternation)),
        QRegularExpression::CaseInsensitiveOption);
    return rx;
}

/** @brief Compiled canvas-surface detector (the surface is named, not a file). */
const QRegularExpression& canvasTokenRx() {
    static const QRegularExpression rx(QStringLiteral("\\bcanvas\\b"),
                                       QRegularExpression::CaseInsensitiveOption);
    return rx;
}

/**
 * @brief Builds the registered-tool-name detector for one call. Tool
 *        names come from the LIVE registry (the caller's ToolService
 *        snapshot), never a hardcoded list, so new tools gate
 *        automatically and the pattern can never drift from the
 *        registry.
 * @param registeredToolNames Names from ToolService::availableTools().
 * @returns Compiled alternation, or an invalid pattern when the list
 *          is empty (callers must skip the path then).
 */
QRegularExpression toolNameRx(const QStringList& registeredToolNames) {
    QStringList escaped;
    escaped.reserve(registeredToolNames.size());
    for (const QString& n : registeredToolNames) {
        const QString t = n.trimmed();
        if (!t.isEmpty())
            escaped.append(QRegularExpression::escape(t));
    }
    if (escaped.isEmpty())
        return QRegularExpression();
    return QRegularExpression(QStringLiteral("\\b(%1)\\b").arg(escaped.join(QLatin1Char('|'))),
                              QRegularExpression::CaseInsensitiveOption);
}

}  // namespace

QString ToolDispatcher::narrationDeferredActionKind(const QString& narration,
                                                    const QStringList& registeredToolNames) {
    if (narration.trimmed().isEmpty())
        return QString();

    const QRegularExpression toolRx = toolNameRx(registeredToolNames);
    if (toolRx.isValid() && !toolRx.pattern().isEmpty() && toolRx.match(narration).hasMatch()) {
        return QStringLiteral("deferred_action");
    }
    if (fileTokenRx().match(narration).hasMatch()) {
        return QStringLiteral("deferred_action");
    }
    if (canvasTokenRx().match(narration).hasMatch()) {
        return QStringLiteral("deferred_action");
    }
    const QString trimmed = narration.trimmed();
    if (trimmed.endsWith(QLatin1Char(':')) || trimmed.endsWith(QChar(0xFF1A))) {
        return QStringLiteral("deferred_action");
    }
    return QString();
}

QSet<QString> ToolDispatcher::announcedActionTargets(const QString& narration,
                                                     const QStringList& registeredToolNames) {
    QSet<QString> targets;
    if (narration.trimmed().isEmpty())
        return targets;

    // Same structural tokens as the gate above (one vocabulary, two
    // consumers): registered tool names, filename tokens normalized to
    // the lowercase basename, and the synthetic "canvas" target.
    const QRegularExpression toolRx = toolNameRx(registeredToolNames);
    if (toolRx.isValid() && !toolRx.pattern().isEmpty()) {
        auto it = toolRx.globalMatch(narration);
        while (it.hasNext()) {
            targets.insert(it.next().captured(1).toLower());
        }
    }

    auto it2 = fileTokenRx().globalMatch(narration);
    while (it2.hasNext()) {
        QString f = it2.next().captured(1).toLower();
        const int slash = f.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0)
            f = f.mid(slash + 1);
        if (!f.isEmpty())
            targets.insert(f);
    }

    if (canvasTokenRx().match(narration).hasMatch()) {
        targets.insert(QStringLiteral("canvas"));
    }
    return targets;
}

bool ToolDispatcher::announcesOnlyExecutedWork(const QString& narration,
                                               const QStringList& registeredToolNames,
                                               const QSet<QString>& executedToolNames,
                                               const QSet<QString>& executedFileBasenames) {
    const QSet<QString> targets = announcedActionTargets(narration, registeredToolNames);

    // No named target at all: with the structural gate this only
    // happens for narration with no concrete token — nothing to
    // suppress against, so candidacy stands only if a gate token
    // existed (the caller pairs this with the gate). Treat any
    // executed call as backing, mirroring the previous contract.
    if (targets.isEmpty()) {
        return !executedToolNames.isEmpty();
    }

    // Named targets: suppress only when EVERY announced target is covered
    // by a call that already ran this turn. One genuinely new target
    // (a file not yet touched, a tool not yet used) keeps the candidacy —
    // that is the legitimate multi-part chaining case ("styles.css is
    // written, next I'll create index.html").
    for (const QString& t : targets) {
        const bool isFile = t.contains(QLatin1Char('.'));
        if (isFile) {
            if (!executedFileBasenames.contains(t))
                return false;
        } else if (t == QStringLiteral("canvas")) {
            // The synthetic canvas target is covered by ANY executed
            // canvas-family call (open_canvas / edit_canvas / read_canvas).
            bool covered = false;
            for (const QString& name : executedToolNames) {
                if (name.contains(QStringLiteral("canvas"))) {
                    covered = true;
                    break;
                }
            }
            if (!covered)
                return false;
        } else {
            if (!executedToolNames.contains(t))
                return false;
        }
    }
    return true;
}

bool ToolDispatcher::continuationIsEcho(const QString& narration) {
    const QString trimmed = narration.trimmed();
    if (trimmed.isEmpty()) {
        // Empty narration: legitimate straight-to-tool turn. Do not
        // update the fingerprint — an empty set would make the NEXT
        // comparison meaningless, and the iteration cap already
        // bounds narration-less loops.
        return false;
    }
    const QSet<QString> currentFp = CascadeController::contentFingerprint(trimmed);
    if (currentFp.isEmpty()) {
        return false;  // All tokens stripped (e.g. "@Bob 1") — no signal.
    }
    bool echo = false;
    if (!m_lastChainNarrationFp.isEmpty()) {
        const double sim = CascadeController::jaccardSimilarity(currentFp, m_lastChainNarrationFp);
        echo = sim >= CascadeController::kCascadeNoveltyThreshold;
        if (echo) {
            qCWarning(verzetaUi) << "ToolDispatcher: tool-chain narration echo detected"
                                 << "(jaccard" << sim << ") — chain will be"
                                 << "force-finalized instead of continuing";
        }
    }
    m_lastChainNarrationFp = currentFp;
    return echo;
}

void ToolDispatcher::resetState() {
    m_lastChainNarrationFp.clear();
    VERZETA_ASSERT_MAIN_THREAD();
    m_pendingCalls.clear();
    m_batchParentMsgId.clear();
    m_iterations = 0;
    m_consecutiveFailures = 0;  // fresh turn → fresh failure streak
    m_batchRequestId = 0;
    m_batchInputs = ToolBatchInputs{};
}

// ---------------------------------------------------------------------------
// dispatchNext — the serial-batch workhorse.
//
//   - Pops one tool call from m_pendingCalls, persists the
//     tool_calls side-table row under the shared m_batchParentMsgId,
//     and runs the tool via QtConcurrent::run (off-thread) for
//     worker-safe tools; SQLite-touching tools (those whose
//     `ToolService::runsOnMainThread(name)` returns true) run inline
//     via a ready-future wrapper so the watcher still fires uniformly.
//   - Completion callback either dispatches the next call via a
//     QTimer::singleShot(0, ...) tail-call (prevents stack blow-up
//     on long synchronous batches) or emits
//     batchCompletedContinueTurn so ChatController can re-open
//     streaming + build the continuation request.
// ---------------------------------------------------------------------------
void ToolDispatcher::dispatchNext() {
    VERZETA_ASSERT_MAIN_THREAD();

    if (m_pendingCalls.isEmpty()) {
        qCWarning(verzetaUi) << "ToolDispatcher::dispatchNext: queue unexpectedly empty";
        return;
    }
    // Every early exit reports the abort, otherwise the turn stays open
    // forever: ConversationRun only finishes on a batch signal.
    if (m_batchParentMsgId.isEmpty()) {
        qCWarning(verzetaUi) << "ToolDispatcher::dispatchNext: no batch parent set";
        m_pendingCalls.clear();
        emit batchFailed(QStringLiteral("Missing parent assistant row id for tool batch"));
        return;
    }
    if (!m_batchInputs.toolService) {
        qCWarning(verzetaUi) << "ToolDispatcher::dispatchNext: no ToolService available";
        m_pendingCalls.clear();
        m_batchParentMsgId.clear();
        emit batchFailed(QStringLiteral("ToolService not attached during tool batch"));
        return;
    }
    if (iterationCapReached(m_iterations)) {
        qCWarning(verzetaUi) << "ToolDispatcher::dispatchNext: tool-iteration cap ("
                             << m_maxToolIterations << ") reached, aborting batch";
        m_pendingCalls.clear();
        m_batchParentMsgId.clear();
        emit batchFailed(
            QStringLiteral("Tool iteration cap (%1) reached").arg(m_maxToolIterations));
        return;
    }

    ++m_iterations;

    const QJsonObject call = m_pendingCalls.takeFirst();
    const QString toolName = call[QStringLiteral("name")].toString();
    const QJsonObject toolArgs = call[QStringLiteral("arguments")].toObject();
    const QString toolCallId = call[QStringLiteral("id")].toString();

    qCDebug(verzetaUi) << "ToolDispatcher: tool call #" << m_iterations << toolName
                       << "| batch-remaining:" << m_pendingCalls.size();

    const QString parentAssistantMsgId = m_batchParentMsgId;

    // Persist the tool_calls row BEFORE dispatching the handler so
    // the ToolCallLogModel picks it up immediately and the
    // side-table entry exists by the time the role=tool result row
    // references it.
    ToolCall callRow;
    callRow.id =
        toolCallId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : toolCallId;
    callRow.messageId = parentAssistantMsgId;
    callRow.toolName = toolName;
    callRow.arguments = toolArgs;
    callRow.status = QStringLiteral("running");
    callRow.startedAt = QDateTime::currentDateTimeUtc();
    if (m_batchInputs.planService && !m_batchInputs.activeTaskPlanId.isEmpty()) {
        const QString ownerAlias = m_batchInputs.responseMemberAlias.isEmpty()
                                       ? QStringLiteral("assistant")
                                       : m_batchInputs.responseMemberAlias;
        const auto stepOpt = m_batchInputs.planService->activeStepForOwnerInConversation(
            m_batchInputs.inflightConvId, ownerAlias);
        if (stepOpt.has_value()) {
            callRow.planStepId = stepOpt->id;
        }
    }
    const QString persistedCallId = m_msgSvc.addToolCall(callRow);
    if (persistedCallId.isEmpty()) {
        qCWarning(verzetaUi) << "addToolCall failed for" << toolName
                             << "(parent msgId:" << parentAssistantMsgId << ")";
    }

    const QString argsJson = QJsonDocument(toolArgs).toJson(QJsonDocument::Compact);
    emit toolCallStarted(toolName, argsJson);

    QString resolvedFolderId;
    if (m_batchInputs.fileService) {
        const QList<Folder> chain =
            m_convSvc.folderChainForConversation(m_batchInputs.inflightConvId);
        const Folder* project = nullptr;
        for (const Folder& f : chain) {
            if (f.isProject()) {
                project = &f;
                break;
            }
        }
        if (project) {
            m_batchInputs.fileService->setActiveProjectContext(project->id, project->name);
            resolvedFolderId = project->id;
        } else {
            m_batchInputs.fileService->setActiveConversation(m_batchInputs.inflightConvId);
            if (!chain.isEmpty()) {
                resolvedFolderId = chain.first().id;
            }
        }
    }
    m_batchInputs.resolvedCallerFolderId = resolvedFolderId;

    // Capture everything the callback needs BEFORE crossing thread
    // boundaries. The callback runs on the main thread (watcher
    // signal), so DB access inside it is safe.
    ToolService* toolSvc = m_batchInputs.toolService;
    const QString convIdCapture = m_batchInputs.inflightConvId;
    const QString callIdCapture = persistedCallId;
    const quint64 requestIdAtDispatch = m_batchRequestId;
    // Snapshot inputs that the async callback reads — preserving
    // pre-extraction semantics even if ChatController's own state
    // changes between dispatch and completion. These are COPIES by
    // design.
    const ToolBatchInputs inputsSnapshot = m_batchInputs;

    QPointer<ToolDispatcher> self(this);
    auto* watcher = new QFutureWatcher<QJsonValue>(this);
    connect(
        watcher,
        &QFutureWatcher<QJsonValue>::finished,
        this,
        [self,
         watcher,
         toolName,
         toolCallId,
         toolArgs,
         convIdCapture,
         callIdCapture,
         requestIdAtDispatch,
         inputsSnapshot]() {
            // Object-lifetime guard.
            if (!self) {
                return;
            }
            watcher->deleteLater();

            // Request-lifetime guard: if resetState() has fired (user
            // cancel, provider error, conversation switch, retry), our
            // batch id has been zeroed and no longer matches. Drop.
            if (self->m_batchRequestId != requestIdAtDispatch) {
                qCWarning(verzetaUi) << "ToolDispatcher: dropping stale tool result for" << toolName
                                     << "— batch request id changed from" << requestIdAtDispatch
                                     << "to" << self->m_batchRequestId;
                return;
            }

            const QJsonValue toolResult = watcher->result();

            QJsonObject resultObj = toolResult.toObject();
            if (resultObj.isEmpty()) {
                resultObj.insert(QStringLiteral("result"), toolResult);
            }
            const QString resultJson =
                QString::fromUtf8(QJsonDocument(resultObj).toJson(QJsonDocument::Compact));

            qCDebug(verzetaUi) << "ToolDispatcher: tool" << toolName
                               << "completed, result keys:" << resultObj.keys()
                               << "convId:" << convIdCapture;

            emit self->toolCallCompleted(toolName, resultJson);

            const QString callStatus = resultObj.contains(QStringLiteral("error"))
                                           ? QStringLiteral("error")
                                           : QStringLiteral("success");
            // Runaway-FAILURE detection (distinct from the iteration budget):
            // count consecutive failed tool calls, reset on ANY success. A
            // long run of successful tools is legitimate work; a run of
            // failures is a stuck loop (a broken tool retried forever) and
            // must stop regardless of the budget.
            if (callStatus == QStringLiteral("error")) {
                ++self->m_consecutiveFailures;
            } else {
                self->m_consecutiveFailures = 0;
            }
            if (!callIdCapture.isEmpty()) {
                self->m_msgSvc.updateToolCallResult(callIdCapture, toolResult, callStatus);
            }

            if (self->m_auditService) {
                // turn_id derives from the LLM request that started this
                // tool batch — same value ChatController stamps on its
                // agent_turn record so the timeline UI can group "agent
                // turn → N tool calls" into one expandable unit.
                const QString turnId = QString::number(requestIdAtDispatch);

                // tool_invoked is universal.
                const QString argsSummary =
                    QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Compact));
                self->m_auditService->record(
                    ActivityEvent::forToolInvoked(QString(),
                                                  convIdCapture,
                                                  turnId,
                                                  inputsSnapshot.responseMemberAlias,
                                                  inputsSnapshot.responseMemberAgentId,
                                                  QString(),
                                                  toolName,
                                                  argsSummary,
                                                  callStatus));

                // file_written: write_file returned {path, written}.
                if (toolName == QStringLiteral("write_file") &&
                    callStatus == QStringLiteral("success")) {
                    const QString path = resultObj.value(QStringLiteral("path")).toString();
                    const qint64 bytes =
                        resultObj.value(QStringLiteral("written")).toVariant().toLongLong();
                    if (!path.isEmpty()) {
                        self->m_auditService->record(
                            ActivityEvent::forFileWritten(QString(),
                                                          convIdCapture,
                                                          turnId,
                                                          inputsSnapshot.responseMemberAlias,
                                                          inputsSnapshot.responseMemberAgentId,
                                                          QString(),
                                                          path,
                                                          bytes));
                    }
                }

                // canvas_edited: edit_canvas returned
                // {canvas_id, revision, lines, byte_size, filename}.
                // The filename is added by the tool (one-line additive
                // change to edit-canvas-tool.cpp in this same commit).
                if (toolName == QStringLiteral("edit_canvas") &&
                    callStatus == QStringLiteral("success")) {
                    const QString fname = resultObj.value(QStringLiteral("filename")).toString();
                    const int revision = resultObj.value(QStringLiteral("revision")).toInt();
                    const int lines = resultObj.value(QStringLiteral("lines")).toInt();
                    self->m_auditService->record(
                        ActivityEvent::forCanvasEdited(QString(),
                                                       convIdCapture,
                                                       turnId,
                                                       inputsSnapshot.responseMemberAlias,
                                                       inputsSnapshot.responseMemberAgentId,
                                                       QString(),
                                                       fname,
                                                       revision,
                                                       lines));
                }
            }

            // request_turn: queue the named alias for cascade dispatch.
            // Cross-collaborator mutation — emit signal and let
            // ChatController own the m_agentCascadeQueue data. Dedup /
            // self-skip handled there.
            if (toolName == QStringLiteral("request_turn")) {
                QString target = resultObj[QStringLiteral("requested")].toString().trimmed();
                if (target.startsWith(QLatin1Char('@'))) {
                    target = target.mid(1).trimmed();
                }
                int emittedCount = 0;
                bool matchedSelf = false;
                if (!target.isEmpty() && inputsSnapshot.membershipService) {
                    const QList<Member> members =
                        inputsSnapshot.membershipService->conversationMembers(convIdCapture);
                    const bool isBroadcast =
                        target.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0 ||
                        target.compare(QStringLiteral("everyone"), Qt::CaseInsensitive) == 0 ||
                        target.compare(QStringLiteral("team"), Qt::CaseInsensitive) == 0;
                    if (isBroadcast) {
                        // Coordinators first (preserving pre-extraction
                        // order) then non-coordinators. Skip self.
                        for (const Member& mem : members) {
                            if (mem.isCoordinator &&
                                mem.alias.compare(inputsSnapshot.responseMemberAlias,
                                                  Qt::CaseInsensitive) != 0) {
                                emit self->requestTurnTargetQueued(mem.alias);
                                ++emittedCount;
                            }
                        }
                        for (const Member& mem : members) {
                            if (!mem.isCoordinator &&
                                mem.alias.compare(inputsSnapshot.responseMemberAlias,
                                                  Qt::CaseInsensitive) != 0) {
                                emit self->requestTurnTargetQueued(mem.alias);
                                ++emittedCount;
                            }
                        }
                    } else {
                        for (const Member& mem : members) {
                            const QString norm =
                                mem.alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
                            if (norm.compare(target, Qt::CaseInsensitive) == 0 ||
                                mem.alias.compare(target, Qt::CaseInsensitive) == 0) {
                                if (mem.alias.compare(inputsSnapshot.responseMemberAlias,
                                                      Qt::CaseInsensitive) != 0) {
                                    emit self->requestTurnTargetQueued(mem.alias);
                                    ++emittedCount;
                                } else {
                                    matchedSelf = true;
                                }
                                break;
                            }
                        }
                    }
                    qCDebug(verzetaUi) << "request_turn: target" << target << "→ emitted"
                                       << emittedCount << "memberTurnQueued signal(s)";
                    if (emittedCount == 0 && matchedSelf) {
                        // Honest zero-emit diagnostics: a SELF-target is a
                        // matched-but-skipped no-op (the caller already
                        // holds the turn), NOT a roster-matching failure.
                        // The old warning blamed alias matching for this
                        // case too, which read as a routing bug in live
                        // logs.
                        qCDebug(verzetaUi) << "request_turn: target" << target
                                           << "is the CALLER — self-requests are a no-op"
                                           << "(the agent already holds the turn)";
                    } else if (emittedCount == 0) {
                        qCWarning(verzetaUi) << "request_turn: alias" << target
                                             << "did not match any member in this conversation "
                                                "(roster size"
                                             << members.size() << "). The agent "
                                             << "called request_turn but no cascade dispatch will "
                                                "fire. Check alias spelling / case.";
                    }
                }
            }

            const QString artifactPath = resultObj[QStringLiteral("path")].toString();
            qCDebug(verzetaUi) << "ToolDispatcher: artifact check, path:" << artifactPath;

            // Persist the role=tool message BEFORE the auto-anchor
            // block so any task_event system row emitted by the
            // auto-anchor path does not interleave between the
            // assistant(tool_calls) row and its paired tool response.
            // The RequestBuilder walk-and-pair step depends on the
            // tool-row landing directly after its assistant parent.
            {
                Message toolMsg;
                toolMsg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                toolMsg.conversationId = convIdCapture;
                toolMsg.role = QStringLiteral("tool");
                toolMsg.content = resultJson;
                toolMsg.createdAt = QDateTime::currentDateTimeUtc();
                toolMsg.metadata = QJsonObject{{QStringLiteral("tool_call_id"), callIdCapture},
                                               {QStringLiteral("tool_name"), toolName}};
                self->m_msgSvc.addMessage(toolMsg);
            }

            // AUTO-ANCHOR: if a "substantive" tool (write_file,
            // run_shell, or any tool that produced a file) fires while
            // no active task is anchored, create a tracked plan
            // retroactively so the observability / task-lifecycle paths
            // have something to attribute the work to. Emits
            // activeTaskAutoAnchored; ChatController's slot owns the
            // active-plan-anchor write (star topology).
            static const QSet<QString> kSubstantiveTools = {
                QStringLiteral("write_file"),
                QStringLiteral("run_shell"),
            };
            const bool producedFile = !artifactPath.isEmpty();
            const bool isSubstantive = kSubstantiveTools.contains(toolName) || producedFile;
            if (isSubstantive && inputsSnapshot.activeTaskPlanId.isEmpty() &&
                inputsSnapshot.taskRunner && inputsSnapshot.planService &&
                !inputsSnapshot.lastUserText.trimmed().isEmpty()) {
                QString projectId, orgId;
                const QList<Folder> chain =
                    self->m_convSvc.folderChainForConversation(convIdCapture);
                for (const Folder& f : chain) {
                    if (f.folderType == QStringLiteral("project") && projectId.isEmpty())
                        projectId = f.id;
                    else if (f.folderType == QStringLiteral("organization") && orgId.isEmpty())
                        orgId = f.id;
                }
                const QString ownerAlias = inputsSnapshot.responseMemberAlias.isEmpty()
                                               ? QStringLiteral("assistant")
                                               : inputsSnapshot.responseMemberAlias;
                QList<PlanStep> step;
                {
                    PlanStep s;
                    s.title = inputsSnapshot.lastUserText.trimmed().left(80);
                    s.description = inputsSnapshot.lastUserText.trimmed();
                    s.ownerAlias = ownerAlias;
                    s.acceptanceCriteria =
                        QStringLiteral("Agent-initiated work: tool %1 fired without an "
                                       "explicit start_task. Auto-anchored by the "
                                       "TaskObserver.")
                            .arg(toolName);
                    step.append(s);
                }
                const QString newPlanId = inputsSnapshot.taskRunner->startTaskFromToolCall(
                    convIdCapture,
                    ownerAlias,
                    inputsSnapshot.lastUserText.trimmed(),
                    step,
                    projectId,
                    orgId);
                if (!newPlanId.isEmpty()) {
                    // Reflect the new anchor into the dispatcher's own
                    // inputs snapshot so subsequent calls in THIS batch
                    // see the task as active (matches pre-extraction
                    // self->m_activeTaskPlanId = newPlanId mutation
                    // semantics from the lambda's `self` view).
                    self->m_batchInputs.activeTaskPlanId = newPlanId;
                    emit self->activeTaskAutoAnchored(newPlanId);
                    qCDebug(verzetaUi)
                        << "AUTO-ANCHORED plan" << newPlanId << "for tool" << toolName
                        << "(no explicit start_task, but substantive work done)";
                }
            }

            // TaskObserver hook — log the tool call to step_artifacts
            // so PlansOverlay can show live activity. Re-read the
            // possibly-updated activeTaskPlanId from the dispatcher's
            // own inputs because the AUTO-ANCHOR block above may have
            // just populated it.
            if (inputsSnapshot.taskObserver && !self->m_batchInputs.activeTaskPlanId.isEmpty()) {
                static const QSet<QString> kSkipLog = {
                    QStringLiteral("start_task"),
                    QStringLiteral("complete_task"),
                    QStringLiteral("stop_task"),
                    QStringLiteral("get_task_status"),
                };
                if (!kSkipLog.contains(toolName)) {
                    const QString argsStr =
                        QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Compact));
                    QString resultSummary;
                    if (resultObj.contains(QStringLiteral("error"))) {
                        resultSummary = QStringLiteral("error: ") +
                                        resultObj[QStringLiteral("error")].toString();
                    } else if (!artifactPath.isEmpty()) {
                        resultSummary = QStringLiteral("wrote %1")
                                            .arg(artifactPath.split(QLatin1Char('/')).last());
                    } else {
                        const QString bodyPreview =
                            QString::fromUtf8(
                                QJsonDocument(resultObj).toJson(QJsonDocument::Compact))
                                .left(160);
                        resultSummary = QStringLiteral("ok: ") + bodyPreview;
                    }
                    const QString invoker = inputsSnapshot.responseMemberAlias.isEmpty()
                                                ? QStringLiteral("assistant")
                                                : inputsSnapshot.responseMemberAlias;
                    inputsSnapshot.taskObserver->recordToolCall(
                        self->m_batchInputs.activeTaskPlanId,
                        toolName,
                        argsStr.left(200),
                        resultSummary,
                        artifactPath,
                        invoker);
                    if (!artifactPath.isEmpty()) {
                        inputsSnapshot.taskObserver->recordFile(
                            self->m_batchInputs.activeTaskPlanId,
                            artifactPath,
                            inputsSnapshot.responseMemberAlias.isEmpty()
                                ? QStringLiteral("assistant")
                                : inputsSnapshot.responseMemberAlias,
                            toolName);
                    }
                }
            }

            // Stamp progress so the heartbeat watchdog doesn't nudge
            // the plan.
            if (inputsSnapshot.taskRunner && !self->m_batchInputs.activeTaskPlanId.isEmpty()) {
                inputsSnapshot.taskRunner->onIntermediateToolExecuted(
                    convIdCapture,
                    inputsSnapshot.responseMemberAlias,
                    inputsSnapshot.responseMemberAgentId,
                    self->m_batchInputs.activeTaskPlanId,
                    QString());
            }

            // If more tool calls are queued for this same assistant
            // response, dispatch the next one WITHOUT starting a new
            // LLM continuation turn — the provider expects the whole
            // parallel batch to resolve before we re-open streaming.
            // Only when the batch fully drains do we emit
            // batchCompletedContinueTurn and let ChatController ask
            // the LLM to continue.
            //
            // The QTimer::singleShot(0, weak.data(), ...) form is
            // deliberate: a direct recursive call blows the stack on
            // long synchronous batches; the QPointer bail-out keeps
            // the call safe if the dispatcher is destroyed between
            // ticks.
            if (!self->m_pendingCalls.isEmpty()) {
                QPointer<ToolDispatcher> weak(self);
                QTimer::singleShot(0, weak.data(), [weak]() {
                    if (!weak)
                        return;
                    weak->dispatchNext();
                });
                return;
            }

            // Batch drained — snapshot the continuation-turn identity
            // from the dispatcher's inputs BEFORE clearing state, emit
            // the completion signal, then clear. ChatController's slot
            // re-opens streaming and calls buildAndSendRequest.
            const QString finishConvId = convIdCapture;
            const QString finishAgentId = inputsSnapshot.responseMemberAgentId;
            const QString finishAlias = inputsSnapshot.responseMemberAlias;

            // Clear batch-identity state but KEEP m_iterations so the
            // cap still applies across continuation turns within a
            // single user turn (mirrors pre-extraction
            // m_toolIterations semantics — reset happens on turn end,
            // not on batch drain).
            self->m_batchParentMsgId.clear();
            self->m_batchRequestId = 0;
            self->m_batchInputs = ToolBatchInputs{};

            emit self->batchCompletedContinueTurn(finishConvId, finishAgentId, finishAlias);
        });

    const QString callerConvId = m_batchInputs.inflightConvId;
    const QString callerAgentId = m_batchInputs.responseMemberAgentId;
    const QString callerAgentAlias = m_batchInputs.responseMemberAlias;
    const QString callerFolderId = m_batchInputs.resolvedCallerFolderId;
    if (toolSvc->runsOnMainThread(toolName)) {
        const QJsonValue syncResult = toolSvc->invokeTool(
            toolName, toolArgs, callerConvId, callerAgentId, callerAgentAlias, callerFolderId);
        watcher->setFuture(QtConcurrent::run([syncResult]() -> QJsonValue { return syncResult; }));
    } else {
        watcher->setFuture(QtConcurrent::run([toolSvc,
                                              toolName,
                                              toolArgs,
                                              callerConvId,
                                              callerAgentId,
                                              callerAgentAlias,
                                              callerFolderId]() -> QJsonValue {
            return toolSvc->invokeTool(
                toolName, toolArgs, callerConvId, callerAgentId, callerAgentAlias, callerFolderId);
        }));
    }
}

}  // namespace Chat

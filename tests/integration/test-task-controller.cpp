// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "helpers/scripted-mock-provider.h"
#include "models/agent-plan.h"
#include "models/conversation.h"
#include "models/db-manager.h"
#include "models/message.h"
#include "services/agent-registry.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/file-service.h"
#include "services/membership-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/plan-service.h"
#include "services/task-controller.h"
#include "services/task-gate-service.h"
#include "services/task-observer.h"
#include "services/task-runner.h"
#include "services/tool-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestTaskController : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<PlanService> m_planSvc;
    std::unique_ptr<TaskRunner> m_taskRunner;
    std::unique_ptr<TaskObserver> m_taskObserver;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    std::unique_ptr<MembershipService> m_membership;
    std::unique_ptr<TaskGateService> m_taskGateSvc;
    std::unique_ptr<ChatController> m_chat;
    std::unique_ptr<TaskController> m_taskCtrl;
    QString m_convId;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    QString seedPlanViaTool(const QString& goal,
                            const QString& stepTitle = QStringLiteral("step")) {
        QJsonObject step{
            {QStringLiteral("title"), stepTitle},
            {QStringLiteral("description"), QStringLiteral("description")},
            {QStringLiteral("acceptance_criteria"), QStringLiteral("done when done")},
        };
        QJsonObject args{
            {QStringLiteral("goal"), goal},
            {QStringLiteral("steps"), QJsonArray{step}},
        };
        const QJsonValue v = m_toolSvc->invokeTool(QStringLiteral("start_task"), args);
        if (!v.isObject())
            return {};
        const QJsonObject result = v.toObject();
        if (result.contains(QStringLiteral("error")))
            return {};
        return result.value(QStringLiteral("plan_id")).toString();
    }

    QString firstStepIdOf(const QString& planId) const {
        const QList<PlanStep> steps = m_planSvc->stepsForPlan(planId);
        if (steps.isEmpty())
            return {};
        return steps.first().id;
    }

    void clearAnchor() {
        const QList<AgentPlan> plans = m_planSvc->plansForConversation(m_convId);
        for (const AgentPlan& p : plans) {
            m_taskRunner->handleStopTask(p.id, QStringLiteral("test cleanup"));
        }
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/tctrl_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();
        m_router->registerProvider(std::make_unique<ScriptedMockProvider>());
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_planSvc = std::make_unique<PlanService>(DbManager::instance());
        m_taskRunner = std::make_unique<TaskRunner>(*m_planSvc);
        m_taskObserver = std::make_unique<TaskObserver>(*m_planSvc);
        m_toolSvc = std::make_unique<ToolService>();
        m_fileSvc = std::make_unique<FileService>();
        m_agentRegistry = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentRegistry->initialize();
        m_membership = std::make_unique<MembershipService>(DbManager::instance());

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_chat->setToolService(m_toolSvc.get());
        m_chat->setPlanService(m_planSvc.get());
        m_chat->setTaskRunner(m_taskRunner.get());
        m_chat->setTaskObserver(m_taskObserver.get());
        m_chat->setFileService(m_fileSvc.get());
        m_chat->setAgentRegistry(m_agentRegistry.get());
        m_chat->setMembershipService(m_membership.get());

        m_taskGateSvc = std::make_unique<TaskGateService>(*m_msgSvc);
        m_taskGateSvc->setPlanService(m_planSvc.get());
        m_taskGateSvc->setTaskRunner(m_taskRunner.get());
        m_taskGateSvc->setTaskObserver(m_taskObserver.get());
        m_chat->setTaskGateService(m_taskGateSvc.get());

        m_taskCtrl =
            std::make_unique<TaskController>(*m_convSvc, *m_msgSvc, *m_planSvc, *m_taskRunner);
        m_taskCtrl->setAgentRegistry(m_agentRegistry.get());
        m_taskCtrl->setMembershipService(m_membership.get());

        m_taskCtrl->setFileService(m_fileSvc.get());

        QObject::connect(m_taskCtrl.get(),
                         &TaskController::taskArtifactReady,
                         m_chat.get(),
                         &ChatController::onTaskArtifactReady,
                         Qt::QueuedConnection);

        m_convId = m_convSvc->createConversation(QStringLiteral("TaskCtrlTest"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);

        m_taskCtrl->registerTaskToolStubs(*m_toolSvc);
        ChatController* chat = m_chat.get();
        m_taskCtrl->installTaskToolHandlers(
            *m_toolSvc,
            *m_taskGateSvc,
            [chat]() -> Chat::CascadeController* { return chat->cascadeInternal(); },
            [chat]() -> QString { return chat->activeConversationId(); });
    }

    void cleanup() {
        m_taskCtrl.reset();
        m_chat.reset();
        m_taskGateSvc.reset();
        m_membership.reset();
        m_agentRegistry.reset();
        m_fileSvc.reset();
        m_toolSvc.reset();
        m_taskObserver.reset();
        m_taskRunner.reset();
        m_planSvc.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_userInitiatedStartTask_happyPath_createsPlanAndEmits() {
        QSignalSpy spy(m_taskCtrl.get(), &TaskController::taskStarted);
        QVERIFY(spy.isValid());

        const int prePlans = m_planSvc->plansForConversation(m_convId).size();
        const int preMsgs = m_msgSvc->getMessages(m_convId).size();

        m_taskCtrl->userInitiatedStartTask(m_convId,
                                           QStringLiteral("write a haiku about pinning bugs"));

        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.size(), 4);

        const QString planId = args.at(0).toString();
        const QString emittedConv = args.at(1).toString();
        QVERIFY(!planId.isEmpty());
        QCOMPARE(emittedConv, m_convId);

        const auto plan = m_planSvc->getPlan(planId);
        QVERIFY(plan.has_value());
        QCOMPARE(plan->conversationId, m_convId);
        QCOMPARE(plan->goal, QStringLiteral("write a haiku about pinning bugs"));

        QCOMPARE(m_planSvc->stepsForPlan(planId).size(), 1);

        const auto msgs = m_msgSvc->getMessages(m_convId);
        QCOMPARE(msgs.size(), preMsgs + 1);
        const Message lastMsg = msgs.last();
        QCOMPARE(lastMsg.role, QStringLiteral("user"));
        QVERIFY(lastMsg.content.contains(QStringLiteral("pinning bugs")));

        QCOMPARE(m_planSvc->plansForConversation(m_convId).size(), prePlans + 1);
    }

    void test_userInitiatedStartTask_emptyGoal_isNoOp() {
        QSignalSpy spy(m_taskCtrl.get(), &TaskController::taskStarted);
        const int prePlans = m_planSvc->plansForConversation(m_convId).size();
        const int preMsgs = m_msgSvc->getMessages(m_convId).size();

        m_taskCtrl->userInitiatedStartTask(m_convId, QString());
        m_taskCtrl->userInitiatedStartTask(m_convId, QStringLiteral("   \t\n"));

        QCOMPARE(spy.count(), 0);
        QCOMPARE(m_planSvc->plansForConversation(m_convId).size(), prePlans);
        QCOMPARE(m_msgSvc->getMessages(m_convId).size(), preMsgs);
    }

    void test_stopPlan_marksPlanFailedAndEmitsPlanStopped() {
        const QString planId = seedPlanViaTool(QStringLiteral("plan to stop"));
        QVERIFY(!planId.isEmpty());

        QSignalSpy spy(m_taskCtrl.get(), &TaskController::planStopped);
        QVERIFY(spy.isValid());

        const bool ok = m_taskCtrl->stopPlan(planId, QStringLiteral("user pressed stop"));
        QVERIFY(ok);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), planId);

        const auto plan = m_planSvc->getPlan(planId);
        QVERIFY(plan.has_value());
        QCOMPARE(plan->status, PlanStatus::Failed);
    }

    void test_stopPlan_unknownPlanId_returnsFalse() {
        QSignalSpy spy(m_taskCtrl.get(), &TaskController::planStopped);

        const bool ok =
            m_taskCtrl->stopPlan(QStringLiteral("does-not-exist"), QStringLiteral("ignored"));
        QVERIFY(!ok);
        QCOMPARE(spy.count(), 0);
    }

    void test_stopAllActivePlansInConversation_stopsAllAndEmits() {
        const QString p1 = seedPlanViaTool(QStringLiteral("plan one"));
        QVERIFY(!p1.isEmpty());
        m_taskRunner->handleStopTask(p1, QStringLiteral("setup-cleanup"));
        const QString p2 = seedPlanViaTool(QStringLiteral("plan two"));
        QVERIFY(!p2.isEmpty());

        QSignalSpy spy(m_taskCtrl.get(), &TaskController::allPlansStoppedInConversation);
        QVERIFY(spy.isValid());

        const int stopped = m_taskCtrl->stopAllActivePlansInConversation(m_convId);
        QVERIFY(stopped >= 1);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), m_convId);

        const QList<AgentPlan> active = m_planSvc->activePlansForConversation(m_convId);
        QCOMPARE(active.size(), 0);
    }

    void test_retryStep_resetsCountersOnExistingStep() {
        const QString planId = seedPlanViaTool(QStringLiteral("retry me"));
        QVERIFY(!planId.isEmpty());
        const QString stepId = firstStepIdOf(planId);
        QVERIFY(!stepId.isEmpty());

        const bool ok = m_taskCtrl->retryStep(stepId, QStringLiteral("user note"));
        QVERIFY(ok);

        QVERIFY(!m_taskCtrl->retryStep(QStringLiteral("nope"), QStringLiteral("note")));
    }

    void test_overrideStepAsDone_marksStepDone() {
        const QString planId = seedPlanViaTool(QStringLiteral("override me"));
        QVERIFY(!planId.isEmpty());
        const QString stepId = firstStepIdOf(planId);
        QVERIFY(!stepId.isEmpty());

        const bool ok = m_taskCtrl->overrideStepAsDone(stepId);
        QVERIFY(ok);

        const auto step = m_planSvc->getStep(stepId);
        QVERIFY(step.has_value());
        QCOMPARE(step->status, StepStatus::Done);

        QVERIFY(!m_taskCtrl->overrideStepAsDone(QStringLiteral("nope")));
    }

    void test_skipStep_marksStepDoneWithSkipReason() {
        const QString planId = seedPlanViaTool(QStringLiteral("skip me"));
        QVERIFY(!planId.isEmpty());
        const QString stepId = firstStepIdOf(planId);
        QVERIFY(!stepId.isEmpty());

        const bool ok = m_taskCtrl->skipStep(stepId, QStringLiteral("not relevant"));
        QVERIFY(ok);

        const auto step = m_planSvc->getStep(stepId);
        QVERIFY(step.has_value());
        QCOMPARE(step->status, StepStatus::Done);
        QVERIFY(step->lastRejectionReason.contains(QStringLiteral("Skipped by user")));

        QVERIFY(!m_taskCtrl->skipStep(QStringLiteral("nope"), QStringLiteral("reason")));
    }

    void test_setPrimaryAgent_writesToConversationRow() {
        const QList<Agent> agents = m_agentRegistry->allAgents();
        QVERIFY(!agents.isEmpty());
        const QString agentId = agents.first().id;
        QVERIFY(!agentId.isEmpty());

        QCOMPARE(m_taskCtrl->primaryAgentId(m_convId), QString());

        m_taskCtrl->setPrimaryAgent(m_convId, agentId);
        QCOMPARE(m_taskCtrl->primaryAgentId(m_convId), agentId);

        const auto conv = m_convSvc->getConversation(m_convId);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->primaryAgentId, agentId);

        m_taskCtrl->setPrimaryAgent(QString(), agentId);
        QCOMPARE(m_taskCtrl->primaryAgentId(QString()), QString());
    }

    void test_taskStartedSignal_movesActivePlanAnchorOnChatController() {
        QObject::connect(m_taskCtrl.get(),
                         &TaskController::taskStarted,
                         m_chat.get(),
                         &ChatController::onExternalTaskStarted);

        QCOMPARE(m_chat->activeTaskPlanId(), QString());

        m_taskCtrl->userInitiatedStartTask(
            m_convId, QStringLiteral("goal that should anchor the chat session"));

        QVERIFY2(!m_chat->activeTaskPlanId().isEmpty(),
                 "ChatController::onExternalTaskStarted did not run after "
                 "TaskController::taskStarted fired — wiring regression "
                 "or a connect-order bug that the wiring guard "
                 "in app-controller.cpp prevents.");
    }

    void test_postTaskEventMessage_writesSystemMessageWithMetadata() {
        const int preCount = m_msgSvc->getMessages(m_convId).size();

        m_taskCtrl->postTaskEventMessage(m_convId,
                                         QStringLiteral("system"),
                                         QStringLiteral("🏁 Task completed: write a haiku"),
                                         QStringLiteral("task_completed"));

        const QList<Message> msgs = m_msgSvc->getMessages(m_convId);
        QCOMPARE(msgs.size(), preCount + 1);

        const Message& m = msgs.last();
        QCOMPARE(m.role, QStringLiteral("system"));
        QVERIFY(m.content.contains(QStringLiteral("Task completed")));

        const QJsonObject meta = m.metadata;
        QCOMPARE(meta.value(QStringLiteral("task_event")).toBool(), true);
        QCOMPARE(meta.value(QStringLiteral("event_type")).toString(),
                 QStringLiteral("task_completed"));

        m_taskCtrl->postTaskEventMessage(QString(),
                                         QStringLiteral("system"),
                                         QStringLiteral("ignored"),
                                         QStringLiteral("ignored"));
        m_taskCtrl->postTaskEventMessage(
            m_convId, QStringLiteral("system"), QString(), QStringLiteral("ignored"));
        QCOMPARE(m_msgSvc->getMessages(m_convId).size(), preCount + 1);
    }
};

QTEST_MAIN(TestTaskController)
#include "test-task-controller.moc"

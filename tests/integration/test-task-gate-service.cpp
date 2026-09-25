// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "models/agent-plan.h"
#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/message-list-model.h"
#include "models/message.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/plan-service.h"
#include "services/task-controller.h"
#include "services/task-gate-service.h"
#include "services/task-observer.h"
#include "services/task-runner.h"
#include "services/tool-service.h"
#include "utils/notification-manager.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestTaskGateService : public QObject {
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
    std::unique_ptr<TaskGateService> m_taskGateSvc;
    std::unique_ptr<TaskController> m_taskCtrl;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provider = nullptr;
    QString m_convId;

    void sendAndWait(const QString& text, int timeoutMs = 10000) {
        m_chat->sendMessage(text, {});
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < timeoutMs) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(),
                 qPrintable(QStringLiteral("ChatController never finished generating within %1ms")
                                .arg(timeoutMs)));
    }

    void waitForGenerationToFinish(int timeoutMs = 10000) {
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < timeoutMs) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(),
                 qPrintable(QStringLiteral("ChatController never finished generating within %1ms")
                                .arg(timeoutMs)));
    }

    QList<Message> currentMessages() const { return m_msgSvc->getRecentMessages(m_convId, 200); }

    static ScriptedMockProvider::ScriptStep textStep(const QString& text) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {text};
        s.finishReason = QStringLiteral("stop");
        s.tokens = 1;
        return s;
    }

  private slots:

    void initTestCase() {
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, []() {
            NotificationManager::instance().shutdown();
        });
    }

    void init() {
        const QString dbPath =
            m_tempDir.path() + QStringLiteral("/tg_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();
        m_planSvc = std::make_unique<PlanService>(DbManager::instance());
        m_taskRunner = std::make_unique<TaskRunner>(*m_planSvc);
        m_taskObserver = std::make_unique<TaskObserver>(*m_planSvc);
        m_toolSvc = std::make_unique<ToolService>();

        auto provider = std::make_unique<ScriptedMockProvider>();
        m_provider = provider.get();
        m_router->registerProvider(std::move(provider));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_chat->setPlanService(m_planSvc.get());
        m_chat->setTaskRunner(m_taskRunner.get());
        m_chat->setTaskObserver(m_taskObserver.get());
        m_chat->setToolService(m_toolSvc.get());

        m_taskGateSvc = std::make_unique<TaskGateService>(*m_msgSvc);
        m_taskGateSvc->setPlanService(m_planSvc.get());
        m_taskGateSvc->setTaskRunner(m_taskRunner.get());
        m_taskGateSvc->setTaskObserver(m_taskObserver.get());
        m_chat->setTaskGateService(m_taskGateSvc.get());

        m_taskCtrl =
            std::make_unique<TaskController>(*m_convSvc, *m_msgSvc, *m_planSvc, *m_taskRunner);
        QObject::connect(m_taskCtrl.get(),
                         &TaskController::taskStarted,
                         m_chat.get(),
                         &ChatController::onExternalTaskStarted);
        QObject::connect(m_taskCtrl.get(),
                         &TaskController::planStopped,
                         m_chat.get(),
                         &ChatController::onExternalPlanStopped);
        QObject::connect(m_taskCtrl.get(),
                         &TaskController::allPlansStoppedInConversation,
                         m_chat.get(),
                         &ChatController::onExternalAllPlansStoppedInConversation);

        m_convId = m_convSvc->createConversation(QStringLiteral("TestTaskGate"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);
    }

    void cleanup() {
        m_chat.reset();
        m_taskCtrl.reset();
        m_taskGateSvc.reset();
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

    void testTaskGate_userInitiatedStartTask_anchorsPlan() {
        m_provider->setScript({textStep(QStringLiteral("working on it"))});

        m_taskCtrl->userInitiatedStartTask(m_convId, QStringLiteral("write a haiku"));
        waitForGenerationToFinish();

        const QString planId = m_chat->activeTaskPlanId();
        QVERIFY2(!planId.isEmpty(), "activeTaskPlanId must be non-empty after start-task");

        const auto pOpt = m_planSvc->getPlan(planId);
        QVERIFY2(pOpt.has_value(), "PlanService must have a row for the anchored plan");
        QVERIFY(pOpt->status != PlanStatus::Completed);
        QVERIFY(pOpt->status != PlanStatus::Failed);
    }

    void testTaskGate_declaredCompletedMarker_terminalsPlan() {
        m_provider->setScript({
            textStep(QStringLiteral("starting")),
            textStep(QStringLiteral("Here it is.\n<task_state>completed</task_state>")),
        });

        m_taskCtrl->userInitiatedStartTask(m_convId, QStringLiteral("write a haiku"));
        waitForGenerationToFinish();

        const QString planId = m_chat->activeTaskPlanId();
        QVERIFY(!planId.isEmpty());

        sendAndWait(QStringLiteral("please continue"));

        bool foundDeclaration = false;
        for (const Message& m : currentMessages()) {
            if (m.role == QStringLiteral("assistant") &&
                m.content.contains(QStringLiteral("Here it is"))) {
                foundDeclaration = true;
                QVERIFY2(!m.content.contains(QStringLiteral("<task_state>")),
                         qPrintable(QStringLiteral("<task_state> marker should be stripped from "
                                                   "persisted content. Got: %1")
                                        .arg(m.content)));
            }
        }
        QVERIFY2(foundDeclaration,
                 "The declared-completion reply must be persisted as an "
                 "assistant row");

        const auto pOpt = m_planSvc->getPlan(planId);
        QVERIFY(pOpt.has_value());
        QCOMPARE(pOpt->status, PlanStatus::Completed);

        QVERIFY2(m_chat->activeTaskPlanId().isEmpty(),
                 "activeTaskPlanId must clear after declared completion");

        bool foundTaskEvent = false;
        for (const Message& m : currentMessages()) {
            if (m.finishReason == QStringLiteral("task_event") &&
                m.content.contains(QStringLiteral("Task completed"))) {
                foundTaskEvent = true;
                break;
            }
        }
        QVERIFY2(foundTaskEvent,
                 "A role=system task_event row must be posted on declared "
                 "completion");
    }

    void testTaskGate_planStatusChangedListener_clearsStaleTag() {
        m_provider->setScript({textStep(QStringLiteral("working"))});

        m_taskCtrl->userInitiatedStartTask(m_convId, QStringLiteral("some task"));
        waitForGenerationToFinish();

        const QString planId = m_chat->activeTaskPlanId();
        QVERIFY(!planId.isEmpty());

        m_planSvc->updatePlanStatus(planId, PlanStatus::Completed);

        QVERIFY2(m_chat->activeTaskPlanId().isEmpty(),
                 "The planStatusChanged listener must clear "
                 "activeTaskPlanId on terminal transition. Regression "
                 "guard for the terminal-transition reset bug.");
    }

    void testTaskGate_switchConversationClearsAndResumes() {
        m_provider->setScript({textStep(QStringLiteral("working"))});

        m_taskCtrl->userInitiatedStartTask(m_convId, QStringLiteral("task in conv 1"));
        waitForGenerationToFinish();

        const QString planId = m_chat->activeTaskPlanId();
        QVERIFY(!planId.isEmpty());

        const QString conv2 = m_convSvc->createConversation(QStringLiteral("Other"));
        QVERIFY(!conv2.isEmpty());

        m_chat->switchConversation(conv2);
        QVERIFY2(m_chat->activeTaskPlanId().isEmpty(),
                 "Switching away must clear activeTaskPlanId");

        m_chat->switchConversation(m_convId);
        QCOMPARE(m_chat->activeTaskPlanId(), planId);
    }
};

QTEST_MAIN(TestTaskGateService)
#include "test-task-gate-service.moc"

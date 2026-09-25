// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/agent-plan.h"
#include "models/db-manager.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/file-service.h"
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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QUuid>

class TestTaskToolHandlers : public QObject {
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
    std::unique_ptr<ChatController> m_chat;
    std::unique_ptr<TaskGateService> m_taskGateSvc;
    std::unique_ptr<TaskController> m_taskCtrl;
    QString m_convId;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    static QJsonObject makeStartTaskArgs(const QString& goal, const QString& stepTitle) {
        QJsonObject step{
            {QStringLiteral("title"), stepTitle},
            {QStringLiteral("description"), QStringLiteral("step description")},
            {QStringLiteral("acceptance_criteria"), QStringLiteral("done when done")},
        };
        QJsonArray steps;
        steps.append(step);
        QJsonObject args{
            {QStringLiteral("goal"), goal},
            {QStringLiteral("steps"), steps},
        };
        return args;
    }

    QString createPlanAndReturnId(const QString& goal, const QString& stepTitle) {
        const QJsonValue resultValue =
            m_toolSvc->invokeTool(QStringLiteral("start_task"), makeStartTaskArgs(goal, stepTitle));
        if (!resultValue.isObject())
            return {};
        const QJsonObject result = resultValue.toObject();
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

  private slots:
    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/ttool_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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
        m_fileSvc = std::make_unique<FileService>();

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_chat->setToolService(m_toolSvc.get());
        m_chat->setPlanService(m_planSvc.get());
        m_chat->setTaskRunner(m_taskRunner.get());
        m_chat->setTaskObserver(m_taskObserver.get());
        m_chat->setFileService(m_fileSvc.get());

        m_taskGateSvc = std::make_unique<TaskGateService>(*m_msgSvc);
        m_taskGateSvc->setPlanService(m_planSvc.get());
        m_taskGateSvc->setTaskRunner(m_taskRunner.get());
        m_taskGateSvc->setTaskObserver(m_taskObserver.get());
        m_chat->setTaskGateService(m_taskGateSvc.get());

        m_taskCtrl =
            std::make_unique<TaskController>(*m_convSvc, *m_msgSvc, *m_planSvc, *m_taskRunner);
        m_taskCtrl->setFileService(m_fileSvc.get());

        QObject::connect(m_taskCtrl.get(),
                         &TaskController::taskArtifactReady,
                         m_chat.get(),
                         &ChatController::onTaskArtifactReady,
                         Qt::QueuedConnection);

        m_convId = m_convSvc->createConversation(QStringLiteral("TaskToolTest"));
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

    void test_startTask_happyPath_createsPlanAndAnchorsConversation() {
        const QJsonValue resultValue = m_toolSvc->invokeTool(
            QStringLiteral("start_task"),
            makeStartTaskArgs(QStringLiteral("write a haiku"), QStringLiteral("Draft haiku")));

        QVERIFY(resultValue.isObject());
        const QJsonObject result = resultValue.toObject();
        QVERIFY2(!result.contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("start_task returned error: %1")
                                .arg(result.value(QStringLiteral("error")).toString())));

        const QString planId = result.value(QStringLiteral("plan_id")).toString();
        QVERIFY(!planId.isEmpty());
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("created"));
        QCOMPARE(result.value(QStringLiteral("steps")).toInt(), 1);

        const auto pOpt = m_planSvc->getPlan(planId);
        QVERIFY(pOpt.has_value());
        QVERIFY(pOpt->status != PlanStatus::Completed);
        QVERIFY(pOpt->status != PlanStatus::Failed);

        QCOMPARE(m_chat->activeTaskPlanId(), planId);
    }

    void test_startTask_idempotencyGuard_returnsExistingPlanId() {
        const QString firstPlanId =
            createPlanAndReturnId(QStringLiteral("first task"), QStringLiteral("Step one"));
        QVERIFY(!firstPlanId.isEmpty());

        const QJsonValue secondValue = m_toolSvc->invokeTool(
            QStringLiteral("start_task"),
            makeStartTaskArgs(QStringLiteral("second task"), QStringLiteral("Would-be step")));
        QVERIFY(secondValue.isObject());
        const QJsonObject secondResult = secondValue.toObject();

        QCOMPARE(secondResult.value(QStringLiteral("status")).toString(),
                 QStringLiteral("already_active"));
        QCOMPARE(secondResult.value(QStringLiteral("plan_id")).toString(), firstPlanId);

        const QList<AgentPlan> plans = m_planSvc->plansForConversation(m_convId);
        QCOMPARE(plans.size(), 1);
        QCOMPARE(m_chat->activeTaskPlanId(), firstPlanId);
    }

    void test_completeTask_emptyAnchor_resolvesConversationOpenPlan() {
        const QString planId = createPlanAndReturnId(QStringLiteral("finish the launch assets"),
                                                     QStringLiteral("Compile assets"));
        QVERIFY(!planId.isEmpty());

        m_taskGateSvc->clearActivePlan();
        QVERIFY(m_chat->activeTaskPlanId().isEmpty());

        const QJsonValue resultValue = m_toolSvc->invokeTool(
            QStringLiteral("complete_task"),
            QJsonObject{{QStringLiteral("reason"), QStringLiteral("all assets delivered")}});
        QVERIFY(resultValue.isObject());
        const QJsonObject result = resultValue.toObject();
        QVERIFY2(!result.contains(QStringLiteral("error")),
                 qPrintable(result.value(QStringLiteral("error")).toString()));
        QCOMPARE(result.value(QStringLiteral("plan_id")).toString(), planId);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));

        const auto pOpt = m_planSvc->getPlan(planId);
        QVERIFY(pOpt.has_value());
        QCOMPARE(pOpt->status, PlanStatus::Completed);

        const QList<PlanStep> steps = m_planSvc->stepsForPlan(planId);
        QVERIFY(!steps.isEmpty());
        for (const PlanStep& st : steps) {
            QCOMPARE(st.status, StepStatus::Done);
        }
    }

    void test_completeTask_noPlanAnywhere_returnsError() {
        const QJsonValue resultValue = m_toolSvc->invokeTool(
            QStringLiteral("complete_task"),
            QJsonObject{{QStringLiteral("reason"), QStringLiteral("nothing to close")}});
        QVERIFY(resultValue.isObject());
        QVERIFY(resultValue.toObject().contains(QStringLiteral("error")));
    }

    void test_getPlanStatus_byPlanId_returnsPlanObjectWithSteps() {
        const QString goal = QStringLiteral("write a haiku and poem");
        const QString planId = createPlanAndReturnId(goal, QStringLiteral("Compose verses"));
        QVERIFY(!planId.isEmpty());
        const QString stepId = firstStepIdOf(planId);
        QVERIFY(!stepId.isEmpty());

        QJsonObject args{{QStringLiteral("plan_id"), planId}};
        const QJsonValue resultValue =
            m_toolSvc->invokeTool(QStringLiteral("get_task_status"), args);
        QVERIFY(resultValue.isObject());
        const QJsonObject result = resultValue.toObject();

        const QJsonArray plans = result.value(QStringLiteral("plans")).toArray();
        QCOMPARE(plans.size(), 1);

        const QJsonObject planObj = plans.first().toObject();
        QCOMPARE(planObj.value(QStringLiteral("id")).toString(), planId);
        QCOMPARE(planObj.value(QStringLiteral("goal")).toString(), goal);
        QVERIFY(planObj.value(QStringLiteral("status")).isString());

        const QJsonArray stepArr = planObj.value(QStringLiteral("steps")).toArray();
        QVERIFY(!stepArr.isEmpty());
        QCOMPARE(stepArr.first().toObject().value(QStringLiteral("id")).toString(), stepId);
    }

    void test_getPlanStatus_byCallerInScope_withEmptyCaller_returnsEmptyList() {
        const QString planId =
            createPlanAndReturnId(QStringLiteral("scope probe"), QStringLiteral("Anchor step"));
        QVERIFY(!planId.isEmpty());

        const QJsonValue resultValue =
            m_toolSvc->invokeTool(QStringLiteral("get_task_status"), QJsonObject{});
        QVERIFY(resultValue.isObject());
        const QJsonObject result = resultValue.toObject();

        QVERIFY(result.contains(QStringLiteral("plans")));
        const QJsonArray plans = result.value(QStringLiteral("plans")).toArray();
        QCOMPARE(plans.size(), 0);
    }

    void test_stopTask_marksPlanFailedAndClearsAnchor() {
        const QString planId =
            createPlanAndReturnId(QStringLiteral("task to stop"), QStringLiteral("First step"));
        QVERIFY(!planId.isEmpty());

        QJsonObject args{
            {QStringLiteral("plan_id"), planId},
            {QStringLiteral("reason"), QStringLiteral("Test-initiated stop")},
        };
        const QJsonValue resultValue = m_toolSvc->invokeTool(QStringLiteral("stop_task"), args);
        QVERIFY(resultValue.isObject());
        const QJsonObject result = resultValue.toObject();

        QCOMPARE(result.value(QStringLiteral("plan_id")).toString(), planId);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("stopped"));

        const auto pOpt = m_planSvc->getPlan(planId);
        QVERIFY(pOpt.has_value());
        QCOMPARE(pOpt->status, PlanStatus::Failed);

        QVERIFY2(m_chat->activeTaskPlanId().isEmpty(),
                 "activeTaskPlanId must clear after stop_task on the "
                 "anchored plan");
    }
};

QTEST_MAIN(TestTaskToolHandlers)
#include "test-task-tool-handlers.moc"

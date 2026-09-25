// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "models/plans-model.h"
#include "services/conversation-service.h"
#include "services/plan-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestPlansModel : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<PlanService> m_planSvc;
    std::unique_ptr<PlansModel> m_model;
    QString m_convId;

    QString createPlanWithTwoSteps(const QString& convId, const QString& goal) {
        AgentPlan plan;
        plan.conversationId = convId;
        plan.goal = goal;
        plan.startedBy = QStringLiteral("user");
        plan.status = PlanStatus::Executing;
        QList<PlanStep> steps;
        for (int i = 0; i < 2; ++i) {
            PlanStep s;
            s.title = QStringLiteral("Step %1").arg(i);
            s.description = QStringLiteral("desc");
            s.ownerAlias = QStringLiteral("alice");
            s.acceptanceCriteria = QStringLiteral("do it");
            steps.append(s);
        }
        return m_planSvc->createPlan(plan, steps);
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_planSvc = std::make_unique<PlanService>(DbManager::instance());
        m_model = std::make_unique<PlansModel>(*m_planSvc);

        m_convId = m_convSvc->createConversation(QStringLiteral("Chat"));
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_model.reset();
        m_planSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_initialState_isEmpty() {
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(m_model->count(), 0);
    }

    void test_setActiveConversation_loadsExistingPlans() {
        createPlanWithTwoSteps(m_convId, QStringLiteral("goal-1"));
        createPlanWithTwoSteps(m_convId, QStringLiteral("goal-2"));

        QSignalSpy resetSpy(m_model.get(), &QAbstractItemModel::modelReset);
        m_model->setActiveConversation(m_convId);

        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(m_model->rowCount(), 2);
    }

    void test_planCreated_insertsRowWhenActive() {
        m_model->setActiveConversation(m_convId);
        QSignalSpy insertedSpy(m_model.get(), &QAbstractItemModel::rowsInserted);

        const QString id = createPlanWithTwoSteps(m_convId, QStringLiteral("g"));
        QVERIFY(!id.isEmpty());

        QCOMPARE(insertedSpy.count(), 1);
        QCOMPARE(m_model->rowCount(), 1);
        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, PlansModel::GoalRole).toString(), QStringLiteral("g"));
        QCOMPARE(m_model->data(idx, PlansModel::TotalStepsRole).toInt(), 2);
        QCOMPARE(m_model->data(idx, PlansModel::DoneStepsRole).toInt(), 0);
    }

    void test_planCreated_otherConversation_isIgnored() {
        m_model->setActiveConversation(m_convId);
        const QString otherConv = m_convSvc->createConversation(QStringLiteral("Other"));

        createPlanWithTwoSteps(otherConv, QStringLiteral("nope"));

        QCOMPARE(m_model->rowCount(), 0);
    }

    void test_planStatusChanged_emitsDataChanged() {
        m_model->setActiveConversation(m_convId);
        const QString id = createPlanWithTwoSteps(m_convId, QStringLiteral("g"));

        QSignalSpy dataSpy(m_model.get(), &QAbstractItemModel::dataChanged);
        QVERIFY(m_planSvc->updatePlanStatus(id, PlanStatus::Completed));

        QVERIFY(dataSpy.count() >= 1);
        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, PlansModel::StatusRole).toString(),
                 QStringLiteral("completed"));
    }

    void test_stepUpdated_refreshesStepRole() {
        m_model->setActiveConversation(m_convId);
        const QString id = createPlanWithTwoSteps(m_convId, QStringLiteral("g"));
        const QList<PlanStep> steps = m_planSvc->stepsForPlan(id);
        QCOMPARE(steps.size(), 2);
        const QString firstStepId = steps.first().id;

        QSignalSpy dataSpy(m_model.get(), &QAbstractItemModel::dataChanged);

        QVERIFY(m_planSvc->updateStepStatus(firstStepId, StepStatus::Done));

        QVERIFY(dataSpy.count() >= 1);

        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, PlansModel::DoneStepsRole).toInt(), 1);
    }

    void test_planDeleted_removesRow() {
        m_model->setActiveConversation(m_convId);
        const QString id = createPlanWithTwoSteps(m_convId, QStringLiteral("g"));
        QCOMPARE(m_model->rowCount(), 1);

        QSignalSpy removedSpy(m_model.get(), &QAbstractItemModel::rowsRemoved);
        QVERIFY(m_planSvc->deletePlan(id));

        QCOMPARE(removedSpy.count(), 1);
        QCOMPARE(m_model->rowCount(), 0);
    }

    void test_busyMembers_populatedOnInProgressStep() {
        m_model->setActiveConversation(m_convId);
        const QString id = createPlanWithTwoSteps(m_convId, QStringLiteral("build"));
        const QList<PlanStep> steps = m_planSvc->stepsForPlan(id);
        QCOMPARE(steps.size(), 2);

        QSignalSpy busySpy(m_model.get(), &PlansModel::busyMembersChanged);

        QVERIFY(m_planSvc->updateStepStatus(steps.first().id, StepStatus::InProgress));

        QVERIFY(busySpy.count() >= 1);
        const QStringList busy = m_model->busyMembers();
        QCOMPARE(busy.size(), 1);
        QVERIFY(busy.first().contains(QStringLiteral("@alice")));
        QVERIFY(busy.first().contains(QStringLiteral("Step 0")));
    }

    void test_hasActiveTask_reflectsPlanLifecycle() {
        m_model->setActiveConversation(m_convId);
        QVERIFY(!m_model->hasActiveTask());

        const QString id = createPlanWithTwoSteps(m_convId, QStringLiteral("g"));
        QVERIFY(m_model->hasActiveTask());

        QVERIFY(m_planSvc->updatePlanStatus(id, PlanStatus::Completed));
        QVERIFY(!m_model->hasActiveTask());
    }

    void test_switchingActiveConversation_reloads() {
        createPlanWithTwoSteps(m_convId, QStringLiteral("a"));
        const QString otherConv = m_convSvc->createConversation(QStringLiteral("Other"));
        createPlanWithTwoSteps(otherConv, QStringLiteral("b"));

        m_model->setActiveConversation(m_convId);
        QCOMPARE(m_model->rowCount(), 1);

        m_model->setActiveConversation(otherConv);
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->data(m_model->index(0, 0), PlansModel::GoalRole).toString(),
                 QStringLiteral("b"));
    }
};

QTEST_MAIN(TestPlansModel)
#include "test-plans-model.moc"

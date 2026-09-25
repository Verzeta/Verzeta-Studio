// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/agent-plan.h"
#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "services/plan-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

class TestPlanService : public QObject {
    Q_OBJECT

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/plansvc_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_svc = std::make_unique<PlanService>(DbManager::instance());

        m_convId = m_convSvc->createConversation(QStringLiteral("Test conv"));
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_svc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void testCreatePlanWithSteps() {
        AgentPlan plan;
        plan.conversationId = m_convId;
        plan.goal = QStringLiteral("Launch Q2 campaign");
        plan.startedBy = QStringLiteral("user");

        QList<PlanStep> steps;
        {
            PlanStep s;
            s.title = QStringLiteral("Draft launch copy");
            s.description = QStringLiteral("playful, CTA-driven");
            s.ownerAlias = QStringLiteral("Creative_Sam");
            s.acceptanceCriteria = QStringLiteral("Copy includes hook + CTA + tone check");
            steps.append(s);
        }
        {
            PlanStep s;
            s.title = QStringLiteral("Build channel plan");
            s.ownerAlias = QStringLiteral("Marketing_Rob");
            s.acceptanceCriteria = QStringLiteral("Channels listed with budget and metric");
            steps.append(s);
        }

        QSignalSpy createdSpy(m_svc.get(), &PlanService::planCreated);

        const QString planId = m_svc->createPlan(plan, steps);
        QVERIFY(!planId.isEmpty());
        QCOMPARE(createdSpy.count(), 1);
        QCOMPARE(createdSpy.first().at(0).toString(), planId);

        const auto p = m_svc->getPlan(planId);
        QVERIFY(p.has_value());
        QCOMPARE(p->conversationId, m_convId);
        QCOMPARE(p->goal, QStringLiteral("Launch Q2 campaign"));
        QCOMPARE(p->status, PlanStatus::Planning);
        QCOMPARE(p->startedBy, QStringLiteral("user"));
        QVERIFY(p->createdAt.isValid());

        const auto loaded = m_svc->stepsForPlan(planId);
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded[0].ordering, 0);
        QCOMPARE(loaded[0].title, QStringLiteral("Draft launch copy"));
        QCOMPARE(loaded[0].ownerAlias, QStringLiteral("Creative_Sam"));
        QCOMPARE(loaded[0].status, StepStatus::Pending);
        QCOMPARE(loaded[1].ordering, 1);
        QCOMPARE(loaded[1].title, QStringLiteral("Build channel plan"));
        QCOMPARE(loaded[1].ownerAlias, QStringLiteral("Marketing_Rob"));
    }

    void testCreatePlanRejectsEmpty() {
        AgentPlan plan;
        plan.conversationId = m_convId;
        plan.goal = QString();
        plan.startedBy = QStringLiteral("user");

        QList<PlanStep> steps;
        {
            PlanStep s;
            s.title = QStringLiteral("x");
            s.ownerAlias = QStringLiteral("y");
            s.acceptanceCriteria = QStringLiteral("z");
            steps.append(s);
        }
        QVERIFY(m_svc->createPlan(plan, steps).isEmpty());

        plan.goal = QStringLiteral("real goal");
        QVERIFY(m_svc->createPlan(plan, {}).isEmpty());
    }

    void testPlanStatusTransition() {
        const QString planId = makeSimplePlan();
        QSignalSpy statusSpy(m_svc.get(), &PlanService::planStatusChanged);

        QVERIFY(m_svc->updatePlanStatus(planId, PlanStatus::Executing));
        QCOMPARE(statusSpy.count(), 1);
        QCOMPARE(m_svc->getPlan(planId)->status, PlanStatus::Executing);

        QVERIFY(m_svc->updatePlanStatus(planId, PlanStatus::Completed));
        QCOMPARE(statusSpy.count(), 2);
        QCOMPARE(m_svc->getPlan(planId)->status, PlanStatus::Completed);
    }


    void testActivePlansForConversationFiltersStatus() {
        const QString p1 = makeSimplePlan();
        const QString p2 = makeSimplePlan();
        QVERIFY(m_svc->updatePlanStatus(p2, PlanStatus::Completed));
        const QString p3 = makeSimplePlan();
        QVERIFY(m_svc->updatePlanStatus(p3, PlanStatus::Executing));

        const auto active = m_svc->activePlansForConversation(m_convId);
        QCOMPARE(active.size(), 2);
        QVERIFY(std::any_of(
            active.begin(), active.end(), [&](const AgentPlan& pl) { return pl.id == p1; }));
        QVERIFY(std::any_of(
            active.begin(), active.end(), [&](const AgentPlan& pl) { return pl.id == p3; }));
    }

    void testActivePlansForOwnerInScope() {
        AgentPlan p1;
        p1.conversationId = m_convId;
        p1.goal = QStringLiteral("goal 1");
        p1.startedBy = QStringLiteral("user");
        p1.projectFolderId = QStringLiteral("proj-A");
        PlanStep s1;
        s1.title = QStringLiteral("t1");
        s1.ownerAlias = QStringLiteral("Alice");
        s1.acceptanceCriteria = QStringLiteral("ac1");
        const QString planA = m_svc->createPlan(p1, {s1});
        QVERIFY(!planA.isEmpty());
        QVERIFY(m_svc->updatePlanStatus(planA, PlanStatus::Executing));

        AgentPlan p2 = p1;
        p2.id.clear();
        p2.goal = QStringLiteral("goal 2");
        p2.projectFolderId = QStringLiteral("proj-B");
        const QString planB = m_svc->createPlan(p2, {s1});
        QVERIFY(!planB.isEmpty());
        QVERIFY(m_svc->updatePlanStatus(planB, PlanStatus::Executing));

        const auto scopedA = m_svc->activePlansForOwnerInScope(
            QStringLiteral("Alice"), QStringLiteral("proj-A"), {});
        QCOMPARE(scopedA.size(), 1);
        QCOMPARE(scopedA.first().id, planA);

        const auto loose = m_svc->activePlansForOwnerInScope(QStringLiteral("Alice"), {}, {});
        QCOMPARE(loose.size(), 2);
    }


    void testAppendStepToPlan() {
        const QString planId = makeSimplePlan();
        const auto before = m_svc->stepsForPlan(planId);
        QCOMPARE(before.size(), 1);

        PlanStep extra;
        extra.title = QStringLiteral("late-added");
        extra.ownerAlias = QStringLiteral("someone");
        extra.acceptanceCriteria = QStringLiteral("done-ness");
        const QString newStepId = m_svc->appendStepToPlan(planId, extra);
        QVERIFY(!newStepId.isEmpty());

        const auto after = m_svc->stepsForPlan(planId);
        QCOMPARE(after.size(), 2);
        QCOMPARE(after[1].ordering, 1);
        QCOMPARE(after[1].title, QStringLiteral("late-added"));
    }

    void testStepStatusAndReason() {
        const QString planId = makeSimplePlan();
        const auto stepId = m_svc->stepsForPlan(planId).first().id;

        QVERIFY(m_svc->updateStepStatus(stepId, StepStatus::InProgress));
        QCOMPARE(m_svc->getStep(stepId)->status, StepStatus::InProgress);

        QVERIFY(m_svc->updateStepStatus(
            stepId, StepStatus::NeedsRework, QStringLiteral("tone too corporate")));
        const auto s = m_svc->getStep(stepId).value();
        QCOMPARE(s.status, StepStatus::NeedsRework);
        QCOMPARE(s.lastRejectionReason, QStringLiteral("tone too corporate"));
    }

    void testStepCountersAndReset() {
        const QString planId = makeSimplePlan();
        const auto stepId = m_svc->stepsForPlan(planId).first().id;

        QVERIFY(m_svc->incrementStepRejectionCount(stepId));
        QVERIFY(m_svc->incrementStepRejectionCount(stepId));
        QVERIFY(m_svc->incrementStepToolRetryCount(stepId));
        QVERIFY(m_svc->incrementStepExecutorTurns(stepId));
        QVERIFY(m_svc->incrementStepExecutorTurns(stepId));
        QVERIFY(m_svc->incrementStepExecutorTurns(stepId));

        auto s = m_svc->getStep(stepId).value();
        QCOMPARE(s.rejectionCount, 2);
        QCOMPARE(s.toolRetryCount, 1);
        QCOMPARE(s.executorTurnsUsed, 3);

        QVERIFY(m_svc->resetStepForRetry(stepId, QStringLiteral("focus on playful tone")));
        s = m_svc->getStep(stepId).value();
        QCOMPARE(s.status, StepStatus::Pending);
        QCOMPARE(s.rejectionCount, 0);
        QCOMPARE(s.toolRetryCount, 0);
        QCOMPARE(s.executorTurnsUsed, 0);
        QCOMPARE(s.lastRejectionReason, QStringLiteral("focus on playful tone"));
    }

    void testActiveStepForOwnerInConversation() {
        AgentPlan plan;
        plan.conversationId = m_convId;
        plan.goal = QStringLiteral("g");
        plan.startedBy = QStringLiteral("user");
        PlanStep s1, s2;
        s1.title = QStringLiteral("s1");
        s1.ownerAlias = QStringLiteral("Alice");
        s1.acceptanceCriteria = QStringLiteral("ac1");
        s2.title = QStringLiteral("s2");
        s2.ownerAlias = QStringLiteral("Bob");
        s2.acceptanceCriteria = QStringLiteral("ac2");

        const QString planId = m_svc->createPlan(plan, {s1, s2});
        QVERIFY(!planId.isEmpty());
        QVERIFY(m_svc->updatePlanStatus(planId, PlanStatus::Executing));

        QVERIFY(
            m_svc->activeStepForOwnerInConversation(m_convId, QStringLiteral("Alice")).has_value());
        QVERIFY(
            m_svc->activeStepForOwnerInConversation(m_convId, QStringLiteral("Bob")).has_value());
        QVERIFY(!m_svc->activeStepForOwnerInConversation(m_convId, QStringLiteral("Carol"))
                     .has_value());

        const auto aliceStep =
            m_svc->activeStepForOwnerInConversation(m_convId, QStringLiteral("Alice")).value();
        QVERIFY(m_svc->updateStepStatus(aliceStep.id, StepStatus::Done));
        QVERIFY(!m_svc->activeStepForOwnerInConversation(m_convId, QStringLiteral("Alice"))
                     .has_value());
    }


    void testAddArtifactAndApprove() {
        const QString planId = makeSimplePlan();
        const auto stepId = m_svc->stepsForPlan(planId).first().id;

        QSignalSpy addSpy(m_svc.get(), &PlanService::artifactAdded);
        QSignalSpy approvalSpy(m_svc.get(), &PlanService::artifactApprovalChanged);

        StepArtifact a;
        a.stepId = stepId;
        a.type = ArtifactType::Draft;
        a.content = QStringLiteral("# Draft\nHello world");
        a.summary = QStringLiteral("Launch copy v1");
        a.submittedByAlias = QStringLiteral("Creative_Sam");
        const QString artId = m_svc->addArtifact(a);
        QVERIFY(!artId.isEmpty());
        QCOMPARE(addSpy.count(), 1);

        const auto list = m_svc->artifactsForStep(stepId);
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().summary, QStringLiteral("Launch copy v1"));
        QCOMPARE(list.first().approved, 0);

        QVERIFY(m_svc->setArtifactApproval(artId, 1));
        QCOMPARE(approvalSpy.count(), 1);
        QCOMPARE(m_svc->artifactsForStep(stepId).first().approved, 1);

        QVERIFY(m_svc->setArtifactApproval(
            artId, -1, QStringLiteral("too corporate|missing CTA").split('|')));
        const auto rejected = m_svc->artifactsForStep(stepId).first();
        QCOMPARE(rejected.approved, -1);
        QCOMPARE(rejected.criticReasons.size(), 2);
        QCOMPARE(rejected.criticReasons.first(), QStringLiteral("too corporate"));
    }


    void testDeletePlanCascadesToStepsAndArtifacts() {
        const QString planId = makeSimplePlan();
        const auto stepId = m_svc->stepsForPlan(planId).first().id;

        StepArtifact a;
        a.stepId = stepId;
        a.type = ArtifactType::Draft;
        a.summary = QStringLiteral("tmp");
        a.submittedByAlias = QStringLiteral("x");
        const QString artId = m_svc->addArtifact(a);
        QVERIFY(!artId.isEmpty());

        QSignalSpy delSpy(m_svc.get(), &PlanService::planDeleted);
        QVERIFY(m_svc->deletePlan(planId));
        QCOMPARE(delSpy.count(), 1);

        QVERIFY(!m_svc->getPlan(planId).has_value());
        QVERIFY(m_svc->stepsForPlan(planId).isEmpty());
        QVERIFY(m_svc->artifactsForStep(stepId).isEmpty());
    }

    void testConversationDeletionCascadesToPlans() {
        const QString planId = makeSimplePlan();
        QVERIFY(m_svc->getPlan(planId).has_value());

        QVERIFY(m_convSvc->deleteConversation(m_convId));

        QVERIFY(!m_svc->getPlan(planId).has_value());
        QVERIFY(m_svc->plansForConversation(m_convId).isEmpty());
    }

    void testBug5_groupPlanDoesNotLeakToNonOwnerProjectMembers() {
        QSqlDatabase db = DbManager::instance().db();

        const QString projectFolderId = QStringLiteral("proj-bug5-folder");
        const QString groupConvId = QStringLiteral("conv-bug5-group");
        const QString aliceId = QStringLiteral("agent-alice");
        const QString bobId = QStringLiteral("agent-bob");
        const QString carolId = QStringLiteral("agent-carol");
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT INTO folders(id, name, parent_id, folder_type, "
                                 "created_at) VALUES (?, 'Bug5 Project', NULL, 'project', ?)"));
        q.addBindValue(projectFolderId);
        q.addBindValue(now);
        QVERIFY2(q.exec(), qPrintable(q.lastError().text()));

        for (const QString& aid : {aliceId, bobId, carolId}) {
            QSqlQuery ag(db);
            ag.prepare(QStringLiteral("INSERT INTO agents(id, name, system_prompt, "
                                      "created_at) VALUES (?, ?, '', ?)"));
            ag.addBindValue(aid);
            ag.addBindValue(aid);
            ag.addBindValue(now);
            QVERIFY2(ag.exec(), qPrintable(ag.lastError().text()));
        }

        for (const QString& aid : {aliceId, bobId, carolId}) {
            QSqlQuery pm(db);
            pm.prepare(QStringLiteral("INSERT INTO project_members(folder_id, agent_id, alias, "
                                      "is_coordinator, joined_at) VALUES (?, ?, ?, 0, ?)"));
            pm.addBindValue(projectFolderId);
            pm.addBindValue(aid);
            pm.addBindValue(aid == aliceId ? QStringLiteral("Alice")
                            : aid == bobId ? QStringLiteral("Bob")
                                           : QStringLiteral("Carol"));
            pm.addBindValue(now);
            QVERIFY2(pm.exec(), qPrintable(pm.lastError().text()));
        }

        QSqlQuery c(db);
        c.prepare(QStringLiteral("INSERT INTO conversations(id, title, folder_id, created_at, "
                                 "updated_at, is_group, group_agent_ids) VALUES (?, 'Team', ?, "
                                 "?, ?, 1, '[]')"));
        c.addBindValue(groupConvId);
        c.addBindValue(projectFolderId);
        c.addBindValue(now);
        c.addBindValue(now);
        QVERIFY2(c.exec(), qPrintable(c.lastError().text()));

        for (const QString& aid : {aliceId, bobId, carolId}) {
            QSqlQuery cm(db);
            cm.prepare(QStringLiteral("INSERT INTO conversation_members(conversation_id, agent_id, "
                                      "alias, is_coordinator, joined_at) VALUES (?, ?, ?, 0, ?)"));
            cm.addBindValue(groupConvId);
            cm.addBindValue(aid);
            cm.addBindValue(aid == aliceId ? QStringLiteral("Alice")
                            : aid == bobId ? QStringLiteral("Bob")
                                           : QStringLiteral("Carol"));
            cm.addBindValue(now);
            QVERIFY2(cm.exec(), qPrintable(cm.lastError().text()));
        }

        AgentPlan plan;
        plan.conversationId = groupConvId;
        plan.projectFolderId = projectFolderId;
        plan.goal = QStringLiteral("Alice writes team colors file");
        plan.startedBy = QStringLiteral("Alice");
        PlanStep step;
        step.title = QStringLiteral("write file");
        step.ownerAlias = QStringLiteral("Alice");
        step.acceptanceCriteria = QStringLiteral("file exists");
        const QString planId = m_svc->createPlan(plan, {step});
        QVERIFY(!planId.isEmpty());
        QVERIFY(m_svc->updatePlanStatus(planId, PlanStatus::Completed));

        const auto aliceCompleted =
            m_svc->plansByAgentIdAcrossChats(aliceId, {QStringLiteral("completed")}, 7);
        QCOMPARE(aliceCompleted.size(), 1);
        QCOMPARE(aliceCompleted[0].id, planId);

        const auto bobCompleted =
            m_svc->plansByAgentIdAcrossChats(bobId, {QStringLiteral("completed")}, 7);
        QCOMPARE(bobCompleted.size(), 0);

        const auto carolCompleted =
            m_svc->plansByAgentIdAcrossChats(carolId, {QStringLiteral("completed")}, 7);
        QCOMPARE(carolCompleted.size(), 0);
    }

    void testBug4_plansExcludingConversation_removesMatching() {
        AgentPlan p1;
        p1.id = QStringLiteral("p1");
        p1.conversationId = QStringLiteral("conv-A");
        p1.goal = QStringLiteral("work in A");
        AgentPlan p2;
        p2.id = QStringLiteral("p2");
        p2.conversationId = QStringLiteral("conv-B");
        p2.goal = QStringLiteral("work in B");
        AgentPlan p3;
        p3.id = QStringLiteral("p3");
        p3.conversationId = QStringLiteral("conv-A");
        p3.goal = QStringLiteral("more work in A");

        const QList<AgentPlan> source{p1, p2, p3};
        const QList<AgentPlan> filtered =
            plansExcludingConversation(source, QStringLiteral("conv-A"));

        QCOMPARE(filtered.size(), 1);
        QCOMPARE(filtered[0].id, QStringLiteral("p2"));
    }

    void testBug4_plansExcludingConversation_emptyConvIdIsNoOp() {
        AgentPlan p1;
        p1.id = QStringLiteral("p1");
        p1.conversationId = QStringLiteral("conv-A");
        AgentPlan p2;
        p2.id = QStringLiteral("p2");
        p2.conversationId = QStringLiteral("conv-B");

        const QList<AgentPlan> filtered = plansExcludingConversation({p1, p2}, QString());
        QCOMPARE(filtered.size(), 2);
    }

    void testBug4_plansExcludingConversation_allMatchReturnsEmpty() {
        AgentPlan p1;
        p1.id = QStringLiteral("p1");
        p1.conversationId = QStringLiteral("conv-A");
        AgentPlan p2;
        p2.id = QStringLiteral("p2");
        p2.conversationId = QStringLiteral("conv-A");

        const QList<AgentPlan> filtered =
            plansExcludingConversation({p1, p2}, QStringLiteral("conv-A"));
        QVERIFY(filtered.isEmpty());
    }

    void testBug5_oneToOnePlansStillVisibleViaRoute3() {
        QSqlDatabase db = DbManager::instance().db();
        const QString projectFolderId = QStringLiteral("proj-bug5-121");
        const QString oneOnOneConvId = QStringLiteral("conv-bug5-121");
        const QString aliceId = QStringLiteral("agent-121-alice");
        const QString bobId = QStringLiteral("agent-121-bob");
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT INTO folders(id, name, parent_id, folder_type, "
                                 "created_at) VALUES (?, 'Bug5 121', NULL, 'project', ?)"));
        q.addBindValue(projectFolderId);
        q.addBindValue(now);
        QVERIFY2(q.exec(), qPrintable(q.lastError().text()));

        for (const QString& aid : {aliceId, bobId}) {
            QSqlQuery ag(db);
            ag.prepare(QStringLiteral("INSERT INTO agents(id, name, system_prompt, "
                                      "created_at) VALUES (?, ?, '', ?)"));
            ag.addBindValue(aid);
            ag.addBindValue(aid);
            ag.addBindValue(now);
            QVERIFY2(ag.exec(), qPrintable(ag.lastError().text()));
        }

        for (const QString& aid : {aliceId, bobId}) {
            QSqlQuery pm(db);
            pm.prepare(QStringLiteral("INSERT INTO project_members(folder_id, agent_id, alias, "
                                      "is_coordinator, joined_at) VALUES (?, ?, ?, 0, ?)"));
            pm.addBindValue(projectFolderId);
            pm.addBindValue(aid);
            pm.addBindValue(aid == aliceId ? QStringLiteral("Alice") : QStringLiteral("Bob"));
            pm.addBindValue(now);
            QVERIFY2(pm.exec(), qPrintable(pm.lastError().text()));
        }

        QSqlQuery c(db);
        c.prepare(QStringLiteral("INSERT INTO conversations(id, title, folder_id, created_at, "
                                 "updated_at, is_group, primary_agent_id) VALUES (?, 'Alice 1:1', "
                                 "?, ?, ?, 0, ?)"));
        c.addBindValue(oneOnOneConvId);
        c.addBindValue(projectFolderId);
        c.addBindValue(now);
        c.addBindValue(now);
        c.addBindValue(aliceId);
        QVERIFY2(c.exec(), qPrintable(c.lastError().text()));

        AgentPlan plan;
        plan.conversationId = oneOnOneConvId;
        plan.projectFolderId = projectFolderId;
        plan.goal = QStringLiteral("Alice 1:1 work");
        plan.startedBy = QStringLiteral("Alice");
        PlanStep step;
        step.title = QStringLiteral("step");
        step.ownerAlias = QStringLiteral("Alice");
        step.acceptanceCriteria = QStringLiteral("done");
        const QString planId = m_svc->createPlan(plan, {step});
        QVERIFY(!planId.isEmpty());
        QVERIFY(m_svc->updatePlanStatus(planId, PlanStatus::Completed));

        const auto alicePlans =
            m_svc->plansByAgentIdAcrossChats(aliceId, {QStringLiteral("completed")}, 7);
        QCOMPARE(alicePlans.size(), 1);

        const auto bobPlans =
            m_svc->plansByAgentIdAcrossChats(bobId, {QStringLiteral("completed")}, 7);
        QCOMPARE(bobPlans.size(), 1);
    }

  private:
    QString makeSimplePlan() {
        AgentPlan plan;
        plan.conversationId = m_convId;
        plan.goal =
            QStringLiteral("demo goal ") + QString::number(QDateTime::currentMSecsSinceEpoch());
        plan.startedBy = QStringLiteral("user");
        PlanStep step;
        step.title = QStringLiteral("first step");
        step.ownerAlias = QStringLiteral("Alice");
        step.acceptanceCriteria = QStringLiteral("done");
        return m_svc->createPlan(plan, {step});
    }

    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<PlanService> m_svc;
};

QTEST_MAIN(TestPlanService)
#include "test-plan-service.moc"

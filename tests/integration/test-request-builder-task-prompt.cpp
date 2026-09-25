// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/agent-plan.h"
#include "models/agent.h"
#include "models/db-manager.h"
#include "services/agent-registry.h"
#include "services/chat/request-builder.h"
#include "services/conversation-service.h"
#include "services/membership-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/plan-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSqlDatabase>
#include <QStandardPaths>

class TestRequestBuilderTaskPrompt : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<PlanService> m_planSvc;
    std::unique_ptr<Chat::RequestBuilder> m_builder;

    QString m_convId;
    QString m_planId;

  private slots:

    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        QStandardPaths::setTestModeEnabled(true);
    }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_router = std::make_unique<ModelRouter>();
        m_planSvc = std::make_unique<PlanService>(DbManager::instance());
        m_builder = std::make_unique<Chat::RequestBuilder>(*m_convSvc, *m_msgSvc, *m_router);

        m_convId = m_convSvc->createConversation(QStringLiteral("Task prompt test"));
        QVERIFY(!m_convId.isEmpty());

        LlmConfig cfg;
        cfg.providerId = QStringLiteral("test");
        cfg.modelName = QStringLiteral("test-model");
        cfg.stream = true;
        cfg.contextWindow = 8192;
        m_convSvc->updateLlmConfig(m_convId, cfg);

        AgentPlan plan;
        plan.conversationId = m_convId;
        plan.goal = QStringLiteral("Build a tiny encryptor CLI and save it");
        plan.startedBy = QStringLiteral("user");
        QList<PlanStep> steps;
        {
            PlanStep s;
            s.title = QStringLiteral("Build it");
            s.ownerAlias = QStringLiteral("assistant");
            s.acceptanceCriteria = QStringLiteral("a runnable file exists");
            steps.append(s);
        }
        m_planId = m_planSvc->createPlan(plan, steps);
        QVERIFY(!m_planId.isEmpty());
    }

    void cleanup() {
        m_builder.reset();
        m_planSvc.reset();
        m_router.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    Chat::BuildRequestInputs anchoredInputs() {
        Chat::BuildRequestInputs in;
        in.activeConvId = m_convId;
        in.inflightConvId = m_convId;
        in.requestId = 1;
        in.lastUserText = QStringLiteral("build it");
        in.activeTaskPlanId = m_planId;
        in.planService = m_planSvc.get();
        return in;
    }

    void test_activeTask_framing_steersToWorkAndComplete() {
        const Chat::BuildResult r = m_builder->buildRequest(anchoredInputs());
        QVERIFY2(r.success, qPrintable(r.errorReason));
        const QString sys = r.request.systemPrompt;

        QVERIFY(sys.contains(QStringLiteral("=== ACTIVE TASK ===")));
        QVERIFY(sys.contains(QStringLiteral("complete_task")));
        QVERIFY(sys.contains(QStringLiteral("write_file")));
        QVERIFY(sys.contains(QStringLiteral("encryptor CLI")));
    }

    void test_activeTask_framing_hasNoOwnerHandoffOrMarker() {
        const Chat::BuildResult r = m_builder->buildRequest(anchoredInputs());
        QVERIFY2(r.success, qPrintable(r.errorReason));
        const QString sys = r.request.systemPrompt;

        QVERIFY(!sys.contains(QStringLiteral("owned by")));
        QVERIFY(!sys.contains(QStringLiteral("NOT you")));
        QVERIFY(!sys.contains(QStringLiteral("do NOT use write_file")));
        QVERIFY(!sys.contains(QStringLiteral("@assistant")));
        QVERIFY(!sys.contains(QStringLiteral("<task_state>")));
        QVERIFY(!sys.contains(QStringLiteral("MANDATORY STATUS MARKER")));
    }


    QString groupTaskSystemPrompt() {
        AgentRegistry agentReg(DbManager::instance());
        agentReg.initialize();
        MembershipService memberSvc(DbManager::instance());
        const QList<Agent> agents = agentReg.allAgents();
        if (agents.size() < 2)
            return {};

        const QString gid = m_convSvc->createGroupConversation(QStringLiteral("Group task"),
                                                               {agents[0].id, agents[1].id});
        if (gid.isEmpty())
            return {};
        if (!memberSvc.addConversationMember(gid, agents[0].id, QStringLiteral("Alice"), true))
            return {};
        if (!memberSvc.addConversationMember(gid, agents[1].id, QStringLiteral("Bob"), false))
            return {};

        LlmConfig cfg;
        cfg.providerId = QStringLiteral("test");
        cfg.modelName = QStringLiteral("test-model");
        cfg.stream = true;
        cfg.contextWindow = 8192;
        m_convSvc->updateLlmConfig(gid, cfg);

        AgentPlan plan;
        plan.conversationId = gid;
        plan.goal = QStringLiteral("Ship the encryptor with docs");
        plan.startedBy = QStringLiteral("Alice");
        QList<PlanStep> steps;
        {
            PlanStep s;
            s.title = QStringLiteral("Ship it");
            s.ownerAlias = QStringLiteral("Alice");
            s.acceptanceCriteria = QStringLiteral("deliverables exist");
            steps.append(s);
        }
        const QString planId = m_planSvc->createPlan(plan, steps);
        if (planId.isEmpty())
            return {};

        Chat::BuildRequestInputs in;
        in.activeConvId = gid;
        in.inflightConvId = gid;
        in.requestId = 1;
        in.lastUserText = QStringLiteral("get it shipped");
        in.activeTaskPlanId = planId;
        in.planService = m_planSvc.get();
        in.agentRegistry = &agentReg;
        in.membershipService = &memberSvc;
        in.responseMemberAlias = QStringLiteral("Alice");
        in.responseMemberAgentId = agents[0].id;
        in.maxAgentCascade = 10;

        const Chat::BuildResult r = m_builder->buildRequest(in);
        if (!r.success)
            return {};
        return r.request.systemPrompt;
    }

    void test_groupContract_actionClause_andTruthfulBudget() {
        const QString sys = groupTaskSystemPrompt();
        QVERIFY(!sys.isEmpty());
        QVERIFY(sys.contains(QStringLiteral("IN THIS SAME TURN")));
        QVERIFY(sys.contains(QStringLiteral("Announcing work without doing it")));
        QVERIFY(!sys.contains(QStringLiteral("AT MOST ONCE")));
    }

    void test_groupContract_pollParticipationRule() {
        const QString sys = groupTaskSystemPrompt();
        QVERIFY(!sys.isEmpty());
        QVERIFY(sys.contains(QStringLiteral("CAST YOUR VOTE")));
        QVERIFY(sys.contains(QStringLiteral("cast_vote")));
        QVERIFY(!sys.contains(QStringLiteral("agreement is not a deliverable")));
        QVERIFY(!sys.contains(QStringLiteral("Never wait for consensus")));
    }

    void test_groupActiveTask_assignmentByMention_groupOnly() {
        const QString groupSys = groupTaskSystemPrompt();
        QVERIFY(!groupSys.isEmpty());
        QVERIFY(groupSys.contains(QStringLiteral("@-mention exactly ONE teammate")));
        QVERIFY(groupSys.contains(QStringLiteral("Never leave the next step unassigned")));

        const Chat::BuildResult r = m_builder->buildRequest(anchoredInputs());
        QVERIFY2(r.success, qPrintable(r.errorReason));
        QVERIFY(!r.request.systemPrompt.contains(QStringLiteral("@-mention exactly ONE teammate")));
    }
};

QTEST_MAIN(TestRequestBuilderTaskPrompt)
#include "test-request-builder-task-prompt.moc"

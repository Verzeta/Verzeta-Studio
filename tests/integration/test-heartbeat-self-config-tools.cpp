// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/heartbeat-config.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/chat/request-builder.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/heartbeat-config-service.h"
#include "../../backend/services/heartbeat-subagent-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/skill-service.h"
#include "../../backend/services/tool-service.h"
#include "../../backend/tools/heartbeat/heartbeat-self-config-tools.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QJsonValue>
#include <QPair>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

class TestHeartbeatSelfConfigTools : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        m_dbPath = dbPath;
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_router = std::make_unique<ModelRouter>();
        m_toolSvc = std::make_unique<ToolService>();
        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_skillSvc = std::make_unique<SkillService>(*m_convs);
        m_skillSvc->setAppDataRootForTesting(m_tempDir.path());
        m_skillSvc->initialize();
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_reqBuilder = std::make_unique<Chat::RequestBuilder>(*m_convs, *m_msgs, *m_router);
        m_configSvc = std::make_unique<HeartbeatConfigService>(DbManager::instance());
        m_cascade = std::make_unique<Chat::CascadeController>(*m_convs, *m_router);
        m_subagentSvc = std::make_unique<HeartbeatSubagentService>(*m_configSvc,
                                                                   *m_agents,
                                                                   *m_convs,
                                                                   *m_msgs,
                                                                   *m_router,
                                                                   *m_toolSvc,
                                                                   *m_skillSvc,
                                                                   *m_reqBuilder,
                                                                   nullptr,
                                                                   nullptr);

        Agent a;
        a.id = QStringLiteral("agent-sct");
        a.name = QStringLiteral("SelfConfigTestAgent");
        a.systemPrompt = QStringLiteral("you are tester");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
        m_convId = m_convs->createConversation(QStringLiteral("SCT Conv"));
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_subagentSvc.reset();
        m_cascade.reset();
        m_configSvc.reset();
        m_reqBuilder.reset();
        m_msgs.reset();
        m_convs.reset();
        m_agents.reset();
        m_skillSvc.reset();
        m_toolSvc.reset();
        m_router.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_agent_defaultHeartbeatFields_roundTrip() {
        Agent a;
        a.id = QStringLiteral("agent-with-hints");
        a.name = QStringLiteral("AgentWithHints");
        a.systemPrompt = QStringLiteral("hi");
        a.createdAt = QDateTime::currentDateTimeUtc();
        a.defaultHeartbeatGoal = QStringLiteral("Watch X for new launches");
        a.defaultHeartbeatSchedule = QStringLiteral("@daily at 09:00");
        a.defaultHeartbeatSurfaceCriteria = QStringLiteral("Engagement > 4x");
        QVERIFY(!m_agents->createAgent(a).isEmpty());

        const Agent rt = m_agents->getAgent(a.id);
        QVERIFY(rt.isValid());
        QCOMPARE(rt.defaultHeartbeatGoal, QStringLiteral("Watch X for new launches"));
        QCOMPARE(rt.defaultHeartbeatSchedule, QStringLiteral("@daily at 09:00"));
        QCOMPARE(rt.defaultHeartbeatSurfaceCriteria, QStringLiteral("Engagement > 4x"));

        Agent u = rt;
        u.defaultHeartbeatGoal = QStringLiteral("Updated goal");
        QVERIFY(m_agents->updateAgent(u));
        QCOMPARE(m_agents->getAgent(u.id).defaultHeartbeatGoal, QStringLiteral("Updated goal"));
    }

    void test_recordChange_andRecentConfigChanges_roundTrip() {
        const QString configId = createConfig(false);

        QVERIFY(m_configSvc->recordChange(configId,
                                          QStringLiteral("goal"),
                                          QStringLiteral("old"),
                                          QStringLiteral("new"),
                                          QStringLiteral("agent")));
        QVERIFY(m_configSvc->recordChange(configId,
                                          QStringLiteral("schedule"),
                                          QString(),
                                          QStringLiteral("@interval 15"),
                                          QStringLiteral("user")));

        const QVariantList rows = m_configSvc->recentConfigChanges(50);
        QCOMPARE(rows.size(), 2);
        QSet<QPair<QString, QString>> seen;
        for (const QVariant& v : rows) {
            const QVariantMap m = v.toMap();
            seen.insert(qMakePair(m.value(QStringLiteral("source")).toString(),
                                  m.value(QStringLiteral("field")).toString()));
        }
        QVERIFY(seen.contains(qMakePair(QStringLiteral("agent"), QStringLiteral("goal"))));
        QVERIFY(seen.contains(qMakePair(QStringLiteral("user"), QStringLiteral("schedule"))));

        QVERIFY(!m_configSvc->recordChange(
            configId, QStringLiteral("x"), QString(), QString(), QStringLiteral("invalid")));
    }

    void test_deletingConfig_cascadesAuditRows() {
        const QString configId = createConfig(false);
        QVERIFY(m_configSvc->recordChange(configId,
                                          QStringLiteral("goal"),
                                          QString(),
                                          QStringLiteral("v1"),
                                          QStringLiteral("user")));
        QCOMPARE(m_configSvc->recentConfigChanges(10).size(), 1);

        QVERIFY(m_configSvc->deleteConfig(configId));
        QCOMPARE(m_configSvc->recentConfigChanges(10).size(), 0);
    }

    void test_allTools_refuseWithoutResponderIdentity() {
        const QString configId = createConfig(true);
        Q_UNUSED(configId);
        Tools::HeartbeatToolDeps deps = makeDeps();

        Tools::SetHeartbeatGoalTool t1(deps);
        QJsonObject args1{{QStringLiteral("text"), QStringLiteral("x")}};
        QVERIFY(t1.invoke(args1).toObject().contains(QStringLiteral("error")));

        Tools::EnableHeartbeatTool t2(deps);
        QVERIFY(t2.invoke(QJsonObject{}).toObject().contains(QStringLiteral("error")));

        Tools::DisableHeartbeatTool t3(deps);
        QVERIFY(t3.invoke(QJsonObject{}).toObject().contains(QStringLiteral("error")));
    }

    void test_allTools_refuseWhenNoConfigForCaller() {
        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();

        Tools::SetHeartbeatGoalTool t(deps);
        const auto out =
            t.invoke(QJsonObject{{QStringLiteral("text"), QStringLiteral("body")}}).toObject();
        QVERIFY(out.contains(QStringLiteral("error")));
        QVERIFY(out.value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("No heartbeat config exists")));
    }

    void test_setHeartbeatGoal_persistsAndAudits() {
        const QString configId = createConfig(true);
        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();

        Tools::SetHeartbeatGoalTool t(deps);
        const auto out =
            t.invoke(QJsonObject{{QStringLiteral("text"), QStringLiteral("Watch new trends")}})
                .toObject();
        QCOMPARE(out.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

        QCOMPARE(m_configSvc->configById(configId).goal, QStringLiteral("Watch new trends"));

        const auto rows = m_configSvc->recentConfigChanges(10);
        QCOMPARE(rows.size(), 1);
        const auto m = rows[0].toMap();
        QCOMPARE(m.value(QStringLiteral("field")).toString(), QStringLiteral("goal"));
        QCOMPARE(m.value(QStringLiteral("source")).toString(), QStringLiteral("agent"));
        QCOMPARE(m.value(QStringLiteral("newValue")).toString(),
                 QStringLiteral("Watch new trends"));
    }

    void test_setHeartbeatGoal_rejectsEmpty() {
        const QString configId = createConfig(true);
        Q_UNUSED(configId);
        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();
        Tools::SetHeartbeatGoalTool t(deps);

        QVERIFY(t.invoke(QJsonObject{{QStringLiteral("text"), QStringLiteral("")}})
                    .toObject()
                    .contains(QStringLiteral("error")));
        QVERIFY(t.invoke(QJsonObject{{QStringLiteral("text"), QStringLiteral("   ")}})
                    .toObject()
                    .contains(QStringLiteral("error")));
        QCOMPARE(m_configSvc->recentConfigChanges(10).size(), 0);
    }

    void test_setHeartbeatSchedule_rejectsBadSchedules() {
        const QString configId = createConfig(true);
        Q_UNUSED(configId);
        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();
        Tools::SetHeartbeatScheduleTool t(deps);

        QVERIFY(t.invoke(QJsonObject{{QStringLiteral("schedule"), QStringLiteral("@interval 4")}})
                    .toObject()
                    .contains(QStringLiteral("error")));
        QVERIFY(t.invoke(QJsonObject{{QStringLiteral("schedule"), QStringLiteral("garbage 42")}})
                    .toObject()
                    .contains(QStringLiteral("error")));
        QVERIFY(t.invoke(QJsonObject{{QStringLiteral("schedule"), QStringLiteral("5m")}})
                    .toObject()
                    .contains(QStringLiteral("error")));
    }

    void test_setHeartbeatSchedule_rejectsExcessFireRate() {
        const QString configId = createConfig(true);
        Q_UNUSED(configId);
        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();
        Tools::SetHeartbeatScheduleTool t(deps);

        const auto out =
            t.invoke(QJsonObject{{QStringLiteral("schedule"), QStringLiteral("@interval 5")}})
                .toObject();
        QVERIFY(out.contains(QStringLiteral("error")));
        QVERIFY(out.value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("max_runs_per_day"), Qt::CaseInsensitive));
    }

    void test_setHeartbeatSchedule_persistsHappyPath() {
        const QString configId = createConfig(true);
        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();
        Tools::SetHeartbeatScheduleTool t(deps);

        const auto out =
            t.invoke(QJsonObject{{QStringLiteral("schedule"), QStringLiteral("@daily at 09:00")}})
                .toObject();
        QCOMPARE(out.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(m_configSvc->configById(configId).schedule, QStringLiteral("@daily at 09:00"));

        const auto rows = m_configSvc->recentConfigChanges(10);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toMap().value(QStringLiteral("field")).toString(),
                 QStringLiteral("schedule"));
        QCOMPARE(rows[0].toMap().value(QStringLiteral("source")).toString(),
                 QStringLiteral("agent"));
    }

    void test_enableHeartbeat_requiresSelfConfigAllowed() {
        QString configId = createConfig(false);
        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();

        Tools::EnableHeartbeatTool t(deps);
        const auto refused = t.invoke(QJsonObject{}).toObject();
        QVERIFY(refused.contains(QStringLiteral("error")));
        QVERIFY(refused.value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("Self-configuration is not allowed"),
                              Qt::CaseInsensitive));
        QCOMPARE(m_configSvc->configById(configId).enabled, false);
        QCOMPARE(m_configSvc->recentConfigChanges(10).size(), 0);

        HeartbeatConfig cfg = m_configSvc->configById(configId);
        cfg.selfConfigAllowed = true;
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());
        const auto okResult = t.invoke(QJsonObject{}).toObject();
        QCOMPARE(okResult.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(m_configSvc->configById(configId).enabled, true);

        const auto rows = m_configSvc->recentConfigChanges(10);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toMap().value(QStringLiteral("field")).toString(),
                 QStringLiteral("enabled"));
        QCOMPARE(rows[0].toMap().value(QStringLiteral("newValue")).toString(), QStringLiteral("1"));
    }

    void test_disableHeartbeat_alwaysAllowed() {
        QString configId = createConfig(false);

        HeartbeatConfig cfg = m_configSvc->configById(configId);
        cfg.enabled = true;
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());

        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();
        Tools::DisableHeartbeatTool t(deps);
        const auto out = t.invoke(QJsonObject{}).toObject();
        QCOMPARE(out.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(m_configSvc->configById(configId).enabled, false);

        const auto rows = m_configSvc->recentConfigChanges(10);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toMap().value(QStringLiteral("field")).toString(),
                 QStringLiteral("enabled"));
        QCOMPARE(rows[0].toMap().value(QStringLiteral("newValue")).toString(), QStringLiteral("0"));
    }

    void test_unchangedValue_doesNotWriteAuditRow() {
        const QString configId = createConfig(true);
        Q_UNUSED(configId);
        m_cascade->setCurrentResponder(QString(), QStringLiteral("agent-sct"));
        Tools::HeartbeatToolDeps deps = makeDeps();

        Tools::SetHeartbeatGoalTool t(deps);
        const auto out =
            t.invoke(QJsonObject{{QStringLiteral("text"), QStringLiteral("initial goal")}})
                .toObject();
        QCOMPARE(out.value(QStringLiteral("status")).toString(), QStringLiteral("unchanged"));
        QCOMPARE(m_configSvc->recentConfigChanges(10).size(), 0);
    }

  private:
    Tools::HeartbeatToolDeps makeDeps() {
        Tools::HeartbeatToolDeps d;
        d.configSvc = m_configSvc.get();
        d.subagentSvc = m_subagentSvc.get();
        d.agents = m_agents.get();
        d.convs = m_convs.get();
        Chat::CascadeController* cascade = m_cascade.get();
        d.cascadeResolver = [cascade]() -> Chat::CascadeController* { return cascade; };
        const QString convId = m_convId;
        d.activeConvIdGetter = [convId]() -> QString { return convId; };
        return d;
    }

    QString createConfig(bool selfAllowed) {
        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-sct-1");
        cfg.agentId = QStringLiteral("agent-sct");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.alias = QString();
        cfg.enabled = false;
        cfg.schedule = QString();
        cfg.goal = QStringLiteral("initial goal");
        cfg.maxRunsPerDay = 24;
        cfg.selfConfigAllowed = selfAllowed;
        const QString id = m_configSvc->upsertConfig(cfg);
        if (id.isEmpty())
            qWarning() << "createConfig upsert failed";
        return id;
    }

    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<SkillService> m_skillSvc;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<Chat::RequestBuilder> m_reqBuilder;
    std::unique_ptr<HeartbeatConfigService> m_configSvc;
    std::unique_ptr<Chat::CascadeController> m_cascade;
    std::unique_ptr<HeartbeatSubagentService> m_subagentSvc;
};

QTEST_MAIN(TestHeartbeatSelfConfigTools)
#include "test-heartbeat-self-config-tools.moc"

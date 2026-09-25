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
#include <QSqlDatabase>

class TestHeartbeatToolPerClient : public QObject {
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

        Agent local;
        local.id = QStringLiteral("agent-local");
        local.name = QStringLiteral("LocalAgent");
        local.systemPrompt = QStringLiteral("you are local");
        local.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(local).isEmpty());

        Agent wire;
        wire.id = QStringLiteral("agent-wire");
        wire.name = QStringLiteral("WireAgent");
        wire.systemPrompt = QStringLiteral("you are wire");
        wire.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(wire).isEmpty());

        m_localConvId = m_convs->createConversation(QStringLiteral("Local Conv"));
        m_wireConvId = m_convs->createConversation(QStringLiteral("Wire Conv"));
        QVERIFY(!m_localConvId.isEmpty());
        QVERIFY(!m_wireConvId.isEmpty());

        m_localConfigId = seedConfig(
            QStringLiteral("agent-local"), m_localConvId, QStringLiteral("local-initial-goal"));
        m_wireConfigId = seedConfig(
            QStringLiteral("agent-wire"), m_wireConvId, QStringLiteral("wire-initial-goal"));
        QVERIFY(!m_localConfigId.isEmpty());
        QVERIFY(!m_wireConfigId.isEmpty());

        m_deps.configSvc = m_configSvc.get();
        m_deps.subagentSvc = m_subagentSvc.get();
        m_deps.agents = m_agents.get();
        m_deps.convs = m_convs.get();
        Chat::CascadeController* cascade = m_cascade.get();
        m_deps.cascadeResolver = [cascade]() -> Chat::CascadeController* { return cascade; };
        const QString localConv = m_localConvId;
        m_deps.activeConvIdGetter = [localConv]() { return localConv; };
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


    void test_setGoal_wireAgent_mutatesWireConfig_NOT_localConfig() {
        m_cascade->setCurrentResponder(QStringLiteral("LocalAgent"), QStringLiteral("agent-local"));

        Tools::SetHeartbeatGoalTool tool(m_deps);
        QJsonObject args;
        args.insert("text", "new wire goal");
        args.insert("__caller_conv_id", m_wireConvId);
        args.insert("__caller_agent_id", QStringLiteral("agent-wire"));
        args.insert("__caller_agent_alias", QStringLiteral("WireAgent"));

        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("status").toString(), QStringLiteral("ok"));

        const auto wireCfg = m_configSvc->configById(m_wireConfigId);
        QCOMPARE(wireCfg.goal, QStringLiteral("new wire goal"));

        const auto localCfg = m_configSvc->configById(m_localConfigId);
        QVERIFY2(localCfg.goal == QStringLiteral("local-initial-goal"),
                 qPrintable(QStringLiteral("Cross-instance corruption — LocalAgent's goal mutated "
                                           "from 'local-initial-goal' to '%1'")
                                .arg(localCfg.goal)));
    }


    void test_setGoal_fallsBackToCascade_whenArgsEmpty() {
        m_cascade->setCurrentResponder(QStringLiteral("LocalAgent"), QStringLiteral("agent-local"));

        Tools::SetHeartbeatGoalTool tool(m_deps);
        QJsonObject args;
        args.insert("text", "local-only update");

        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));

        const auto localCfg = m_configSvc->configById(m_localConfigId);
        QCOMPARE(localCfg.goal, QStringLiteral("local-only update"));

        const auto wireCfg = m_configSvc->configById(m_wireConfigId);
        QCOMPARE(wireCfg.goal, QStringLiteral("wire-initial-goal"));
    }


    void test_setGoal_errorsCleanly_whenBothEmpty() {
        Tools::SetHeartbeatGoalTool tool(m_deps);
        QJsonObject args;
        args.insert("text", "should fail");

        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        const QString err = result.value("error").toString();
        QVERIFY2(
            err.contains("responder", Qt::CaseInsensitive) ||
                err.contains("cascade", Qt::CaseInsensitive),
            qPrintable(QStringLiteral("Expected 'no responder identity' error, got: %1").arg(err)));
    }

  private:
    QString seedConfig(const QString& agentId, const QString& convId, const QString& goal) {
        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-%1").arg(agentId);
        cfg.agentId = agentId;
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = convId;
        cfg.alias = QString();
        cfg.enabled = false;
        cfg.schedule = QString();
        cfg.goal = goal;
        cfg.maxRunsPerDay = 24;
        cfg.selfConfigAllowed = true;
        return m_configSvc->upsertConfig(cfg);
    }

    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<SkillService> m_skillSvc;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<Chat::RequestBuilder> m_reqBuilder;
    std::unique_ptr<HeartbeatConfigService> m_configSvc;
    std::unique_ptr<Chat::CascadeController> m_cascade;
    std::unique_ptr<HeartbeatSubagentService> m_subagentSvc;
    Tools::HeartbeatToolDeps m_deps;
    QString m_localConvId;
    QString m_wireConvId;
    QString m_localConfigId;
    QString m_wireConfigId;
};

QTEST_MAIN(TestHeartbeatToolPerClient)
#include "test-heartbeat-tool-per-client.moc"

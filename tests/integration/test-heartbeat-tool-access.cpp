// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/tool-calling-schema.h"
#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/heartbeat-config.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/request-builder.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/heartbeat-config-service.h"
#include "../../backend/services/heartbeat-subagent-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/skill-service.h"
#include "../../backend/services/tool-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>


class MockProvider : public ILLMProvider {
    Q_OBJECT
  public:
    explicit MockProvider(const QString& id, QObject* parent = nullptr)
        : ILLMProvider(parent), m_id(id) {
        m_models = {id + QStringLiteral("-model-a")};
    }

    QString providerId() const override { return m_id; }
    QString displayName() const override { return m_id; }
    QStringList availableModels() override { return m_models; }
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }
    void refreshModels() override { emit modelsRefreshed(m_models); }

    void sendRequest(const LlmRequest& req) override {
        ++m_sendCallCount;
        m_lastRequest = req;
    }

    void cancelRequest() override { ++m_cancelCallCount; }

    int sendCallCount() const { return m_sendCallCount; }
    const LlmRequest& lastRequest() const { return m_lastRequest; }

    void emitChunk(const QString& delta) {
        LlmChunk c;
        c.delta = delta;
        emit chunkReceived(c);
    }
    void emitFinished(const QString& reason, int tokens) { emit requestFinished(reason, tokens); }

  private:
    QString m_id;
    QStringList m_models;
    int m_sendCallCount = 0;
    int m_cancelCallCount = 0;
    LlmRequest m_lastRequest;
};


class TestHeartbeatToolAccess : public QObject {
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

        ToolSchema toolA;
        toolA.name = QStringLiteral("synthetic_tool_alpha");
        toolA.description = QStringLiteral("Synthetic test tool A.");
        m_toolSvc->registerTool(toolA, [](const QJsonObject&) {
            return QJsonValue(QJsonObject{{"ok", true}});
        });

        ToolSchema toolB;
        toolB.name = QStringLiteral("synthetic_tool_beta");
        toolB.description = QStringLiteral("Synthetic test tool B.");
        m_toolSvc->registerTool(toolB, [](const QJsonObject&) {
            return QJsonValue(QJsonObject{{"ok", true}});
        });

        auto fgMock = std::make_unique<MockProvider>(QStringLiteral("ollama"));
        m_fgPtr = fgMock.get();
        m_router->registerProvider(std::move(fgMock));

        auto bgMock = std::make_unique<MockProvider>(QStringLiteral("ollama"));
        m_bgPtr = bgMock.get();
        m_router->registerBackgroundProvider(std::move(bgMock));

        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_skillSvc = std::make_unique<SkillService>(*m_convs);
        m_skillSvc->setAppDataRootForTesting(m_tempDir.path());
        m_skillSvc->initialize();
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_reqBuilder = std::make_unique<Chat::RequestBuilder>(*m_convs, *m_msgs, *m_router);
        m_configSvc = std::make_unique<HeartbeatConfigService>(DbManager::instance());

        Agent a;
        a.id = QStringLiteral("agent-h8");
        a.name = QStringLiteral("ToolAccessAgent-29H8");
        a.systemPrompt = QStringLiteral("You are agent H8.");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
        m_convId = m_convs->createConversation(QStringLiteral("Test Conv"));
        QVERIFY(!m_convId.isEmpty());

        m_svc = std::make_unique<HeartbeatSubagentService>(*m_configSvc,
                                                           *m_agents,
                                                           *m_convs,
                                                           *m_msgs,
                                                           *m_router,
                                                           *m_toolSvc,
                                                           *m_skillSvc,
                                                           *m_reqBuilder,
                                                           nullptr,
                                                           nullptr);
        m_svc->setTickIntervalMs(60000);
        m_svc->setRunTimeoutMs(2000);
    }

    void cleanup() {
        m_svc.reset();
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

    void test_HeartbeatRequestCarriesAllRegisteredTools() {
        HeartbeatConfig cfg;
        cfg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        cfg.agentId = QStringLiteral("agent-h8");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.alias = QString();
        cfg.enabled = true;
        cfg.goal = QStringLiteral("Test heartbeat tool access.");
        cfg.schedule = QStringLiteral("@interval 5");
        cfg.maxRunsPerDay = 24;
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());

        m_svc->runNow(cfg.id);

        QTRY_COMPARE_WITH_TIMEOUT(m_bgPtr->sendCallCount(), 1, 2000);

        const LlmRequest& req = m_bgPtr->lastRequest();
        QVERIFY2(!req.availableTools.isEmpty(),
                 "H8 regression: heartbeat request has empty availableTools");

        QSet<QString> seenTools;
        for (const ToolSchema& s : req.availableTools) {
            seenTools.insert(s.name);
        }
        QVERIFY(seenTools.contains(QStringLiteral("synthetic_tool_alpha")));
        QVERIFY(seenTools.contains(QStringLiteral("synthetic_tool_beta")));

        QCOMPARE(req.availableTools.size(), m_toolSvc->availableTools().size());

        QCOMPARE(m_fgPtr->sendCallCount(), 0);
    }

    void test_LateRegisteredToolFlowsThroughOnNextRun() {
        HeartbeatConfig cfg;
        cfg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        cfg.agentId = QStringLiteral("agent-h8");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.alias = QString();
        cfg.enabled = true;
        cfg.goal = QStringLiteral("Late-registered tool test.");
        cfg.schedule = QStringLiteral("@interval 5");
        cfg.maxRunsPerDay = 24;
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());

        m_svc->runNow(cfg.id);
        QTRY_COMPARE_WITH_TIMEOUT(m_bgPtr->sendCallCount(), 1, 2000);

        const int firstRunToolCount = m_bgPtr->lastRequest().availableTools.size();
        QCOMPARE(firstRunToolCount, 2);

        m_bgPtr->emitFinished(QStringLiteral("stop"), 1);

        ToolSchema toolC;
        toolC.name = QStringLiteral("synthetic_tool_gamma");
        toolC.description = QStringLiteral("Synthetic test tool C.");
        m_toolSvc->registerTool(toolC, [](const QJsonObject&) {
            return QJsonValue(QJsonObject{{"ok", true}});
        });

        m_svc->runNow(cfg.id);
        QTRY_COMPARE_WITH_TIMEOUT(m_bgPtr->sendCallCount(), 2, 2000);

        QSet<QString> seenTools;
        for (const ToolSchema& s : m_bgPtr->lastRequest().availableTools) {
            seenTools.insert(s.name);
        }
        QVERIFY(seenTools.contains(QStringLiteral("synthetic_tool_gamma")));
        QCOMPARE(m_bgPtr->lastRequest().availableTools.size(), 3);
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<SkillService> m_skillSvc;
    MockProvider* m_fgPtr = nullptr;
    MockProvider* m_bgPtr = nullptr;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<Chat::RequestBuilder> m_reqBuilder;
    std::unique_ptr<HeartbeatConfigService> m_configSvc;
    std::unique_ptr<HeartbeatSubagentService> m_svc;
};

QTEST_MAIN(TestHeartbeatToolAccess)
#include "test-heartbeat-tool-access.moc"

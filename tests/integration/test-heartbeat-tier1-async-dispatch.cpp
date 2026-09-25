// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
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
#include <QSqlQuery>
#include <QSqlRecord>


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
    int cancelCallCount() const { return m_cancelCallCount; }
    const LlmRequest& lastRequest() const { return m_lastRequest; }

    void emitChunk(const QString& delta) {
        LlmChunk c;
        c.delta = delta;
        emit chunkReceived(c);
    }
    void emitFinished(const QString& reason, int tokens) { emit requestFinished(reason, tokens); }
    void emitError(const QString& msg) { emit requestError(msg); }

  private:
    QString m_id;
    QStringList m_models;
    int m_sendCallCount = 0;
    int m_cancelCallCount = 0;
    LlmRequest m_lastRequest;
};


class TestHeartbeatTier1AsyncDispatch : public QObject {
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
        a.id = QStringLiteral("agent-A");
        a.name = QStringLiteral("Agent A");
        a.systemPrompt = QStringLiteral("You are agent A.");
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

    void test_runNow_dispatchesThroughBackgroundSlotOnly() {
        const QString configId = createConfig();

        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());

        QCOMPARE(m_bgPtr->sendCallCount(), 1);
        QCOMPARE(m_fgPtr->sendCallCount(), 0);
    }

    void test_endToEndSuccess_persistsParsedReport() {
        const QString configId = createConfig();
        QSignalSpy startedSpy(m_svc.get(), &HeartbeatSubagentService::runStarted);
        QSignalSpy completedSpy(m_svc.get(), &HeartbeatSubagentService::runCompleted);

        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());
        QCOMPARE(startedSpy.count(), 1);

        m_bgPtr->emitChunk(QStringLiteral("TITLE: My research\n"));
        m_bgPtr->emitChunk(QStringLiteral("RESULTS:\n"));
        m_bgPtr->emitChunk(QStringLiteral("Found three trends.\n"));
        m_bgPtr->emitChunk(QStringLiteral("SUMMARY:\n"));
        m_bgPtr->emitChunk(QStringLiteral("Trends X, Y, Z noted.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 50);

        QVERIFY(completedSpy.wait(1000) || completedSpy.count() >= 1);
        QCOMPARE(completedSpy.count(), 1);
        QCOMPARE(completedSpy[0][1].toString(), QStringLiteral("success"));

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT outcome, title, body, summary, parent_review_status "
                                 "FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(runId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("success"));
        QCOMPARE(q.value(1).toString(), QStringLiteral("My research"));
        QVERIFY(q.value(2).toString().contains(QStringLiteral("Found three")));
        QVERIFY(q.value(3).toString().contains(QStringLiteral("Trends X, Y, Z")));
        QCOMPARE(q.value(4).toString(), QStringLiteral("not_reviewable_yet"));

        const auto cfg = m_configSvc->configById(configId);
        QCOMPARE(cfg.lastFireOutcome, QStringLiteral("success"));
        QVERIFY(!cfg.lastFireAt.isEmpty());
    }

    void test_errorPath_recordsErrorRow() {
        const QString configId = createConfig();
        QSignalSpy failedSpy(m_svc.get(), &HeartbeatSubagentService::runFailed);

        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());

        m_bgPtr->emitError(QStringLiteral("simulated upstream 503"));
        QVERIFY(failedSpy.wait(1000) || failedSpy.count() >= 1);
        QCOMPARE(failedSpy.count(), 1);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT outcome, error FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(runId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("error"));
        QVERIFY(q.value(1).toString().contains(QStringLiteral("503")));
    }

    void test_globallyPausedRefusesRunNow() {
        const QString configId = createConfig();
        m_svc->setGloballyPaused(true);
        QVERIFY(m_svc->runNow(configId).isEmpty());
        QCOMPARE(m_bgPtr->sendCallCount(), 0);
    }

    void test_foregroundDoesNotSeeBackgroundChunks() {
        const QString configId = createConfig();

        QSignalSpy fgChunkSpy(m_router.get(), &ModelRouter::chunkReceived);
        QSignalSpy bgChunkSpy(m_router.get(), &ModelRouter::backgroundChunkReceived);

        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());

        m_bgPtr->emitChunk(QStringLiteral("TITLE: t\nRESULTS: r\nSUMMARY: s\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 5);

        QCOMPARE(bgChunkSpy.count(), 1);
        QCOMPARE(fgChunkSpy.count(), 0);
    }

  private:
    QString createConfig() {
        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-test-1");
        cfg.agentId = QStringLiteral("agent-A");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.enabled = true;
        cfg.schedule = QStringLiteral("@interval 5");
        cfg.goal = QStringLiteral("test goal");
        cfg.maxRunsPerDay = 100;
        m_configSvc->upsertConfig(cfg);
        return cfg.id;
    }

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

QTEST_MAIN(TestHeartbeatTier1AsyncDispatch)
#include "test-heartbeat-tier1-async-dispatch.moc"

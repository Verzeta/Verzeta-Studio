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

class TestHeartbeatTier2Review : public QObject {
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
        a.id = QStringLiteral("agent-tier2");
        a.name = QStringLiteral("Tier2Agent");
        a.systemPrompt = QStringLiteral("You are tier-2 agent.");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
        m_convId = m_convs->createConversation(QStringLiteral("Tier2 Conv"));
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

    void test_gateOff_skipsTier2_marksNotReviewableYet() {
        const QString configId = createConfig();
        QSignalSpy decisionSpy(m_svc.get(), &HeartbeatSubagentService::surfaceDecision);
        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());
        QCOMPARE(m_bgPtr->sendCallCount(), 1);

        m_bgPtr->emitChunk(
            QStringLiteral("TITLE: T1 done\nRESULTS:\nfound it\nSUMMARY:\nfound something.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 30);

        QCOMPARE(m_bgPtr->sendCallCount(), 1);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT parent_review_status, surface_status, surfaced_message_id "
                                 "FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(runId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("not_reviewable_yet"));
        QCOMPARE(q.value(1).toString(), QStringLiteral("skipped_by_gate"));
        QVERIFY(q.value(2).toString().isEmpty());

        QCOMPARE(decisionSpy.count(), 1);
        QCOMPARE(decisionSpy[0][1].toString(), QStringLiteral("skipped_by_gate"));
        QVERIFY(decisionSpy[0][2].toString().isEmpty());
    }

    void test_gateOn_agentSkips_marksSkippedByAgent() {
        QVERIFY(m_convs->setHeartbeatAutoSurface(m_convId, true, 5));

        const QString configId = createConfig();
        QSignalSpy decisionSpy(m_svc.get(), &HeartbeatSubagentService::surfaceDecision);
        const QString runId = m_svc->runNow(configId);

        m_bgPtr->emitChunk(QStringLiteral("TITLE: T1\nRESULTS:\n.\nSUMMARY:\nshort.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 10);

        QCOMPARE(m_bgPtr->sendCallCount(), 2);

        m_bgPtr->emitChunk(QStringLiteral("[SKIP]"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 5);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT parent_review_status, surface_status, surfaced_message_id "
                                 "FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(runId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("reviewed"));
        QCOMPARE(q.value(1).toString(), QStringLiteral("skipped_by_agent"));
        QVERIFY(q.value(2).toString().isEmpty());

        const auto convMsgs = m_msgs->getMessages(m_convId);
        for (const auto& m : convMsgs) {
            QVERIFY2(m.role != QStringLiteral("assistant"),
                     "[SKIP] path must NOT insert an assistant message");
        }

        QCOMPARE(decisionSpy.last()[1].toString(), QStringLiteral("skipped_by_agent"));
    }

    void test_gateOn_agentPosts_persistsAutoSurfaceMessage() {
        QVERIFY(m_convs->setHeartbeatAutoSurface(m_convId, true, 5));

        const QString configId = createConfig();
        const QString runId = m_svc->runNow(configId);

        m_bgPtr->emitChunk(QStringLiteral("TITLE: T1\nRESULTS:\nstuff\nSUMMARY:\nfound stuff.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 10);
        QCOMPARE(m_bgPtr->sendCallCount(), 2);

        const QString postBody = QStringLiteral("Quick update: found three new trends overnight.");
        m_bgPtr->emitChunk(postBody);
        m_bgPtr->emitFinished(QStringLiteral("stop"), 5);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT parent_review_status, surface_status, surfaced_message_id "
                                 "FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(runId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("reviewed"));
        QCOMPARE(q.value(1).toString(), QStringLiteral("posted_auto"));
        const QString surfacedMsgId = q.value(2).toString();
        QVERIFY(!surfacedMsgId.isEmpty());

        QSqlQuery m(DbManager::instance().db());
        m.prepare(QStringLiteral("SELECT role, content, agent_id, member_alias, metadata "
                                 "FROM messages WHERE id = ?"));
        m.addBindValue(surfacedMsgId);
        QVERIFY(m.exec());
        QVERIFY(m.next());
        QCOMPARE(m.value(0).toString(), QStringLiteral("assistant"));
        QCOMPARE(m.value(1).toString(), postBody);
        QCOMPARE(m.value(2).toString(), QStringLiteral("agent-tier2"));

        const QString metaJson = m.value(4).toString();
        QVERIFY(metaJson.contains(QStringLiteral("heartbeat_surface_auto")));
        QVERIFY(metaJson.contains(runId));
    }

    void test_gateOn_dailyCapReached_skipsByRateLimit() {
        QVERIFY(m_convs->setHeartbeatAutoSurface(m_convId, true, 1));

        const QString configId = createConfig();

        const QString fakeMsgId = QStringLiteral("seed-msg-id");
        QSqlQuery insMsg(DbManager::instance().db());
        insMsg.prepare(
            QStringLiteral("INSERT INTO messages (id, conversation_id, role, content,"
                           "  created_at, token_count, finish_reason, metadata, agent_id,"
                           "  member_alias, turn_id) "
                           "VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?)"));
        insMsg.addBindValue(fakeMsgId);
        insMsg.addBindValue(m_convId);
        insMsg.addBindValue(QStringLiteral("assistant"));
        insMsg.addBindValue(QStringLiteral("seed"));
        insMsg.addBindValue(QDateTime::currentMSecsSinceEpoch());
        insMsg.addBindValue(QStringLiteral("stop"));
        insMsg.addBindValue(QStringLiteral("{}"));
        insMsg.addBindValue(QStringLiteral("agent-tier2"));
        insMsg.addBindValue(QString());
        insMsg.addBindValue(fakeMsgId);
        QVERIFY(insMsg.exec());

        QSqlQuery insReport(DbManager::instance().db());
        insReport.prepare(
            QStringLiteral("INSERT INTO heartbeat_reports ("
                           "  id, config_id, started_at, completed_at, outcome,"
                           "  title, body, summary, parent_review_status, surface_status,"
                           "  surfaced_message_id, error)"
                           " VALUES (?, ?, ?, ?, 'success', '', '', '', 'reviewed',"
                           "  'posted_auto', ?, '')"));
        const QString seedReportId = QStringLiteral("seed-rep-id");
        const QString isoNow = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        insReport.addBindValue(seedReportId);
        insReport.addBindValue(configId);
        insReport.addBindValue(isoNow);
        insReport.addBindValue(isoNow);
        insReport.addBindValue(fakeMsgId);
        QVERIFY(insReport.exec());

        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());
        m_bgPtr->emitChunk(QStringLiteral("TITLE: T1\nRESULTS:\n.\nSUMMARY:\n.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 5);

        QCOMPARE(m_bgPtr->sendCallCount(), 1);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT parent_review_status, surface_status "
                                 "FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(runId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("not_reviewable_yet"));
        QCOMPARE(q.value(1).toString(), QStringLiteral("skipped_by_rate_limit"));
    }

  private:
    QString createConfig() {
        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-tier2-1");
        cfg.agentId = QStringLiteral("agent-tier2");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.enabled = true;
        cfg.schedule = QStringLiteral("@interval 5");
        cfg.goal = QStringLiteral("monitor things");
        cfg.surfaceCriteria = QStringLiteral("anything new + relevant");
        cfg.maxRunsPerDay = 100;
        const QString id = m_configSvc->upsertConfig(cfg);
        if (id.isEmpty()) {
            qWarning() << "createConfig: upsertConfig failed";
        }
        return id;
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

QTEST_MAIN(TestHeartbeatTier2Review)
#include "test-heartbeat-tier2-review.moc"

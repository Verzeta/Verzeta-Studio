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
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
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
    void sendRequest(const LlmRequest&) override { ++m_sendCallCount; }
    void cancelRequest() override { ++m_cancelCallCount; }
    int sendCallCount() const { return m_sendCallCount; }
    void emitFinished(const QString& reason, int tokens) { emit requestFinished(reason, tokens); }
    void emitChunk(const QString& delta) {
        LlmChunk c;
        c.delta = delta;
        emit chunkReceived(c);
    }

  private:
    QString m_id;
    QStringList m_models;
    int m_sendCallCount = 0;
    int m_cancelCallCount = 0;
};

class TestHeartbeatCrashRestart : public QObject {
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
        auto bgMock = std::make_unique<MockProvider>(QStringLiteral("ollama"));
        m_bgPtr = bgMock.get();
        m_router->registerBackgroundProvider(std::move(bgMock));
        auto fgMock = std::make_unique<MockProvider>(QStringLiteral("ollama"));
        m_router->registerProvider(std::move(fgMock));
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
        a.id = QStringLiteral("agent-cr");
        a.name = QStringLiteral("CrashRestartAgent");
        a.systemPrompt = QStringLiteral("you are tester");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
        m_convId = m_convs->createConversation(QStringLiteral("CR Conv"));
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

    void test_missedFiresAmnesty_singleFireOnResume() {
        const QDateTime T0 =
            QDateTime::fromString(QStringLiteral("2026-05-02T12:00:00.000"), Qt::ISODateWithMs);
        QVERIFY(T0.isValid());
        m_svc->setClockFn([T0]() { return T0; });

        const QDateTime longAgo = T0.addDays(-2);
        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-cr-amnesty-1");
        cfg.agentId = QStringLiteral("agent-cr");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.enabled = true;
        cfg.schedule = QStringLiteral("@interval 5");
        cfg.goal = QStringLiteral("amnesty test");
        cfg.maxRunsPerDay = 9999;
        cfg.lastFireAt = longAgo.toString(Qt::ISODateWithMs);
        cfg.lastFireOutcome = QStringLiteral("success");
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());

        QMetaObject::invokeMethod(m_svc.get(), "onScheduleTick", Qt::DirectConnection);

        QCOMPARE(m_bgPtr->sendCallCount(), 1);

        m_bgPtr->emitChunk(QStringLiteral("TITLE: t\nRESULTS:\n.\nSUMMARY:\n.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 5);

        const HeartbeatConfig refreshed = m_configSvc->configById(cfg.id);
        const QDateTime anchorAfter =
            QDateTime::fromString(refreshed.lastFireAt, Qt::ISODateWithMs);
        QVERIFY(anchorAfter.isValid());
        QVERIFY2(anchorAfter >= T0, "amnesty: anchor must clamp to >= now after dispatch");

        QMetaObject::invokeMethod(m_svc.get(), "onScheduleTick", Qt::DirectConnection);
        QCOMPARE(m_bgPtr->sendCallCount(), 1);
    }

    void test_anchorAdvances_evenOnRateLimitedOutcome() {
        const QDateTime T0 =
            QDateTime::fromString(QStringLiteral("2026-05-02T12:00:00.000"), Qt::ISODateWithMs);
        m_svc->setClockFn([T0]() { return T0; });

        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-cr-rl-1");
        cfg.agentId = QStringLiteral("agent-cr");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.enabled = true;
        cfg.schedule = QStringLiteral("@interval 5");
        cfg.goal = QStringLiteral("rate-limit test");
        cfg.maxRunsPerDay = 1;
        cfg.lastFireAt = T0.addSecs(-3600).toString(Qt::ISODateWithMs);
        cfg.lastFireOutcome = QStringLiteral("success");
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());

        QSqlQuery insert(DbManager::instance().db());
        insert.prepare(
            QStringLiteral("INSERT INTO heartbeat_reports ("
                           "  id, config_id, started_at, completed_at, outcome,"
                           "  title, body, summary, parent_review_status, surface_status,"
                           "  surfaced_message_id, error)"
                           " VALUES (?, ?, ?, ?, 'success', '', '', '', "
                           "  'reviewed', 'pending', '', '')"));
        const QString recentIso = T0.addSecs(-1800).toString(Qt::ISODateWithMs);
        insert.addBindValue(QStringLiteral("seed-success"));
        insert.addBindValue(cfg.id);
        insert.addBindValue(recentIso);
        insert.addBindValue(recentIso);
        QVERIFY(insert.exec());

        const QDateTime anchorBefore =
            QDateTime::fromString(m_configSvc->configById(cfg.id).lastFireAt, Qt::ISODateWithMs);

        QMetaObject::invokeMethod(m_svc.get(), "onScheduleTick", Qt::DirectConnection);

        QCOMPARE(m_bgPtr->sendCallCount(), 0);

        const HeartbeatConfig refreshed = m_configSvc->configById(cfg.id);
        QCOMPARE(refreshed.lastFireOutcome, QStringLiteral("rate_limited"));

        const QDateTime anchorAfter =
            QDateTime::fromString(refreshed.lastFireAt, Qt::ISODateWithMs);
        QVERIFY(anchorAfter > anchorBefore);
    }

    void test_initialize_logsAmnestyNotice_operatorFriendly() {
        m_svc->initialize();
        const QStringList lines = m_svc->recentLogLines();

        bool foundAmnestyEffect = false;
        bool foundJargon = false;
        for (const QString& line : lines) {
            if (line.contains(QStringLiteral("not replayed"), Qt::CaseInsensitive) ||
                line.contains(QStringLiteral("missed runs"), Qt::CaseInsensitive)) {
                foundAmnestyEffect = true;
            }
            static const QRegularExpression kInternalPlanRefRx(
                QStringLiteral("\\b(?:Plan|Phase)\\s+[0-9A-Z]"),
                QRegularExpression::CaseInsensitiveOption);
            if (line.contains(QStringLiteral("§2.9")) ||
                line.contains(QStringLiteral("lastFireAt")) ||
                kInternalPlanRefRx.match(line).hasMatch()) {
                foundJargon = true;
            }
        }
        QVERIFY2(foundAmnestyEffect,
                 "initialize() must log the user-visible amnesty effect for the diagnostics panel");
        QVERIFY2(!foundJargon,
                 "initialize() log must NOT leak engineering identifiers (section markers, "
                 "lastFireAt, internal plan refs) into the user-facing panel");
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_convId;

    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<SkillService> m_skillSvc;
    MockProvider* m_bgPtr = nullptr;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<Chat::RequestBuilder> m_reqBuilder;
    std::unique_ptr<HeartbeatConfigService> m_configSvc;
    std::unique_ptr<HeartbeatSubagentService> m_svc;
};

QTEST_MAIN(TestHeartbeatCrashRestart)
#include "test-heartbeat-crash-restart.moc"

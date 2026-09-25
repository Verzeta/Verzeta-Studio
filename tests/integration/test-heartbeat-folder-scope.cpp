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

class TestHeartbeatFolderScope : public QObject {
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
        a.id = QStringLiteral("agent-folder");
        a.name = QStringLiteral("FolderAgent");
        a.systemPrompt = QStringLiteral("you are folder-scoped");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());

        m_folderId = m_convs->createFolder(QStringLiteral("Test Project"));
        QVERIFY(!m_folderId.isEmpty());
        m_targetConvId = m_convs->createConversation(QStringLiteral("Target Chat"));
        QVERIFY(!m_targetConvId.isEmpty());
        QVERIFY(m_convs->moveToFolder(m_targetConvId, m_folderId));
        m_otherConvId = m_convs->createConversation(QStringLiteral("Other Chat"));
        QVERIFY(!m_otherConvId.isEmpty());
        QVERIFY(m_convs->moveToFolder(m_otherConvId, m_folderId));

        Message m1;
        m1.id = QStringLiteral("ctx-msg-1");
        m1.conversationId = m_targetConvId;
        m1.role = QStringLiteral("user");
        m1.content = QStringLiteral("target conv hello");
        m1.createdAt = QDateTime::currentDateTimeUtc().addSecs(-100);
        QVERIFY(!m_msgs->addMessage(m1).isEmpty());
        Message m2;
        m2.id = QStringLiteral("ctx-msg-2");
        m2.conversationId = m_otherConvId;
        m2.role = QStringLiteral("user");
        m2.content = QStringLiteral("other conv hello");
        m2.createdAt = QDateTime::currentDateTimeUtc().addSecs(-50);
        QVERIFY(!m_msgs->addMessage(m2).isEmpty());

        QVERIFY(m_convs->setHeartbeatAutoSurface(m_targetConvId, true, 10));

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

    void test_folderScopeDispatches_runNow() {
        const QString configId = createFolderConfig(m_targetConvId);
        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());
        QCOMPARE(m_bgPtr->sendCallCount(), 1);
        QCOMPARE(m_bgPtr->lastRequest().turnKind, QStringLiteral("heartbeat_subagent"));
    }

    void test_folderDispatch_includesCrossContextRecall() {
        const QString configId = createFolderConfig(m_targetConvId);
        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());

        const auto msgs = m_bgPtr->lastRequest().messages;
        QCOMPARE(msgs.size(), 2);
        QCOMPARE(msgs[0].content, QStringLiteral("target conv hello"));
        QCOMPARE(msgs[1].content, QStringLiteral("other conv hello"));
    }

    void test_folderTier2_postsIntoTargetConv() {
        const QString configId = createFolderConfig(m_targetConvId);
        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());

        m_bgPtr->emitChunk(QStringLiteral("TITLE: t\nRESULTS:\nstuff\nSUMMARY:\nfound stuff.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 10);
        QCOMPARE(m_bgPtr->sendCallCount(), 2);

        const QString postBody = QStringLiteral("Project alert: target conv update incoming.");
        m_bgPtr->emitChunk(postBody);
        m_bgPtr->emitFinished(QStringLiteral("stop"), 5);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT surface_status, surfaced_message_id "
                                 "FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(runId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("posted_auto"));
        const QString msgId = q.value(1).toString();
        QVERIFY(!msgId.isEmpty());

        QSqlQuery mq(DbManager::instance().db());
        mq.prepare(QStringLiteral("SELECT conversation_id, content FROM messages WHERE id = ?"));
        mq.addBindValue(msgId);
        QVERIFY(mq.exec());
        QVERIFY(mq.next());
        QCOMPARE(mq.value(0).toString(), m_targetConvId);
        QCOMPARE(mq.value(1).toString(), postBody);
    }

    void test_folderTier2_emptyTarget_overlayOnly() {
        const QString configId = createFolderConfig(QString());
        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());

        m_bgPtr->emitChunk(QStringLiteral("TITLE: x\nRESULTS:\n.\nSUMMARY:\n.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 5);

        QCOMPARE(m_bgPtr->sendCallCount(), 1);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT surface_status FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(runId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("skipped_by_gate"));
    }

    void test_convScopeInFolder_pullsFolderWideSiblingRecall() {
        Message primary;
        primary.id = QStringLiteral("primary-msg-1");
        primary.conversationId = m_targetConvId;
        primary.role = QStringLiteral("user");
        primary.content = QStringLiteral("primary conv recent msg");
        primary.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgs->addMessage(primary).isEmpty());

        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-conv-in-folder-1");
        cfg.agentId = QStringLiteral("agent-folder");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_targetConvId;
        cfg.alias = QString();
        cfg.enabled = true;
        cfg.schedule = QString();
        cfg.goal = QStringLiteral("conv-in-folder goal");
        cfg.maxRunsPerDay = 100;
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());

        const QString runId = m_svc->runNow(cfg.id);
        QVERIFY(!runId.isEmpty());

        const auto msgs = m_bgPtr->lastRequest().messages;
        QCOMPARE(msgs.size(), 3);
        QCOMPARE(msgs[0].content, QStringLiteral("other conv hello"));
        QCOMPARE(msgs[1].content, QStringLiteral("target conv hello"));
        QCOMPARE(msgs[2].content, QStringLiteral("primary conv recent msg"));
    }

    void test_convScopeOutsideFolder_noSiblingRecall() {
        const QString rootConvId = m_convs->createConversation(QStringLiteral("Root Chat"));
        QVERIFY(!rootConvId.isEmpty());
        Message m;
        m.id = QStringLiteral("root-msg-1");
        m.conversationId = rootConvId;
        m.role = QStringLiteral("user");
        m.content = QStringLiteral("root chat msg");
        m.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgs->addMessage(m).isEmpty());

        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-root-conv-1");
        cfg.agentId = QStringLiteral("agent-folder");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = rootConvId;
        cfg.alias = QString();
        cfg.enabled = true;
        cfg.schedule = QString();
        cfg.goal = QStringLiteral("root-conv goal");
        cfg.maxRunsPerDay = 100;
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());

        const QString runId = m_svc->runNow(cfg.id);
        QVERIFY(!runId.isEmpty());

        const auto msgs = m_bgPtr->lastRequest().messages;
        QCOMPARE(msgs.size(), 1);
        QCOMPARE(msgs[0].content, QStringLiteral("root chat msg"));
    }

    void test_folderDeletion_cascadesConfigsAndReports() {
        const QString configId = createFolderConfig(m_targetConvId);
        const QString runId = m_svc->runNow(configId);
        QVERIFY(!runId.isEmpty());
        m_bgPtr->emitChunk(QStringLiteral("TITLE: t\nRESULTS:\n.\nSUMMARY:\n.\n"));
        m_bgPtr->emitFinished(QStringLiteral("stop"), 5);

        QCOMPARE(m_configSvc->configsInScope(HeartbeatScopeType::Folder, m_folderId).size(), 1);

        QObject::connect(m_convs.get(),
                         &ConversationService::folderDeleted,
                         m_configSvc.get(),
                         &HeartbeatConfigService::onFolderDeleted);

        QVERIFY(m_convs->deleteFolder(m_folderId));

        QCOMPARE(m_configSvc->configsInScope(HeartbeatScopeType::Folder, m_folderId).size(), 0);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM heartbeat_reports WHERE config_id = ?"));
        q.addBindValue(configId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }

  private:
    QString createFolderConfig(const QString& targetConvId) {
        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-folder-%1").arg(QDateTime::currentMSecsSinceEpoch());
        cfg.agentId = QStringLiteral("agent-folder");
        cfg.scopeType = HeartbeatScopeType::Folder;
        cfg.scopeId = m_folderId;
        cfg.alias = QStringLiteral("FolderMember");
        cfg.enabled = true;
        cfg.schedule = QString();
        cfg.goal = QStringLiteral("monitor folder activity");
        cfg.surfaceCriteria = QStringLiteral("any meaningful change");
        cfg.maxRunsPerDay = 100;
        cfg.autoSurfaceTargetConversationId = targetConvId;
        const QString id = m_configSvc->upsertConfig(cfg);
        if (id.isEmpty())
            qWarning() << "createFolderConfig upsert failed";
        return id;
    }

    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_folderId;
    QString m_targetConvId;
    QString m_otherConvId;

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

QTEST_MAIN(TestHeartbeatFolderScope)
#include "test-heartbeat-folder-scope.moc"

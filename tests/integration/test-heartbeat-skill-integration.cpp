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

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QUuid>

class HsiMockProvider : public ILLMProvider {
    Q_OBJECT
  public:
    explicit HsiMockProvider(QObject* p = nullptr) : ILLMProvider(p) {}
    QString providerId() const override { return "ollama"; }
    QString displayName() const override { return "ollama"; }
    QStringList availableModels() override { return {"ollama-model-a"}; }
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }
    void refreshModels() override {}
    void sendRequest(const LlmRequest& r) override {
        m_lastRequest = r;
        ++m_n;
    }
    void cancelRequest() override {}
    int n() const { return m_n; }
    const LlmRequest& last() const { return m_lastRequest; }

  private:
    int m_n = 0;
    LlmRequest m_lastRequest;
};

class TestHeartbeatSkillIntegration : public QObject {
    Q_OBJECT

  private:
    static void writeFile(const QString& abs, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
    }
    void seedAndApprove(const QString& id) {
        const QString folder = m_srcDir->path() + "/" + id;
        const QString fm =
            QStringLiteral("---\nname: %1\ndescription: skill %1\nversion: 1.0.0\n---\n"
                           "Body.\n")
                .arg(id);
        writeFile(folder + "/SKILL.md", fm.toUtf8());
        QVERIFY(m_skills->importSkillFolder(folder).isEmpty());
        QVERIFY(m_skills->approveSkill(id));
    }

  private slots:
    void init() {
        QVERIFY(m_dbDir.isValid());
        const QString dbPath =
            m_dbDir.path() + QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        m_dbPath = dbPath;
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_appData = std::make_unique<QTemporaryDir>();
        m_srcDir = std::make_unique<QTemporaryDir>();
        m_router = std::make_unique<ModelRouter>();
        m_router->registerProvider(std::make_unique<HsiMockProvider>());
        auto bg = std::make_unique<HsiMockProvider>();
        m_bgPtr = bg.get();
        m_router->registerBackgroundProvider(std::move(bg));
        m_router->setActiveProvider("ollama", "ollama-model-a");

        m_toolSvc = std::make_unique<ToolService>();
        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_skills = std::make_unique<SkillService>(*m_convs);
        m_skills->setAppDataRootForTesting(m_appData->path());
        m_skills->initialize();
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_reqBuilder = std::make_unique<Chat::RequestBuilder>(*m_convs, *m_msgs, *m_router);
        m_configSvc = std::make_unique<HeartbeatConfigService>(DbManager::instance());

        Agent a;
        a.id = "agent-hsi";
        a.name = "HSI-Agent";
        a.systemPrompt = "You are HSI.";
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
        m_convId = m_convs->createConversation("HSI Conv");

        seedAndApprove("twitter-campaign-planner");
        seedAndApprove("github-issue-triage");

        m_svc = std::make_unique<HeartbeatSubagentService>(*m_configSvc,
                                                           *m_agents,
                                                           *m_convs,
                                                           *m_msgs,
                                                           *m_router,
                                                           *m_toolSvc,
                                                           *m_skills,
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
        m_skills.reset();
        m_convs.reset();
        m_agents.reset();
        m_toolSvc.reset();
        m_router.reset();
        m_srcDir.reset();
        m_appData.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase("verzeta_main");
        QFile::remove(m_dbPath);
    }

    HeartbeatConfig makeConfig(HeartbeatScopeType type, const QString& scopeId) {
        HeartbeatConfig cfg;
        cfg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        cfg.agentId = "agent-hsi";
        cfg.scopeType = type;
        cfg.scopeId = scopeId;
        cfg.enabled = true;
        cfg.goal = "test";
        cfg.schedule = "@interval 5";
        cfg.maxRunsPerDay = 24;
        return cfg;
    }

    void test_ConvHeartbeatGetsConvPreferredSkills() {
        QVERIFY(m_skills->setPreferredSkills(
            "conversation_1to1", m_convId, {"twitter-campaign-planner"}));
        const auto cfg = makeConfig(HeartbeatScopeType::Conversation1to1, m_convId);
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());
        m_svc->runNow(cfg.id);
        QTRY_COMPARE_WITH_TIMEOUT(m_bgPtr->n(), 1, 2000);
        const QString sys = m_bgPtr->last().systemPrompt;
        QVERIFY(sys.contains("AVAILABLE SKILLS"));
        QVERIFY(sys.contains("twitter-campaign-planner"));
    }

    void test_FolderHeartbeatGetsFolderPreferredSkills() {
        const QString folderId = m_convs->createFolder("Project");
        QVERIFY(m_skills->setPreferredSkills("folder", folderId, {"github-issue-triage"}));
        const auto cfg = makeConfig(HeartbeatScopeType::Folder, folderId);
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());
        m_svc->runNow(cfg.id);
        QTRY_COMPARE_WITH_TIMEOUT(m_bgPtr->n(), 1, 2000);
        const QString sys = m_bgPtr->last().systemPrompt;
        QVERIFY(sys.contains("AVAILABLE SKILLS"));
        QVERIFY(sys.contains("github-issue-triage"));
    }

    void test_ExposeOnlyHeaderTextDiffersInHeartbeat() {
        QVERIFY(m_skills->setPreferredSkills(
            "conversation_1to1", m_convId, {"twitter-campaign-planner"}));
        QVERIFY(m_skills->setExposeOnlyPreferred("conversation_1to1", m_convId, true));
        const auto cfg = makeConfig(HeartbeatScopeType::Conversation1to1, m_convId);
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());
        m_svc->runNow(cfg.id);
        QTRY_COMPARE_WITH_TIMEOUT(m_bgPtr->n(), 1, 2000);
        const QString sys = m_bgPtr->last().systemPrompt;
        QVERIFY(sys.contains("only skills available"));
        QVERIFY(!sys.contains("discover_skills"));
    }

    void test_NoPreferredSkillsLayerOmitted() {
        const auto cfg = makeConfig(HeartbeatScopeType::Conversation1to1, m_convId);
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());
        m_svc->runNow(cfg.id);
        QTRY_COMPARE_WITH_TIMEOUT(m_bgPtr->n(), 1, 2000);
        QVERIFY(!m_bgPtr->last().systemPrompt.contains("AVAILABLE SKILLS"));
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    QString m_convId;
    HsiMockProvider* m_bgPtr = nullptr;
    std::unique_ptr<QTemporaryDir> m_appData;
    std::unique_ptr<QTemporaryDir> m_srcDir;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<SkillService> m_skills;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<Chat::RequestBuilder> m_reqBuilder;
    std::unique_ptr<HeartbeatConfigService> m_configSvc;
    std::unique_ptr<HeartbeatSubagentService> m_svc;
};

QTEST_MAIN(TestHeartbeatSkillIntegration)
#include "test-heartbeat-skill-integration.moc"

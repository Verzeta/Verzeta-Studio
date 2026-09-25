// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/tool-calling-schema.h"
#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/request-builder.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/skill-service.h"
#include "../../backend/services/tool-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>

class StubProvider : public ILLMProvider {
    Q_OBJECT
  public:
    explicit StubProvider(QObject* parent = nullptr) : ILLMProvider(parent) {}
    QString providerId() const override { return "ollama"; }
    QString displayName() const override { return "ollama"; }
    QStringList availableModels() override { return {"ollama-model-a"}; }
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }
    void refreshModels() override {}
    void sendRequest(const LlmRequest&) override {}
    void cancelRequest() override {}
};

class TestRequestBuilderSkills : public QObject {
    Q_OBJECT

  private:
    static void writeFile(const QString& abs, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
    }
    void seedSkill(const QString& id,
                   const QString& desc,
                   const QStringList& tags = {},
                   const QStringList& declaredTools = {}) {
        const QString folder = m_srcDir->path() + "/" + id;
        QString fm =
            QStringLiteral("---\nname: %1\ndescription: %2\nversion: 1.0.0\n").arg(id, desc);
        if (!tags.isEmpty()) {
            fm += QStringLiteral("tags: [%1]\n").arg(tags.join(", "));
        }
        if (!declaredTools.isEmpty()) {
            fm += QStringLiteral("tools: [%1]\n").arg(declaredTools.join(QStringLiteral(", ")));
        }
        fm += QStringLiteral("---\nBody.\n");
        writeFile(folder + "/SKILL.md", fm.toUtf8());
        QVERIFY(m_skillSvc->importSkillFolder(folder).isEmpty());
        QVERIFY(m_skillSvc->approveSkill(id));
    }

    void registerStubTool(const QString& name) {
        ToolSchema sch;
        sch.name = name;
        sch.description = QStringLiteral("stub for tests");
        ToolHandler h = [](const QJsonObject&) -> QJsonValue { return QJsonObject{}; };
        m_tools->registerTool(sch, h);
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
        m_router->registerProvider(std::make_unique<StubProvider>());
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));
        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_skillSvc = std::make_unique<SkillService>(*m_convs);
        m_skillSvc->setAppDataRootForTesting(m_appData->path());
        m_skillSvc->initialize();
        m_tools = std::make_unique<ToolService>();
        m_rb = std::make_unique<Chat::RequestBuilder>(*m_convs, *m_msgs, *m_router);

        Agent a;
        a.id = "agent-rbs";
        a.name = "RBS-Agent";
        a.systemPrompt = "You are RBS.";
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
        m_convId = m_convs->createConversation("RBS Conv");
        QVERIFY(!m_convId.isEmpty());

        seedSkill("twitter-campaign-planner",
                  "Plan and draft platform-specific campaign posts.",
                  {"marketing", "social"});
        seedSkill(
            "github-issue-triage", "Sort inbound GitHub issues by severity and label.", {"github"});
        seedSkill("notion-database-workflow",
                  "Manage Notion databases via the official API.",
                  {"notion"});
    }

    void cleanup() {
        m_rb.reset();
        m_tools.reset();
        m_skillSvc.reset();
        m_msgs.reset();
        m_convs.reset();
        m_agents.reset();
        m_router.reset();
        m_srcDir.reset();
        m_appData.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    Chat::BuildRequestInputs makeInputs() {
        Chat::BuildRequestInputs in;
        in.agentRegistry = m_agents.get();
        in.skillService = m_skillSvc.get();
        in.activeConvId = m_convId;
        in.inflightConvId = m_convId;
        in.requestId = 1;
        in.responseMemberAgentId = "agent-rbs";
        in.responseMemberAlias = "";
        in.lastUserText = "hello";
        in.toolsEnabled = false;
        return in;
    }

    void test_FullLayerOmittedWhenNoPreferredList() {
        const auto br = m_rb->buildRequest(makeInputs());
        QVERIFY(br.success);
        QVERIFY2(!br.request.systemPrompt.contains("=== AVAILABLE SKILLS ==="),
                 "preferred-list block must be omitted when resolved "
                 "list is empty");
    }

    void test_OneLinerPointerEmittedWhenNoPreferredAndApprovedExist() {
        const auto br = m_rb->buildRequest(makeInputs());
        QVERIFY(br.success);
        const QString sys = br.request.systemPrompt;
        QVERIFY2(sys.contains("=== SKILLS AVAILABLE ==="),
                 "pointer block must be emitted when no preferred list "
                 "but approved skills exist");
        QVERIFY2(sys.contains("discover_skills"),
                 "pointer must direct the agent to discover_skills");
        QVERIFY2(sys.contains("3 approved skill"),
                 qPrintable(QStringLiteral("expected '3 approved skill' "
                                           "in pointer; got: %1")
                                .arg(sys)));
    }

    void test_LayerPresentWithPreferredSkillsExposeOnlyOff() {
        QVERIFY(m_skillSvc->setPreferredSkills(
            "conversation_1to1", m_convId, {"twitter-campaign-planner", "github-issue-triage"}));
        auto in = makeInputs();
        const auto resolved = m_skillSvc->resolveForConversation(m_convId);
        in.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        in.skillsExposeOnly = resolved.exposeOnly;
        const auto br = m_rb->buildRequest(in);
        QVERIFY(br.success);
        const QString sys = br.request.systemPrompt;
        QVERIFY(sys.contains("=== AVAILABLE SKILLS ==="));
        QVERIFY(sys.contains("twitter-campaign-planner"));
        QVERIFY(sys.contains("github-issue-triage"));
        QVERIFY(sys.contains("discover_skills"));
    }

    void test_LayerExposeOnlyHeaderTextDiffers() {
        QVERIFY(m_skillSvc->setPreferredSkills(
            "conversation_1to1", m_convId, {"twitter-campaign-planner"}));
        QVERIFY(m_skillSvc->setExposeOnlyPreferred("conversation_1to1", m_convId, true));
        auto in = makeInputs();
        const auto resolved = m_skillSvc->resolveForConversation(m_convId);
        in.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        in.skillsExposeOnly = resolved.exposeOnly;
        const auto br = m_rb->buildRequest(in);
        QVERIFY(br.success);
        const QString sys = br.request.systemPrompt;
        QVERIFY(sys.contains("AVAILABLE SKILLS"));
        QVERIFY(sys.contains("only skills available"));
        QVERIFY2(!sys.contains("discover_skills"),
                 "exposeOnly=true header must NOT mention discover_skills");
    }

    void test_PreferredOrderingPreserved() {
        QVERIFY(m_skillSvc->setPreferredSkills(
            "conversation_1to1", m_convId, {"github-issue-triage", "twitter-campaign-planner"}));
        auto in = makeInputs();
        const auto resolved = m_skillSvc->resolveForConversation(m_convId);
        in.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        in.skillsExposeOnly = resolved.exposeOnly;
        const auto br = m_rb->buildRequest(in);
        const QString sys = br.request.systemPrompt;
        const int posGithub = sys.indexOf("github-issue-triage");
        const int posTwit = sys.indexOf("twitter-campaign-planner");
        QVERIFY(posGithub > 0);
        QVERIFY(posTwit > 0);
        QVERIFY2(posGithub < posTwit, "preferred ordering must flow into the prompt");
    }


    void test_ReadyTagAppearsForSkillsWithAllToolsAvailable() {
        seedSkill("file-search-skill",
                  "Search for files matching a pattern.",
                  {},
                  {"read_file", "list_files"});
        registerStubTool("read_file");
        registerStubTool("list_files");
        QVERIFY(
            m_skillSvc->setPreferredSkills("conversation_1to1", m_convId, {"file-search-skill"}));
        auto in = makeInputs();
        in.toolService = m_tools.get();
        in.toolsEnabled = true;
        const auto resolved = m_skillSvc->resolveForConversation(m_convId);
        in.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        in.skillsExposeOnly = resolved.exposeOnly;
        const auto br = m_rb->buildRequest(in);
        const QString sys = br.request.systemPrompt;
        QVERIFY(sys.contains("file-search-skill"));
        QVERIFY2(sys.contains("[READY]"),
                 qPrintable(QStringLiteral("expected [READY] tag in prompt; got: %1").arg(sys)));
        QVERIFY(!sys.contains("[BLOCKED:"));
    }

    void test_BlockedTagListsMissingTools() {
        seedSkill("shell-using-skill", "Runs a shell pipeline.", {}, {"run_shell", "read_file"});
        registerStubTool("read_file");
        QVERIFY(
            m_skillSvc->setPreferredSkills("conversation_1to1", m_convId, {"shell-using-skill"}));
        auto in = makeInputs();
        in.toolService = m_tools.get();
        in.toolsEnabled = true;
        const auto resolved = m_skillSvc->resolveForConversation(m_convId);
        in.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        in.skillsExposeOnly = resolved.exposeOnly;
        const auto br = m_rb->buildRequest(in);
        const QString sys = br.request.systemPrompt;
        QVERIFY(sys.contains("shell-using-skill"));
        QVERIFY2(sys.contains("[BLOCKED: missing run_shell]"),
                 qPrintable(QStringLiteral("expected [BLOCKED: missing run_shell] in prompt; "
                                           "got: %1")
                                .arg(sys)));
        QVERIFY(!sys.contains("[READY]"));
    }

    void test_ReadySkillsReorderedBeforeBlocked() {
        seedSkill("blocked-skill", "Needs missing tool.", {}, {"missing_tool"});
        seedSkill("ready-skill", "Needs only available tool.", {}, {"read_file"});
        registerStubTool("read_file");
        QVERIFY(m_skillSvc->setPreferredSkills(
            "conversation_1to1", m_convId, {"blocked-skill", "ready-skill"}));
        auto in = makeInputs();
        in.toolService = m_tools.get();
        in.toolsEnabled = true;
        const auto resolved = m_skillSvc->resolveForConversation(m_convId);
        in.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        in.skillsExposeOnly = resolved.exposeOnly;
        const auto br = m_rb->buildRequest(in);
        const QString sys = br.request.systemPrompt;
        const int posReady = sys.indexOf("ready-skill");
        const int posBlocked = sys.indexOf("blocked-skill");
        QVERIFY(posReady > 0);
        QVERIFY(posBlocked > 0);
        QVERIFY2(posReady < posBlocked,
                 qPrintable(QStringLiteral("ready skill must appear before blocked skill — got "
                                           "ready@%1 blocked@%2")
                                .arg(posReady)
                                .arg(posBlocked)));
    }

    void test_NoAnnotationWhenSkillHasNoDeclaredTools() {
        QVERIFY(m_skillSvc->setPreferredSkills(
            "conversation_1to1", m_convId, {"twitter-campaign-planner"}));
        auto in = makeInputs();
        in.toolService = m_tools.get();
        in.toolsEnabled = true;
        const auto resolved = m_skillSvc->resolveForConversation(m_convId);
        in.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        in.skillsExposeOnly = resolved.exposeOnly;
        const auto br = m_rb->buildRequest(in);
        const QString sys = br.request.systemPrompt;
        QVERIFY(sys.contains("twitter-campaign-planner"));
        QVERIFY2(!sys.contains("[READY]"), "no annotation for skills with no declared_tools");
        QVERIFY(!sys.contains("[BLOCKED:"));
    }

    void test_NoAnnotationWhenToolServiceUnset() {
        seedSkill("declared-tools-skill", "Has declared tools.", {}, {"run_shell"});
        QVERIFY(m_skillSvc->setPreferredSkills(
            "conversation_1to1", m_convId, {"declared-tools-skill"}));
        auto in = makeInputs();
        const auto resolved = m_skillSvc->resolveForConversation(m_convId);
        in.resolvedPreferredSkillIds = resolved.preferredSkillIds;
        in.skillsExposeOnly = resolved.exposeOnly;
        const auto br = m_rb->buildRequest(in);
        const QString sys = br.request.systemPrompt;
        QVERIFY(sys.contains("declared-tools-skill"));
        QVERIFY2(!sys.contains("[READY]"), "no readiness annotation when toolService is null");
        QVERIFY(!sys.contains("[BLOCKED:"));
    }

    void test_PointerNotEmittedWhenNoApprovedSkillsInstalled() {
        QVERIFY(m_skillSvc->blockSkill("twitter-campaign-planner"));
        QVERIFY(m_skillSvc->blockSkill("github-issue-triage"));
        QVERIFY(m_skillSvc->blockSkill("notion-database-workflow"));
        const auto br = m_rb->buildRequest(makeInputs());
        QVERIFY(br.success);
        const QString sys = br.request.systemPrompt;
        QVERIFY2(!sys.contains("SKILLS AVAILABLE"),
                 qPrintable(QStringLiteral("pointer must NOT fire with zero approved skills; "
                                           "got: %1")
                                .arg(sys)));
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<QTemporaryDir> m_appData;
    std::unique_ptr<QTemporaryDir> m_srcDir;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<SkillService> m_skillSvc;
    std::unique_ptr<ToolService> m_tools;
    std::unique_ptr<Chat::RequestBuilder> m_rb;
};

QTEST_MAIN(TestRequestBuilderSkills)
#include "test-request-builder-skills.moc"

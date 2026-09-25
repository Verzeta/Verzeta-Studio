// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/tool-calling-schema.h"
#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/request-builder.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/membership-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/tool-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QSet>
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

class TestRequestBuilderToolScope : public QObject {
    Q_OBJECT

  private:
    void registerScopedTool(const QString& name, ToolScope scope) {
        ToolSchema sch;
        sch.name = name;
        sch.description = QStringLiteral("stub for scope tests");
        sch.scope = scope;
        ToolHandler h = [](const QJsonObject&) -> QJsonValue { return QJsonObject{}; };
        m_tools->registerTool(sch, h);
    }

    QSet<QString> offeredTools(const QString& convId) {
        Chat::BuildRequestInputs in;
        in.agentRegistry = m_agents.get();
        in.membershipService = m_members.get();
        in.toolService = m_tools.get();
        in.activeConvId = convId;
        in.inflightConvId = convId;
        in.requestId = 1;
        in.responseMemberAgentId = "agent-x";
        in.lastUserText = "hello";
        in.toolsEnabled = true;
        const auto br = m_rb->buildRequest(in);
        QSet<QString> names;
        for (const ToolSchema& t : br.request.availableTools) {
            names.insert(t.name);
        }
        return names;
    }

  private slots:
    void init() {
        QVERIFY(m_dbDir.isValid());
        m_dbPath =
            m_dbDir.path() + QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_router = std::make_unique<ModelRouter>();
        m_router->registerProvider(std::make_unique<StubProvider>());
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));
        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_members = std::make_unique<MembershipService>(DbManager::instance());
        m_tools = std::make_unique<ToolService>();
        m_rb = std::make_unique<Chat::RequestBuilder>(*m_convs, *m_msgs, *m_router);

        Agent a;
        a.id = "agent-x";
        a.name = "X-Agent";
        a.systemPrompt = "You are X.";
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());

        registerScopedTool(QStringLiteral("uni_tool"), ToolScope::Universal);
        registerScopedTool(QStringLiteral("grp_tool"), ToolScope::GroupOnly);
        registerScopedTool(QStringLiteral("prj_tool"), ToolScope::ProjectOnly);
    }

    void cleanup() {
        m_rb.reset();
        m_tools.reset();
        m_members.reset();
        m_msgs.reset();
        m_convs.reset();
        m_agents.reset();
        m_router.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_PlainChat_onlyUniversal() {
        const QString conv = m_convs->createConversation("Plain");
        QVERIFY(!conv.isEmpty());
        const auto names = offeredTools(conv);
        QVERIFY2(names.contains("uni_tool"), "Universal tool must be offered everywhere");
        QVERIFY2(!names.contains("grp_tool"),
                 "GroupOnly tool must NOT be offered in a single-agent chat");
        QVERIFY2(!names.contains("prj_tool"),
                 "ProjectOnly tool must NOT be offered outside a project");
    }

    void test_GroupChat_keepsGroupNotProject() {
        const QString conv = m_convs->createGroupConversation("Group", {QStringLiteral("agent-x")});
        QVERIFY(!conv.isEmpty());
        const auto names = offeredTools(conv);
        QVERIFY(names.contains("uni_tool"));
        QVERIFY2(names.contains("grp_tool"),
                 "GroupOnly tool MUST be offered in a group chat (no degradation)");
        QVERIFY2(!names.contains("prj_tool"),
                 "ProjectOnly tool must NOT be offered in a non-project group");
    }

    void test_ProjectChat_keepsProjectNotGroup() {
        const QString folder = m_convs->createFolder("Proj");
        QVERIFY(!folder.isEmpty());
        QVERIFY(m_convs->updateFolderMetadata(
            folder, QStringLiteral("project"), QStringLiteral("goal"), QStringLiteral("desc"), {}));
        const QString conv = m_convs->createConversation("PChat", folder);
        QVERIFY(!conv.isEmpty());
        const auto names = offeredTools(conv);
        QVERIFY(names.contains("uni_tool"));
        QVERIFY2(names.contains("prj_tool"),
                 "ProjectOnly tool MUST be offered in a project chat (no degradation)");
        QVERIFY2(!names.contains("grp_tool"),
                 "GroupOnly tool must NOT be offered in a project 1:1 chat");
    }

    void test_ProjectGroupChat_keepsBoth() {
        const QString folder = m_convs->createFolder("ProjG");
        QVERIFY(!folder.isEmpty());
        QVERIFY(m_convs->updateFolderMetadata(
            folder, QStringLiteral("project"), QStringLiteral("goal"), QStringLiteral("desc"), {}));
        const QString conv =
            m_convs->createGroupConversation("PG", {QStringLiteral("agent-x")}, folder);
        QVERIFY(!conv.isEmpty());
        const auto names = offeredTools(conv);
        QVERIFY(names.contains("uni_tool"));
        QVERIFY2(names.contains("grp_tool"), "Project GROUP chat keeps GroupOnly tools");
        QVERIFY2(names.contains("prj_tool"), "Project group chat keeps ProjectOnly tools");
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<MembershipService> m_members;
    std::unique_ptr<ToolService> m_tools;
    std::unique_ptr<Chat::RequestBuilder> m_rb;
};

QTEST_MAIN(TestRequestBuilderToolScope)
#include "test-request-builder-tool-scope.moc"

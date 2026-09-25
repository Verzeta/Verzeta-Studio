// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/tool-calling-schema.h"
#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/llm-config.h"
#include "../../backend/models/member.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/request-builder.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/membership-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/tool-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QDateTime>
#include <QSqlDatabase>

class IdStubProvider : public ILLMProvider {
    Q_OBJECT
  public:
    explicit IdStubProvider(const QString& id, QObject* parent = nullptr)
        : ILLMProvider(parent), m_id(id) {}

    QString providerId() const override { return m_id; }
    QString displayName() const override { return m_id; }
    QStringList availableModels() override { return {m_id + QStringLiteral("-model")}; }
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }
    void refreshModels() override {}
    void sendRequest(const LlmRequest&) override {}
    void cancelRequest() override {}

  private:
    QString m_id;
};

class TestPerAgentProvider : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MembershipService> m_members;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<ToolService> m_tools;
    std::unique_ptr<Chat::RequestBuilder> m_rb;

    static constexpr auto kAgentB = "agent-b";
    static constexpr auto kAgentA = "agent-a";
    static constexpr auto kAgentGhost = "agent-ghost";
    static constexpr auto kAgentModelOnly = "agent-model-only";
    static constexpr auto kAgentNoOverride = "agent-no-override";
    static constexpr auto kAgentWhitelist = "agent-whitelist";
    static constexpr auto kAgentEmptyWl = "agent-empty-wl";

    void seedAgent(const QString& id,
                   const QString& name,
                   const QString& provider,
                   const QString& model,
                   const QStringList& allowedTools) {
        Agent a;
        a.id = id;
        a.name = name;
        a.systemPrompt = QStringLiteral("You are %1.").arg(name);
        a.modelProvider = provider;
        a.modelName = model;
        a.allowedTools = allowedTools;
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
    }

    void registerStubTool(const QString& name) {
        ToolSchema sch;
        sch.name = name;
        sch.description = QStringLiteral("stub for tests");
        ToolHandler h = [](const QJsonObject&) -> QJsonValue { return QJsonObject{}; };
        m_tools->registerTool(sch, h);
    }

    Chat::BuildRequestInputs inputsFor(const QString& agentId, bool withTools = false) {
        Chat::BuildRequestInputs in;
        in.agentRegistry = m_agents.get();
        in.membershipService = m_members.get();
        in.activeConvId = m_convId;
        in.inflightConvId = m_convId;
        in.requestId = 1;
        in.responseMemberAgentId = agentId;
        in.responseMemberAlias = QString();
        in.lastUserText = QStringLiteral("test message");
        in.toolsEnabled = withTools;
        if (withTools) {
            in.toolService = m_tools.get();
        }
        return in;
    }

  private slots:
    void init() {
        QVERIFY(m_dbDir.isValid());
        m_dbPath =
            m_dbDir.path() + QStringLiteral("/pap_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_router = std::make_unique<ModelRouter>();
        m_router->registerProvider(std::make_unique<IdStubProvider>(QStringLiteral("provider-a")));
        m_router->registerProvider(std::make_unique<IdStubProvider>(QStringLiteral("provider-b")));
        m_router->registerProvider(std::make_unique<IdStubProvider>(QStringLiteral("provider-c")));
        m_router->setActiveProvider(QStringLiteral("provider-a"),
                                    QStringLiteral("router-active-model"));

        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_members = std::make_unique<MembershipService>(DbManager::instance());
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_tools = std::make_unique<ToolService>();
        m_rb = std::make_unique<Chat::RequestBuilder>(*m_convs, *m_msgs, *m_router);

        seedAgent(QString::fromLatin1(kAgentB),
                  QStringLiteral("AgentB"),
                  QStringLiteral("provider-b"),
                  QStringLiteral("model-b1"),
                  {});
        seedAgent(QString::fromLatin1(kAgentA),
                  QStringLiteral("AgentA"),
                  QStringLiteral("provider-a"),
                  QStringLiteral("model-a1"),
                  {});
        seedAgent(QString::fromLatin1(kAgentGhost),
                  QStringLiteral("AgentGhost"),
                  QStringLiteral("ghost-provider"),
                  QStringLiteral("ghost-model"),
                  {});
        seedAgent(QString::fromLatin1(kAgentModelOnly),
                  QStringLiteral("AgentModelOnly"),
                  QString(),
                  QStringLiteral("model-only-x"),
                  {});
        seedAgent(QString::fromLatin1(kAgentNoOverride),
                  QStringLiteral("AgentNoOverride"),
                  QString(),
                  QString(),
                  {});
        seedAgent(QString::fromLatin1(kAgentWhitelist),
                  QStringLiteral("AgentWhitelist"),
                  QString(),
                  QString(),
                  {QStringLiteral("read_file")});
        seedAgent(QString::fromLatin1(kAgentEmptyWl),
                  QStringLiteral("AgentEmptyWl"),
                  QString(),
                  QString(),
                  {});

        m_convId = m_convs->createConversation(QStringLiteral("PAP Conv"));
        QVERIFY(!m_convId.isEmpty());
        LlmConfig convCfg;
        convCfg.providerId = QStringLiteral("provider-a");
        convCfg.modelName = QStringLiteral("conv-model-a");
        convCfg.stream = true;
        convCfg.contextWindow = 16384;
        QVERIFY(m_convs->updateLlmConfig(m_convId, convCfg));

        registerStubTool(QStringLiteral("read_file"));
        registerStubTool(QStringLiteral("write_file"));
        registerStubTool(QStringLiteral("run_shell"));
    }

    void cleanup() {
        m_rb.reset();
        m_tools.reset();
        m_msgs.reset();
        m_members.reset();
        m_convs.reset();
        m_agents.reset();
        m_router.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_agentProviderOverride_winsOverConversationConfig() {
        const auto br = m_rb->buildRequest(inputsFor(QString::fromLatin1(kAgentB)));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-b"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("model-b1"));
    }

    void test_unregisteredAgentProvider_fallsBackToConversationDefault() {
        const auto br = m_rb->buildRequest(inputsFor(QString::fromLatin1(kAgentGhost)));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-a"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("conv-model-a"));
    }

    void test_modelOnlyOverride_keepsConversationProvider() {
        const auto br = m_rb->buildRequest(inputsFor(QString::fromLatin1(kAgentModelOnly)));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-a"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("model-only-x"));
    }

    void test_noAgentOverride_usesConversationConfig() {
        const auto br = m_rb->buildRequest(inputsFor(QString::fromLatin1(kAgentNoOverride)));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-a"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("conv-model-a"));
    }

    void test_allowedToolsWhitelist_intersectsAvailableTools() {
        const auto br = m_rb->buildRequest(inputsFor(QString::fromLatin1(kAgentWhitelist), true));
        QVERIFY2(br.success, qPrintable(br.errorReason));

        QStringList toolNames;
        for (const ToolSchema& t : br.request.availableTools) {
            toolNames << t.name;
        }
        QVERIFY2(toolNames.contains(QStringLiteral("read_file")),
                 qPrintable(QStringLiteral("read_file must survive the whitelist; got: %1")
                                .arg(toolNames.join(QStringLiteral(", ")))));
        QVERIFY2(!toolNames.contains(QStringLiteral("write_file")),
                 "write_file must be filtered out by the whitelist");
        QVERIFY2(!toolNames.contains(QStringLiteral("run_shell")),
                 "run_shell must be filtered out by the whitelist");
        QCOMPARE(br.request.availableTools.size(), 1);
    }

    void test_emptyWhitelist_noToolRestriction() {
        const auto br = m_rb->buildRequest(inputsFor(QString::fromLatin1(kAgentEmptyWl), true));
        QVERIFY2(br.success, qPrintable(br.errorReason));

        QStringList toolNames;
        for (const ToolSchema& t : br.request.availableTools) {
            toolNames << t.name;
        }
        QVERIFY(toolNames.contains(QStringLiteral("read_file")));
        QVERIFY(toolNames.contains(QStringLiteral("write_file")));
        QVERIFY(toolNames.contains(QStringLiteral("run_shell")));
    }

    void test_twoAgentsTwoProviders_inOneCascade() {
        const auto turnA = m_rb->buildRequest(inputsFor(QString::fromLatin1(kAgentA)));
        QVERIFY2(turnA.success, qPrintable(turnA.errorReason));
        QCOMPARE(turnA.request.config.providerId, QStringLiteral("provider-a"));
        QCOMPARE(turnA.request.config.modelName, QStringLiteral("model-a1"));

        const auto turnB = m_rb->buildRequest(inputsFor(QString::fromLatin1(kAgentB)));
        QVERIFY2(turnB.success, qPrintable(turnB.errorReason));
        QCOMPARE(turnB.request.config.providerId, QStringLiteral("provider-b"));
        QCOMPARE(turnB.request.config.modelName, QStringLiteral("model-b1"));

        QVERIFY(turnA.request.config.providerId != turnB.request.config.providerId);
    }


  private:
    QString seedProjectFolder() {
        const QString fid = m_convs->createFolder(QStringLiteral("Proj"));
        m_convs->updateFolderMetadata(
            fid, QStringLiteral("project"), QStringLiteral("goal"), QStringLiteral("desc"), {});
        return fid;
    }

    Chat::BuildRequestInputs memberInputs(const QString& convId,
                                          const QString& agentId,
                                          const QString& alias,
                                          bool withTools = false) {
        Chat::BuildRequestInputs in;
        in.agentRegistry = m_agents.get();
        in.membershipService = m_members.get();
        in.activeConvId = convId;
        in.inflightConvId = convId;
        in.requestId = 1;
        in.responseMemberAgentId = agentId;
        in.responseMemberAlias = alias;
        in.lastUserText = QStringLiteral("test message");
        in.toolsEnabled = withTools;
        if (withTools)
            in.toolService = m_tools.get();
        return in;
    }

  private slots:
    void test_projectGroupChat_memberOverrideWinsOverTemplate() {
        const QString fid = seedProjectFolder();
        const QString gconv = m_convs->createGroupConversation(
            QStringLiteral("Team Chat"), {QString::fromLatin1(kAgentB)}, fid);
        QVERIFY(!gconv.isEmpty());
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("provider-a");
        cfg.modelName = QStringLiteral("conv-model-a");
        cfg.contextWindow = 16384;
        QVERIFY(m_convs->updateLlmConfig(gconv, cfg));
        QVERIFY(m_members->addProjectMember(fid,
                                            QString::fromLatin1(kAgentB),
                                            QStringLiteral("Writer1"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QStringLiteral("provider-c"),
                                            QStringLiteral("model-c1"),
                                            {}));

        const auto br = m_rb->buildRequest(
            memberInputs(gconv, QString::fromLatin1(kAgentB), QStringLiteral("Writer1")));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-c"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("model-c1"));
    }

    void test_projectDirectChat_resolvesViaMemberAlias() {
        const QString fid = seedProjectFolder();
        const QString dconv =
            m_convs->createConversation(QStringLiteral("Chat with @Writer1"), fid);
        QVERIFY(!dconv.isEmpty());
        QVERIFY(m_convs->updatePrimaryAgent(dconv, QString::fromLatin1(kAgentB)));
        QVERIFY(m_convs->setConversationMemberAlias(dconv, QStringLiteral("Writer1")));
        QVERIFY(m_members->addProjectMember(fid,
                                            QString::fromLatin1(kAgentB),
                                            QStringLiteral("Writer1"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QStringLiteral("provider-c"),
                                            QStringLiteral("model-c1"),
                                            {}));

        const auto br =
            m_rb->buildRequest(memberInputs(dconv, QString::fromLatin1(kAgentB), QString()));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-c"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("model-c1"));
    }

    void test_nonProjectGroupChat_resolvesViaConversationMembers() {
        const QString gconv = m_convs->createGroupConversation(
            QStringLiteral("Standalone Group"), {QString::fromLatin1(kAgentB)}, QString());
        QVERIFY(!gconv.isEmpty());
        QVERIFY(m_members->addConversationMember(gconv,
                                                 QString::fromLatin1(kAgentB),
                                                 QStringLiteral("Writer1"),
                                                 false,
                                                 QStringLiteral("user"),
                                                 QString(),
                                                 QStringLiteral("provider-c"),
                                                 QStringLiteral("model-c1"),
                                                 {}));

        const auto br = m_rb->buildRequest(
            memberInputs(gconv, QString::fromLatin1(kAgentB), QStringLiteral("Writer1")));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-c"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("model-c1"));
    }

    void test_memberScoped_unresolvedAlias_selfHealsToConvDefault() {
        const QString fid = seedProjectFolder();
        const QString gconv = m_convs->createGroupConversation(
            QStringLiteral("Team Chat"), {QString::fromLatin1(kAgentB)}, fid);
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("provider-a");
        cfg.modelName = QStringLiteral("conv-model-a");
        cfg.contextWindow = 16384;
        QVERIFY(m_convs->updateLlmConfig(gconv, cfg));

        const auto br = m_rb->buildRequest(
            memberInputs(gconv, QString::fromLatin1(kAgentB), QStringLiteral("NoSuchMember")));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-a"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("conv-model-a"));
    }

    void test_memberScoped_emptyOverride_usesConvDefault() {
        const QString fid = seedProjectFolder();
        const QString gconv = m_convs->createGroupConversation(
            QStringLiteral("Team Chat"), {QString::fromLatin1(kAgentB)}, fid);
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("provider-a");
        cfg.modelName = QStringLiteral("conv-model-a");
        cfg.contextWindow = 16384;
        QVERIFY(m_convs->updateLlmConfig(gconv, cfg));
        QVERIFY(m_members->addProjectMember(
            fid, QString::fromLatin1(kAgentB), QStringLiteral("Writer1")));

        const auto br = m_rb->buildRequest(
            memberInputs(gconv, QString::fromLatin1(kAgentB), QStringLiteral("Writer1")));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QCOMPARE(br.request.config.providerId, QStringLiteral("provider-a"));
        QCOMPARE(br.request.config.modelName, QStringLiteral("conv-model-a"));
    }

    void test_memberScoped_toolWhitelist() {
        const QString fid = seedProjectFolder();
        const QString gconv = m_convs->createGroupConversation(
            QStringLiteral("Team Chat"), {QString::fromLatin1(kAgentB)}, fid);
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("provider-a");
        cfg.modelName = QStringLiteral("conv-model-a");
        cfg.contextWindow = 16384;
        QVERIFY(m_convs->updateLlmConfig(gconv, cfg));
        QVERIFY(m_members->addProjectMember(fid,
                                            QString::fromLatin1(kAgentB),
                                            QStringLiteral("Writer1"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QString(),
                                            QString(),
                                            {QStringLiteral("read_file")}));

        const auto br = m_rb->buildRequest(
            memberInputs(gconv, QString::fromLatin1(kAgentB), QStringLiteral("Writer1"), true));
        QVERIFY2(br.success, qPrintable(br.errorReason));
        QStringList toolNames;
        for (const ToolSchema& t : br.request.availableTools) {
            toolNames << t.name;
        }
        QCOMPARE(br.request.availableTools.size(), 1);
        QVERIFY(toolNames.contains(QStringLiteral("read_file")));
        QVERIFY(!toolNames.contains(QStringLiteral("write_file")));
        QVERIFY(!toolNames.contains(QStringLiteral("run_shell")));
    }
};

QTEST_MAIN(TestPerAgentProvider)
#include "test-per-agent-provider.moc"

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/tool-calling-schema.h"
#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/message.h"
#include "../../backend/models/tool-call.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/request-builder.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/history-budgeter.h"
#include "../../backend/services/membership-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/tool-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QUuid>

namespace {
const QString kBigMarker = QStringLiteral("NEXA_BIG_BODY_MARKER");
const QString kSerpMarker = QStringLiteral("NEXA_SERP_MARKER");
QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
}  // namespace

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

class TestRequestBuilderPayloadFit : public QObject {
    Q_OBJECT

  private:
    void
    seedToolGroup(const QString& toolName, const QJsonObject& args, const QString& resultJson) {
        Message asst;
        asst.id = newId();
        asst.conversationId = m_conv;
        asst.role = QStringLiteral("assistant");
        asst.finishReason = QStringLiteral("tool_calls");
        asst.content = QStringLiteral("(working)");
        const QString asstId = m_msgs->addMessage(asst);
        QVERIFY(!asstId.isEmpty());

        ToolCall tc;
        tc.id = QStringLiteral("call_%1").arg(++m_callSeq);
        tc.messageId = asstId;
        tc.toolName = toolName;
        tc.arguments = args;
        tc.result = QJsonDocument::fromJson(resultJson.toUtf8()).object();
        tc.status = QStringLiteral("success");
        QVERIFY(!m_msgs->addToolCall(tc).isEmpty());

        Message toolRow;
        toolRow.id = newId();
        toolRow.conversationId = m_conv;
        toolRow.role = QStringLiteral("tool");
        toolRow.content = resultJson;
        toolRow.metadata = QJsonObject{{QStringLiteral("tool_call_id"), tc.id}};
        QVERIFY(!m_msgs->addMessage(toolRow).isEmpty());
    }

    void seedUser(const QString& text) {
        Message u;
        u.id = newId();
        u.conversationId = m_conv;
        u.role = QStringLiteral("user");
        u.content = text;
        QVERIFY(!m_msgs->addMessage(u).isEmpty());
    }

    void registerStubTool(const QString& name) {
        ToolSchema s;
        s.name = name;
        s.description = QStringLiteral("stub");
        ToolHandler h = [](const QJsonObject&) -> QJsonValue { return QJsonObject{}; };
        m_tools->registerTool(s, h);
    }

    static QString serializeHistory(const LlmRequest& req) {
        QString out = req.systemPrompt;
        for (const LlmMessage& m : req.messages) {
            out += m.content;
            if (!m.toolCallsJson.isEmpty()) {
                out += QString::fromUtf8(
                    QJsonDocument(m.toolCallsJson).toJson(QJsonDocument::Compact));
            }
        }
        return out;
    }

  private slots:
    void init() {
        QVERIFY(m_dbDir.isValid());
        m_dbPath =
            m_dbDir.path() + QStringLiteral("/t_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

        registerStubTool(QStringLiteral("write_file"));
        registerStubTool(QStringLiteral("search_web"));
        registerStubTool(QStringLiteral("read_file"));

        m_conv = m_convs->createConversation("Nexa Encryptor");
        QVERIFY(!m_conv.isEmpty());

        const QString bigBody = kBigMarker + QStringLiteral("\n") + QString(4000, QLatin1Char('A'));
        seedUser(QStringLiteral("Build the Nexa Encryptor app"));
        seedToolGroup(QStringLiteral("write_file"),
                      QJsonObject{{"filename", "index.html"}, {"content", bigBody}},
                      QStringLiteral("{\"ok\":true,\"path\":\"index.html\"}"));
        seedToolGroup(QStringLiteral("write_file"),
                      QJsonObject{{"filename", "index.html"}, {"content", bigBody}},
                      QStringLiteral("{\"ok\":true,\"path\":\"index.html\"}"));
        const QString serp =
            QStringLiteral("{\"answer\":\"AES-256 strong\",\"results\":[{\"title\":\"T\","
                           "\"url\":\"u\",\"snippet\":\"") +
            kSerpMarker + QString(3000, QLatin1Char('S')) + QStringLiteral("\"}]}");
        seedToolGroup(
            QStringLiteral("search_web"), QJsonObject{{"query", "encryption pricing"}}, serp);
        seedUser(QStringLiteral("continue please"));
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

    void test_bigPayloadsDigested_andRequestLeavesOutputRoom() {
        Chat::BuildRequestInputs in;
        in.agentRegistry = m_agents.get();
        in.membershipService = m_members.get();
        in.toolService = m_tools.get();
        in.activeConvId = m_conv;
        in.inflightConvId = m_conv;
        in.requestId = 1;
        in.lastUserText = QStringLiteral("continue please");
        in.toolsEnabled = true;

        const auto br = m_rb->buildRequest(in);
        QVERIFY2(br.success, qPrintable(br.errorReason));

        const QString hist = serializeHistory(br.request);
        QVERIFY2(!hist.contains(kBigMarker),
                 "verbatim write bodies must be digested out of history");
        QVERIFY2(hist.contains(kSerpMarker), "a fresh in-window search result must stay visible");

        const int floor = 3072;
        const int estPrompt = HistoryBudgeter::estimateTokens(hist);
        QVERIFY2(estPrompt + floor <= br.request.config.contextWindow,
                 qPrintable(QStringLiteral("estPrompt %1 + floor %2 must fit num_ctx %3")
                                .arg(estPrompt)
                                .arg(floor)
                                .arg(br.request.config.contextWindow)));

        QVERIFY(br.contextFillPercent >= 0 && br.contextFillPercent < 100);

        for (const LlmMessage& m : br.request.messages) {
            if (m.role == QStringLiteral("tool")) {
                QVERIFY(!m.toolCallId.isEmpty());
            }
        }
    }

    void test_oneToOne_continuationNudges() {
        const QString conv = m_convs->createConversation("Nudge");
        QVERIFY(!conv.isEmpty());
        Message u;
        u.id = newId();
        u.conversationId = conv;
        u.role = QStringLiteral("user");
        u.content = QStringLiteral("do it");
        QVERIFY(!m_msgs->addMessage(u).isEmpty());
        Message a;
        a.id = newId();
        a.conversationId = conv;
        a.role = QStringLiteral("assistant");
        a.finishReason = QStringLiteral("tool_calls");
        a.content = QStringLiteral("(working)");
        QVERIFY(!m_msgs->addMessage(a).isEmpty());
        ToolCall tc;
        tc.id = QStringLiteral("call_n1");
        tc.messageId = a.id;
        tc.toolName = QStringLiteral("search_web");
        tc.arguments = QJsonObject{{"query", "x"}};
        tc.result = QJsonObject{{"answer", "y"}};
        tc.status = QStringLiteral("success");
        QVERIFY(!m_msgs->addToolCall(tc).isEmpty());
        Message tr;
        tr.id = newId();
        tr.conversationId = conv;
        tr.role = QStringLiteral("tool");
        tr.content = QStringLiteral("{\"answer\":\"y\"}");
        tr.metadata = QJsonObject{{"tool_call_id", tc.id}};
        QVERIFY(!m_msgs->addMessage(tr).isEmpty());

        auto build = [&](int retryAttempt, const QString& reason, bool postTool) {
            Chat::BuildRequestInputs in;
            in.agentRegistry = m_agents.get();
            in.membershipService = m_members.get();
            in.toolService = m_tools.get();
            in.activeConvId = conv;
            in.inflightConvId = conv;
            in.requestId = 1;
            in.lastUserText = QStringLiteral("do it");
            in.toolsEnabled = true;
            in.echoRetryAttempt = retryAttempt;
            in.retryReason = reason;
            in.isPostToolContinuation = postTool;
            return m_rb->buildRequest(in);
        };
        auto hasNudge = [](const Chat::BuildResult& br, const QString& needle) {
            for (const LlmMessage& m : br.request.messages) {
                if (m.role == QStringLiteral("system") && m.content.contains(needle)) {
                    return true;
                }
            }
            return false;
        };

        const auto rRetry = build(1, QStringLiteral("empty"), false);
        QVERIFY(rRetry.success);
        QVERIFY2(hasNudge(rRetry, QStringLiteral("CONTINUE — your previous turn")),
                 "empty-retry must append the firm continuation nudge");

        const auto rPost = build(0, QString(), true);
        QVERIFY(rPost.success);
        QVERIFY2(hasNudge(rPost, QStringLiteral("tool result above is ready")),
                 "post-tool continuation must append the gentle keep-going nudge");

        const auto rPlain = build(0, QString(), false);
        QVERIFY(rPlain.success);
        QVERIFY(!hasNudge(rPlain, QStringLiteral("CONTINUE — your previous turn")));
        QVERIFY(!hasNudge(rPlain, QStringLiteral("tool result above is ready")));
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    QString m_conv;
    int m_callSeq = 0;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<MembershipService> m_members;
    std::unique_ptr<ToolService> m_tools;
    std::unique_ptr<Chat::RequestBuilder> m_rb;
};

QTEST_MAIN(TestRequestBuilderPayloadFit)
#include "test-request-builder-payload-fit.moc"

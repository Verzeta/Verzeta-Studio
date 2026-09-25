// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/member.h"
#include "../../backend/models/message-list-model.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat-controller.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/export-service.h"
#include "../../backend/services/file-service.h"
#include "../../backend/services/membership-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/slash-command-service.h"
#include "../../backend/services/tool-service.h"
#include "../../backend/utils/process-sandbox.h"
#include "helpers/scripted-mock-provider.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestConversationRunCharacterization : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ChatController> m_chat;
    std::unique_ptr<ProcessSandbox> m_sandbox;
    std::unique_ptr<FileService> m_files;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<MembershipService> m_membership;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    ScriptedMockProvider* m_provider = nullptr;
    QString m_convId;

    static ScriptedMockProvider::ScriptStep makeTextStep(const QString& text, int tokens = 0) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {text};
        s.finishReason = QStringLiteral("stop");
        s.tokens = tokens;
        return s;
    }

    static ScriptedMockProvider::ScriptStep makeToolCallStep(const QString& callId,
                                                             const QString& toolName) {
        ScriptedMockProvider::ScriptStep s;
        s.finishReason = QStringLiteral("tool_calls");
        QJsonObject call;
        call.insert(QStringLiteral("id"), callId);
        call.insert(QStringLiteral("name"), toolName);
        call.insert(QStringLiteral("arguments"), QJsonObject{});
        s.toolCallsJson.append(call);
        return s;
    }

    void waitNotGenerating(int deadlineMs = 4000) {
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < deadlineMs) {
            QTest::qWait(10);
        }
    }

    int assistantRowCount() const {
        int n = 0;
        for (const Message& m : m_msgSvc->getMessages(m_convId)) {
            if (m.role == QStringLiteral("assistant"))
                ++n;
        }
        return n;
    }

    int userRowCount() const {
        int n = 0;
        for (const Message& m : m_msgSvc->getMessages(m_convId)) {
            if (m.role == QStringLiteral("user"))
                ++n;
        }
        return n;
    }

  private slots:
    void init() {
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();

        auto provider = std::make_unique<ScriptedMockProvider>();
        m_provider = provider.get();
        m_router->registerProvider(std::move(provider));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_sandbox = std::make_unique<ProcessSandbox>();
        m_files = std::make_unique<FileService>();
        m_toolSvc = std::make_unique<ToolService>();
        m_toolSvc->registerBuiltInTools(*m_sandbox, *m_files);
        m_chat->setToolService(m_toolSvc.get());

        m_membership = std::make_unique<MembershipService>(DbManager::instance());
        m_agentRegistry = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentRegistry->initialize();
        m_chat->setMembershipService(m_membership.get());
        m_chat->setAgentRegistry(m_agentRegistry.get());

        m_convId = m_convSvc->createConversation(QStringLiteral("CharChat"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);
    }

    void cleanup() {
        m_chat.reset();
        m_agentRegistry.reset();
        m_membership.reset();
        m_toolSvc.reset();
        m_files.reset();
        m_sandbox.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_singleTurn_streamAccumulatesAndPersists() {
        m_provider->setScript(
            {{{QStringLiteral("Hello "), QStringLiteral("there!")}, QStringLiteral("stop"), 5}});

        QSignalSpy genSpy(m_chat.get(), &ChatController::isGeneratingChanged);
        QVERIFY(genSpy.isValid());
        QVERIFY(!m_chat->isGenerating());

        m_chat->sendMessage(QStringLiteral("hi"), {});
        waitNotGenerating();

        QVERIFY(!m_chat->isGenerating());
        QVERIFY2(genSpy.count() >= 2, "isGeneratingChanged did not toggle true then false");

        QCOMPARE(userRowCount(), 1);
        QCOMPARE(assistantRowCount(), 1);
        bool foundAccumulated = false;
        for (const Message& m : m_msgSvc->getMessages(m_convId)) {
            if (m.role == QStringLiteral("assistant") &&
                m.content == QStringLiteral("Hello there!")) {
                foundAccumulated = true;
            }
        }
        QVERIFY2(foundAccumulated, "streamed chunks did not accumulate into the final row");

        QCOMPARE(m_msgSvc->streamingMessagesForConversation(m_convId).size(), 0);
        QVERIFY(!m_chat->messages()->hasStreamingMessage());

        QCOMPARE(m_chat->lastAssistantMessage(), QStringLiteral("Hello there!"));
    }

    void test_contextFillPercent_updatesAfterBuild() {
        m_provider->setScript({makeTextStep(QStringLiteral("ok"))});

        QSignalSpy fillSpy(m_chat.get(), &ChatController::contextFillPercentChanged);
        QVERIFY(fillSpy.isValid());

        m_chat->sendMessage(QStringLiteral("measure me"), {});
        waitNotGenerating();

        QVERIFY2(fillSpy.count() >= 1, "contextFillPercent never changed across a real build");
        QVERIFY(m_chat->contextFillPercent() >= 0);
    }

    void test_toolCallTurn_dispatchesThenContinues() {
        ScriptedMockProvider::ScriptStep turn1 =
            makeToolCallStep(QStringLiteral("call-1"), QStringLiteral("search_messages"));
        turn1.chunks = {QStringLiteral("Let me look that up.")};
        m_provider->setScript({turn1, makeTextStep(QStringLiteral("Here is the answer."))});

        m_chat->sendMessage(QStringLiteral("find something"), {});
        waitNotGenerating();

        QVERIFY(!m_chat->isGenerating());
        QCOMPARE(m_provider->stepsConsumed(), 2);

        bool foundContinuation = false;
        for (const Message& m : m_msgSvc->getMessages(m_convId)) {
            if (m.role == QStringLiteral("assistant") &&
                m.content == QStringLiteral("Here is the answer.")) {
                foundContinuation = true;
            }
        }
        QVERIFY2(foundContinuation, "tool-call continuation reply did not persist");
        QCOMPARE(m_msgSvc->streamingMessagesForConversation(m_convId).size(), 0);
    }

    void test_groupCascade_routesToExplicitlyMentionedMembers() {
        const QString coordId =
            m_agentRegistry->getOrCreateBuiltinByName(QStringLiteral("Project Manager"));
        const QString researcherId =
            m_agentRegistry->getOrCreateBuiltinByName(QStringLiteral("Researcher"));
        QVERIFY(!coordId.isEmpty());
        QVERIFY(!researcherId.isEmpty());

        const QString groupId = m_convSvc->createGroupConversation(
            QStringLiteral("Group"), QStringList{coordId, researcherId}, QString{});
        QVERIFY(!groupId.isEmpty());

        QList<Member> members;
        for (const auto& pair :
             QList<QPair<QString, QString>>{{coordId, QStringLiteral("Coord")},
                                            {researcherId, QStringLiteral("Researcher")}}) {
            Member m;
            m.agentId = pair.first;
            m.alias = pair.second;
            m.isCoordinator = (pair.second == QStringLiteral("Coord"));
            m.joinedAt = QDateTime::currentDateTimeUtc();
            members.append(m);
        }
        QVERIFY(m_membership->setConversationMembers(groupId, members));

        m_chat->switchConversation(groupId);

        m_provider->setScript({
            makeTextStep(QStringLiteral("Coord here.")),
            makeTextStep(QStringLiteral("Researcher here.")),
        });

        QSignalSpy turnSpy(m_chat->cascadeInternal(), &Chat::CascadeController::memberTurnStarted);
        QVERIFY(turnSpy.isValid());

        m_chat->sendMessage(QStringLiteral("@all please introduce"), {});

        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < 5000) {
            QTest::qWait(20);
        }

        QVERIFY2(turnSpy.count() >= 1, "group cascade never routed to a next member");
        int groupAsst = 0;
        for (const Message& m : m_msgSvc->getMessages(groupId)) {
            if (m.role == QStringLiteral("assistant"))
                ++groupAsst;
        }
        QVERIFY2(groupAsst >= 2, "group cascade did not produce both members' replies");
    }

    void test_retry_removesLastPairAndResends() {
        m_provider->setScript({
            makeTextStep(QStringLiteral("first answer")),
            makeTextStep(QStringLiteral("second answer")),
        });

        m_chat->sendMessage(QStringLiteral("the question"), {});
        waitNotGenerating();
        QCOMPARE(userRowCount(), 1);
        QCOMPARE(assistantRowCount(), 1);

        m_chat->retryLastMessage();
        waitNotGenerating();

        QCOMPARE(userRowCount(), 1);
        QCOMPARE(assistantRowCount(), 2);
        QCOMPARE(m_chat->lastAssistantMessage(), QStringLiteral("second answer"));
        QCOMPARE(m_provider->stepsConsumed(), 2);
    }

    void test_queuedUserMessage_preemptsWithDistinctFinishReason() {
        m_provider->setScript({
            makeTextStep(QStringLiteral("first reply")),
            makeTextStep(QStringLiteral("steered reply")),
        });

        QSignalSpy cascadeSpy(m_chat->cascadeInternal(), &Chat::CascadeController::cascadeComplete);
        QVERIFY(cascadeSpy.isValid());

        bool sentSecond = false;
        QString queuedSnapshot;
        QObject::connect(m_chat.get(), &ChatController::isGeneratingChanged, m_chat.get(), [&]() {
            if (sentSecond)
                return;
            sentSecond = true;
            m_chat->sendMessage(QStringLiteral("steer me"), {});
            queuedSnapshot = m_chat->queuedUserText();
        });

        m_chat->sendMessage(QStringLiteral("first"), {});

        QElapsedTimer wall;
        wall.start();
        while (cascadeSpy.count() < 2 && wall.elapsed() < 5000) {
            QTest::qWait(20);
        }

        QCOMPARE(queuedSnapshot, QStringLiteral("steer me"));

        bool sawPreempt = false;
        for (const QList<QVariant>& args : cascadeSpy) {
            if (args.at(1).toString() == QStringLiteral("user_message_pending")) {
                sawPreempt = true;
                break;
            }
        }
        QVERIFY2(sawPreempt, "cascade did not force-finalize with user_message_pending");
        QCOMPARE(m_chat->queuedUserText(), QString());
    }

    void test_compactSlash_shortCircuitsNoProviderTurn() {
        SlashCommandService slash;
        slash.setConversationService(m_convSvc.get());
        slash.setMessageService(m_msgSvc.get());
        slash.setMessageListModel(m_chat->messages());
        slash.setActiveConversationIdGetter([this]() { return m_chat->activeConversationId(); });
        m_chat->setSlashCommandService(&slash);

        m_provider->setScript({makeTextStep(QStringLiteral("seeded"))});
        m_chat->sendMessage(QStringLiteral("seed"), {});
        waitNotGenerating();
        const int stepsBefore = m_provider->stepsConsumed();

        const int userBefore = userRowCount();
        m_chat->sendMessage(QStringLiteral("/compact"), {});
        QTest::qWait(50);

        QCOMPARE(m_provider->stepsConsumed(), stepsBefore);
        QCOMPARE(userRowCount(), userBefore);
        QVERIFY(!m_chat->isGenerating());
        bool sawReceipt = false;
        for (const Message& m : m_msgSvc->ephemeralMessagesForConversation(m_convId)) {
            if (m.content.contains(QStringLiteral("compact"), Qt::CaseInsensitive)) {
                sawReceipt = true;
            }
        }
        QVERIFY2(sawReceipt,
                 "/compact did not route through the slash "
                 "service to post an ephemeral receipt");
    }

    void test_flashmemorySlash_wipesHistoryNoProviderTurn() {
        SlashCommandService slash;
        slash.setConversationService(m_convSvc.get());
        slash.setMessageService(m_msgSvc.get());
        slash.setMessageListModel(m_chat->messages());
        slash.setActiveConversationIdGetter([this]() { return m_chat->activeConversationId(); });
        m_chat->setSlashCommandService(&slash);

        m_provider->setScript({
            makeTextStep(QStringLiteral("a")),
            makeTextStep(QStringLiteral("b")),
        });
        m_chat->sendMessage(QStringLiteral("one"), {});
        waitNotGenerating();
        m_chat->sendMessage(QStringLiteral("two"), {});
        waitNotGenerating();
        QVERIFY(m_msgSvc->getMessages(m_convId).size() >= 4);
        const int stepsBefore = m_provider->stepsConsumed();

        m_chat->sendMessage(QStringLiteral("/flashmemory confirm"), {});
        QTest::qWait(50);

        QCOMPARE(m_provider->stepsConsumed(), stepsBefore);
        QVERIFY(!m_chat->isGenerating());

        QCOMPARE(userRowCount(), 0);
        QCOMPARE(assistantRowCount(), 0);
    }
};

QTEST_MAIN(TestConversationRunCharacterization)
#include "test-conversation-run-characterization.moc"

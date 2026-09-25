// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "helpers/scripted-ragp-backend.h"
#include "models/agent.h"
#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/message-list-model.h"
#include "services/agent-registry.h"
#include "services/chat-controller.h"
#include "services/chat/action-intent-confirmer.h"
#include "services/chat/cascade-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/membership-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/ragp/ragp-service.h"

#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestChatControllerEmptyFinish : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentRegistry> m_agentReg;
    std::unique_ptr<MembershipService> m_memberSvc;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provider = nullptr;
    QString m_convId;

    QString makeGroupConv() {
        const QList<Agent> agents = m_agentReg->allAgents();
        if (agents.size() < 2)
            return {};
        const QString gid = m_convSvc->createGroupConversation(QStringLiteral("GroupChat"),
                                                               {agents[0].id, agents[1].id});
        if (gid.isEmpty())
            return {};
        if (!m_memberSvc->addConversationMember(gid, agents[0].id, QStringLiteral("Alice"), true))
            return {};
        if (!m_memberSvc->addConversationMember(gid, agents[1].id, QStringLiteral("Bob"), false))
            return {};
        m_chat->switchConversation(gid);
        return gid;
    }

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    void scheduleScript(QList<ScriptedMockProvider::ScriptStep> script) {
        m_provider->setScript(std::move(script));
    }

    void sendAndWait(const QString& text) {
        QSignalSpy genSpy(m_chat.get(), &ChatController::isGeneratingChanged);
        m_chat->sendMessage(text, {});
        const int deadline = 2000;
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < deadline) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(), "ChatController never finished generating within 2s");
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();

        auto provider = std::make_unique<ScriptedMockProvider>();
        m_provider = provider.get();
        m_router->registerProvider(std::move(provider));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_agentReg = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentReg->initialize();
        m_memberSvc = std::make_unique<MembershipService>(DbManager::instance());

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_chat->setMembershipService(m_memberSvc.get());

        m_convId = m_convSvc->createConversation(QStringLiteral("TestChat"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);
    }

    void cleanup() {
        m_chat.reset();
        m_memberSvc.reset();
        m_agentReg.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_normalNonEmptyStop_persistsAssistantRow() {
        scheduleScript({{{QStringLiteral("hello world")}, QStringLiteral("stop"), 3}});
        sendAndWait(QStringLiteral("hi"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int userCount = 0, asstCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("user"))
                ++userCount;
            if (m.role == QStringLiteral("assistant"))
                ++asstCount;
        }
        QCOMPARE(userCount, 1);
        QCOMPARE(asstCount, 1);
        const auto streamSlots = m_msgSvc->streamingMessagesForConversation(m_convId);
        QCOMPARE(streamSlots.size(), 0);
    }

    void test_emptyContentStop_abortsPlaceholder_noBlankRow() {
        scheduleScript({{{}, QStringLiteral("stop"), 0}});
        sendAndWait(QStringLiteral("say something maybe"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int asstCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant"))
                ++asstCount;
        }
        QCOMPARE(asstCount, 0);

        const auto streamSlots = m_msgSvc->streamingMessagesForConversation(m_convId);
        QCOMPARE(streamSlots.size(), 0);
        QVERIFY(!m_chat->messages()->hasStreamingMessage());
    }

    void test_1to1_emptyThenNonEmpty_retryRecovers() {
        scheduleScript({{{}, QStringLiteral("stop"), 0},
                        {{QStringLiteral("Recovered real reply")}, QStringLiteral("stop"), 3}});
        sendAndWait(QStringLiteral("build it"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int asstCount = 0;
        QString asstContent;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                ++asstCount;
                asstContent = m.content;
            }
        }
        QCOMPARE(asstCount, 1);
        QVERIFY2(asstContent.contains(QStringLiteral("Recovered real reply")),
                 "the auto-retry must land the real reply — no user nudge needed");
        QVERIFY2(m_provider->stepsConsumed() == 2,
                 "both steps consumed → a retry WAS dispatched after the empty stop");
        QVERIFY(!m_chat->isGenerating());
        QCOMPARE(m_msgSvc->streamingMessagesForConversation(m_convId).size(), 0);
    }

    void test_1to1_allEmpty_boundedRetriesNoHang() {
        scheduleScript({{{}, QStringLiteral("stop"), 0},
                        {{}, QStringLiteral("stop"), 0},
                        {{}, QStringLiteral("stop"), 0},
                        {{}, QStringLiteral("stop"), 0}});
        sendAndWait(QStringLiteral("go"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int asstCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant"))
                ++asstCount;
        }
        QCOMPARE(asstCount, 0);
        QVERIFY2(!m_chat->isGenerating(), "must not hang after the bounded retries are exhausted");
        QVERIFY2(m_provider->stepsConsumed() == 4,
                 "initial + 3 bounded retries = 4 empty dispatches, then exhaust");
        QCOMPARE(m_msgSvc->streamingMessagesForConversation(m_convId).size(), 0);
    }

    void test_emptyContentLength_abortsPlaceholder_noBlankRow() {
        scheduleScript({{{}, QStringLiteral("length"), 0}});
        sendAndWait(QStringLiteral("x"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int asstCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant"))
                ++asstCount;
        }
        QCOMPARE(asstCount, 0);
        const auto streamSlots = m_msgSvc->streamingMessagesForConversation(m_convId);
        QCOMPARE(streamSlots.size(), 0);
    }

    void test_emptyContentError_abortsPlaceholder_noBlankRow() {
        ScriptedMockProvider::ScriptStep step;
        step.finishReason = QStringLiteral("error");
        step.isError = true;
        step.errorMessage = QStringLiteral("simulated");
        scheduleScript({step});

        sendAndWait(QStringLiteral("x"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int asstCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant"))
                ++asstCount;
        }
        QCOMPARE(asstCount, 0);
        QVERIFY(!m_chat->messages()->hasStreamingMessage());
    }

    void test_taskFlow_turn1NonEmpty_turn2Empty_noBlankBubble() {
        ScriptedMockProvider::ScriptStep turn1;
        turn1.chunks = {QStringLiteral("Here is the information you "
                                       "asked about:")};
        turn1.finishReason = QStringLiteral("stop");
        turn1.tokens = 8;
        scheduleScript({turn1});
        sendAndWait(QStringLiteral("please write a file"));

        {
            const QList<Message> rows = m_msgSvc->getMessages(m_convId);
            int asstCount = 0;
            for (const Message& m : rows) {
                if (m.role == QStringLiteral("assistant"))
                    ++asstCount;
            }
            QCOMPARE(asstCount, 1);
        }

        scheduleScript({{{}, QStringLiteral("stop"), 0}});
        sendAndWait(QStringLiteral("go ahead"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int userCount = 0, asstCount = 0, emptyAsstCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("user"))
                ++userCount;
            if (m.role == QStringLiteral("assistant")) {
                ++asstCount;
                if (m.content.trimmed().isEmpty())
                    ++emptyAsstCount;
            }
        }
        QCOMPARE(userCount, 2);
        QCOMPARE(asstCount, 1);
        QCOMPARE(emptyAsstCount, 0);

        const auto streamSlots = m_msgSvc->streamingMessagesForConversation(m_convId);
        QCOMPARE(streamSlots.size(), 0);
        QVERIFY(!m_chat->messages()->hasStreamingMessage());
    }

    void test_negativeControl_nonEmptyStopStillPersists() {
        scheduleScript({{{QStringLiteral("only "), QStringLiteral("one "), QStringLiteral("word")},
                         QStringLiteral("stop"),
                         3}});
        sendAndWait(QStringLiteral("hi"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        bool found = false;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant") &&
                m.content == QStringLiteral("only one word")) {
                found = true;
            }
        }
        QVERIFY(found);
    }

    void test_1to1_claimWithoutFilename_acceptedNotDeleted() {
        scheduleScript(
            {{{QStringLiteral("I'll write a file for you now.")}, QStringLiteral("stop"), 6}});
        sendAndWait(QStringLiteral("please write the report"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int asstCount = 0;
        QString asstContent;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                ++asstCount;
                asstContent = m.content;
            }
        }
        QVERIFY2(m_provider->stepsConsumed() == 1,
                 "a 1:1 claim must NOT trigger a destructive delete-retry");
        QCOMPARE(asstCount, 1);
        QVERIFY2(asstContent == QStringLiteral("I'll write a file for you now."),
                 "the reply is kept verbatim, not deleted");
        QVERIFY(!m_chat->isGenerating());
    }

    void test_1to1_deferredAction_notRetriedWithoutLlmConfirm() {
        scheduleScript({{{QStringLiteral("I will first create a basic styles.css to "
                                         "make the website appealing.")},
                         QStringLiteral("stop"),
                         9}});
        sendAndWait(QStringLiteral("build the website"));

        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int asstCount = 0;
        bool transientLeft = false;
        QString asstContent;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                ++asstCount;
                asstContent = m.content;
            }
            if (m.content.contains(QStringLiteral("Checking whether to continue"))) {
                transientLeft = true;
            }
        }
        QVERIFY2(m_provider->stepsConsumed() == 1,
                 "without an LLM confirmation a deferred-action must NOT retry");
        QCOMPARE(asstCount, 1);
        QVERIFY2(asstContent.contains(QStringLiteral("styles.css")),
                 "the announced reply is accepted, not deleted");
        QVERIFY2(!transientLeft, "the transient 'checking' row must be removed when done");
        QVERIFY(!m_chat->isGenerating());
    }

    void test_1to1_thinkingMisroute_promotedNotRetried() {
        ScriptedMockProvider::ScriptStep step;
        step.thinkingChunks = {QStringLiteral("Here is the full launch blueprint with all "
                                              "three sections the user asked for.")};
        step.finishReason = QStringLiteral("stop");
        step.tokens = 9;
        scheduleScript({step});
        sendAndWait(QStringLiteral("give me the blueprint"));

        QCOMPARE(m_provider->stepsConsumed(), 1);
        const QList<Message> rows = m_msgSvc->getMessages(m_convId);
        int asstCount = 0;
        bool retryNote = false;
        QString asstContent, asstThinking;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                ++asstCount;
                asstContent = m.content;
                asstThinking = m.thinkingContent;
            }
            if (m.content.contains(QStringLiteral("empty response"))) {
                retryNote = true;
            }
        }
        QCOMPARE(asstCount, 1);
        QVERIFY2(asstContent.contains(QStringLiteral("launch blueprint")),
                 "the misrouted reply must be promoted to visible content");
        QVERIFY2(asstThinking.isEmpty(),
                 "the sidecar is cleared on promotion (no duplicate render)");
        QVERIFY2(!retryNote, "no empty-retry note for a recovered reply");
        QVERIFY(!m_chat->isGenerating());
    }


    void test_group_deferredConfirmed_keepsReplyAndContinuesSameAlias() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto* conf = m_chat->intentConfirmerInternal();
        QVERIFY2(conf, "group run must expose the intent confirmer");
        int deciderCalls = 0;
        conf->setTestDecider([&deciderCalls](const QString&) {
            ++deciderCalls;
            return Chat::ActionIntentConfirmer::Result::Confirmed;
        });

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.9;
        scripted->response.selfPendingAction = true;
        scripted->response.pendingActionHint = QStringLiteral("create pricing.md");
        auto* scriptedRaw = scripted.get();
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        scheduleScript(
            {{{QStringLiteral("I'll create pricing.md right away.")}, QStringLiteral("stop"), 9},
             {{QStringLiteral("All done here.")}, QStringLiteral("stop"), 9}});
        sendAndWait(QStringLiteral("Alice, please produce the pricing doc"));

        QVERIFY2(scriptedRaw->callCount >= 1,
                 "the structural gate must force the classify to Tier 3 "
                 "even though the reply has no @-mention");
        QCOMPARE(deciderCalls, 0);
        QVERIFY2(m_provider->stepsConsumed() == 2,
                 "a self-pending classify verdict must dispatch exactly "
                 "one continuation turn");

        const QList<Message> rows = m_msgSvc->getMessages(gid);
        int asstCount = 0;
        bool announceKept = false, noteNamesAlias = false;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                ++asstCount;
                if (m.content.contains(QStringLiteral("pricing.md"))) {
                    announceKept = true;
                }
            }
            if (m.content.contains(QStringLiteral("@Alice described a "
                                                  "file/tool action"))) {
                noteNamesAlias = true;
            }
        }
        QCOMPARE(asstCount, 2);
        QVERIFY2(announceKept, "the announced reply must be KEPT, never deleted");
        QVERIFY2(noteNamesAlias, "the visible continuation note must name the alias");
        QVERIFY(!m_chat->isGenerating());
    }

    void test_group_continuationRequest_endsWithUserFollowThrough() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.9;
        scripted->response.selfPendingAction = true;
        scripted->response.pendingActionHint = QStringLiteral("write specs");
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        scheduleScript({{{QStringLiteral("I am calling write_file to output "
                                         "wireframe_specs.md now.")},
                         QStringLiteral("stop"),
                         9},
                        {{QStringLiteral("Done.")}, QStringLiteral("stop"), 9}});
        sendAndWait(QStringLiteral("Alice, produce the specs"));

        QCOMPARE(m_provider->stepsConsumed(), 2);
        const auto& reqs = m_provider->capturedRequests();
        QVERIFY(reqs.size() >= 2);
        const LlmRequest& cont = reqs.last();
        QVERIFY(!cont.messages.isEmpty());
        const LlmMessage& last = cont.messages.last();
        QCOMPARE(last.role, QStringLiteral("user"));
        QVERIFY2(last.content.contains(QStringLiteral("Automated follow-through")),
                 qPrintable(last.content.left(120)));
        QVERIFY2(last.content.contains(QStringLiteral("@Alice")),
                 "the group follow-through must address the alias");
    }

    void test_group_imageCompletion_resumesRequestingAgent() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());
        const QList<Agent> agents = m_agentReg->allAgents();
        QVERIFY(agents.size() >= 1);

        QTemporaryFile png(QStringLiteral("imgXXXXXX.png"));
        QVERIFY(png.open());
        png.write(QByteArrayLiteral("fake-png-bytes"));
        png.close();

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.9;
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        scheduleScript({{{QStringLiteral("Generating the hero image now — it "
                                         "will arrive shortly.")},
                         QStringLiteral("stop"),
                         9},
                        {{QStringLiteral("Reviewed — the hero image looks "
                                         "great, moving on.")},
                         QStringLiteral("stop"),
                         9},
                        {{QStringLiteral("Second follow-up reply.")}, QStringLiteral("stop"), 9},
                        {{QStringLiteral("Third follow-up reply.")}, QStringLiteral("stop"), 9}});
        sendAndWait(QStringLiteral("Alice, create the hero image"));
        QCOMPARE(m_provider->stepsConsumed(), 1);

        m_chat->onImageReadyForFollowUp(gid, QStringLiteral("Alice"), agents[0].id, png.fileName());
        QTRY_VERIFY_WITH_TIMEOUT(!m_chat->isGenerating(), 2000);
        QCOMPARE(m_provider->stepsConsumed(), 2);

        const auto& reqs = m_provider->capturedRequests();
        QVERIFY(reqs.size() >= 2);
        const LlmMessage& last = reqs.last().messages.last();
        QCOMPARE(last.role, QStringLiteral("user"));
        QVERIFY2(last.content.contains(QStringLiteral("finished generating")),
                 qPrintable(last.content.left(120)));
        QVERIFY2(!last.images.isEmpty(),
                 "the generated image must ride the follow-through "
                 "so the agent sees its own output");

        m_chat->onImageReadyForFollowUp(gid, QStringLiteral("Alice"), agents[0].id, png.fileName());
        QTRY_VERIFY_WITH_TIMEOUT(!m_chat->isGenerating(), 2000);
        QCOMPARE(m_provider->stepsConsumed(), 3);
        m_chat->onImageReadyForFollowUp(gid, QStringLiteral("Alice"), agents[0].id, png.fileName());
        QTest::qWait(50);
        QCOMPARE(m_provider->stepsConsumed(), 3);
        QVERIFY(!m_chat->isGenerating());
    }

    void test_group_danglingPayloadColon_getsFollowThrough() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.9;
        scripted->response.selfPendingAction = true;
        scripted->response.pendingActionHint = QStringLiteral("deliver the announced copy draft");
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        scheduleScript({{{QStringLiteral("Tone locked — relentlessly positive. "
                                         "Here is the initial copy draft:")},
                         QStringLiteral("stop"),
                         9},
                        {{QStringLiteral("Draft: Ice cream magic that grows "
                                         "with you. Done.")},
                         QStringLiteral("stop"),
                         9}});
        sendAndWait(QStringLiteral("Alice, write the landing page copy"));

        QVERIFY2(m_provider->stepsConsumed() == 2,
                 "a reply ending at a payload boundary (trailing colon) "
                 "must reach the LLM decider and continue");
        const auto& reqs = m_provider->capturedRequests();
        QVERIFY(reqs.size() >= 2);
        const LlmMessage& last = reqs.last().messages.last();
        QCOMPARE(last.role, QStringLiteral("user"));
        QVERIFY2(last.content.contains(QStringLiteral("Automated follow-through")),
                 qPrintable(last.content.left(120)));
        QVERIFY(!m_chat->isGenerating());
    }

    void test_group_selfPending_withUndispatchableTarget_stillNudges() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.95;
        scripted->response.selfPendingAction = true;
        scripted->response.pendingActionHint = QStringLiteral("write wireframe specs file");
        Ragp::Target t;
        t.alias = QStringLiteral("Bob");
        t.intent = Ragp::Intent::REFERENCE;
        scripted->response.targets.append(t);
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        scheduleScript({{{QStringLiteral("I am calling write_file to output "
                                         "wireframe_specs.md now.")},
                         QStringLiteral("stop"),
                         9},
                        {{QStringLiteral("Done — specs written.")}, QStringLiteral("stop"), 9}});
        sendAndWait(QStringLiteral("Alice, produce the specs file"));

        QVERIFY2(m_provider->stepsConsumed() == 2,
                 "an undispatchable classified target must not veto "
                 "the self-pending continuation");
        const QList<Message> rows = m_msgSvc->getMessages(gid);
        bool noteNamesAlias = false;
        for (const Message& m : rows) {
            if (m.content.contains(QStringLiteral("@Alice described a "
                                                  "file/tool action"))) {
                noteNamesAlias = true;
            }
        }
        QVERIFY2(noteNamesAlias,
                 "the visible continuation note must fire when nothing "
                 "was dispatched");
        QVERIFY(!m_chat->isGenerating());
    }

    void test_group_deferredRejected_acceptedWithoutContinuation() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto* conf = m_chat->intentConfirmerInternal();
        QVERIFY(conf);
        int deciderCalls = 0;
        conf->setTestDecider([&deciderCalls](const QString&) {
            ++deciderCalls;
            return Chat::ActionIntentConfirmer::Result::Rejected;
        });

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.9;
        scripted->response.selfPendingAction = false;
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        scheduleScript(
            {{{QStringLiteral("I'll create pricing.md right away.")}, QStringLiteral("stop"), 9}});
        sendAndWait(QStringLiteral("Alice, please produce the pricing doc"));

        QCOMPARE(deciderCalls, 0);
        QCOMPARE(m_provider->stepsConsumed(), 1);
        const QList<Message> rows = m_msgSvc->getMessages(gid);
        int asstCount = 0;
        bool noteLeft = false;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant"))
                ++asstCount;
            if (m.content.contains(QStringLiteral("described a file/tool "
                                                  "action"))) {
                noteLeft = true;
            }
        }
        QCOMPARE(asstCount, 1);
        QVERIFY2(!noteLeft, "a none-verdict must not leave a nudge note");
        QVERIFY(!m_chat->isGenerating());
    }

    void test_group_delegateVerdict_dispatchesWhenIntentGateDropsAll() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.95;
        scripted->response.selfPendingAction = false;
        scripted->response.pendingDelegateAlias = QStringLiteral("Bob");
        scripted->response.pendingActionHint = QStringLiteral("write wireframe_specs.md");
        Ragp::Target t;
        t.alias = QStringLiteral("Bob");
        t.intent = Ragp::Intent::ACKNOWLEDGMENT;
        scripted->response.targets.append(t);
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        scheduleScript(
            {{{QStringLiteral("Bob, thank you for the summary! Please "
                              "go ahead and write wireframe_specs.md "
                              "now.")},
              QStringLiteral("stop"),
              9},
             {{QStringLiteral("On it — writing the specs.")}, QStringLiteral("stop"), 9}});
        sendAndWait(QStringLiteral("Alice, coordinate the specs work"));

        QVERIFY2(m_provider->stepsConsumed() == 2,
                 "the delegate verdict must dispatch the named member "
                 "when the intent gate dropped every classified target");
        const auto& reqs = m_provider->capturedRequests();
        QVERIFY(reqs.size() >= 2);
        for (const LlmMessage& msg : reqs.last().messages) {
            QVERIFY2(!msg.content.contains(QStringLiteral("Automated follow-through")),
                     "a delegate dispatch must not carry the "
                     "self-continuation nudge");
        }
        const QList<Message> rows = m_msgSvc->getMessages(gid);
        int asstCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant"))
                ++asstCount;
        }
        QCOMPARE(asstCount, 2);
        QVERIFY(!m_chat->isGenerating());
    }

    void test_group_selfPendingPlusRoutedTarget_speakerRequeued() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.95;
        scripted->response.selfPendingAction = true;
        scripted->response.pendingActionHint = QStringLiteral("write Marketing_Email_Sequence.md");
        Ragp::Target t;
        t.alias = QStringLiteral("Bob");
        t.intent = Ragp::Intent::DELEGATE_TASK;
        scripted->response.targets.append(t);
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        scheduleScript(
            {{{QStringLiteral("I'll write the copy into "
                              "Marketing_Email_Sequence.md. @Bob, when "
                              "this is done, mock up the flow. Creating "
                              "the file now.")},
              QStringLiteral("stop"),
              9},
             {{QStringLiteral("Understood, standing by for the file.")}, QStringLiteral("stop"), 9},
             {{QStringLiteral("Done — the four emails are written.")}, QStringLiteral("stop"), 9},
             {{QStringLiteral("spare step")}, QStringLiteral("stop"), 9}});
        sendAndWait(QStringLiteral("Alice, produce the email sequence"));

        QVERIFY2(m_provider->stepsConsumed() >= 3,
                 "the speaker's self-pending work must get a turn after "
                 "the routed target — not be dropped on the floor");
        const QList<Message> rows = m_msgSvc->getMessages(gid);
        int aliceCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant") && m.memberAlias == QStringLiteral("Alice")) {
                ++aliceCount;
            }
        }
        QVERIFY2(aliceCount >= 2, "Alice must speak again after Bob (the re-queued turn)");
        bool settleNoted = false;
        for (const Message& m : rows) {
            if (m.content.contains(QStringLiteral("Round settled"))) {
                settleNoted = true;
            }
        }
        QVERIFY2(settleNoted,
                 "a multi-turn autonomous round must end with the "
                 "visible round-settled note");
        QVERIFY(!m_chat->isGenerating());
    }

    void test_group_allTargetsIntentSkipped_emitsSuppression() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        scripted->response.confidence = 0.95;
        Ragp::Target t;
        t.alias = QStringLiteral("Bob");
        t.intent = Ragp::Intent::ACKNOWLEDGMENT;
        scripted->response.targets.append(t);
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();

        QSignalSpy suppressSpy(m_chat->cascadeInternal(),
                               &Chat::CascadeController::mentionRoutingSuppressed);

        scheduleScript({{{QStringLiteral("Thanks @Bob, great catch — nothing else "
                                         "needed from me.")},
                         QStringLiteral("stop"),
                         9}});
        sendAndWait(QStringLiteral("Alice, anything left?"));

        QCOMPARE(m_provider->stepsConsumed(), 1);
        QVERIFY2(suppressSpy.count() >= 1,
                 "classified-but-undispatched targets must surface via "
                 "mentionRoutingSuppressed, never end silently");
        const QList<QVariant> args = suppressSpy.first();
        QCOMPARE(args.at(2).toStringList(), QStringList{QStringLiteral("Bob")});
        QVERIFY(!m_chat->isGenerating());
    }

    void test_group_delegation_neverReachesConfirmer() {
        const QString gid = makeGroupConv();
        QVERIFY(!gid.isEmpty());

        auto* conf = m_chat->intentConfirmerInternal();
        QVERIFY(conf);
        int deciderCalls = 0;
        conf->setTestDecider([&deciderCalls](const QString&) {
            ++deciderCalls;
            return Chat::ActionIntentConfirmer::Result::Confirmed;
        });

        scheduleScript({{{QStringLiteral("@Bob, please create pricing.md and share "
                                         "it here.")},
                         QStringLiteral("stop"),
                         9}});
        sendAndWait(QStringLiteral("Alice, get the pricing organised"));

        QCOMPARE(deciderCalls, 0);
        QCOMPARE(m_provider->stepsConsumed(), 1);
        const QList<Message> rows = m_msgSvc->getMessages(gid);
        int asstCount = 0;
        bool nudgeLeft = false;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant"))
                ++asstCount;
            if (m.content.contains(QStringLiteral("described a file/tool "
                                                  "action"))) {
                nudgeLeft = true;
            }
        }
        QCOMPARE(asstCount, 1);
        QVERIFY2(!nudgeLeft, "a delegation must not trigger a continuation nudge");
        QVERIFY(!m_chat->isGenerating());
    }
};

QTEST_MAIN(TestChatControllerEmptyFinish)
#include "test-chat-controller-empty-finish.moc"

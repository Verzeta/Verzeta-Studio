// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "helpers/scripted-ragp-backend.h"
#include "models/agent.h"
#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/member.h"
#include "models/message-list-model.h"
#include "models/message.h"
#include "services/agent-registry.h"
#include "services/chat-controller.h"
#include "services/conversation-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/membership-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/ragp/ragp-service.h"
#include "services/ragp/ragp-types.h"
#include "utils/notification-manager.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

class TestCascadeController : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    std::unique_ptr<MembershipService> m_membership;
    std::unique_ptr<ConversationController> m_convController;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provider = nullptr;
    ScriptedRagpBackend* m_scripted = nullptr;
    QString m_groupConvId;
    QString m_agentAId;
    QString m_agentBId;
    QString m_agentCId;

    void sendAndWait(const QString& text, int timeoutMs = 20000) {
        m_chat->sendMessage(text, {});
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < timeoutMs) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(),
                 qPrintable(QStringLiteral("ChatController never finished generating within %1ms")
                                .arg(timeoutMs)));
    }

    QList<Message> currentMessages() const {
        return m_msgSvc->getRecentMessages(m_groupConvId, 200);
    }

    QString createAgent(const QString& name) {
        Agent a;
        a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        a.name = name;
        a.description = QStringLiteral("Test agent %1").arg(name);
        a.systemPrompt = QStringLiteral("You are %1.").arg(name);
        a.defaultPattern = QStringLiteral("direct");
        a.createdAt = QDateTime::currentDateTimeUtc();
        const QString id = m_agentRegistry->createAgent(a);
        Q_ASSERT(!id.isEmpty());
        return id;
    }

    QString createGroup(const QList<QPair<QString, QString>>& idAliasPairs) {
        QVariantList members;
        for (int i = 0; i < idAliasPairs.size(); ++i) {
            QVariantMap m;
            m[QStringLiteral("agentId")] = idAliasPairs[i].first;
            m[QStringLiteral("alias")] = idAliasPairs[i].second;
            m[QStringLiteral("isCoordinator")] = (i == 0);
            members.append(m);
        }
        const QString id =
            m_convController->newGroupConversation(QStringLiteral("TestCascade"), members);
        Q_ASSERT(!id.isEmpty());
        m_chat->switchConversation(id);
        return id;
    }

    static ScriptedMockProvider::ScriptStep textStep(const QString& text) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {text};
        s.finishReason = QStringLiteral("stop");
        s.tokens = 1;
        return s;
    }

  private slots:

    void initTestCase() {
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, []() {
            NotificationManager::instance().shutdown();
        });
    }

    void init() {
        const QString dbPath =
            m_tempDir.path() + QStringLiteral("/cc_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();
        m_agentRegistry = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentRegistry->initialize();
        m_membership = std::make_unique<MembershipService>(DbManager::instance());

        auto provider = std::make_unique<ScriptedMockProvider>();
        m_provider = provider.get();
        m_router->registerProvider(std::move(provider));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_convController = std::make_unique<ConversationController>(*m_convSvc, *m_router);
        m_convController->setMembershipService(m_membership.get());

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_chat->setAgentRegistry(m_agentRegistry.get());
        m_chat->setMembershipService(m_membership.get());
        QObject::connect(m_convController.get(),
                         &ConversationController::conversationAboutToBeDeleted,
                         m_chat.get(),
                         &ChatController::onExternalConversationAboutToBeDeleted);
        QObject::connect(m_convController.get(),
                         &ConversationController::conversationDeleted,
                         m_chat.get(),
                         &ChatController::onExternalConversationDeleted);

        m_agentAId = createAgent(QStringLiteral("Alice"));
        m_agentBId = createAgent(QStringLiteral("Bob"));
        m_agentCId = createAgent(QStringLiteral("Carol"));

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        m_scripted = scripted.get();
        Ragp::Classification c;
        c.confidence = 0.95;
        c.source = QStringLiteral("test-scripted");
        for (const QString& alias :
             {QStringLiteral("Alice"), QStringLiteral("Bob"), QStringLiteral("Carol")}) {
            Ragp::Target t;
            t.alias = alias;
            t.intent = Ragp::Intent::DELEGATE_RESPONSE;
            c.targets.append(t);
        }
        m_scripted->response = c;
        m_chat->ragpService()->setBackend(std::move(scripted));
        m_chat->ragpService()->clearCache();
    }

    void cleanup() {
        m_chat.reset();
        m_convController.reset();
        m_membership.reset();
        m_agentRegistry.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void testCascade_basicChain_twoAgentsRespondInOrder() {
        m_groupConvId = createGroup({
            {m_agentAId, QStringLiteral("Alice")},
            {m_agentBId, QStringLiteral("Bob")},
        });
        m_chat->switchConversation(m_groupConvId);

        m_provider->setScript({
            textStep(QStringLiteral("@Bob take over")),
            textStep(QStringLiteral("thanks, done")),
        });

        sendAndWait(QStringLiteral("@Alice please start"));

        QCOMPARE(m_provider->stepsConsumed(), 2);

        const QList<Message> rows = currentMessages();
        QStringList assistantAliases;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                assistantAliases << m.memberAlias;
            }
        }
        QCOMPARE(assistantAliases.size(), 2);
        QCOMPARE(assistantAliases[0], QStringLiteral("Alice"));
        QCOMPARE(assistantAliases[1], QStringLiteral("Bob"));
    }

    void testCascade_unsureBackend_rulesStillRouteExplicitMention() {
        m_groupConvId = createGroup({
            {m_agentAId, QStringLiteral("Alice")},
            {m_agentBId, QStringLiteral("Bob")},
        });
        m_chat->switchConversation(m_groupConvId);

        auto junkBackend = std::make_unique<ScriptedRagpBackend>();
        Ragp::Classification junk;
        junk.confidence = 0.0;
        for (const QString& a : {QStringLiteral("Alice"), QStringLiteral("Bob")}) {
            Ragp::Target t;
            t.alias = a;
            t.intent = Ragp::Intent::DELEGATE_RESPONSE;
            junk.targets.append(t);
        }
        junkBackend->response = junk;
        m_chat->ragpService()->setBackend(std::move(junkBackend));
        m_chat->ragpService()->clearCache();

        m_provider->setScript({
            textStep(QStringLiteral("@Bob could you please take the "
                                    "lead on the first step?")),
            textStep(QStringLiteral("On it — starting now.")),
        });

        sendAndWait(QStringLiteral("@Alice please kick us off"));

        QCOMPARE(m_provider->stepsConsumed(), 2);
        const QList<Message> rows = currentMessages();
        QStringList assistantAliases;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                assistantAliases << m.memberAlias;
            }
        }
        QCOMPARE(assistantAliases.size(), 2);
        QCOMPARE(assistantAliases[0], QStringLiteral("Alice"));
        QCOMPARE(assistantAliases[1], QStringLiteral("Bob"));
    }


    void testCascade_perMemberCap_halts() {
        m_groupConvId = createGroup({
            {m_agentAId, QStringLiteral("Alice")},
            {m_agentBId, QStringLiteral("Bob")},
        });
        m_chat->switchConversation(m_groupConvId);

        m_provider->setScript({
            textStep(QStringLiteral("@Bob 1")),
            textStep(QStringLiteral("@Alice 1")),
            textStep(QStringLiteral("@Bob 2")),
            textStep(QStringLiteral("@Alice 2")),
            textStep(QStringLiteral("@Bob 3")),
            textStep(QStringLiteral("@Alice 3")),
        });

        sendAndWait(QStringLiteral("@Alice begin"));

        QCOMPARE(m_provider->stepsConsumed(), 6);

        int aliceCount = 0, bobCount = 0;
        for (const Message& m : currentMessages()) {
            if (m.role != QStringLiteral("assistant"))
                continue;
            if (m.memberAlias == QStringLiteral("Alice"))
                ++aliceCount;
            else if (m.memberAlias == QStringLiteral("Bob"))
                ++bobCount;
        }
        QCOMPARE(aliceCount, 3);
        QCOMPARE(bobCount, 3);
    }


    void testCascade_userMentionEmitsSignal() {
        m_groupConvId = createGroup({
            {m_agentAId, QStringLiteral("Alice")},
            {m_agentBId, QStringLiteral("Bob")},
        });
        m_chat->switchConversation(m_groupConvId);

        QSignalSpy spy(m_chat.get(), &ChatController::userMentionedInGroup);

        m_provider->setScript({
            textStep(QStringLiteral("@owner please confirm")),
        });

        sendAndWait(QStringLiteral("@Alice heads up"));

        QCOMPARE(m_provider->stepsConsumed(), 1);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), m_groupConvId);
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("Alice"));
        QVERIFY(spy.at(0).at(2).toString().contains(QStringLiteral("@owner please confirm")));

        int assistantCount = 0;
        for (const Message& m : currentMessages()) {
            if (m.role == QStringLiteral("assistant"))
                ++assistantCount;
        }
        QCOMPARE(assistantCount, 1);
    }


    void testCascade_multiMentionInUserMessage_queuesRest() {
        m_groupConvId = createGroup({
            {m_agentAId, QStringLiteral("Alice")},
            {m_agentBId, QStringLiteral("Bob")},
        });
        m_chat->switchConversation(m_groupConvId);

        m_provider->setScript({
            textStep(QStringLiteral("sure, noted")),
            textStep(QStringLiteral("agreed, noted")),
        });

        sendAndWait(QStringLiteral("Hi @Alice and @Bob please jump in"));

        QCOMPARE(m_provider->stepsConsumed(), 2);

        QStringList assistantAliases;
        for (const Message& m : currentMessages()) {
            if (m.role == QStringLiteral("assistant")) {
                assistantAliases << m.memberAlias;
            }
        }
        QCOMPARE(assistantAliases.size(), 2);
        QCOMPARE(assistantAliases[0], QStringLiteral("Alice"));
        QCOMPARE(assistantAliases[1], QStringLiteral("Bob"));
    }
};

QTEST_MAIN(TestCascadeController)
#include "test-cascade-controller.moc"

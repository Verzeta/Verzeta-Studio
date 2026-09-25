// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "helpers/scripted-mock-provider.h"
#include "helpers/scripted-ragp-backend.h"
#include "models/agent.h"
#include "models/db-manager.h"
#include "models/member.h"
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
#include "services/task-gate-service.h"
#include "utils/notification-manager.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

class TestCascadeDefaultBias : public QObject {
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
    std::unique_ptr<TaskGateService> m_taskGateSvc;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provider = nullptr;
    ScriptedRagpBackend* m_scripted = nullptr;
    QString m_groupConvId;
    QString m_aliceId;
    QString m_bobId;
    QString m_writerId;

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

    static ScriptedMockProvider::ScriptStep textStep(const QString& text) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {text};
        s.finishReason = QStringLiteral("stop");
        s.tokens = 1;
        return s;
    }

    int countAssistantRowsByAlias(const QString& alias) const {
        const auto msgs = m_msgSvc->getRecentMessages(m_groupConvId, 200);
        int n = 0;
        for (const auto& m : msgs) {
            if (m.role == QStringLiteral("assistant") &&
                m.memberAlias.compare(alias, Qt::CaseInsensitive) == 0) {
                ++n;
            }
        }
        return n;
    }

    void scriptRagp(Ragp::Intent intent, const QString& targetAlias, double confidence) {
        Ragp::Classification c;
        c.confidence = confidence;
        c.source = QStringLiteral("test-scripted");
        Ragp::Target t;
        t.alias = targetAlias;
        t.intent = intent;
        c.targets.append(t);
        m_scripted->response = c;
        m_chat->ragpService()->clearCache();
    }

    void scriptRagpRouteAll(double confidence) {
        Ragp::Classification c;
        c.confidence = confidence;
        c.source = QStringLiteral("test-scripted");
        for (const QString& alias : {QStringLiteral("Alice"), QStringLiteral("Bob")}) {
            Ragp::Target t;
            t.alias = alias;
            t.intent = Ragp::Intent::DELEGATE_RESPONSE;
            c.targets.append(t);
        }
        m_scripted->response = c;
        m_chat->ragpService()->clearCache();
    }

  private slots:

    void initTestCase() {
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, []() {
            NotificationManager::instance().shutdown();
        });
    }

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/cdb_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

        m_taskGateSvc = std::make_unique<TaskGateService>(*m_msgSvc);
        m_chat->setTaskGateService(m_taskGateSvc.get());

        QObject::connect(m_convController.get(),
                         &ConversationController::conversationAboutToBeDeleted,
                         m_chat.get(),
                         &ChatController::onExternalConversationAboutToBeDeleted);
        QObject::connect(m_convController.get(),
                         &ConversationController::conversationDeleted,
                         m_chat.get(),
                         &ChatController::onExternalConversationDeleted);

        m_aliceId = createAgent(QStringLiteral("Alice"));
        m_bobId = createAgent(QStringLiteral("Bob"));
        m_writerId = createAgent(QStringLiteral("TestWriter"));

        QVariantList members;
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_aliceId;
            m[QStringLiteral("alias")] = QStringLiteral("Alice");
            m[QStringLiteral("isCoordinator")] = true;
            members.append(m);
        }
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_bobId;
            m[QStringLiteral("alias")] = QStringLiteral("Bob");
            m[QStringLiteral("isCoordinator")] = false;
            members.append(m);
        }
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_writerId;
            m[QStringLiteral("alias")] = QStringLiteral("Writer");
            m[QStringLiteral("isCoordinator")] = false;
            members.append(m);
        }
        m_groupConvId = m_convController->newGroupConversation(
            QStringLiteral("CascadeBias"), members, QString());
        QVERIFY(!m_groupConvId.isEmpty());
        m_chat->switchConversation(m_groupConvId);

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        m_scripted = scripted.get();
        Ragp::Service* svc = m_chat->ragpService();
        QVERIFY(svc != nullptr);
        svc->setBackend(std::move(scripted));
        svc->clearCache();
    }

    void cleanup() {
        m_chat.reset();
        m_taskGateSvc.reset();
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

    void test_unknownIntentHighConfidence_suppressesDispatch() {
        m_provider->setScript({
            textStep(QStringLiteral("Hi @Bob, just thinking out loud.")),
            textStep(QStringLiteral("Bob here, what's up?")),
        });
        scriptRagp(Ragp::Intent::UNKNOWN, QStringLiteral("Bob"), 0.9);

        sendAndWait(QStringLiteral("kick off"));

        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Alice")), 1);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Bob")), 0);
    }

    void test_routingIntentLowConfidence_rulesStillRouteExplicitMention() {
        m_provider->setScript({
            textStep(QStringLiteral("Hi @Bob, sure thing.")),
            textStep(QStringLiteral("Bob here.")),
        });
        scriptRagp(Ragp::Intent::DELEGATE_RESPONSE, QStringLiteral("Bob"), 0.05);

        sendAndWait(QStringLiteral("hello"));

        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Alice")), 1);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Bob")), 1);
    }

    void test_routingIntentHighConfidence_dispatches() {
        m_provider->setScript({
            textStep(QStringLiteral("@Bob, you take it from here.")),
            textStep(QStringLiteral("Got it, doing it now.")),
        });
        scriptRagp(Ragp::Intent::DELEGATE_RESPONSE, QStringLiteral("Bob"), 0.95);

        sendAndWait(QStringLiteral("kick off"));

        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Alice")), 1);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Bob")), 1);
    }

    void test_acknowledgmentIntent_suppressesDispatch() {
        m_provider->setScript({
            textStep(QStringLiteral("Thanks @Bob for the great work earlier!")),
            textStep(QStringLiteral("Bob here, you're welcome!")),
        });
        scriptRagp(Ragp::Intent::ACKNOWLEDGMENT, QStringLiteral("Bob"), 0.95);

        sendAndWait(QStringLiteral("hello"));

        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Alice")), 1);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Bob")), 0);
    }

    void test_noMentionInContent_skipsClassifierEntirely() {
        m_provider->setScript({
            textStep(QStringLiteral("Hello team! How is everyone?")),
        });
        scriptRagp(Ragp::Intent::DELEGATE_RESPONSE, QStringLiteral("Bob"), 0.95);

        sendAndWait(QStringLiteral("kick off"));

        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Alice")), 1);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Bob")), 0);
        QCOMPARE(m_scripted->callCount, 0);
    }

    void test_saturation_endsCascadeOnContentLoop() {
        m_provider->setScript({
            textStep(QStringLiteral("@Bob the deployment plan looks reasonable and "
                                    "thorough overall.")),
            textStep(QStringLiteral("@Alice the deployment plan looks reasonable and "
                                    "thorough overall.")),
            textStep(QStringLiteral("@Alice acknowledged, my analysis points to a "
                                    "completely separate technical conclusion.")),
        });
        scriptRagpRouteAll(0.95);

        sendAndWait(QStringLiteral("kick off"));

        QCOMPARE(m_provider->stepsConsumed(), 3);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Alice")), 1);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Bob")), 1);
    }

    void test_deferredQueue_firesAfterImmediateCascadeSettles() {
        m_provider->setScript({
            textStep(QStringLiteral("@Bob apple banana cherry vintage compass.")),
            textStep(QStringLiteral("@Alice mountain forest river telescope mineral.")),
            textStep(QStringLiteral("@Bob volcano lightning ocean penguin obsidian.")),
            textStep(QStringLiteral("@Alice glacier savanna meteor saxophone binoculars.")),
            textStep(QStringLiteral("@Bob hammock ferret xylophone aurora cinder.")),
            textStep(QStringLiteral("@Alice marigold zebra parchment quasar lighthouse.")),
            textStep(QStringLiteral("Writer here, summarising the discussion above.")),
        });

        Ragp::Classification c;
        c.confidence = 0.95;
        c.source = QStringLiteral("test-scripted");
        for (const QString& alias : {QStringLiteral("Alice"), QStringLiteral("Bob")}) {
            Ragp::Target t;
            t.alias = alias;
            t.intent = Ragp::Intent::DELEGATE_RESPONSE;
            c.targets.append(t);
        }
        m_scripted->response = c;
        m_chat->ragpService()->clearCache();

        sendAndWait(QStringLiteral("@Alice and @Writer collaborate"));

        QCOMPARE(m_provider->stepsConsumed(), 7);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Alice")), 3);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Bob")), 3);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Writer")), 1);

        const auto rows = m_msgSvc->getRecentMessages(m_groupConvId, 200);
        QString lastAssistantAlias;
        for (const auto& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                lastAssistantAlias = m.memberAlias;
            }
        }
        QCOMPARE(lastAssistantAlias, QStringLiteral("Writer"));
    }

    void test_dissimilar_doesNotTriggerSaturation() {
        m_provider->setScript({
            textStep(QStringLiteral("@Bob apple banana cherry deployment vintage compass.")),
            textStep(QStringLiteral("@Alice mountain forest river telescope mineral satellite.")),
            textStep(QStringLiteral("@Bob volcano lightning ocean penguin obsidian comet.")),
            textStep(QStringLiteral("@Alice glacier savanna meteor saxophone binoculars cactus.")),
            textStep(QStringLiteral("@Bob hammock ferret xylophone aurora cinder blanket.")),
            textStep(QStringLiteral("@Alice marigold zebra parchment quasar lighthouse jasmine.")),
        });
        scriptRagpRouteAll(0.95);

        sendAndWait(QStringLiteral("kick off"));

        QCOMPARE(m_provider->stepsConsumed(), 6);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Alice")), 3);
        QCOMPARE(countAssistantRowsByAlias(QStringLiteral("Bob")), 3);
    }
};

QTEST_MAIN(TestCascadeDefaultBias)
#include "test-cascade-default-bias.moc"

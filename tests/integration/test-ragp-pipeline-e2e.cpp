// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "helpers/scripted-ragp-backend.h"
#include "models/agent.h"
#include "models/db-manager.h"
#include "services/agent-registry.h"
#include "services/chat-controller.h"
#include "services/conversation-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/membership-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/ragp/iragp-backend.h"
#include "services/ragp/ragp-service.h"
#include "services/ragp/ragp-types.h"
#include "services/task-gate-service.h"
#include "utils/notification-manager.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFuture>
#include <QPromise>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

class TestRagpPipelineE2E : public QObject {
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
    QString m_agentAId;
    QString m_agentBId;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    QString createAgent(const QString& name) {
        Agent a;
        a.id = uuid();
        a.name = name;
        a.description = QStringLiteral("Test agent %1").arg(name);
        a.systemPrompt = QStringLiteral("You are %1.").arg(name);
        a.defaultPattern = QStringLiteral("direct");
        a.createdAt = QDateTime::currentDateTimeUtc();
        const QString id = m_agentRegistry->createAgent(a);
        Q_ASSERT(!id.isEmpty());
        return id;
    }

    void sendAndWait(const QString& text, int timeoutMs = 20000) {
        m_chat->sendMessage(text, {});
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < timeoutMs) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(), "ChatController did not finish within timeout");
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
            m_tempDir.path() +
            QStringLiteral("/rpe_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

        m_agentAId = createAgent(QStringLiteral("Alice"));
        m_agentBId = createAgent(QStringLiteral("Bob"));

        QVariantList members;
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_agentAId;
            m[QStringLiteral("alias")] = QStringLiteral("Alice");
            m[QStringLiteral("isCoordinator")] = true;
            members.append(m);
        }
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_agentBId;
            m[QStringLiteral("alias")] = QStringLiteral("Bob");
            m[QStringLiteral("isCoordinator")] = false;
            members.append(m);
        }
        m_groupConvId =
            m_convController->newGroupConversation(QStringLiteral("RagpE2E"), members, QString());
        QVERIFY(!m_groupConvId.isEmpty());
        m_chat->switchConversation(m_groupConvId);

        auto scripted = std::make_unique<ScriptedRagpBackend>();
        m_scripted = scripted.get();
        m_scripted->response.source = QStringLiteral("test-scripted");
        m_scripted->response.confidence = 1.0;
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

    void test_tier3_calledOnce_forAmbiguousMention() {
        m_provider->setScript({
            textStep(QStringLiteral("Hey @Bob, quick question")),
        });
        QCOMPARE(m_scripted->callCount, 0);

        sendAndWait(QStringLiteral("kick it off"));

        QCOMPARE(m_scripted->callCount, 1);
    }

    void test_tier2_cacheHit_onIdenticalContent() {
        m_provider->setScript({
            textStep(QStringLiteral("Hey @Bob, quick question")),
            textStep(QStringLiteral("Hey @Bob, quick question")),
        });
        sendAndWait(QStringLiteral("first"));
        QCOMPARE(m_scripted->callCount, 1);

        sendAndWait(QStringLiteral("second"));
        QCOMPARE(m_scripted->callCount, 1);
    }
};

QTEST_MAIN(TestRagpPipelineE2E)
#include "test-ragp-pipeline-e2e.moc"

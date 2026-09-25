// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "models/db-manager.h"
#include "models/llm-config.h"
#include "services/agent-settings-controller.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/message-service.h"
#include "services/model-router.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSqlDatabase>

class TestRequestBuilderRouting : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentSettingsController> m_agentSettings;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provX = nullptr;
    ScriptedMockProvider* m_provY = nullptr;

    void sendAndWait(const QString& text) {
        m_chat->sendMessage(text, {});
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < 3000) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(), "ChatController never finished generating within 3s");
    }

    static ScriptedMockProvider::ScriptStep okStep() {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {QStringLiteral("ok")};
        s.finishReason = QStringLiteral("stop");
        s.tokens = 1;
        return s;
    }

    void setConvProviderModel(const QString& convId,
                              const QString& providerId,
                              const QString& modelName) {
        const auto conv = m_convSvc->getConversation(convId);
        QVERIFY(conv.has_value());
        LlmConfig cfg = LlmConfig::fromJson(conv->llmConfig);
        cfg.providerId = providerId;
        cfg.modelName = modelName;
        cfg.stream = true;
        m_convSvc->updateLlmConfig(convId, cfg);
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/rbr_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();

        auto px = std::make_unique<ScriptedMockProvider>(QStringLiteral("provider-x"),
                                                         QStringList{QStringLiteral("model-x")});
        m_provX = px.get();
        m_router->registerProvider(std::move(px));

        auto py = std::make_unique<ScriptedMockProvider>(QStringLiteral("provider-y"),
                                                         QStringList{QStringLiteral("model-y")});
        m_provY = py.get();
        m_router->registerProvider(std::move(py));

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_agentSettings = std::make_unique<AgentSettingsController>(*m_convSvc, *m_router);
        QObject::connect(
            m_chat.get(),
            &ChatController::activeConversationChanged,
            m_agentSettings.get(),
            [this]() { m_agentSettings->setActiveConversationId(m_chat->activeConversationId()); });
    }

    void cleanup() {
        m_chat.reset();
        m_agentSettings.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void testRoutesToConvProvider_notActiveView() {
        const QString convA = m_convSvc->createConversation(QStringLiteral("A"));
        const QString convB = m_convSvc->createConversation(QStringLiteral("B"));
        QVERIFY(!convA.isEmpty());
        QVERIFY(!convB.isEmpty());
        setConvProviderModel(convA, QStringLiteral("provider-y"), QStringLiteral("model-y"));
        setConvProviderModel(convB, QStringLiteral("provider-x"), QStringLiteral("model-x"));

        m_chat->switchConversation(convA);
        QCOMPARE(m_router->activeProviderId(), QStringLiteral("provider-y"));

        m_chat->switchConversation(convB);
        m_provX->setScript({okStep()});
        sendAndWait(QStringLiteral("hello-from-B"));

        QCOMPARE(m_provX->capturedRequests().size(), 1);
        QCOMPARE(m_provY->capturedRequests().size(), 0);
        const LlmRequest& req = m_provX->capturedRequests().first();
        QCOMPARE(req.conversationId, convB);
        QCOMPARE(req.config.providerId, QStringLiteral("provider-x"));
        QCOMPARE(req.config.modelName, QStringLiteral("model-x"));
    }

    void testActiveSwitchDoesNotRetargetOtherConv() {
        const QString convA = m_convSvc->createConversation(QStringLiteral("A"));
        const QString convB = m_convSvc->createConversation(QStringLiteral("B"));
        setConvProviderModel(convA, QStringLiteral("provider-y"), QStringLiteral("model-y"));
        setConvProviderModel(convB, QStringLiteral("provider-x"), QStringLiteral("model-x"));

        m_chat->switchConversation(convA);
        m_chat->switchConversation(convB);
        QCOMPARE(m_router->activeProviderId(), QStringLiteral("provider-x"));

        m_chat->switchConversation(convA);
        m_provY->setScript({okStep()});
        sendAndWait(QStringLiteral("hello-from-A"));

        QCOMPARE(m_provY->capturedRequests().size(), 1);
        QCOMPARE(m_provX->capturedRequests().size(), 0);
        const LlmRequest& req = m_provY->capturedRequests().first();
        QCOMPARE(req.conversationId, convA);
        QCOMPARE(req.config.providerId, QStringLiteral("provider-y"));
        QCOMPARE(req.config.modelName, QStringLiteral("model-y"));
    }

    void testNewConversationUsesAppDefault() {
        m_router->setActiveProvider(QStringLiteral("provider-x"), QStringLiteral("model-x"));

        m_provX->setScript({okStep()});
        sendAndWait(QStringLiteral("first-ever-message"));

        QCOMPARE(m_provX->capturedRequests().size(), 1);
        const LlmRequest& req = m_provX->capturedRequests().first();
        QCOMPARE(req.config.providerId, QStringLiteral("provider-x"));
        QCOMPARE(req.config.modelName, QStringLiteral("model-x"));
        QCOMPARE(m_provY->capturedRequests().size(), 0);
    }

    void testSingleChatStillDispatchesToConvProvider() {
        const QString conv = m_convSvc->createConversation(QStringLiteral("solo"));
        setConvProviderModel(conv, QStringLiteral("provider-x"), QStringLiteral("model-x"));
        m_chat->switchConversation(conv);

        m_provX->setScript({okStep()});
        sendAndWait(QStringLiteral("ping"));

        QCOMPARE(m_provX->capturedRequests().size(), 1);
        const LlmRequest& req = m_provX->capturedRequests().first();
        QVERIFY2(!req.config.providerId.isEmpty(),
                 "req.config.providerId must always be stamped — never "
                 "empty for an existing conversation");
        QCOMPARE(req.config.providerId, QStringLiteral("provider-x"));
    }
};

QTEST_MAIN(TestRequestBuilderRouting)
#include "test-request-builder-routing.moc"

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/message-list-model.h"
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
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>
#include <QVariantMap>

class TestRequestBuilder : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentSettingsController> m_agentSettings;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provider = nullptr;
    QString m_convId;

    void sendAndWait(const QString& text) {
        m_chat->sendMessage(text, {});
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < 3000) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(), "ChatController never finished generating within 3s");
    }

    static ScriptedMockProvider::ScriptStep
    defaultStep(const QString& reply = QStringLiteral("ok")) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {reply};
        s.finishReason = QStringLiteral("stop");
        s.tokens = 1;
        return s;
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() + QStringLiteral("/rb_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_agentSettings = std::make_unique<AgentSettingsController>(*m_convSvc, *m_router);
        QObject::connect(
            m_chat.get(),
            &ChatController::activeConversationChanged,
            m_agentSettings.get(),
            [this]() { m_agentSettings->setActiveConversationId(m_chat->activeConversationId()); });

        m_convId = m_convSvc->createConversation(QStringLiteral("TestRB"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);
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


    void testBuildRequest_basicShapeAfterFirstSend() {
        m_provider->setScript({defaultStep()});
        sendAndWait(QStringLiteral("hello"));

        QCOMPARE(m_provider->capturedRequests().size(), 1);
        const LlmRequest& req = m_provider->capturedRequests().first();

        QVERIFY2(
            req.requestId > 0,
            qPrintable(QStringLiteral("request id must be non-zero, got %1").arg(req.requestId)));
        QCOMPARE(req.conversationId, m_convId);

        QVERIFY2(!req.messages.isEmpty(), "messages list must not be empty for a sent turn");
        const LlmMessage& last = req.messages.last();
        QCOMPARE(last.role, QStringLiteral("user"));
        QCOMPARE(last.content, QStringLiteral("hello"));

        QCOMPARE(req.config.modelName, QStringLiteral("mock-model"));

        QVERIFY2(req.availableTools.isEmpty(),
                 "availableTools must be empty without a ToolService");
    }


    void testBuildRequest_systemPromptIncludesConversationOverride() {
        const QString kOverride = QStringLiteral("<PHASE-1-TEST-MARKER-SYS-PROMPT>");

        QVariantMap cfg;
        cfg[QStringLiteral("systemPrompt")] = kOverride;
        m_agentSettings->saveConversationConfig(cfg);

        m_provider->setScript({defaultStep()});
        sendAndWait(QStringLiteral("ping"));

        QCOMPARE(m_provider->capturedRequests().size(), 1);
        const LlmRequest& req = m_provider->capturedRequests().first();
        QVERIFY2(req.systemPrompt.contains(kOverride),
                 qPrintable(QStringLiteral("system prompt must include conversation override. "
                                           "Got first 200 chars:\n%1")
                                .arg(req.systemPrompt.left(200))));
    }


    void testBuildRequest_historyPreservedAcrossTurns() {
        m_provider->setScript({
            defaultStep(QStringLiteral("first-reply")),
            defaultStep(QStringLiteral("second-reply")),
        });

        sendAndWait(QStringLiteral("first-user"));
        sendAndWait(QStringLiteral("second-user"));

        QCOMPARE(m_provider->capturedRequests().size(), 2);
        const LlmRequest& secondReq = m_provider->capturedRequests().at(1);

        int iUser1 = -1, iAssist1 = -1, iUser2 = -1;
        for (int i = 0; i < secondReq.messages.size(); ++i) {
            const LlmMessage& m = secondReq.messages.at(i);
            if (m.role == QStringLiteral("user") && m.content == QStringLiteral("first-user")) {
                iUser1 = i;
            } else if (m.role == QStringLiteral("assistant") &&
                       m.content.contains(QStringLiteral("first-reply"))) {
                iAssist1 = i;
            } else if (m.role == QStringLiteral("user") &&
                       m.content == QStringLiteral("second-user")) {
                iUser2 = i;
            }
        }
        QVERIFY2(iUser1 >= 0, "first-user message missing from history");
        QVERIFY2(iAssist1 >= 0, "first-reply assistant message missing from history");
        QVERIFY2(iUser2 >= 0, "second-user message missing from history");
        QVERIFY2(iUser1 < iAssist1 && iAssist1 < iUser2,
                 qPrintable(QStringLiteral("history order wrong: user1=%1 assist1=%2 user2=%3")
                                .arg(iUser1)
                                .arg(iAssist1)
                                .arg(iUser2)));
    }


    void testBuildRequest_configMirrorsConversation() {
        QVariantMap cfg;
        cfg[QStringLiteral("temperature")] = 0.42;
        cfg[QStringLiteral("maxTokens")] = 1337;
        cfg[QStringLiteral("streaming")] = false;
        m_agentSettings->saveConversationConfig(cfg);

        m_provider->setScript({defaultStep()});
        sendAndWait(QStringLiteral("x"));

        QCOMPARE(m_provider->capturedRequests().size(), 1);
        const LlmConfig& c = m_provider->capturedRequests().first().config;
        QCOMPARE(c.temperature, 0.42);
        QCOMPARE(c.maxTokens, 1337);
        QCOMPARE(c.stream, false);
    }


    void testBuildRequest_requestIdNonZeroOnEverySend() {
        m_provider->setScript({
            defaultStep(QStringLiteral("a")),
            defaultStep(QStringLiteral("b")),
            defaultStep(QStringLiteral("c")),
        });

        sendAndWait(QStringLiteral("one"));
        sendAndWait(QStringLiteral("two"));
        sendAndWait(QStringLiteral("three"));

        QCOMPARE(m_provider->capturedRequests().size(), 3);
        for (int i = 0; i < 3; ++i) {
            const quint64 id = m_provider->capturedRequests().at(i).requestId;
            QVERIFY2(id > 0,
                     qPrintable(QStringLiteral("request id on send %1 must be non-zero, got %2")
                                    .arg(i)
                                    .arg(id)));
        }
    }


    void testBuildRequest_conversationIdSnapshottedAtSendTime() {
        const QString otherConv = m_convSvc->createConversation(QStringLiteral("Other"));
        QVERIFY(!otherConv.isEmpty());
        QVERIFY(otherConv != m_convId);

        m_provider->setScript({defaultStep()});
        sendAndWait(QStringLiteral("in-orig-conv"));

        QCOMPARE(m_provider->capturedRequests().size(), 1);
        QCOMPARE(m_provider->capturedRequests().first().conversationId, m_convId);

        m_chat->switchConversation(otherConv);
        QCOMPARE(m_provider->capturedRequests().first().conversationId, m_convId);
    }
};

QTEST_MAIN(TestRequestBuilder)
#include "test-request-builder.moc"

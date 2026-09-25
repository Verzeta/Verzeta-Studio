// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "models/db-manager.h"
#include "models/message.h"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QUuid>

namespace {

const QString kThinkingMarker = QStringLiteral("VERZETA_THINKING_LEAK_SENTINEL_DO_NOT_REMOVE");

}

class TestRequestBuilderNoThinkingLeak : public QObject {
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

    static ScriptedMockProvider::ScriptStep defaultStep() {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {QStringLiteral("ok")};
        s.finishReason = QStringLiteral("stop");
        s.tokens = 1;
        return s;
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/rb_thinking_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

        m_convId = m_convSvc->createConversation(QStringLiteral("ThinkingLeakRegression"));
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

    void testRequestBuilderNeverReadsThinkingContent() {
        {
            Message u;
            u.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            u.conversationId = m_convId;
            u.role = QStringLiteral("user");
            u.content = QStringLiteral("First question.");
            u.createdAt = QDateTime::currentDateTimeUtc();
            u.turnId = u.id;
            QVERIFY(!m_msgSvc->addMessage(u).isEmpty());
        }

        {
            Message a;
            a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            a.conversationId = m_convId;
            a.role = QStringLiteral("assistant");
            a.content = QStringLiteral("Visible answer the model produced.");
            a.createdAt = QDateTime::currentDateTimeUtc();
            a.finishReason = QStringLiteral("stop");
            a.turnId = a.id;
            QString thinking;
            thinking.reserve(50000);
            while (thinking.size() < 50000) {
                thinking += kThinkingMarker + QStringLiteral("\n");
            }
            a.thinkingContent = thinking;
            QVERIFY(!m_msgSvc->addMessage(a).isEmpty());
        }

        m_provider->setScript({defaultStep()});
        sendAndWait(QStringLiteral("Second question — please answer."));

        QCOMPARE(m_provider->capturedRequests().size(), 1);
        const LlmRequest& req = m_provider->capturedRequests().first();

        QVERIFY2(!req.systemPrompt.contains(kThinkingMarker),
                 "L3 INVARIANT BROKEN: thinking marker found in systemPrompt — "
                 "RequestBuilder is reading Message::thinkingContent");

        for (int i = 0; i < req.messages.size(); ++i) {
            const LlmMessage& m = req.messages.at(i);
            QVERIFY2(!m.content.contains(kThinkingMarker),
                     qPrintable(QStringLiteral("L3 INVARIANT BROKEN: thinking marker found in "
                                               "messages[%1].content")
                                    .arg(i)));
            QVERIFY2(!m.role.contains(kThinkingMarker),
                     qPrintable(QStringLiteral("L3 INVARIANT BROKEN: thinking marker found in "
                                               "messages[%1].role")
                                    .arg(i)));
            QVERIFY2(!m.speakerName.contains(kThinkingMarker),
                     qPrintable(QStringLiteral("L3 INVARIANT BROKEN: thinking marker found in "
                                               "messages[%1].speakerName")
                                    .arg(i)));
            QVERIFY2(!m.toolCallId.contains(kThinkingMarker),
                     qPrintable(QStringLiteral("L3 INVARIANT BROKEN: thinking marker found in "
                                               "messages[%1].toolCallId")
                                    .arg(i)));
        }

        QString serialised;
        serialised += req.systemPrompt;
        for (const LlmMessage& m : req.messages) {
            serialised += m.role;
            serialised += QStringLiteral("|");
            serialised += m.content;
            serialised += QStringLiteral("|");
            serialised += m.speakerName;
            serialised += QStringLiteral("|");
            serialised += m.toolCallId;
            serialised += QStringLiteral("\n");
        }
        QVERIFY2(!serialised.contains(kThinkingMarker),
                 "L3 INVARIANT BROKEN: thinking marker found in the "
                 "serialised LlmRequest — a request-side code path is "
                 "reading Message::thinkingContent that this test did "
                 "not directly enumerate.");
    }
};

QTEST_MAIN(TestRequestBuilderNoThinkingLeak)
#include "test-request-builder-no-thinking-leak.moc"

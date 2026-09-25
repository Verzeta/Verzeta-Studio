// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/message-list-model.h"
#include "models/message.h"
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

class TestStreamingManager : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
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

    QList<Message> currentMessages() const { return m_msgSvc->getRecentMessages(m_convId, 100); }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() + QStringLiteral("/sm_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

        m_convId = m_convSvc->createConversation(QStringLiteral("TestSM"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);
    }

    void cleanup() {
        m_chat.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void testStreaming_multipleChunksConcatenateIntoSingleRow() {
        ScriptedMockProvider::ScriptStep step;
        step.chunks = {QStringLiteral("Hello "), QStringLiteral("world"), QStringLiteral("!")};
        step.finishReason = QStringLiteral("stop");
        step.tokens = 5;
        m_provider->setScript({step});

        sendAndWait(QStringLiteral("say hi"));

        const QList<Message> rows = currentMessages();
        int userCount = 0;
        int assistantCount = 0;
        QString assistantContent;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("user"))
                ++userCount;
            else if (m.role == QStringLiteral("assistant")) {
                ++assistantCount;
                assistantContent = m.content;
            }
        }
        QCOMPARE(userCount, 1);
        QCOMPARE(assistantCount, 1);
        QCOMPARE(assistantContent, QStringLiteral("Hello world!"));
    }


    void testStreaming_errorMidStreamLeavesNoStuckRow() {
        ScriptedMockProvider::ScriptStep step;
        step.chunks = {QStringLiteral("partial content before error")};
        step.isError = true;
        step.errorMessage = QStringLiteral("Simulated provider error");
        m_provider->setScript({step});

        sendAndWait(QStringLiteral("will fail"));

        const QList<Message> rows = currentMessages();
        for (const Message& m : rows) {
            QVERIFY2(m.role != QStringLiteral("assistant"),
                     qPrintable(QStringLiteral("Unexpected assistant row after error: "
                                               "content=%1")
                                    .arg(m.content.left(80))));
        }

        bool userRowFound = false;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("user")) {
                userRowFound = true;
                break;
            }
        }
        QVERIFY2(userRowFound,
                 "User message must be persisted even though the "
                 "assistant stream errored");

        QCOMPARE(m_chat->isGenerating(), false);
    }


    void testStreaming_userStopMidStreamIsClean() {
        ScriptedMockProvider::ScriptStep step;
        step.chunks = {QStringLiteral("not important")};
        step.finishReason = QStringLiteral("stop");
        step.tokens = 1;
        m_provider->setScript({step});

        sendAndWait(QStringLiteral("ping"));

        m_chat->stopGeneration();
        QCOMPARE(m_chat->isGenerating(), false);

        const QList<Message> rows = currentMessages();
        int assistantCount = 0;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant")) {
                ++assistantCount;
            }
        }
        QCOMPARE(assistantCount, 1);
    }


    void testStreaming_lastAssistantMessageExposedAfterFinalize() {
        ScriptedMockProvider::ScriptStep step;
        step.chunks = {QStringLiteral("The answer is "), QStringLiteral("42.")};
        step.finishReason = QStringLiteral("stop");
        step.tokens = 2;
        m_provider->setScript({step});

        sendAndWait(QStringLiteral("q"));

        QCOMPARE(m_chat->lastAssistantMessage(), QStringLiteral("The answer is 42."));
    }


    void testStreaming_sequentialStreamsDoNotCrossContaminate() {
        ScriptedMockProvider::ScriptStep s1;
        s1.chunks = {QStringLiteral("first-reply")};
        s1.finishReason = QStringLiteral("stop");
        s1.tokens = 1;
        ScriptedMockProvider::ScriptStep s2;
        s2.chunks = {QStringLiteral("second-reply")};
        s2.finishReason = QStringLiteral("stop");
        s2.tokens = 1;
        m_provider->setScript({s1, s2});

        sendAndWait(QStringLiteral("one"));
        sendAndWait(QStringLiteral("two"));

        const QList<Message> rows = currentMessages();
        QStringList roles, contents;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("user") || m.role == QStringLiteral("assistant")) {
                roles << m.role;
                contents << m.content;
            }
        }
        QCOMPARE(roles.size(), 4);
        QCOMPARE(roles[0], QStringLiteral("user"));
        QCOMPARE(roles[1], QStringLiteral("assistant"));
        QCOMPARE(roles[2], QStringLiteral("user"));
        QCOMPARE(roles[3], QStringLiteral("assistant"));
        QCOMPARE(contents[0], QStringLiteral("one"));
        QCOMPARE(contents[1], QStringLiteral("first-reply"));
        QCOMPARE(contents[2], QStringLiteral("two"));
        QCOMPARE(contents[3], QStringLiteral("second-reply"));
    }
};

QTEST_MAIN(TestStreamingManager)
#include "test-streaming-manager.moc"

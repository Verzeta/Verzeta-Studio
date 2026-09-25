// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "models/db-manager.h"
#include "models/message.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/task-gate-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QUuid>

class TestRequestIdGating : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<TaskGateService> m_taskGateSvc;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provider = nullptr;
    QString m_convId;

    void sendAndWait(const QString& text, int timeoutMs = 2000) {
        m_chat->sendMessage(text, {});
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < timeoutMs) {
            QTest::qWait(5);
        }
        QVERIFY2(!m_chat->isGenerating(), "ChatController never finished generating");
    }

    int assistantRowCount() const {
        int n = 0;
        const QList<Message> msgs = m_msgSvc->getRecentMessages(m_convId, 200);
        for (const Message& m : msgs) {
            if (m.role == QStringLiteral("assistant"))
                ++n;
        }
        return n;
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

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_taskGateSvc = std::make_unique<TaskGateService>(*m_msgSvc);
        m_chat->setTaskGateService(m_taskGateSvc.get());

        m_convId = m_convSvc->createConversation(QStringLiteral("GateTest"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);
    }

    void cleanup() {
        m_chat.reset();
        m_taskGateSvc.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_lateChunk_afterTurnFinished_isDropped() {
        m_provider->setScript(
            {ScriptedMockProvider::ScriptStep{.chunks = {QStringLiteral("live-chunk")},
                                              .finishReason = QStringLiteral("stop"),
                                              .tokens = 1}});
        sendAndWait(QStringLiteral("hi"));

        const int asstBefore = assistantRowCount();
        QCOMPARE(asstBefore, 1);

        LlmChunk late;
        late.delta = QStringLiteral("ghost-delta");
        m_provider->emitLateChunk(late);

        QTest::qWait(20);

        QCOMPARE(assistantRowCount(), asstBefore);
        QVERIFY(!m_chat->isGenerating());
        const auto streamSlots = m_msgSvc->streamingMessagesForConversation(m_convId);
        QCOMPARE(streamSlots.size(), 0);
    }

    void test_lateRequestFinished_afterTurnFinished_isDropped() {
        m_provider->setScript(
            {ScriptedMockProvider::ScriptStep{.chunks = {QStringLiteral("ok")},
                                              .finishReason = QStringLiteral("stop"),
                                              .tokens = 1}});
        sendAndWait(QStringLiteral("hello"));

        const int asstBefore = assistantRowCount();
        QCOMPARE(asstBefore, 1);

        m_provider->emitLateRequestFinished(QStringLiteral("stop"), 99);
        QTest::qWait(20);

        QCOMPARE(assistantRowCount(), asstBefore);
        QVERIFY(!m_chat->isGenerating());
    }

    void test_crossTurnStaleChunk_doesNotPolluteLiveTurn() {
        m_provider->setScript(
            {ScriptedMockProvider::ScriptStep{.chunks = {QStringLiteral("first-turn")},
                                              .finishReason = QStringLiteral("stop"),
                                              .tokens = 1},
             ScriptedMockProvider::ScriptStep{.chunks = {QStringLiteral("second-turn")},
                                              .finishReason = QStringLiteral("stop"),
                                              .tokens = 1}});

        sendAndWait(QStringLiteral("first"));
        QCOMPARE(assistantRowCount(), 1);

        sendAndWait(QStringLiteral("second"));
        QCOMPARE(assistantRowCount(), 2);

        LlmChunk ghost;
        ghost.delta = QStringLiteral("ghost-of-turn-1");
        m_provider->emitLateChunk(ghost);
        QTest::qWait(20);

        const QList<Message> all = m_msgSvc->getRecentMessages(m_convId, 200);
        int asstCount = 0;
        for (const Message& m : all) {
            if (m.role != QStringLiteral("assistant"))
                continue;
            ++asstCount;
            QVERIFY(!m.content.contains(QStringLiteral("ghost-of-turn-1")));
        }
        QCOMPARE(asstCount, 2);
    }
};

QTEST_MAIN(TestRequestIdGating)
#include "test-request-id-gating.moc"

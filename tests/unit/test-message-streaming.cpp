// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "services/message-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestMessageStreaming : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    QString m_convA;
    QString m_convB;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

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

        m_convA = m_convSvc->createConversation(QStringLiteral("A"));
        m_convB = m_convSvc->createConversation(QStringLiteral("B"));
    }

    void cleanup() {
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_beginStreaming_emitsStartedSignal() {
        QSignalSpy startedSpy(m_msgSvc.get(), &MessageService::messageStreamingStarted);

        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(
            m_convA, msgId, QStringLiteral("assistant"), QString{}, QString{});

        QCOMPARE(startedSpy.count(), 1);
        QVERIFY(m_msgSvc->hasStreamingMessage(msgId));
        QCOMPARE(m_msgSvc->streamingConversationId(msgId), m_convA);
        QCOMPARE(m_msgSvc->streamingContent(msgId), QString{});
    }

    void test_duplicateBegin_isRejected() {
        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));

        QSignalSpy startedSpy(m_msgSvc.get(), &MessageService::messageStreamingStarted);
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));
        QCOMPARE(startedSpy.count(), 0);
        QVERIFY(m_msgSvc->hasStreamingMessage(msgId));
    }

    void test_appendChunk_accumulatesContent() {
        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));

        QSignalSpy chunkSpy(m_msgSvc.get(), &MessageService::messageContentStreamed);
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("Hello"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral(", world!"));

        QVERIFY(chunkSpy.wait(200));
        QCOMPARE(chunkSpy.count(), 1);
        QCOMPARE(chunkSpy.first().at(2).toString(), QStringLiteral("Hello, world!"));
        QCOMPARE(m_msgSvc->streamingContent(msgId), QStringLiteral("Hello, world!"));
    }

    void test_rewriteContent_replacesAndSignals() {
        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("raw"));

        QSignalSpy rewroteSpy(m_msgSvc.get(), &MessageService::messageContentRewritten);
        m_msgSvc->rewriteStreamingContent(msgId, QStringLiteral("sanitised"));

        QCOMPARE(rewroteSpy.count(), 1);
        QCOMPARE(m_msgSvc->streamingContent(msgId), QStringLiteral("sanitised"));
    }

    void test_finalize_flushesToDbAndClearsSlot() {
        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("done"));

        QSignalSpy addedSpy(m_msgSvc.get(), &MessageService::messageAdded);
        QVERIFY(m_msgSvc->finalizeStreamingMessage(msgId,
                                                   7,
                                                   QStringLiteral("stop"),
                                                   QStringLiteral("<p>done</p>"),
                                                   QStringLiteral("test-model"),
                                                   QJsonObject{}));

        QCOMPARE(addedSpy.count(), 1);
        QVERIFY(!m_msgSvc->hasStreamingMessage(msgId));

        const QList<Message> rows = m_msgSvc->getMessages(m_convA);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().id, msgId);
        QCOMPARE(rows.first().content, QStringLiteral("done"));
        QCOMPARE(rows.first().tokenCount, 7);
        QCOMPARE(rows.first().finishReason, QStringLiteral("stop"));
    }

    void test_abort_dropsSlotAndEmitsAbortSignal() {
        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("partial"));

        QSignalSpy abortedSpy(m_msgSvc.get(), &MessageService::messageStreamingAborted);
        QSignalSpy addedSpy(m_msgSvc.get(), &MessageService::messageAdded);

        m_msgSvc->abortStreamingMessage(msgId);

        QCOMPARE(abortedSpy.count(), 1);
        QCOMPARE(addedSpy.count(), 0);
        QVERIFY(!m_msgSvc->hasStreamingMessage(msgId));

        const QList<Message> rows = m_msgSvc->getMessages(m_convA);
        QCOMPARE(rows.size(), 0);
    }

    void test_addMessage_rejectsDeletedConversation() {
        QVERIFY(m_convSvc->deleteConversation(m_convA));

        Message m;
        m.id = uuid();
        m.conversationId = m_convA;
        m.role = QStringLiteral("user");
        m.content = QStringLiteral("zombie");
        m.createdAt = QDateTime::currentDateTimeUtc();

        QSignalSpy addedSpy(m_msgSvc.get(), &MessageService::messageAdded);
        const QString saved = m_msgSvc->addMessage(m);

        QVERIFY(saved.isEmpty());
        QCOMPARE(addedSpy.count(), 0);
    }

    void test_beginStreamingMessage_rejectsDeletedConversation() {
        QVERIFY(m_convSvc->deleteConversation(m_convA));

        const QString msgId = uuid();
        QSignalSpy startedSpy(m_msgSvc.get(), &MessageService::messageStreamingStarted);
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));

        QCOMPARE(startedSpy.count(), 0);
        QVERIFY(!m_msgSvc->hasStreamingMessage(msgId));
    }

    void test_finalizeEmptyToolCallsRow_persistsAndClearsStream() {
        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));

        QSignalSpy addedSpy(m_msgSvc.get(), &MessageService::messageAdded);
        QVERIFY(m_msgSvc->finalizeStreamingMessage(msgId,
                                                   0,
                                                   QStringLiteral("tool_calls"),
                                                   QString{},
                                                   QStringLiteral("test-model"),
                                                   QJsonObject{}));

        QCOMPARE(addedSpy.count(), 1);
        QVERIFY(!m_msgSvc->hasStreamingMessage(msgId));

        const QList<Message> rows = m_msgSvc->getMessages(m_convA);
        bool found = false;
        for (const Message& m : rows) {
            if (m.id == msgId) {
                found = true;
                QCOMPARE(m.content, QString{});
                QCOMPARE(m.finishReason, QStringLiteral("tool_calls"));
            }
        }
        QVERIFY(found);
    }

    void test_finalizeAfterConversationDeleted_emitsAbort() {
        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("partial"));

        QVERIFY(m_convSvc->deleteConversation(m_convA));

        QSignalSpy addedSpy(m_msgSvc.get(), &MessageService::messageAdded);
        QSignalSpy abortedSpy(m_msgSvc.get(), &MessageService::messageStreamingAborted);

        const bool ok = m_msgSvc->finalizeStreamingMessage(msgId,
                                                           0,
                                                           QStringLiteral("stop"),
                                                           QString{},
                                                           QStringLiteral("test-model"),
                                                           QJsonObject{});

        QVERIFY(!ok);
        QCOMPARE(addedSpy.count(), 0);
        QCOMPARE(abortedSpy.count(), 1);
        QVERIFY(!m_msgSvc->hasStreamingMessage(msgId));
    }

    void test_conversationDeletedMidStream_abortsSlot() {
        const QString msgId = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, msgId, QStringLiteral("assistant"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("partial"));
        QVERIFY(m_msgSvc->hasStreamingMessage(msgId));

        QSignalSpy abortedSpy(m_msgSvc.get(), &MessageService::messageStreamingAborted);

        m_msgSvc->onConversationDeleted(m_convA);

        QCOMPARE(abortedSpy.count(), 1);
        QVERIFY(!m_msgSvc->hasStreamingMessage(msgId));
    }

    void test_crossConversationIsolation() {
        const QString mA = uuid();
        const QString mB = uuid();
        m_msgSvc->beginStreamingMessage(m_convA, mA, QStringLiteral("assistant"));
        m_msgSvc->beginStreamingMessage(m_convB, mB, QStringLiteral("assistant"));

        m_msgSvc->appendStreamingChunk(mA, QStringLiteral("one"));
        m_msgSvc->appendStreamingChunk(mB, QStringLiteral("two"));

        QCOMPARE(m_msgSvc->streamingContent(mA), QStringLiteral("one"));
        QCOMPARE(m_msgSvc->streamingContent(mB), QStringLiteral("two"));

        const auto forA = m_msgSvc->streamingMessagesForConversation(m_convA);
        QCOMPARE(forA.size(), 1);
        QCOMPARE(forA.first().id, mA);
        QCOMPARE(forA.first().content, QStringLiteral("one"));

        const auto forB = m_msgSvc->streamingMessagesForConversation(m_convB);
        QCOMPARE(forB.size(), 1);
        QCOMPARE(forB.first().id, mB);

        m_msgSvc->abortStreamingMessage(mA);
        QVERIFY(!m_msgSvc->hasStreamingMessage(mA));
        QVERIFY(m_msgSvc->hasStreamingMessage(mB));
    }
};

QTEST_MAIN(TestMessageStreaming)
#include "test-message-streaming.moc"

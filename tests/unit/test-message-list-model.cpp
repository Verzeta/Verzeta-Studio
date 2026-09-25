// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "models/message-list-model.h"
#include "services/conversation-service.h"
#include "services/message-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestMessageListModel : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<MessageListModel> m_model;
    QString m_convId;

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_model = std::make_unique<MessageListModel>(*m_msgSvc);

        m_convId = m_convSvc->createConversation(QStringLiteral("Test Conv"));
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_model.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_initialState_isEmpty() {
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(m_model->count(), 0);
        QVERIFY(!m_model->hasStreamingMessage());
        QVERIFY(m_model->activeConversationId().isEmpty());
    }

    void test_setActiveConversation_loadsPersistedRows() {
        for (int i = 0; i < 3; ++i) {
            Message msg;
            msg.id = QStringLiteral("msg-%1").arg(i);
            msg.conversationId = m_convId;
            msg.role = QStringLiteral("user");
            msg.content = QStringLiteral("Message %1").arg(i);
            msg.createdAt = QDateTime::currentDateTimeUtc();
            QVERIFY(!m_msgSvc->addMessage(msg).isEmpty());
        }

        QSignalSpy countSpy(m_model.get(), &MessageListModel::countChanged);
        m_model->setActiveConversation(m_convId);

        QCOMPARE(m_model->rowCount(), 3);
        QCOMPARE(m_model->count(), 3);
        QCOMPARE(m_model->activeConversationId(), m_convId);
        QVERIFY(countSpy.count() >= 1);
    }

    void test_addMessage_signalInsertsRow() {
        m_model->setActiveConversation(m_convId);
        QSignalSpy rowsInsertedSpy(m_model.get(), &MessageListModel::rowsInserted);

        Message msg;
        msg.id = QStringLiteral("new-1");
        msg.conversationId = m_convId;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Hello");
        msg.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgSvc->addMessage(msg).isEmpty());

        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(rowsInsertedSpy.count(), 1);
        const QModelIndex idx = m_model->index(0);
        QCOMPARE(m_model->data(idx, MessageListModel::ContentRole).toString(),
                 QStringLiteral("Hello"));
    }

    void test_addMessage_otherConversation_ignored() {
        m_model->setActiveConversation(m_convId);
        const QString otherConvId = m_convSvc->createConversation(QStringLiteral("Other"));

        Message msg;
        msg.id = QStringLiteral("other-1");
        msg.conversationId = otherConvId;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Not for us");
        msg.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgSvc->addMessage(msg).isEmpty());

        QCOMPARE(m_model->rowCount(), 0);
    }

    void test_streamingLifecycle_fullFlow() {
        m_model->setActiveConversation(m_convId);

        QSignalSpy rowsInsertedSpy(m_model.get(), &MessageListModel::rowsInserted);
        QSignalSpy streamSpy(m_model.get(), &MessageListModel::streamingChanged);

        const QString msgId = QStringLiteral("stream-1");
        m_msgSvc->beginStreamingMessage(m_convId, msgId, QStringLiteral("assistant"));

        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(rowsInsertedSpy.count(), 1);
        QVERIFY(m_model->hasStreamingMessage());
        QVERIFY(streamSpy.count() >= 1);

        const QModelIndex idx = m_model->index(0);
        QVERIFY(m_model->data(idx, MessageListModel::IsStreamingRole).toBool());

        QSignalSpy dataSpy(m_model.get(), &MessageListModel::dataChanged);
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("Hello"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral(" world"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("!"));

        QVERIFY(dataSpy.wait(200));
        QCOMPARE(m_model->rowCount(), 1);
        QVERIFY(dataSpy.count() >= 1);
        QCOMPARE(m_model->data(idx, MessageListModel::ContentRole).toString(),
                 QStringLiteral("Hello world!"));
        QVERIFY(m_model->data(idx, MessageListModel::IsStreamingRole).toBool());

        QVERIFY(m_msgSvc->finalizeStreamingMessage(msgId,
                                                   42,
                                                   QStringLiteral("stop"),
                                                   QStringLiteral("<p>Hello world!</p>"),
                                                   QStringLiteral("test-model"),
                                                   QJsonObject{}));

        QVERIFY(!m_model->hasStreamingMessage());
        QCOMPARE(m_model->rowCount(), 1);
        QVERIFY(!m_model->data(idx, MessageListModel::IsStreamingRole).toBool());
        QCOMPARE(m_model->data(idx, MessageListModel::TokenCountRole).toInt(), 42);
        QCOMPARE(m_model->data(idx, MessageListModel::FinishReasonRole).toString(),
                 QStringLiteral("stop"));
    }

    void test_finalizeEmptyToolCallsRow_removesPlaceholder() {
        m_model->setActiveConversation(m_convId);

        const QString msgId = QStringLiteral("stream-tool-only");
        m_msgSvc->beginStreamingMessage(m_convId, msgId, QStringLiteral("assistant"));
        QCOMPARE(m_model->rowCount(), 1);
        QVERIFY(m_model->data(m_model->index(0), MessageListModel::IsStreamingRole).toBool());

        QSignalSpy rowsRemovedSpy(m_model.get(), &MessageListModel::rowsRemoved);
        QVERIFY(m_msgSvc->finalizeStreamingMessage(msgId,
                                                   0,
                                                   QStringLiteral("tool_calls"),
                                                   QString{},
                                                   QStringLiteral("test-model"),
                                                   QJsonObject{}));

        QCOMPARE(rowsRemovedSpy.count(), 1);
        QCOMPARE(m_model->rowCount(), 0);
        QVERIFY(!m_model->hasStreamingMessage());

        const QList<Message> dbRows = m_msgSvc->getMessages(m_convId);
        bool found = false;
        for (const Message& m : dbRows) {
            if (m.id == msgId) {
                found = true;
                break;
            }
        }
        QVERIFY(found);
    }

    void test_abortStreamingMessage_removesRow() {
        m_model->setActiveConversation(m_convId);
        const QString msgId = QStringLiteral("stream-abort");
        m_msgSvc->beginStreamingMessage(m_convId, msgId, QStringLiteral("assistant"));
        QCOMPARE(m_model->rowCount(), 1);

        QSignalSpy rowsRemovedSpy(m_model.get(), &MessageListModel::rowsRemoved);
        m_msgSvc->abortStreamingMessage(msgId);

        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(rowsRemovedSpy.count(), 1);
        QVERIFY(!m_model->hasStreamingMessage());
    }

    void test_rewriteStreamingContent_replacesContent() {
        m_model->setActiveConversation(m_convId);
        const QString msgId = QStringLiteral("stream-rewrite");
        m_msgSvc->beginStreamingMessage(m_convId, msgId, QStringLiteral("assistant"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("draft"));

        m_msgSvc->rewriteStreamingContent(msgId, QStringLiteral("final"));

        const QModelIndex idx = m_model->index(0);
        QCOMPARE(m_model->data(idx, MessageListModel::ContentRole).toString(),
                 QStringLiteral("final"));
    }

    void test_deleteMessage_signalRemovesRow() {
        Message msg;
        msg.id = QStringLiteral("to-delete");
        msg.conversationId = m_convId;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("bye");
        msg.createdAt = QDateTime::currentDateTimeUtc();
        m_msgSvc->addMessage(msg);

        m_model->setActiveConversation(m_convId);
        QCOMPARE(m_model->rowCount(), 1);

        QSignalSpy rowsRemovedSpy(m_model.get(), &MessageListModel::rowsRemoved);
        QVERIFY(m_msgSvc->deleteMessage(msg.id));

        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(rowsRemovedSpy.count(), 1);
    }

    void test_roleNames_allRolesMapped() {
        const QHash<int, QByteArray> names = m_model->roleNames();
        QVERIFY(names.values().contains(QByteArrayLiteral("id")));
        QVERIFY(names.values().contains(QByteArrayLiteral("role")));
        QVERIFY(names.values().contains(QByteArrayLiteral("content")));
        QVERIFY(names.values().contains(QByteArrayLiteral("contentHtml")));
        QVERIFY(names.values().contains(QByteArrayLiteral("createdAt")));
        QVERIFY(names.values().contains(QByteArrayLiteral("tokenCount")));
        QVERIFY(names.values().contains(QByteArrayLiteral("modelUsed")));
        QVERIFY(names.values().contains(QByteArrayLiteral("finishReason")));
        QVERIFY(names.values().contains(QByteArrayLiteral("isStreaming")));
        QVERIFY(names.values().contains(QByteArrayLiteral("metadata")));
    }

    void test_streaming_otherConversation_isolated() {
        m_model->setActiveConversation(m_convId);
        const QString otherConvId = m_convSvc->createConversation(QStringLiteral("Other"));

        const QString msgId = QStringLiteral("stream-other");
        m_msgSvc->beginStreamingMessage(otherConvId, msgId, QStringLiteral("assistant"));
        m_msgSvc->appendStreamingChunk(msgId, QStringLiteral("hidden"));

        QCOMPARE(m_model->rowCount(), 0);
        QVERIFY(!m_model->hasStreamingMessage());

        m_model->setActiveConversation(otherConvId);
        QCOMPARE(m_model->rowCount(), 1);
        QVERIFY(m_model->hasStreamingMessage());
        QCOMPARE(m_model->data(m_model->index(0), MessageListModel::ContentRole).toString(),
                 QStringLiteral("hidden"));
    }
};

QTEST_MAIN(TestMessageListModel)
#include "test-message-list-model.moc"

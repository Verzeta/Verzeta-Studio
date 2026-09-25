// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "services/message-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>

class TestMessageServiceGetMessage : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        m_dbPath = dbPath;
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());
        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());

        m_convId = m_convSvc->createConversation(QStringLiteral("Test Conv"));
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_roundTrip_returnsAddedMessage() {
        Message m;
        m.id = QStringLiteral("msg-rt-001");
        m.conversationId = m_convId;
        m.role = QStringLiteral("assistant");
        m.content = QStringLiteral("Background activity report.");
        m.createdAt = QDateTime::currentDateTimeUtc();

        QCOMPARE(m_msgSvc->addMessage(m), m.id);

        const Message got = m_msgSvc->getMessage(m.id);
        QCOMPARE(got.id, m.id);
        QCOMPARE(got.conversationId, m_convId);
        QCOMPARE(got.role, QStringLiteral("assistant"));
        QCOMPARE(got.content, QStringLiteral("Background activity report."));
    }

    void test_missingId_returnsEmptyMessage() {
        const Message got = m_msgSvc->getMessage(QStringLiteral("does-not-exist-uuid"));
        QVERIFY(got.id.isEmpty());
        QVERIFY(got.conversationId.isEmpty());
        QVERIFY(got.role.isEmpty());
    }

    void test_deletedMessage_returnsEmpty() {
        Message m;
        m.id = QStringLiteral("msg-del-001");
        m.conversationId = m_convId;
        m.role = QStringLiteral("user");
        m.content = QStringLiteral("delete me");
        m.createdAt = QDateTime::currentDateTimeUtc();
        QCOMPARE(m_msgSvc->addMessage(m), m.id);

        QCOMPARE(m_msgSvc->getMessage(m.id).id, m.id);

        QVERIFY(m_msgSvc->deleteMessage(m.id));

        const Message got = m_msgSvc->getMessage(m.id);
        QVERIFY(got.id.isEmpty());
    }

    void test_emptyIdInput_returnsEmptyWithoutDbHit() {
        const Message got = m_msgSvc->getMessage(QString());
        QVERIFY(got.id.isEmpty());
    }

    void test_getMessage_exposesConversationIdForSurfaceCollisionPath() {
        const QString otherConvId = m_convSvc->createConversation(QStringLiteral("Other Conv"));
        QVERIFY(!otherConvId.isEmpty());
        QVERIFY(otherConvId != m_convId);

        Message m;
        m.id = QStringLiteral("msg-conv-001");
        m.conversationId = otherConvId;
        m.role = QStringLiteral("assistant");
        m.content = QStringLiteral("hello from other conv");
        m.createdAt = QDateTime::currentDateTimeUtc();
        QCOMPARE(m_msgSvc->addMessage(m), m.id);

        const Message got = m_msgSvc->getMessage(m.id);
        QCOMPARE(got.conversationId, otherConvId);
        QVERIFY(got.conversationId != m_convId);
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    QString m_convId;
};

QTEST_MAIN(TestMessageServiceGetMessage)
#include "test-message-service-get-message.moc"

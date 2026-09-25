// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/models/message.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/message-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QSqlDatabase>

class TestInterveningSystemMessage : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    QString m_convId;

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_convId = m_convSvc->createConversation(QStringLiteral("Test"));
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_helperProducesRoleSystem() {
        const QString id = m_msgSvc->addInterveningSystemMessage(
            m_convId, QStringLiteral("hello from system"), QJsonObject{});
        QVERIFY(!id.isEmpty());

        const Message persisted = m_msgSvc->getMessage(id);
        QCOMPARE(persisted.id, id);
        QCOMPARE(persisted.conversationId, m_convId);
        QCOMPARE(persisted.role, QStringLiteral("system"));
        QCOMPARE(persisted.content, QStringLiteral("hello from system"));
    }

    void test_helperRoundTripsMetadata() {
        QJsonObject meta;
        meta.insert(QStringLiteral("poll_id"), QStringLiteral("p-001"));
        meta.insert(QStringLiteral("produced_by"), QStringLiteral("poll_service"));
        meta.insert(QStringLiteral("poll_question"), QStringLiteral("Typer or Click?"));

        const QString id = m_msgSvc->addInterveningSystemMessage(
            m_convId, QStringLiteral("Poll started: Typer or Click?"), meta);
        QVERIFY(!id.isEmpty());

        const Message persisted = m_msgSvc->getMessage(id);
        QCOMPARE(persisted.metadata.value(QStringLiteral("poll_id")).toString(),
                 QStringLiteral("p-001"));
        QCOMPARE(persisted.metadata.value(QStringLiteral("produced_by")).toString(),
                 QStringLiteral("poll_service"));
        QCOMPARE(persisted.metadata.value(QStringLiteral("poll_question")).toString(),
                 QStringLiteral("Typer or Click?"));
    }

    void test_emptyConvIdRejected() {
        const int before = m_msgSvc->getMessages(m_convId).size();
        const QString id = m_msgSvc->addInterveningSystemMessage(
            QString(), QStringLiteral("orphan body"), QJsonObject{});
        QVERIFY(id.isEmpty());
        QCOMPARE(m_msgSvc->getMessages(m_convId).size(), before);
    }

    void test_uniqueIdsPerCall() {
        const QString a = m_msgSvc->addInterveningSystemMessage(
            m_convId, QStringLiteral("same body"), QJsonObject{});
        const QString b = m_msgSvc->addInterveningSystemMessage(
            m_convId, QStringLiteral("same body"), QJsonObject{});
        QVERIFY(!a.isEmpty());
        QVERIFY(!b.isEmpty());
        QVERIFY(a != b);

        const auto rows = m_msgSvc->getMessages(m_convId);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].role, QStringLiteral("system"));
        QCOMPARE(rows[1].role, QStringLiteral("system"));
    }

    void test_helperRowInsertedBetweenToolCallAndResult() {
        Message assistantToolCall;
        assistantToolCall.id = QStringLiteral("msg-assistant");
        assistantToolCall.conversationId = m_convId;
        assistantToolCall.role = QStringLiteral("assistant");
        assistantToolCall.content = QStringLiteral("calling start_poll");
        assistantToolCall.finishReason = QStringLiteral("tool_calls");
        assistantToolCall.createdAt = QDateTime::currentDateTime();
        QVERIFY(!m_msgSvc->addMessage(assistantToolCall).isEmpty());

        QTest::qWait(2);

        const QString sysId = m_msgSvc->addInterveningSystemMessage(
            m_convId,
            QStringLiteral("Poll started: live scenario"),
            QJsonObject{{QStringLiteral("produced_by"), QStringLiteral("poll_service")}});
        QVERIFY(!sysId.isEmpty());

        QTest::qWait(2);

        Message toolResult;
        toolResult.id = QStringLiteral("msg-tool");
        toolResult.conversationId = m_convId;
        toolResult.role = QStringLiteral("tool");
        toolResult.content = QStringLiteral("{\"poll_id\":\"p-1\"}");
        toolResult.createdAt = QDateTime::currentDateTime();
        QVERIFY(!m_msgSvc->addMessage(toolResult).isEmpty());

        const auto rows = m_msgSvc->getMessages(m_convId);
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].role, QStringLiteral("assistant"));
        QCOMPARE(rows[0].finishReason, QStringLiteral("tool_calls"));
        QCOMPARE(rows[1].role, QStringLiteral("system"));
        QCOMPARE(rows[1].id, sysId);
        QCOMPARE(rows[2].role, QStringLiteral("tool"));
    }
};

QTEST_MAIN(TestInterveningSystemMessage)
#include "test-intervening-system-message.moc"

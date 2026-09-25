// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "services/message-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QSqlDatabase>

class TestMessageService : public QObject {
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


    void testAddAndRetrieve() {
        Message msg;
        msg.id = QStringLiteral("msg-001");
        msg.conversationId = m_convId;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Hello!");
        msg.createdAt = QDateTime::currentDateTimeUtc();

        QCOMPARE(m_msgSvc->addMessage(msg), msg.id);

        const auto messages = m_msgSvc->getMessages(m_convId);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages[0].id, QStringLiteral("msg-001"));
        QCOMPARE(messages[0].role, QStringLiteral("user"));
        QCOMPARE(messages[0].content, QStringLiteral("Hello!"));
    }


    void testMessageOrdering() {
        for (int i = 1; i <= 3; ++i) {
            Message msg;
            msg.id = QStringLiteral("msg-ord-%1").arg(i);
            msg.conversationId = m_convId;
            msg.role = QStringLiteral("user");
            msg.content = QStringLiteral("Message %1").arg(i);
            msg.createdAt = QDateTime::fromMSecsSinceEpoch(1000LL * i);
            QVERIFY(!m_msgSvc->addMessage(msg).isEmpty());
        }

        const auto messages = m_msgSvc->getMessages(m_convId);
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages[0].id, QStringLiteral("msg-ord-1"));
        QCOMPARE(messages[1].id, QStringLiteral("msg-ord-2"));
        QCOMPARE(messages[2].id, QStringLiteral("msg-ord-3"));
    }


    void testPagination() {
        for (int i = 1; i <= 10; ++i) {
            Message msg;
            msg.id = QStringLiteral("msg-page-%1").arg(i);
            msg.conversationId = m_convId;
            msg.role = QStringLiteral("user");
            msg.content = QStringLiteral("Page message %1").arg(i);
            msg.createdAt = QDateTime::fromMSecsSinceEpoch(1000LL * i);
            QVERIFY(!m_msgSvc->addMessage(msg).isEmpty());
        }

        const auto page = m_msgSvc->getMessagePage(m_convId, 3, 4);
        QCOMPARE(page.size(), 3);
        QCOMPARE(page[0].id, QStringLiteral("msg-page-5"));
        QCOMPARE(page[1].id, QStringLiteral("msg-page-6"));
        QCOMPARE(page[2].id, QStringLiteral("msg-page-7"));
    }


    void testTokenTotal() {
        auto addMsg = [&](const QString& id, int tokens) {
            Message msg;
            msg.id = id;
            msg.conversationId = m_convId;
            msg.role = QStringLiteral("user");
            msg.content = QStringLiteral("content");
            msg.tokenCount = tokens;
            msg.createdAt = QDateTime::currentDateTimeUtc();
            return m_msgSvc->addMessage(msg);
        };

        QVERIFY(!addMsg(QStringLiteral("msg-tok-1"), 100).isEmpty());
        QVERIFY(!addMsg(QStringLiteral("msg-tok-2"), 200).isEmpty());
        QVERIFY(!addMsg(QStringLiteral("msg-tok-3"), 300).isEmpty());

        QCOMPARE(m_msgSvc->getTokenTotal(m_convId), 600);
    }


    void testHtmlCache() {
        Message msg;
        msg.id = QStringLiteral("msg-html-1");
        msg.conversationId = m_convId;
        msg.role = QStringLiteral("assistant");
        msg.content = QStringLiteral("**Bold** text");
        msg.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgSvc->addMessage(msg).isEmpty());

        const QString html = QStringLiteral("<p><strong>Bold</strong> text</p>");
        QVERIFY(m_msgSvc->updateContentHtml(QStringLiteral("msg-html-1"), html));

        const auto messages = m_msgSvc->getMessages(m_convId);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages[0].contentHtml, html);
    }


    void testAttachmentCrud() {
        Message msg;
        msg.id = QStringLiteral("msg-att-1");
        msg.conversationId = m_convId;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("See attached file");
        msg.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgSvc->addMessage(msg).isEmpty());

        Attachment att;
        att.id = QStringLiteral("att-001");
        att.messageId = QStringLiteral("msg-att-1");
        att.type = QStringLiteral("file");
        att.filename = QStringLiteral("report.txt");
        att.mimeType = QStringLiteral("text/plain");
        att.createdAt = QDateTime::currentDateTimeUtc();
        QCOMPARE(m_msgSvc->addAttachment(att), att.id);

        const auto attachments = m_msgSvc->getAttachments(QStringLiteral("msg-att-1"));
        QCOMPARE(attachments.size(), 1);
        QCOMPARE(attachments[0].filename, QStringLiteral("report.txt"));
        QCOMPARE(attachments[0].type, QStringLiteral("file"));

        QVERIFY(m_msgSvc->deleteAttachment(QStringLiteral("att-001")));
        QCOMPARE(m_msgSvc->getAttachments(QStringLiteral("msg-att-1")).size(), 0);
    }


    void testToolCallCrud() {
        Message msg;
        msg.id = QStringLiteral("msg-tc-1");
        msg.conversationId = m_convId;
        msg.role = QStringLiteral("assistant");
        msg.content = QStringLiteral("I will run a command.");
        msg.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgSvc->addMessage(msg).isEmpty());

        ToolCall tc;
        tc.id = QStringLiteral("tc-001");
        tc.messageId = QStringLiteral("msg-tc-1");
        tc.toolName = QStringLiteral("run_shell");
        tc.arguments = QJsonObject{{QStringLiteral("command"), QStringLiteral("ls -la")}};
        tc.status = QStringLiteral("pending");
        tc.startedAt = QDateTime::currentDateTimeUtc();
        QCOMPARE(m_msgSvc->addToolCall(tc), tc.id);

        const QJsonValue result =
            QJsonObject{{QStringLiteral("stdout"), QStringLiteral("file.txt")}};
        QVERIFY(m_msgSvc->updateToolCallResult(
            QStringLiteral("tc-001"), result, QStringLiteral("success")));

        const auto toolCalls = m_msgSvc->getToolCalls(QStringLiteral("msg-tc-1"));
        QCOMPARE(toolCalls.size(), 1);
        QCOMPARE(toolCalls[0].status, QStringLiteral("success"));
        QVERIFY(toolCalls[0].result.isObject());
    }


    void testTurnIdRoundTrip() {
        Message u;
        u.id = QStringLiteral("user-1");
        u.conversationId = m_convId;
        u.role = QStringLiteral("user");
        u.content = QStringLiteral("hi");
        u.createdAt = QDateTime::currentDateTimeUtc();
        u.turnId = u.id;
        QCOMPARE(m_msgSvc->addMessage(u), u.id);

        Message a;
        a.id = QStringLiteral("asst-1");
        a.conversationId = m_convId;
        a.role = QStringLiteral("assistant");
        a.content = QStringLiteral("hello!");
        a.createdAt = QDateTime::currentDateTimeUtc().addSecs(1);
        a.turnId = u.id;
        QCOMPARE(m_msgSvc->addMessage(a), a.id);

        const auto loaded = m_msgSvc->getMessages(m_convId);
        QCOMPARE(loaded.size(), 2);
        for (const Message& m : loaded) {
            QCOMPARE(m.turnId, u.id);
        }
    }


    void testToolCallsForTurnGrouping() {
        Message u1;
        u1.id = QStringLiteral("u1");
        u1.conversationId = m_convId;
        u1.role = QStringLiteral("user");
        u1.content = QStringLiteral("q1");
        u1.createdAt = QDateTime::currentDateTimeUtc();
        u1.turnId = u1.id;
        QVERIFY(!m_msgSvc->addMessage(u1).isEmpty());

        Message a1a;
        a1a.id = QStringLiteral("a1a");
        a1a.conversationId = m_convId;
        a1a.role = QStringLiteral("assistant");
        a1a.content = QString();
        a1a.createdAt = QDateTime::currentDateTimeUtc().addSecs(1);
        a1a.turnId = u1.id;
        QVERIFY(!m_msgSvc->addMessage(a1a).isEmpty());

        Message a1b;
        a1b.id = QStringLiteral("a1b");
        a1b.conversationId = m_convId;
        a1b.role = QStringLiteral("assistant");
        a1b.content = QStringLiteral("done");
        a1b.createdAt = QDateTime::currentDateTimeUtc().addSecs(2);
        a1b.turnId = u1.id;
        QVERIFY(!m_msgSvc->addMessage(a1b).isEmpty());

        ToolCall tcA;
        tcA.id = QStringLiteral("tcA");
        tcA.messageId = a1a.id;
        tcA.toolName = QStringLiteral("read_file");
        tcA.arguments = QJsonObject{{QStringLiteral("path"), QStringLiteral("/x")}};
        tcA.status = QStringLiteral("pending");
        tcA.startedAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgSvc->addToolCall(tcA).isEmpty());

        ToolCall tcB;
        tcB.id = QStringLiteral("tcB");
        tcB.messageId = a1b.id;
        tcB.toolName = QStringLiteral("write_file");
        tcB.arguments = QJsonObject{{QStringLiteral("path"), QStringLiteral("/y")}};
        tcB.status = QStringLiteral("pending");
        tcB.startedAt = QDateTime::currentDateTimeUtc().addSecs(1);
        QVERIFY(!m_msgSvc->addToolCall(tcB).isEmpty());

        Message u2;
        u2.id = QStringLiteral("u2");
        u2.conversationId = m_convId;
        u2.role = QStringLiteral("user");
        u2.content = QStringLiteral("q2");
        u2.createdAt = QDateTime::currentDateTimeUtc().addSecs(10);
        u2.turnId = u2.id;
        QVERIFY(!m_msgSvc->addMessage(u2).isEmpty());

        Message a2;
        a2.id = QStringLiteral("a2");
        a2.conversationId = m_convId;
        a2.role = QStringLiteral("assistant");
        a2.content = QString();
        a2.createdAt = QDateTime::currentDateTimeUtc().addSecs(11);
        a2.turnId = u2.id;
        QVERIFY(!m_msgSvc->addMessage(a2).isEmpty());

        ToolCall tcC;
        tcC.id = QStringLiteral("tcC");
        tcC.messageId = a2.id;
        tcC.toolName = QStringLiteral("search_web");
        tcC.arguments = QJsonObject{{QStringLiteral("q"), QStringLiteral("z")}};
        tcC.status = QStringLiteral("pending");
        tcC.startedAt = QDateTime::currentDateTimeUtc().addSecs(11);
        QVERIFY(!m_msgSvc->addToolCall(tcC).isEmpty());

        auto turn1Calls = m_msgSvc->toolCallsForTurn(a1a.id);
        QCOMPARE(turn1Calls.size(), 2);
        QStringList names;
        for (const ToolCall& t : turn1Calls)
            names << t.toolName;
        QVERIFY(names.contains(QStringLiteral("read_file")));
        QVERIFY(names.contains(QStringLiteral("write_file")));
        QVERIFY(!names.contains(QStringLiteral("search_web")));

        auto turn2Calls = m_msgSvc->toolCallsForTurn(a2.id);
        QCOMPARE(turn2Calls.size(), 1);
        QCOMPARE(turn2Calls[0].toolName, QStringLiteral("search_web"));

        auto agentYCalls = m_msgSvc->toolCallsForTurn(a1a.id, QStringLiteral("agent-y"));
        QCOMPARE(agentYCalls.size(), 0);
    }


    void testToolCallsForConversation() {
        Message u;
        u.id = QStringLiteral("u-conv");
        u.conversationId = m_convId;
        u.role = QStringLiteral("user");
        u.content = QStringLiteral("q");
        u.createdAt = QDateTime::currentDateTimeUtc();
        u.turnId = u.id;
        QVERIFY(!m_msgSvc->addMessage(u).isEmpty());

        Message a;
        a.id = QStringLiteral("a-conv");
        a.conversationId = m_convId;
        a.role = QStringLiteral("assistant");
        a.content = QString();
        a.createdAt = QDateTime::currentDateTimeUtc().addSecs(1);
        a.turnId = u.id;
        QVERIFY(!m_msgSvc->addMessage(a).isEmpty());

        for (int i = 0; i < 3; ++i) {
            ToolCall tc;
            tc.id = QStringLiteral("conv-tc-%1").arg(i);
            tc.messageId = a.id;
            tc.toolName = QStringLiteral("tool_%1").arg(i);
            tc.arguments = QJsonObject{};
            tc.status = QStringLiteral("pending");
            tc.startedAt = QDateTime::currentDateTimeUtc().addSecs(i);
            QVERIFY(!m_msgSvc->addToolCall(tc).isEmpty());
        }

        const auto all = m_msgSvc->toolCallsForConversation(m_convId);
        QCOMPARE(all.size(), 3);
        QCOMPARE(all[0].toolName, QStringLiteral("tool_0"));
        QCOMPARE(all[2].toolName, QStringLiteral("tool_2"));
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
};

QTEST_MAIN(TestMessageService)
#include "test-message-service.moc"

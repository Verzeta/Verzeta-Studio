// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/message.h"
#include "../../backend/models/tool-call.h"
#include "../../backend/services/chat-controller.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/export-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QUuid>

class TestWalkAndPairPermutations : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ChatController> m_chat;
    QString m_convId;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    QString writeUser(const QString& convId, const QString& content) {
        Message m;
        m.id = uuid();
        m.conversationId = convId;
        m.role = QStringLiteral("user");
        m.content = content;
        m.createdAt = QDateTime::currentDateTimeUtc();
        return m_msgSvc->addMessage(m);
    }

    QString writeAssistant(const QString& convId,
                           const QString& content,
                           const QString& finishReason = QStringLiteral("stop")) {
        Message m;
        m.id = uuid();
        m.conversationId = convId;
        m.role = QStringLiteral("assistant");
        m.content = content;
        m.finishReason = finishReason;
        m.createdAt = QDateTime::currentDateTimeUtc();
        return m_msgSvc->addMessage(m);
    }

    QString writeToolCallRow(const QString& parentMsgId,
                             const QString& toolName,
                             const QJsonObject& args = {}) {
        ToolCall tc;
        tc.id = uuid();
        tc.messageId = parentMsgId;
        tc.toolName = toolName;
        tc.arguments = args;
        tc.status = QStringLiteral("running");
        tc.startedAt = QDateTime::currentDateTimeUtc();
        return m_msgSvc->addToolCall(tc);
    }

    QString writeToolMessage(const QString& convId,
                             const QString& toolCallId,
                             const QString& toolName,
                             const QString& resultJson) {
        Message m;
        m.id = uuid();
        m.conversationId = convId;
        m.role = QStringLiteral("tool");
        m.content = resultJson;
        m.createdAt = QDateTime::currentDateTimeUtc();
        m.metadata = QJsonObject{
            {QStringLiteral("tool_call_id"), toolCallId},
            {QStringLiteral("tool_name"), toolName},
        };
        return m_msgSvc->addMessage(m);
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
        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_convId = m_convSvc->createConversation(QStringLiteral("WalkPair"));
        QVERIFY(!m_convId.isEmpty());
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

    void test_partialPairing_stripsToolCalls_dropsToolRow() {
        writeUser(m_convId, QStringLiteral("do two things"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("on it"), QStringLiteral("tool_calls"));
        const QString tc1 =
            writeToolCallRow(asstId,
                             QStringLiteral("write_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/a")}});
        const QString tc2 =
            writeToolCallRow(asstId,
                             QStringLiteral("read_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/b")}});
        Q_UNUSED(tc2);

        writeToolMessage(
            m_convId, tc1, QStringLiteral("write_file"), QStringLiteral("{\"ok\":true}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QVERIFY2(out[1].toolCallsJson.isEmpty(),
                 "Partial pairing must STRIP tool_calls from the "
                 "assistant — emitting half a tool_calls list would "
                 "leave provider waiting for unpaired tool responses.");
    }

    void test_staleToolRow_endsWindow_partialPairFallback() {
        writeUser(m_convId, QStringLiteral("do a thing"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("on it"), QStringLiteral("tool_calls"));
        const QString tc1 = writeToolCallRow(asstId, QStringLiteral("write_file"), QJsonObject{});
        Q_UNUSED(tc1);

        const QString stale = QStringLiteral("stale-id-no-match");
        writeToolMessage(
            m_convId, stale, QStringLiteral("write_file"), QStringLiteral("{\"ok\":\"stale\"}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QVERIFY(out[1].toolCallsJson.isEmpty());
    }

    void test_assistantMidWindow_endsWindow() {
        writeUser(m_convId, QStringLiteral("hello"));
        const QString asst1 =
            writeAssistant(m_convId, QStringLiteral("first"), QStringLiteral("tool_calls"));
        writeToolCallRow(asst1, QStringLiteral("write_file"), QJsonObject{});

        writeAssistant(m_convId, QStringLiteral("second"), QStringLiteral("stop"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 3);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QVERIFY(out[1].toolCallsJson.isEmpty());
        QCOMPARE(out[2].role, QStringLiteral("assistant"));
        QCOMPARE(out[2].content, QStringLiteral("second"));
    }

    void test_liveHelper_producesInterveningSystem_walkAndPair() {
        writeUser(m_convId, QStringLiteral("start a poll please"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("starting poll"), QStringLiteral("tool_calls"));
        const QString tcId = writeToolCallRow(
            asstId,
            QStringLiteral("start_poll"),
            QJsonObject{{QStringLiteral("question"), QStringLiteral("Pick A or B?")}});

        QJsonObject meta;
        meta.insert(QStringLiteral("poll_id"), QStringLiteral("p-live-1"));
        meta.insert(QStringLiteral("produced_by"), QStringLiteral("poll_service"));
        const QString sysId = m_msgSvc->addInterveningSystemMessage(
            m_convId, QStringLiteral("Poll started: Pick A or B?"), meta);
        QVERIFY2(!sysId.isEmpty(),
                 "Helper must persist the row successfully — empty "
                 "id here would mean addMessage rejected the row, "
                 "breaking the documented contract.");

        writeToolMessage(
            m_convId, tcId, QStringLiteral("start_poll"), QStringLiteral("{\"id\":\"p-live-1\"}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 4);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QCOMPARE(out[1].toolCallsJson.size(), 1);
        QCOMPARE(out[2].role, QStringLiteral("tool"));
        QCOMPARE(out[2].toolCallId, tcId);
        QCOMPARE(out[3].role, QStringLiteral("system"));
        QVERIFY(out[3].content.contains(QStringLiteral("Poll started")));
    }

    void test_toolRowWithEmptyId_doesNotPair() {
        writeUser(m_convId, QStringLiteral("do a thing"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("on it"), QStringLiteral("tool_calls"));
        const QString tc1 = writeToolCallRow(asstId, QStringLiteral("write_file"), QJsonObject{});
        Q_UNUSED(tc1);

        writeToolMessage(m_convId,
                         QString{},
                         QStringLiteral("write_file"),
                         QStringLiteral("{\"oops\":\"no id\"}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QVERIFY(out[1].toolCallsJson.isEmpty());
    }
};

QTEST_MAIN(TestWalkAndPairPermutations)
#include "test-walk-and-pair-permutations.moc"

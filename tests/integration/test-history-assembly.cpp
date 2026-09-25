// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "models/db-manager.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/membership-service.h"
#include "services/message-service.h"
#include "services/model-router.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QUuid>

class CapturingMockProvider : public ILLMProvider {
    Q_OBJECT
  public:
    explicit CapturingMockProvider(QObject* parent = nullptr) : ILLMProvider(parent) {
        m_models = QStringList{QStringLiteral("mock-model")};
    }
    QString providerId() const override { return QStringLiteral("mock"); }
    QString displayName() const override { return QStringLiteral("Mock"); }
    QStringList availableModels() override { return m_models; }
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }
    void refreshModels() override { emit modelsRefreshed(m_models); }
    void cancelRequest() override {}

    void sendRequest(const LlmRequest& req) override {
        m_lastRequest = req;
        LlmChunk c;
        c.delta = QStringLiteral("ok");
        emit chunkReceived(c);
        emit requestFinished(QStringLiteral("stop"), 1);
    }

    const LlmRequest& lastRequest() const { return m_lastRequest; }

  private:
    QStringList m_models;
    LlmRequest m_lastRequest;
};

class TestHistoryAssembly : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ChatController> m_chat;
    CapturingMockProvider* m_provider = nullptr;
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
                           const QString& finishReason = QStringLiteral("stop"),
                           const QString& memberAlias = {}) {
        Message m;
        m.id = uuid();
        m.conversationId = convId;
        m.role = QStringLiteral("assistant");
        m.content = content;
        m.finishReason = finishReason;
        m.memberAlias = memberAlias;
        m.createdAt = QDateTime::currentDateTimeUtc();
        return m_msgSvc->addMessage(m);
    }

    QString
    writeToolCallRow(const QString& parentMsgId, const QString& toolName, const QJsonObject& args) {
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

    QString writeSystemTaskEvent(const QString& convId, const QString& content) {
        Message m;
        m.id = uuid();
        m.conversationId = convId;
        m.role = QStringLiteral("system");
        m.content = content;
        m.finishReason = QStringLiteral("task_event");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m.metadata = QJsonObject{
            {QStringLiteral("task_event"), true},
            {QStringLiteral("task_event_type"), QStringLiteral("started")},
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

        auto provider = std::make_unique<CapturingMockProvider>();
        m_provider = provider.get();
        m_router->registerProvider(std::move(provider));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_convId = m_convSvc->createConversation(QStringLiteral("Hist"));
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

    void test_vanillaHistory_passesThrough() {
        writeUser(m_convId, QStringLiteral("hello"));
        writeAssistant(m_convId, QStringLiteral("hi back"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[0].content, QStringLiteral("hello"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QCOMPARE(out[1].content, QStringLiteral("hi back"));
        QVERIFY(out[1].toolCallsJson.isEmpty());
    }

    void test_wellFormedToolPair_reconstructed() {
        writeUser(m_convId, QStringLiteral("write a file"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("I'll do it:"), QStringLiteral("tool_calls"));
        const QString tcId =
            writeToolCallRow(asstId,
                             QStringLiteral("write_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/x")}});
        QVERIFY(!tcId.isEmpty());
        writeToolMessage(m_convId,
                         tcId,
                         QStringLiteral("write_file"),
                         QStringLiteral("{\"path\":\"/tmp/x\",\"written\":true}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 3);
        QCOMPARE(out[0].role, QStringLiteral("user"));

        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QCOMPARE(out[1].content, QStringLiteral("I'll do it:"));
        QCOMPARE(out[1].toolCallsJson.size(), 1);
        const QJsonObject tcObj = out[1].toolCallsJson[0].toObject();
        QCOMPARE(tcObj[QStringLiteral("id")].toString(), tcId);
        QCOMPARE(tcObj[QStringLiteral("type")].toString(), QStringLiteral("function"));
        QCOMPARE(tcObj[QStringLiteral("function")].toObject()[QStringLiteral("name")].toString(),
                 QStringLiteral("write_file"));

        QCOMPARE(out[2].role, QStringLiteral("tool"));
        QCOMPARE(out[2].toolCallId, tcId);
        QVERIFY(out[2].content.contains(QStringLiteral("written")));
    }

    void test_multipleParallelToolCalls_reconstructed() {
        writeUser(m_convId, QStringLiteral("do two things"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("on it:"), QStringLiteral("tool_calls"));
        const QString tc1 =
            writeToolCallRow(asstId,
                             QStringLiteral("write_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/a")}});
        const QString tc2 =
            writeToolCallRow(asstId,
                             QStringLiteral("read_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/b")}});
        writeToolMessage(
            m_convId, tc1, QStringLiteral("write_file"), QStringLiteral("{\"ok\":true}"));
        writeToolMessage(
            m_convId, tc2, QStringLiteral("read_file"), QStringLiteral("{\"data\":\"hi\"}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 4);
        QCOMPARE(out[1].toolCallsJson.size(), 2);

        QCOMPARE(out[2].role, QStringLiteral("tool"));
        QCOMPARE(out[3].role, QStringLiteral("tool"));
        QSet<QString> emitted;
        emitted.insert(out[2].toolCallId);
        emitted.insert(out[3].toolCallId);
        QVERIFY(emitted.contains(tc1));
        QVERIFY(emitted.contains(tc2));
    }

    void test_legacyOrphanAssistant_strippedAndToolDropped() {
        writeUser(m_convId, QStringLiteral("legacy turn"));
        writeAssistant(m_convId, QStringLiteral("I'll do it:"), QStringLiteral("tool_calls"));
        writeToolMessage(m_convId,
                         QStringLiteral("dangling-id"),
                         QStringLiteral("write_file"),
                         QStringLiteral("{\"ok\":true}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QVERIFY(out[1].toolCallsJson.isEmpty());
    }

    void test_bareOrphanTool_dropped() {
        writeUser(m_convId, QStringLiteral("hi"));
        writeToolMessage(m_convId,
                         QStringLiteral("ghost-id"),
                         QStringLiteral("write_file"),
                         QStringLiteral("{\"ghost\":true}"));
        writeAssistant(m_convId, QStringLiteral("hello"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
    }

    void test_groupChat_prefixesAssistantWithAlias() {
        writeUser(m_convId, QStringLiteral("intro"));
        writeAssistant(m_convId,
                       QStringLiteral("hi everyone"),
                       QStringLiteral("stop"),
                       QStringLiteral("PM Alice"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, true, QString{});

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QCOMPARE(out[1].content, QStringLiteral("(PM_Alice said)\nhi everyone"));
    }

    void test_groupChat_stripsLeadingDoubledAliasPrefix() {
        writeUser(m_convId, QStringLiteral("intro"));
        writeAssistant(m_convId,
                       QStringLiteral("(PM_Alice said)\n(PM_Alice said)\nhello all"),
                       QStringLiteral("stop"),
                       QStringLiteral("PM Alice"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, true, QString{});

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QCOMPARE(out[1].content, QStringLiteral("(PM_Alice said)\nhello all"));
    }

    void test_groupChat_toolPairWithAliasPrefix() {
        writeUser(m_convId, QStringLiteral("write a file"));
        const QString engId = writeAssistant(m_convId,
                                             QStringLiteral("on it:"),
                                             QStringLiteral("tool_calls"),
                                             QStringLiteral("Engineer"));
        const QString tcId =
            writeToolCallRow(engId,
                             QStringLiteral("write_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/x")}});
        writeToolMessage(
            m_convId, tcId, QStringLiteral("write_file"), QStringLiteral("{\"ok\":true}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, true, QString{});

        QCOMPARE(out.size(), 3);
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QCOMPARE(out[1].content, QStringLiteral("(Engineer said)\non it:"));
        QCOMPARE(out[1].toolCallsJson.size(), 1);
        QCOMPARE(out[2].role, QStringLiteral("tool"));
        QCOMPARE(out[2].toolCallId, tcId);
    }

    void test_excludeMsgId_isHonored() {
        const QString u = writeUser(m_convId, QStringLiteral("hi"));
        const QString a1 = writeAssistant(m_convId, QStringLiteral("first"));
        const QString a2 = writeAssistant(m_convId, QStringLiteral("second"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, a2);

        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].content, QStringLiteral("hi"));
        QCOMPARE(out[1].content, QStringLiteral("first"));
        Q_UNUSED(u);
    }

    void test_endToEnd_providerSeesPairedHistory() {
        writeUser(m_convId, QStringLiteral("write a file"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("on it:"), QStringLiteral("tool_calls"));
        const QString tcId =
            writeToolCallRow(asstId,
                             QStringLiteral("write_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/x")}});
        writeToolMessage(
            m_convId, tcId, QStringLiteral("write_file"), QStringLiteral("{\"ok\":true}"));

        m_chat->sendMessage(QStringLiteral("more please"), {});

        const LlmRequest& req = m_provider->lastRequest();
        QVERIFY(req.messages.size() >= 4);

        bool foundAssistantWithCalls = false;
        bool foundToolWithId = false;
        for (const LlmMessage& m : req.messages) {
            if (m.role == QStringLiteral("assistant") && !m.toolCallsJson.isEmpty()) {
                foundAssistantWithCalls = true;
                const QJsonObject tcObj = m.toolCallsJson[0].toObject();
                QCOMPARE(tcObj[QStringLiteral("id")].toString(), tcId);
            }
            if (m.role == QStringLiteral("tool") && m.toolCallId == tcId) {
                foundToolWithId = true;
            }
        }
        QVERIFY2(foundAssistantWithCalls,
                 "assistant tool_calls field never populated in outbound request");
        QVERIFY2(foundToolWithId, "tool message tool_call_id never populated in outbound request");
    }

    void test_bug6_layerB_toleratesSystemRowBetweenAssistantAndTool() {
        writeUser(m_convId, QStringLiteral("write a file please"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("I'll do it:"), QStringLiteral("tool_calls"));
        const QString tcId =
            writeToolCallRow(asstId,
                             QStringLiteral("write_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/x")}});
        QVERIFY(!tcId.isEmpty());

        writeSystemTaskEvent(m_convId, QStringLiteral("🎯 Task started: demo"));

        writeToolMessage(m_convId,
                         tcId,
                         QStringLiteral("write_file"),
                         QStringLiteral("{\"path\":\"/tmp/x\",\"written\":true}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 4);
        QCOMPARE(out[0].role, QStringLiteral("user"));

        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QCOMPARE(out[1].toolCallsJson.size(), 1);
        QCOMPARE(out[1].toolCallsJson[0].toObject()[QStringLiteral("id")].toString(), tcId);

        QCOMPARE(out[2].role, QStringLiteral("tool"));
        QCOMPARE(out[2].toolCallId, tcId);
        QVERIFY(out[2].content.contains(QStringLiteral("written")));

        QCOMPARE(out[3].role, QStringLiteral("system"));
        QVERIFY(out[3].content.contains(QStringLiteral("Task started")));
    }

    void test_bug6_layerB_toleratesMultipleSystemRowsInToolWindow() {
        writeUser(m_convId, QStringLiteral("do two things"));
        const QString asstId =
            writeAssistant(m_convId, QStringLiteral("on it:"), QStringLiteral("tool_calls"));
        const QString tc1 =
            writeToolCallRow(asstId,
                             QStringLiteral("write_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/a")}});
        const QString tc2 =
            writeToolCallRow(asstId,
                             QStringLiteral("read_file"),
                             QJsonObject{{QStringLiteral("path"), QStringLiteral("/b")}});

        writeSystemTaskEvent(m_convId, QStringLiteral("🎯 event 1"));
        writeToolMessage(
            m_convId, tc1, QStringLiteral("write_file"), QStringLiteral("{\"ok\":true}"));
        writeSystemTaskEvent(m_convId, QStringLiteral("🎯 event 2"));
        writeToolMessage(
            m_convId, tc2, QStringLiteral("read_file"), QStringLiteral("{\"data\":\"hi\"}"));

        const auto db = m_msgSvc->getMessages(m_convId);
        const auto out = m_chat->assembleLlmHistory(db, false, QString{});

        QCOMPARE(out.size(), 6);
        QCOMPARE(out[0].role, QStringLiteral("user"));
        QCOMPARE(out[1].role, QStringLiteral("assistant"));
        QCOMPARE(out[1].toolCallsJson.size(), 2);

        QCOMPARE(out[2].role, QStringLiteral("tool"));
        QCOMPARE(out[3].role, QStringLiteral("tool"));
        QCOMPARE(out[4].role, QStringLiteral("system"));
        QCOMPARE(out[5].role, QStringLiteral("system"));
    }
};

QTEST_MAIN(TestHistoryAssembly)
#include "test-history-assembly.moc"

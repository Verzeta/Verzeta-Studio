// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "api/tool-calling-schema.h"
#include "helpers/scripted-mock-provider.h"
#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/message-list-model.h"
#include "models/message.h"
#include "models/tool-call.h"
#include "services/chat-controller.h"
#include "services/chat/tool-dispatcher.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/tool-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestToolDispatcher : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provider = nullptr;
    QString m_convId;

    void sendAndWait(const QString& text, int timeoutMs = 10000) {
        m_chat->sendMessage(text, {});
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < timeoutMs) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(),
                 qPrintable(QStringLiteral("ChatController never finished generating within %1ms")
                                .arg(timeoutMs)));
    }

    QList<Message> currentMessages() const { return m_msgSvc->getRecentMessages(m_convId, 200); }

    void registerEchoTool() {
        ToolSchema schema;
        schema.name = QStringLiteral("echo");
        schema.description = QStringLiteral("Returns its arguments back.");
        ToolParameterSchema p;
        p.name = QStringLiteral("payload");
        p.type = QStringLiteral("string");
        p.description = QStringLiteral("Any string to echo back.");
        p.required = false;
        schema.parameters.append(p);

        m_toolSvc->registerTool(schema, [](const QJsonObject& args) -> QJsonValue {
            QJsonObject out;
            out.insert(QStringLiteral("echoed"), args);
            return out;
        });
    }

    static QJsonObject makeEchoCall(const QString& id, const QString& payload) {
        QJsonObject args;
        args.insert(QStringLiteral("payload"), payload);
        QJsonObject call;
        call.insert(QStringLiteral("id"), id);
        call.insert(QStringLiteral("name"), QStringLiteral("echo"));
        call.insert(QStringLiteral("arguments"), args);
        return call;
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() + QStringLiteral("/td_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();
        m_toolSvc = std::make_unique<ToolService>();

        auto provider = std::make_unique<ScriptedMockProvider>();
        m_provider = provider.get();
        m_router->registerProvider(std::move(provider));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_convId = m_convSvc->createConversation(QStringLiteral("TestTD"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);
    }

    void cleanup() {
        m_chat.reset();
        m_toolSvc.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void testToolDispatch_singleCall_persistsTripleAndResumes() {
        registerEchoTool();
        m_chat->setToolService(m_toolSvc.get());

        ScriptedMockProvider::ScriptStep toolStep;
        toolStep.chunks = {};
        toolStep.finishReason = QStringLiteral("tool_calls");
        toolStep.tokens = 1;
        toolStep.toolCallsJson = {
            makeEchoCall(QStringLiteral("call_0"), QStringLiteral("hi")),
        };

        ScriptedMockProvider::ScriptStep finalStep;
        finalStep.chunks = {QStringLiteral("all done")};
        finalStep.finishReason = QStringLiteral("stop");
        finalStep.tokens = 2;

        m_provider->setScript({toolStep, finalStep});

        sendAndWait(QStringLiteral("go"));

        QCOMPARE(m_provider->stepsConsumed(), 2);

        const QList<Message> rows = currentMessages();

        QStringList roles;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("user") || m.role == QStringLiteral("assistant") ||
                m.role == QStringLiteral("tool")) {
                roles << m.role;
            }
        }
        QCOMPARE(roles.size(), 4);
        QCOMPARE(roles[0], QStringLiteral("user"));
        QCOMPARE(roles[1], QStringLiteral("assistant"));
        QCOMPARE(roles[2], QStringLiteral("tool"));
        QCOMPARE(roles[3], QStringLiteral("assistant"));

        QString parentAssistantId;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant") &&
                m.finishReason == QStringLiteral("tool_calls")) {
                parentAssistantId = m.id;
                break;
            }
        }
        QVERIFY2(!parentAssistantId.isEmpty(),
                 "No assistant row with finishReason=tool_calls persisted");

        const QList<ToolCall> calls = m_msgSvc->getToolCalls(parentAssistantId);
        QCOMPARE(calls.size(), 1);
        QCOMPARE(calls.first().toolName, QStringLiteral("echo"));
        QCOMPARE(calls.first().messageId, parentAssistantId);
        QCOMPARE(calls.first().status, QStringLiteral("success"));

        const Message& lastAssistant = rows.last();
        QCOMPARE(lastAssistant.role, QStringLiteral("assistant"));
        QCOMPARE(lastAssistant.content.trimmed(), QStringLiteral("all done"));
    }


    void testToolDispatch_multiBatch_sharedParent_singleContinuation() {
        registerEchoTool();
        m_chat->setToolService(m_toolSvc.get());

        ScriptedMockProvider::ScriptStep batchStep;
        batchStep.chunks = {};
        batchStep.finishReason = QStringLiteral("tool_calls");
        batchStep.tokens = 1;
        batchStep.toolCallsJson = {
            makeEchoCall(QStringLiteral("call_A"), QStringLiteral("a")),
            makeEchoCall(QStringLiteral("call_B"), QStringLiteral("b")),
            makeEchoCall(QStringLiteral("call_C"), QStringLiteral("c")),
        };

        ScriptedMockProvider::ScriptStep finalStep;
        finalStep.chunks = {QStringLiteral("batch drained")};
        finalStep.finishReason = QStringLiteral("stop");
        finalStep.tokens = 3;

        m_provider->setScript({batchStep, finalStep});

        sendAndWait(QStringLiteral("run three tools"), 15000);

        QCOMPARE(m_provider->stepsConsumed(), 2);

        const QList<Message> rows = currentMessages();

        QString parentId;
        int parentIdx = -1;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].role == QStringLiteral("assistant") &&
                rows[i].finishReason == QStringLiteral("tool_calls")) {
                parentId = rows[i].id;
                parentIdx = i;
                break;
            }
        }
        QVERIFY2(!parentId.isEmpty(), "No assistant row with finishReason=tool_calls persisted");

        const QList<ToolCall> calls = m_msgSvc->getToolCalls(parentId);
        QCOMPARE(calls.size(), 3);
        for (const ToolCall& tc : calls) {
            QCOMPARE(tc.messageId, parentId);
            QCOMPARE(tc.toolName, QStringLiteral("echo"));
            QCOMPARE(tc.status, QStringLiteral("success"));
        }

        QCOMPARE(rows.value(parentIdx + 1).role, QStringLiteral("tool"));
        QCOMPARE(rows.value(parentIdx + 2).role, QStringLiteral("tool"));
        QCOMPARE(rows.value(parentIdx + 3).role, QStringLiteral("tool"));

        QCOMPARE(rows.last().role, QStringLiteral("assistant"));
        QCOMPARE(rows.last().content.trimmed(), QStringLiteral("batch drained"));

        QSet<QString> expectedIds;
        for (const ToolCall& tc : calls)
            expectedIds.insert(tc.id);
        for (int i = parentIdx + 1; i <= parentIdx + 3; ++i) {
            const QString tcid = rows[i].metadata[QStringLiteral("tool_call_id")].toString();
            QVERIFY2(expectedIds.contains(tcid),
                     qPrintable(QStringLiteral("role=tool row at index %1 has tool_call_id=%2 "
                                               "that is NOT in the side-table — would "
                                               "be dropped on next turn (regression guard)")
                                    .arg(i)
                                    .arg(tcid)));
        }
    }


    void testToolDispatch_iterationCap_halts() {
        registerEchoTool();
        m_chat->setToolService(m_toolSvc.get());

        const int kSteps = Chat::ToolDispatcher::kDefaultToolIterations + 5;
        QList<ScriptedMockProvider::ScriptStep> loop;
        for (int i = 0; i < kSteps; ++i) {
            ScriptedMockProvider::ScriptStep step;
            step.chunks = {};
            step.finishReason = QStringLiteral("tool_calls");
            step.tokens = 1;
            step.toolCallsJson = {
                makeEchoCall(QStringLiteral("call_loop_%1").arg(i),
                             QStringLiteral("iter%1").arg(i)),
            };
            loop.append(step);
        }
        ScriptedMockProvider::ScriptStep stopStep;
        stopStep.chunks = {QStringLiteral("finally stopping")};
        stopStep.finishReason = QStringLiteral("stop");
        stopStep.tokens = 1;
        loop.append(stopStep);

        m_provider->setScript(loop);

        sendAndWait(QStringLiteral("spin"), 20000);

        const int kBound = Chat::ToolDispatcher::kDefaultToolIterations + 1;
        QVERIFY2(m_provider->stepsConsumed() <= kBound,
                 qPrintable(QStringLiteral("Dispatch consumed %1 scripted steps — expected <=%2 "
                                           "(default budget + optional continuation)")
                                .arg(m_provider->stepsConsumed())
                                .arg(kBound)));

        QCOMPARE(m_chat->isGenerating(), false);
    }


    void testToolDispatch_noToolService_gracefulSkip() {
        QVERIFY(m_chat != nullptr);

        ScriptedMockProvider::ScriptStep step;
        step.chunks = {QStringLiteral("i would call a tool")};
        step.finishReason = QStringLiteral("tool_calls");
        step.tokens = 1;
        step.toolCallsJson = {
            makeEchoCall(QStringLiteral("call_x"), QStringLiteral("doesn't matter")),
        };
        m_provider->setScript({step});

        sendAndWait(QStringLiteral("try a tool"));

        QCOMPARE(m_provider->stepsConsumed(), 1);

        const QList<Message> rows = currentMessages();

        for (const Message& m : rows) {
            QVERIFY2(m.role != QStringLiteral("tool"),
                     qPrintable(QStringLiteral("Unexpected role=tool row without ToolService: "
                                               "content=%1")
                                    .arg(m.content.left(80))));
        }

        bool sawToolCallsAssistant = false;
        for (const Message& m : rows) {
            if (m.role == QStringLiteral("assistant") &&
                m.finishReason == QStringLiteral("tool_calls")) {
                sawToolCallsAssistant = true;
                const QList<ToolCall> calls = m_msgSvc->getToolCalls(m.id);
                QCOMPARE(calls.size(), 0);
            }
        }
        QVERIFY2(sawToolCallsAssistant,
                 "Expected the assistant row that declared tool_calls "
                 "to be persisted even with no ToolService attached");

        QCOMPARE(m_chat->isGenerating(), false);
    }
};

QTEST_MAIN(TestToolDispatcher)
#include "test-tool-dispatcher.moc"

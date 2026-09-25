// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "models/db-manager.h"
#include "models/message.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/rag-service.h"
#include "services/search-service.h"
#include "services/settings-service.h"
#include "services/subagent-run-service.h"
#include "services/task-gate-service.h"
#include "services/tool-service.h"
#include "tools/tool-registration-context.h"
#include "tools/tool-registration.h"
#include "utils/http-client.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QUuid>

class TestMemoryToolHandlers : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<EmbeddingWorker> m_embWorker;
    std::unique_ptr<RagService> m_ragSvc;
    std::unique_ptr<SearchService> m_searchSvc;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<TaskGateService> m_taskGateSvc;
    std::unique_ptr<ChatController> m_chat;
    QString m_convId;

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
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();
        m_embWorker = std::make_unique<EmbeddingWorker>();
        m_ragSvc = std::make_unique<RagService>(DbManager::instance(), *m_embWorker);
        m_searchSvc = std::make_unique<SearchService>(DbManager::instance(), *m_ragSvc);
        m_toolSvc = std::make_unique<ToolService>();

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_chat->setToolService(m_toolSvc.get());

        m_taskGateSvc = std::make_unique<TaskGateService>(*m_msgSvc);
        m_chat->setTaskGateService(m_taskGateSvc.get());

        m_convId = m_convSvc->createConversation(QStringLiteral("MemoryToolsTest"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);

        Tools::RegistrationContext ctx;
        ctx.searchService = m_searchSvc.get();
        ctx.msgService = m_msgSvc.get();
        ctx.convService = m_convSvc.get();
        ctx.chatController = m_chat.get();
        Tools::registerAllBuiltInTools(*m_toolSvc, ctx);
    }

    void cleanup() {
        m_chat.reset();
        m_taskGateSvc.reset();
        m_toolSvc.reset();
        m_searchSvc.reset();
        m_ragSvc.reset();
        m_embWorker.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_searchMessages_emptyQuery_returnsError() {
        QJsonObject args;
        args[QStringLiteral("query")] = QString();
        const QJsonValue result = m_toolSvc->invokeTool(QStringLiteral("search_messages"), args);
        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_searchMessages_validQuery_returnsMatchesAndCount() {
        QJsonObject args;
        args[QStringLiteral("query")] = QStringLiteral("anything");
        const QJsonValue result = m_toolSvc->invokeTool(QStringLiteral("search_messages"), args);
        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QVERIFY(obj.contains(QStringLiteral("matches")));
        QVERIFY(obj.contains(QStringLiteral("count")));
        QVERIFY(obj[QStringLiteral("matches")].isArray());
    }

    void test_readConversation_currentKeyword_resolvesActiveConv() {
        Message m;
        m.id = uuid();
        m.conversationId = m_convId;
        m.role = QStringLiteral("user");
        m.content = QStringLiteral("hello memory tools");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m_msgSvc->addMessage(m);

        QJsonObject args;
        args[QStringLiteral("conversation_id")] = QStringLiteral("current");
        const QJsonValue result = m_toolSvc->invokeTool(QStringLiteral("read_conversation"), args);

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("conversationId")].toString(), m_convId);
        QVERIFY(obj.contains(QStringLiteral("messages")));
        QVERIFY(obj[QStringLiteral("messages")].isArray());
        QVERIFY(obj[QStringLiteral("count")].toInt() >= 1);
    }

    void test_readConversation_explicitConvId_bypassesCurrentResolution() {
        const QString otherConvId =
            m_convSvc->createConversation(QStringLiteral("MemoryToolsTest-Other"));
        QVERIFY(!otherConvId.isEmpty());

        Message m;
        m.id = uuid();
        m.conversationId = otherConvId;
        m.role = QStringLiteral("assistant");
        m.content = QStringLiteral("other conv payload");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m_msgSvc->addMessage(m);

        QJsonObject args;
        args[QStringLiteral("conversation_id")] = otherConvId;
        const QJsonValue result = m_toolSvc->invokeTool(QStringLiteral("read_conversation"), args);

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("conversationId")].toString(), otherConvId);
        QVERIFY(obj[QStringLiteral("messages")].isArray());
        QVERIFY(obj[QStringLiteral("count")].toInt() >= 1);
    }

    void test_listProjectConversations_noProject_returnsError() {
        const QJsonValue result =
            m_toolSvc->invokeTool(QStringLiteral("list_project_conversations"), {});
        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_subagentTools_registerViaMainRegistrationContext() {
        QVERIFY(!m_toolSvc->hasTool(QStringLiteral("spawn_subagent")));
        QVERIFY(!m_toolSvc->hasTool(QStringLiteral("check_subagent")));

        SettingsService settings(DbManager::instance());
        SubagentRunService runSvc(DbManager::instance(), *m_toolSvc, *m_router, settings);

        ToolService freshSvc;
        Tools::RegistrationContext ctx;
        ctx.searchService = m_searchSvc.get();
        ctx.msgService = m_msgSvc.get();
        ctx.convService = m_convSvc.get();
        ctx.chatController = m_chat.get();
        ctx.subagentService = &runSvc;
        Tools::registerAllBuiltInTools(freshSvc, ctx);

        QVERIFY(freshSvc.hasTool(QStringLiteral("spawn_subagent")));
        QVERIFY(freshSvc.hasTool(QStringLiteral("check_subagent")));
    }
};

QTEST_MAIN(TestMemoryToolHandlers)
#include "test-memory-tool-handlers.moc"

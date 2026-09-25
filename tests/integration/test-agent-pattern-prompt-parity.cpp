// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "models/db-manager.h"
#include "models/message-list-model.h"
#include "services/agent-service.h"
#include "services/agent-settings-controller.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/file-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/rag-service.h"
#include "services/tool-service.h"
#include "utils/process-sandbox.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QVariantMap>

class TestAgentPatternPromptParity : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ToolService> m_tools;
    std::unique_ptr<ProcessSandbox> m_sandbox;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<EmbeddingWorker> m_worker;
    std::unique_ptr<RagService> m_rag;
    std::unique_ptr<AgentService> m_agent;
    std::unique_ptr<AgentSettingsController> m_agentSettings;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_provider = nullptr;
    QString m_convId;

    void sendAndWait(const QString& text) {
        m_chat->sendMessage(text, {});
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < 3000) {
            QTest::qWait(10);
        }
        QVERIFY2(!m_chat->isGenerating(), "ChatController never finished generating within 3s");
    }

    static ScriptedMockProvider::ScriptStep
    stopStep(const QString& reply = QStringLiteral("done")) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {reply};
        s.finishReason = QStringLiteral("stop");
        s.tokens = 1;
        return s;
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/ppar_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

        m_tools = std::make_unique<ToolService>();
        m_sandbox = std::make_unique<ProcessSandbox>();
        m_fileSvc = std::make_unique<FileService>();
        m_tools->registerBuiltInTools(*m_sandbox, *m_fileSvc);

        m_worker = std::make_unique<EmbeddingWorker>();
        m_rag = std::make_unique<RagService>(DbManager::instance(), *m_worker);

        m_agent = std::make_unique<AgentService>(*m_router, *m_tools, *m_rag);

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_chat->setAgentService(m_agent.get());

        m_agentSettings = std::make_unique<AgentSettingsController>(*m_convSvc, *m_router);
        QObject::connect(
            m_chat.get(),
            &ChatController::activeConversationChanged,
            m_agentSettings.get(),
            [this]() { m_agentSettings->setActiveConversationId(m_chat->activeConversationId()); });
        QObject::connect(m_agentSettings.get(),
                         &AgentSettingsController::agentPatternChanged,
                         m_chat.get(),
                         &ChatController::onExternalAgentPatternChanged);

        m_convId = m_convSvc->createConversation(QStringLiteral("prompt-parity-test"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);

        QVariantMap cfg;
        cfg[QStringLiteral("systemPrompt")] =
            QStringLiteral("<CUSTOM-SYS-PROMPT-MARKER-AGENT-TEST>");
        m_agentSettings->saveConversationConfig(cfg);
    }

    void cleanup() {
        m_chat.reset();
        m_agentSettings.reset();
        m_agent.reset();
        m_rag.reset();
        m_worker.reset();
        m_fileSvc.reset();
        m_sandbox.reset();
        m_tools.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_agentPattern_systemPromptHasLayeredPieces() {
        m_agentSettings->setAgentPattern(QStringLiteral("react"));
        m_provider->setScript({stopStep()});

        sendAndWait(QStringLiteral("hello from agent"));

        QCOMPARE(m_provider->capturedRequests().size(), 1);
        const LlmRequest& req = m_provider->capturedRequests().first();

        QVERIFY2(req.systemPrompt.contains(QStringLiteral("<CUSTOM-SYS-PROMPT-MARKER-AGENT-TEST>")),
                 qPrintable(QStringLiteral("conversation override missing from agent-path "
                                           "system prompt. Got first 400 chars:\n%1")
                                .arg(req.systemPrompt.left(400))));

        QVERIFY2(req.systemPrompt.contains(QStringLiteral("Now (UTC):")),
                 qPrintable(QStringLiteral("Agent-path system prompt missing 'Now (UTC):' "
                                           "time layer. RequestBuilder::buildRequest is "
                                           "supposed to prepend it unconditionally.\n"
                                           "Full prompt first 800 chars:\n%1")
                                .arg(req.systemPrompt.left(800))));
    }


    void test_directPattern_systemPromptHasTimeLayer() {
        m_agentSettings->setAgentPattern(QStringLiteral("direct"));
        m_provider->setScript({stopStep()});

        sendAndWait(QStringLiteral("hello from direct"));

        QCOMPARE(m_provider->capturedRequests().size(), 1);
        const LlmRequest& req = m_provider->capturedRequests().first();
        QVERIFY2(req.systemPrompt.contains(QStringLiteral("Now (UTC):")),
                 qPrintable(QStringLiteral("Direct path missing 'Now (UTC):' marker — "
                                           "RequestBuilder regression.\nFull prompt first "
                                           "800 chars:\n%1")
                                .arg(req.systemPrompt.left(800))));
        QVERIFY(req.systemPrompt.contains(QStringLiteral("<CUSTOM-SYS-PROMPT-MARKER-AGENT-TEST>")));
    }
};

QTEST_MAIN(TestAgentPatternPromptParity)
#include "test-agent-pattern-prompt-parity.moc"

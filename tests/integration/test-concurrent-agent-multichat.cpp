// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/tool-calling-schema.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/llm-config.h"
#include "../../backend/models/message.h"
#include "../../backend/services/agent-service.h"
#include "../../backend/services/chat-controller.h"
#include "../../backend/services/chat/provider-scheduler.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/export-service.h"
#include "../../backend/services/file-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/rag-service.h"
#include "../../backend/services/tool-service.h"
#include "../../backend/utils/process-sandbox.h"
#include "../../backend/workers/embedding-worker.h"
#include "helpers/scripted-mock-provider.h"

#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>
#include <QDateTime>
#include <QSqlDatabase>

class TestConcurrentAgentMultichat : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<Chat::ProviderScheduler> m_sched;
    std::unique_ptr<ToolService> m_tools;
    std::unique_ptr<ProcessSandbox> m_sandbox;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<EmbeddingWorker> m_worker;
    std::unique_ptr<RagService> m_rag;
    std::unique_ptr<AgentService> m_agentFacade;
    std::unique_ptr<ChatController> m_ccA;
    std::unique_ptr<ChatController> m_ccB;
    ScriptedMockProvider* m_providerX = nullptr;
    ScriptedMockProvider* m_providerY = nullptr;

    std::atomic<Qt::HANDLE> m_toolThreadA{nullptr};
    std::atomic<Qt::HANDLE> m_toolThreadB{nullptr};

    static ScriptedMockProvider::ScriptStep textStep(const QString& t) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {t};
        s.finishReason = QStringLiteral("stop");
        return s;
    }

    static ScriptedMockProvider::ScriptStep toolStep(const QString& toolName,
                                                     const QString& callId) {
        ScriptedMockProvider::ScriptStep s;
        s.finishReason = QStringLiteral("tool_calls");
        QJsonObject tc;
        tc[QStringLiteral("id")] = callId;
        tc[QStringLiteral("name")] = toolName;
        tc[QStringLiteral("arguments")] = QJsonObject{};
        s.toolCallsJson = {tc};
        return s;
    }

    void pump(int ms = 60) { QTest::qWait(ms); }

    void
    pinConvToAgentProvider(const QString& convId, const QString& providerId, const QString& model) {
        LlmConfig cfg;
        cfg.providerId = providerId;
        cfg.modelName = model;
        cfg.stream = true;
        cfg.agentPattern = QStringLiteral("react");
        cfg.toolsEnabled = true;
        m_convSvc->updateLlmConfig(convId, cfg);
    }

    int assistantRowCount(const QString& convId) const {
        int n = 0;
        for (const Message& m : m_msgSvc->getMessages(convId)) {
            if (m.role == QStringLiteral("assistant"))
                ++n;
        }
        return n;
    }

  private slots:
    void init() {
        m_dbPath =
            m_tempDir.path() +
            QStringLiteral("/concurrent_agent_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();
        m_sched = std::make_unique<Chat::ProviderScheduler>();

        auto px = std::make_unique<ScriptedMockProvider>(QStringLiteral("provX"),
                                                         QStringList{QStringLiteral("model-x")});
        m_providerX = px.get();
        m_router->registerProvider(std::move(px));
        auto py = std::make_unique<ScriptedMockProvider>(QStringLiteral("provY"),
                                                         QStringList{QStringLiteral("model-y")});
        m_providerY = py.get();
        m_router->registerProvider(std::move(py));
        m_router->setActiveProvider(QStringLiteral("provX"), QStringLiteral("model-x"));

        m_tools = std::make_unique<ToolService>();
        m_sandbox = std::make_unique<ProcessSandbox>();
        m_fileSvc = std::make_unique<FileService>();
        m_tools->registerBuiltInTools(*m_sandbox, *m_fileSvc);
        m_worker = std::make_unique<EmbeddingWorker>();
        m_rag = std::make_unique<RagService>(DbManager::instance(), *m_worker);

        Qt::HANDLE mainThread = QThread::currentThreadId();
        Q_UNUSED(mainThread)
        ToolSchema schemaA;
        schemaA.name = QStringLiteral("residency_probe_a");
        schemaA.description = QStringLiteral("records its thread (conv A)");
        m_tools->registerTool(schemaA, [this](const QJsonObject&) -> QJsonValue {
            m_toolThreadA.store(QThread::currentThreadId());
            return QJsonObject{{QStringLiteral("ok"), true}};
        });
        m_tools->setRunsOnMainThread(QStringLiteral("residency_probe_a"), true);

        ToolSchema schemaB;
        schemaB.name = QStringLiteral("residency_probe_b");
        schemaB.description = QStringLiteral("records its thread (conv B)");
        m_tools->registerTool(schemaB, [this](const QJsonObject&) -> QJsonValue {
            m_toolThreadB.store(QThread::currentThreadId());
            return QJsonObject{{QStringLiteral("ok"), true}};
        });
        m_tools->setRunsOnMainThread(QStringLiteral("residency_probe_b"), true);

        m_agentFacade = std::make_unique<AgentService>(*m_router, *m_tools, *m_rag);

        m_ccA = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_ccB = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        for (ChatController* cc : {m_ccA.get(), m_ccB.get()}) {
            cc->setToolService(m_tools.get());
            cc->setRagService(m_rag.get());
            cc->setAgentService(m_agentFacade.get());
            cc->setProviderScheduler(m_sched.get());
        }
    }

    void cleanup() {
        m_ccA.reset();
        m_ccB.reset();
        m_agentFacade.reset();
        m_rag.reset();
        m_worker.reset();
        m_fileSvc.reset();
        m_sandbox.reset();
        m_tools.reset();
        m_sched.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
        m_toolThreadA.store(nullptr);
        m_toolThreadB.store(nullptr);
    }

    void test_agentPattern_differentProviders_bothInFlightAtOnce() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToAgentProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToAgentProvider(b, QStringLiteral("provY"), QStringLiteral("model-y"));

        m_providerX->setHoldInFlight(true);
        m_providerY->setHoldInFlight(true);
        m_providerX->setScript({textStep(QStringLiteral("x-final"))});
        m_providerY->setScript({textStep(QStringLiteral("y-final"))});

        m_ccA->switchConversation(a);
        m_ccB->switchConversation(b);
        m_ccA->sendMessage(QStringLiteral("agent A go"), {});
        m_ccB->sendMessage(QStringLiteral("agent B go"), {});
        pump();

        QVERIFY2(m_providerX->hasHeldRequest(),
                 "agent A never reached provider X (agent path serialized?)");
        QVERIFY2(m_providerY->hasHeldRequest(),
                 "agent B never reached provider Y (agent path serialized?)");
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provY")), 1);
        QVERIFY(m_ccA->isConversationGenerating(a));
        QVERIFY(m_ccB->isConversationGenerating(b));

        m_providerX->releaseHeld();
        m_providerY->releaseHeld();
        pump();
        QCOMPARE(assistantRowCount(a), 1);
        QCOMPARE(assistantRowCount(b), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 0);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provY")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_agentPattern_sameProvider_serializes_thenAutoDispatch() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToAgentProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToAgentProvider(b, QStringLiteral("provX"), QStringLiteral("model-x"));

        m_providerX->setHoldInFlight(true);
        m_providerX->setScript(
            {textStep(QStringLiteral("first")), textStep(QStringLiteral("second"))});

        m_ccA->switchConversation(a);
        m_ccB->switchConversation(b);
        m_ccA->sendMessage(QStringLiteral("agent A"), {});
        m_ccB->sendMessage(QStringLiteral("agent B"), {});
        pump();

        QVERIFY2(m_providerX->hasHeldRequest(), "first agent step never reached the provider");
        QCOMPARE(m_providerX->stepsConsumed(), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 1);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provX")), 1);

        m_providerX->releaseHeld();
        pump();
        QCOMPARE(assistantRowCount(a), 1);
        QCOMPARE(m_providerX->stepsConsumed(), 2);
        QVERIFY2(m_providerX->hasHeldRequest(),
                 "second agent did not auto-dispatch after the first freed "
                 "the provider slot");
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 1);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provX")), 0);

        m_providerX->releaseHeld();
        pump();
        QCOMPARE(assistantRowCount(b), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_agentPattern_residencyAwareTool_runsOnMainThread_perRun() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToAgentProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToAgentProvider(b, QStringLiteral("provY"), QStringLiteral("model-y"));

        m_providerX->setScript({toolStep(QStringLiteral("residency_probe_a"),
                                         QStringLiteral("11111111-1111-1111-1111-111111111111")),
                                textStep(QStringLiteral("A-done"))});
        m_providerY->setScript({toolStep(QStringLiteral("residency_probe_b"),
                                         QStringLiteral("22222222-2222-2222-2222-222222222222")),
                                textStep(QStringLiteral("B-done"))});

        const Qt::HANDLE mainThread = QThread::currentThreadId();

        m_ccA->switchConversation(a);
        m_ccB->switchConversation(b);
        m_ccA->sendMessage(QStringLiteral("use your tool A"), {});
        m_ccB->sendMessage(QStringLiteral("use your tool B"), {});

        for (int i = 0; i < 40 && (assistantRowCount(a) == 0 || assistantRowCount(b) == 0); ++i) {
            pump(40);
        }

        QCOMPARE(assistantRowCount(a), 1);
        QCOMPARE(assistantRowCount(b), 1);

        QVERIFY2(m_toolThreadA.load() != nullptr, "conv A residency tool never ran");
        QVERIFY2(m_toolThreadB.load() != nullptr, "conv B residency tool never ran");
        QCOMPARE(m_toolThreadA.load(), mainThread);
        QCOMPARE(m_toolThreadB.load(), mainThread);

        QVERIFY(!m_sched->anyInFlight());
    }
};

QTEST_MAIN(TestConcurrentAgentMultichat)
#include "test-concurrent-agent-multichat.moc"

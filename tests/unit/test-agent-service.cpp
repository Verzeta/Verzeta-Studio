// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "models/db-manager.h"
#include "services/agent-service.h"
#include "services/file-service.h"
#include "services/model-router.h"
#include "services/rag-service.h"
#include "services/tool-service.h"
#include "utils/process-sandbox.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>

#include <atomic>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QVariantList>


class MockProvider : public ILLMProvider {
    Q_OBJECT

    struct QueuedResponse {
        QString text;
        QJsonObject toolCallJson;
        QString finishReason;
    };

  public:
    explicit MockProvider(QObject* parent = nullptr) : ILLMProvider(parent) {}


    void queueResponse(const QString& text, const QString& finishReason = "stop") {
        m_queue.append({text, {}, finishReason});
    }

    void queueToolCallResponse(const QString& toolName,
                               const QJsonObject& args,
                               const QString& callId = "call-1") {
        QJsonObject tc;
        tc[QStringLiteral("id")] = callId;
        tc[QStringLiteral("name")] = toolName;
        tc[QStringLiteral("arguments")] = args;
        m_queue.append({"", tc, "tool_calls"});
    }

    int callCount() const { return m_callCount; }


    QString providerId() const override { return QStringLiteral("mock"); }
    QString displayName() const override { return QStringLiteral("Mock"); }
    QStringList availableModels() override { return {QStringLiteral("mock-model")}; }
    int contextWindowFor(const QString&) override { return 0; }
    void refreshModels() override {}
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }
    void cancelRequest() override { m_cancelled = true; }

    void sendRequest(const LlmRequest& request) override {
        Q_UNUSED(request)
        m_cancelled = false;

        if (m_callCount >= m_queue.size()) {
            QTimer::singleShot(0, this, [this]() {
                LlmChunk ch;
                ch.delta = QStringLiteral("(no more responses)");
                ch.finishReason = QString{};
                emit chunkReceived(ch);
                emit requestFinished(QStringLiteral("stop"), 0);
            });
            ++m_callCount;
            return;
        }

        QueuedResponse resp = m_queue[m_callCount++];
        QTimer::singleShot(0, this, [this, resp]() {
            if (m_cancelled)
                return;

            if (!resp.toolCallJson.isEmpty()) {
                LlmChunk ch;
                ch.toolCallJson = resp.toolCallJson;
                ch.finishReason = QString{};
                emit chunkReceived(ch);
            } else if (!resp.text.isEmpty()) {
                LlmChunk ch;
                ch.delta = resp.text;
                ch.finishReason = QString{};
                emit chunkReceived(ch);
            }
            emit requestFinished(resp.finishReason, 10);
        });
    }

  private:
    QList<QueuedResponse> m_queue;
    int m_callCount = 0;
    bool m_cancelled = false;
};


class TestAgentService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

    ModelRouter* m_router = nullptr;
    MockProvider* m_mock = nullptr;
    ToolService* m_tools = nullptr;
    EmbeddingWorker* m_worker = nullptr;
    RagService* m_rag = nullptr;
    ProcessSandbox* m_sandbox = nullptr;
    FileService* m_fileService = nullptr;

    bool waitForSignal(QSignalSpy& spy, int timeoutMs = 2000) { return spy.wait(timeoutMs); }

    LlmRequest makeRequest(const QString& text = QStringLiteral("Hello")) {
        LlmRequest req;
        req.conversationId = QStringLiteral("test-conv");
        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = text;
        req.messages = {msg};
        req.config.modelName = QStringLiteral("mock-model");
        req.config.providerId = QStringLiteral("mock");
        return req;
    }

    AgentConfig makeConfig(AgentPattern pattern = AgentPattern::ReAct, int maxIterations = 10) {
        AgentConfig cfg;
        cfg.pattern = pattern;
        cfg.maxIterations = maxIterations;
        return cfg;
    }

  private slots:

    void initTestCase() { QVERIFY(m_tempDir.isValid()); }

    void init() {
        m_dbPath = m_tempDir.filePath(
            QStringLiteral("agent_test_%1.db").arg(QDateTime::currentMSecsSinceEpoch()));
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_router = new ModelRouter(this);
        m_mock = new MockProvider(this);
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(m_mock));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_tools = new ToolService(this);
        m_sandbox = new ProcessSandbox(this);
        m_fileService = new FileService(this);
        m_tools->registerBuiltInTools(*m_sandbox, *m_fileService);

        m_worker = new EmbeddingWorker(this);
        m_rag = new RagService(DbManager::instance(), *m_worker, this);
    }

    void cleanup() {
        delete m_router;
        m_router = nullptr;
        m_mock = nullptr;
        delete m_tools;
        m_tools = nullptr;
        delete m_sandbox;
        m_sandbox = nullptr;
        delete m_fileService;
        m_fileService = nullptr;
        delete m_worker;
        m_worker = nullptr;
        delete m_rag;
        m_rag = nullptr;

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_react_toolCallLoop_twoIterations() {
        m_mock->queueToolCallResponse(QStringLiteral("get_current_time"), QJsonObject{});
        m_mock->queueResponse(QStringLiteral("The time is 12:00"));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);
        QSignalSpy errorSpy(&agent, &AgentService::errorOccurred);

        agent.execute(makeRequest(), makeConfig(AgentPattern::ReAct));

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(errorSpy.count(), 0);
        QVERIFY(agent.currentIteration() >= 2);
        QCOMPARE(finishedSpy.count(), 1);

        const QString result = finishedSpy.first().first().toString();
        QVERIFY(!result.isEmpty());
    }

    void test_react_mainThreadTool_runsOnMainThread() {
        QThread* const mainThread = QThread::currentThread();
        std::atomic<QThread*> ranOn{nullptr};

        ToolSchema probe;
        probe.name = QStringLiteral("thread_probe");
        probe.description = QStringLiteral("Records the thread it runs on.");
        m_tools->registerTool(probe, [&ranOn](const QJsonObject&) -> QJsonValue {
            ranOn.store(QThread::currentThread());
            return QJsonObject{{QStringLiteral("ok"), true}};
        });
        m_tools->setRunsOnMainThread(QStringLiteral("thread_probe"), true);

        m_mock->queueToolCallResponse(QStringLiteral("thread_probe"), QJsonObject{});
        m_mock->queueResponse(QStringLiteral("done"));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);
        QSignalSpy errorSpy(&agent, &AgentService::errorOccurred);

        agent.execute(makeRequest(), makeConfig(AgentPattern::ReAct));

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(errorSpy.count(), 0);
        QVERIFY2(ranOn.load() != nullptr, "main-thread tool was never invoked");
        QCOMPARE(ranOn.load(), mainThread);
    }

    void test_react_toolReceivesCallerConversationId() {
        QString seenConvId = QStringLiteral("<unset>");

        ToolSchema probe;
        probe.name = QStringLiteral("conv_probe");
        probe.description = QStringLiteral("Records its caller conv id.");
        m_tools->registerTool(probe, [&seenConvId](const QJsonObject& args) -> QJsonValue {
            seenConvId = args.value(QStringLiteral("__caller_conv_id")).toString();
            return QJsonObject{{QStringLiteral("ok"), true}};
        });
        m_tools->setRunsOnMainThread(QStringLiteral("conv_probe"), true);

        m_mock->queueToolCallResponse(QStringLiteral("conv_probe"), QJsonObject{});
        m_mock->queueResponse(QStringLiteral("done"));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        agent.execute(makeRequest(), makeConfig(AgentPattern::ReAct));

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(seenConvId, QStringLiteral("test-conv"));
    }

    void test_react_toolReceivesCallerAgentAlias() {
        QString seenAlias = QStringLiteral("<unset>");

        ToolSchema probe;
        probe.name = QStringLiteral("agent_probe");
        probe.description = QStringLiteral("Records its caller agent alias.");
        m_tools->registerTool(probe, [&seenAlias](const QJsonObject& args) -> QJsonValue {
            seenAlias = args.value(QStringLiteral("__caller_agent_alias")).toString();
            return QJsonObject{{QStringLiteral("ok"), true}};
        });
        m_tools->setRunsOnMainThread(QStringLiteral("agent_probe"), true);

        m_mock->queueToolCallResponse(QStringLiteral("agent_probe"), QJsonObject{});
        m_mock->queueResponse(QStringLiteral("done"));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        AgentConfig cfg = makeConfig(AgentPattern::ReAct);
        cfg.callerAgentAlias = QStringLiteral("Researcher");
        agent.execute(makeRequest(), cfg);

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(seenAlias, QStringLiteral("Researcher"));
    }

    void test_react_maxIterations_stopsAtLimit() {
        for (int i = 0; i < 5; ++i) {
            m_mock->queueToolCallResponse(QStringLiteral("get_current_time"),
                                          QJsonObject{},
                                          QStringLiteral("call-%1").arg(i));
        }
        m_mock->queueResponse(QStringLiteral("fallback"));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy errorSpy(&agent, &AgentService::errorOccurred);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        AgentConfig cfg = makeConfig(AgentPattern::ReAct);
        cfg.maxIterations = 2;
        agent.execute(makeRequest(), cfg);

        bool gotSignal = errorSpy.wait(3000) || finishedSpy.count() > 0;
        QVERIFY(gotSignal);
        QVERIFY(agent.currentIteration() <= cfg.maxIterations + 1);
    }

    void test_react_cancel_stopsCleanly() {
        for (int i = 0; i < 10; ++i) {
            m_mock->queueToolCallResponse(QStringLiteral("get_current_time"),
                                          QJsonObject{},
                                          QStringLiteral("call-%1").arg(i));
        }

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);
        QSignalSpy runSpy(&agent, &AgentService::isRunningChanged);

        agent.execute(makeRequest(), makeConfig(AgentPattern::ReAct));
        QVERIFY(agent.isRunning());

        agent.cancel();

        bool stopped = !agent.isRunning() || runSpy.wait(1000);
        QVERIFY(stopped);
        QVERIFY(!agent.isRunning());
    }

    void test_react_noTools_singleIteration() {
        m_mock->queueResponse(QStringLiteral("Direct answer, no tools needed."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);
        QSignalSpy errorSpy(&agent, &AgentService::errorOccurred);

        agent.execute(makeRequest(), makeConfig(AgentPattern::ReAct));

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(errorSpy.count(), 0);
        QCOMPARE(agent.currentIteration(), 1);

        const QString result = finishedSpy.first().first().toString();
        QCOMPARE(result, QStringLiteral("Direct answer, no tools needed."));
    }

    void test_planner_planGeneration_emitsPlanReady() {
        const QString planJson = QStringLiteral("[{\"step_id\":1,\"description\":\"List files\","
                                                "\"tool_name\":null,\"arguments\":{}},"
                                                "{\"step_id\":2,\"description\":\"Read config\","
                                                "\"tool_name\":null,\"arguments\":{}}]");
        m_mock->queueResponse(planJson);
        m_mock->queueResponse(QStringLiteral("Done."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy planSpy(&agent, &AgentService::planReady);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        agent.execute(makeRequest(), makeConfig(AgentPattern::PlannerExecutor));

        QVERIFY(finishedSpy.wait(3000));
        QVERIFY(planSpy.count() >= 1);

        const QStringList steps = planSpy.first().first().toStringList();
        QVERIFY(!steps.isEmpty());
        QCOMPARE(steps.size(), 2);
        QCOMPARE(steps[0], QStringLiteral("List files"));
        QCOMPARE(steps[1], QStringLiteral("Read config"));
    }

    void test_planner_stepExecution_invokesToolsInOrder() {
        const QString planJson =
            QStringLiteral("[{\"step_id\":1,\"description\":\"Get time\","
                           "\"tool_name\":\"get_current_time\",\"arguments\":{}}]");
        m_mock->queueResponse(planJson);
        m_mock->queueResponse(QStringLiteral("Time retrieved."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy stepSpy(&agent, &AgentService::agentStepCompleted);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        agent.execute(makeRequest(), makeConfig(AgentPattern::PlannerExecutor));

        QVERIFY(finishedSpy.wait(3000));
        QVERIFY(stepSpy.count() >= 2);
    }

    void test_planner_synthesis_producesFinalAnswer() {
        const QString planJson = QStringLiteral("[{\"step_id\":1,\"description\":\"Think\","
                                                "\"tool_name\":null,\"arguments\":{}}]");
        m_mock->queueResponse(planJson);
        m_mock->queueResponse(QStringLiteral("Final synthesized answer."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        agent.execute(makeRequest(), makeConfig(AgentPattern::PlannerExecutor));

        QVERIFY(finishedSpy.wait(3000));
        const QString result = finishedSpy.first().first().toString();
        QCOMPARE(result, QStringLiteral("Final synthesized answer."));
    }

    void test_router_intentClassification_dispatchesSpecialized() {
        m_mock->queueResponse(QStringLiteral("code"));
        m_mock->queueResponse(QStringLiteral("Here is the code solution."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);
        QSignalSpy stepSpy(&agent, &AgentService::agentStepCompleted);

        agent.execute(makeRequest(QStringLiteral("Write a hello world program")),
                      makeConfig(AgentPattern::Router));

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(finishedSpy.count(), 1);

        bool foundIntentStep = false;
        for (int i = 0; i < stepSpy.count(); ++i) {
            const QString desc = stepSpy[i][1].toString();
            if (desc.contains(QStringLiteral("code"), Qt::CaseInsensitive)) {
                foundIntentStep = true;
                break;
            }
        }
        QVERIFY(foundIntentStep);
    }

    void test_multiAgent_subAgentsComplete() {
        const QString decomposeJson =
            QStringLiteral("[{\"task_id\":\"1\",\"description\":\"Task A\",\"prompt\":\"Do A\"},"
                           "{\"task_id\":\"2\",\"description\":\"Task B\",\"prompt\":\"Do B\"}]");
        m_mock->queueResponse(decomposeJson);
        m_mock->queueResponse(QStringLiteral("Result of A."));
        m_mock->queueResponse(QStringLiteral("Result of B."));
        m_mock->queueResponse(QStringLiteral("Combined A and B."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);
        QSignalSpy stepSpy(&agent, &AgentService::agentStepStarted);

        agent.execute(makeRequest(), makeConfig(AgentPattern::MultiAgent));

        QVERIFY(finishedSpy.wait(5000));
        QCOMPARE(finishedSpy.count(), 1);
        QVERIFY(stepSpy.count() >= 3);
    }

    void test_multiAgent_synthesis_combinesSubResults() {
        const QString decomposeJson =
            QStringLiteral("[{\"task_id\":\"1\",\"description\":\"Single task\","
                           "\"prompt\":\"Do the task\"}]");
        m_mock->queueResponse(decomposeJson);
        m_mock->queueResponse(QStringLiteral("Sub-task completed."));
        m_mock->queueResponse(QStringLiteral("Synthesized final result."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        agent.execute(makeRequest(), makeConfig(AgentPattern::MultiAgent));

        QVERIFY(finishedSpy.wait(5000));
        const QString result = finishedSpy.first().first().toString();
        QCOMPARE(result, QStringLiteral("Synthesized final result."));
    }

    void test_memoryAugmented_noSeparateRetrieval() {
        m_mock->queueResponse(QStringLiteral("Memory-augmented response."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);
        QSignalSpy stepSpy(&agent, &AgentService::agentStepStarted);

        agent.execute(makeRequest(), makeConfig(AgentPattern::MemoryAugmented));

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(finishedSpy.count(), 1);

        for (int i = 0; i < stepSpy.count(); ++i) {
            QVERIFY(!stepSpy[i][1].toString().contains(QStringLiteral("emories")));
        }
    }

    void test_memoryAugmented_returnsResult() {
        m_mock->queueResponse(QStringLiteral("Answer with memory context."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        agent.execute(makeRequest(QStringLiteral("What did we discuss last time?")),
                      makeConfig(AgentPattern::MemoryAugmented));

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(finishedSpy.count(), 1);
        QVERIFY(!finishedSpy.first().first().toString().isEmpty());
    }

    void test_confirmation_approve_executesTool() {
        m_mock->queueToolCallResponse(
            QStringLiteral("get_current_time"), QJsonObject{}, QStringLiteral("call-x"));
        m_mock->queueResponse(QStringLiteral("Time: 12:00"));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy toolReqSpy(&agent, &AgentService::toolCallRequested);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        AgentConfig cfg = makeConfig(AgentPattern::ReAct);
        cfg.requireConfirmation = true;
        agent.execute(makeRequest(), cfg);

        QVERIFY(toolReqSpy.wait(3000));
        QCOMPARE(toolReqSpy.count(), 1);

        const ToolCall call = toolReqSpy.first().first().value<ToolCall>();
        agent.approveToolCall(call.id);

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(finishedSpy.count(), 1);
    }

    void test_confirmation_deny_skipsTool() {
        m_mock->queueToolCallResponse(
            QStringLiteral("get_current_time"), QJsonObject{}, QStringLiteral("call-y"));
        m_mock->queueResponse(QStringLiteral("Proceeding without tool."));

        AgentService agent(*m_router, *m_tools, *m_rag);
        QSignalSpy toolReqSpy(&agent, &AgentService::toolCallRequested);
        QSignalSpy finishedSpy(&agent, &AgentService::finished);

        AgentConfig cfg = makeConfig(AgentPattern::ReAct);
        cfg.requireConfirmation = true;
        agent.execute(makeRequest(), cfg);

        QVERIFY(toolReqSpy.wait(3000));
        QCOMPARE(toolReqSpy.count(), 1);

        const ToolCall call = toolReqSpy.first().first().value<ToolCall>();
        agent.denyToolCall(call.id);

        QVERIFY(finishedSpy.wait(3000));
        QCOMPARE(finishedSpy.count(), 1);
    }

    void test_signalSequence_startBeforeComplete() {
        m_mock->queueResponse(QStringLiteral("Sequential answer."));

        AgentService agent(*m_router, *m_tools, *m_rag);

        QList<QString> signalOrder;
        connect(&agent, &AgentService::agentStepStarted, this, [&signalOrder](int, const QString&) {
            signalOrder.append(QStringLiteral("started"));
        });
        connect(&agent,
                &AgentService::agentStepCompleted,
                this,
                [&signalOrder](int, const QString&, bool) {
                    signalOrder.append(QStringLiteral("completed"));
                });

        QSignalSpy finishedSpy(&agent, &AgentService::finished);
        agent.execute(makeRequest(), makeConfig(AgentPattern::ReAct));

        QVERIFY(finishedSpy.wait(3000));
        QVERIFY(!signalOrder.isEmpty());

        int startCount = signalOrder.count(QStringLiteral("started"));
        int completeCount = signalOrder.count(QStringLiteral("completed"));
        QVERIFY(startCount >= completeCount);

        QCOMPARE(signalOrder.first(), QStringLiteral("started"));
    }
};

QTEST_MAIN(TestAgentService)

Q_DECLARE_METATYPE(ToolCall)

#include "test-agent-service.moc"

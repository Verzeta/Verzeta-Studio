// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/services/chat-controller.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/chat/tool-dispatcher.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/export-service.h"
#include "../../backend/services/file-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/tool-service.h"
#include "../../backend/utils/process-sandbox.h"
#include "helpers/scripted-mock-provider.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestCapHitCascadeFinalize : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ChatController> m_chat;
    std::unique_ptr<ProcessSandbox> m_sandbox;
    std::unique_ptr<FileService> m_files;
    std::unique_ptr<ToolService> m_toolSvc;
    ScriptedMockProvider* m_provider = nullptr;
    QString m_convId;

    ScriptedMockProvider::ScriptStep makeToolCallStep(int n) const {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {};
        s.finishReason = QStringLiteral("tool_calls");
        QJsonObject call;
        call.insert(QStringLiteral("id"), QStringLiteral("call-%1").arg(n));
        call.insert(QStringLiteral("name"), QStringLiteral("bogus_tool_%1").arg(n));
        call.insert(QStringLiteral("arguments"), QJsonObject{});
        s.toolCallsJson.append(call);
        return s;
    }

  private slots:
    void init() {
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();

        auto provider = std::make_unique<ScriptedMockProvider>();
        m_provider = provider.get();
        m_router->registerProvider(std::move(provider));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);

        m_sandbox = std::make_unique<ProcessSandbox>();
        m_files = std::make_unique<FileService>();
        m_toolSvc = std::make_unique<ToolService>();
        m_toolSvc->registerBuiltInTools(*m_sandbox, *m_files);
        m_chat->setToolService(m_toolSvc.get());

        m_convId = m_convSvc->createConversation(QStringLiteral("TestCap"));
        QVERIFY(!m_convId.isEmpty());
        m_chat->switchConversation(m_convId);
    }

    void cleanup() {
        m_chat.reset();
        m_toolSvc.reset();
        m_files.reset();
        m_sandbox.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_capHit_force_finalizes_cascade() {
        QList<ScriptedMockProvider::ScriptStep> script;
        for (int i = 1; i <= 11; ++i) {
            script.append(makeToolCallStep(i));
        }
        m_provider->setScript(script);

        QSignalSpy cascadeSpy(m_chat->cascadeInternal(), &Chat::CascadeController::cascadeComplete);
        QVERIFY(cascadeSpy.isValid());

        m_chat->sendMessage(QStringLiteral("trigger the cap"), {});

        QElapsedTimer wallClock;
        wallClock.start();
        const int deadlineMs = 5000;
        while (cascadeSpy.count() == 0 && wallClock.elapsed() < deadlineMs) {
            QTest::qWait(20);
        }
        QVERIFY2(cascadeSpy.count() >= 1,
                 qPrintable(QStringLiteral("Cascade DID NOT finalize within %1ms — the "
                                           "kMaxToolIterations cap-hit branch is letting "
                                           "the cascade hang. This is the iter21a "
                                           "regression mode.")
                                .arg(deadlineMs)));

        const QString finishReason = cascadeSpy.at(0).at(1).toString();
        QVERIFY2(finishReason == QStringLiteral("tool_failure_cap"),
                 qPrintable(QStringLiteral("cascadeComplete fired with finishReason=\"%1\" — "
                                           "expected \"tool_failure_cap\" from the failure-cap "
                                           "force-finalize path.")
                                .arg(finishReason)));

        QVERIFY(!m_chat->isGenerating());

        QCOMPARE(m_provider->stepsConsumed(),
                 Chat::ToolDispatcher::kMaxConsecutiveToolFailures + 1);
    }

    void test_forceCascadeComplete_emits_with_finishReason() {
        QSignalSpy completeSpy(m_chat->cascadeInternal(),
                               &Chat::CascadeController::cascadeComplete);
        QVERIFY(completeSpy.isValid());

        Chat::CascadeRouteInputs inputs;
        inputs.convId = m_convId;
        inputs.content = QStringLiteral("forced");
        inputs.requestId = 42;
        inputs.finishReason = QStringLiteral("tool_iteration_cap");
        inputs.totalTokens = 0;
        inputs.elapsedMs = 0;

        m_chat->cascadeInternal()->forceCascadeComplete(inputs);

        QCOMPARE(completeSpy.count(), 1);
        QCOMPARE(completeSpy.at(0).at(1).toString(), QStringLiteral("tool_iteration_cap"));
    }
};

QTEST_MAIN(TestCapHitCascadeFinalize)
#include "test-cap-hit-cascade-finalize.moc"

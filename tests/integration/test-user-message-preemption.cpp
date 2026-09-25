// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/services/chat-controller.h"
#include "../../backend/services/chat/cascade-controller.h"
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
#include <QSignalSpy>
#include <QSqlDatabase>

class TestUserMessagePreemption : public QObject {
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

    static ScriptedMockProvider::ScriptStep makeTextStep(const QString& text) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {text};
        s.finishReason = QStringLiteral("stop");
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

        m_convId = m_convSvc->createConversation(QStringLiteral("TestPreempt"));
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

    void test_queuedUserMessage_preempts_cascade_at_turn_boundary() {
        m_provider->setScript({
            makeTextStep(QStringLiteral("Reply to the first message.")),
            makeTextStep(QStringLiteral("Reply to the steering message.")),
        });

        QSignalSpy cascadeSpy(m_chat->cascadeInternal(), &Chat::CascadeController::cascadeComplete);
        QVERIFY(cascadeSpy.isValid());

        bool sentSecond = false;
        QString queuedSnapshot;
        QObject::connect(m_chat.get(), &ChatController::isGeneratingChanged, m_chat.get(), [&]() {
            if (sentSecond)
                return;
            sentSecond = true;
            m_chat->sendMessage(QStringLiteral("steer me"), {});
            queuedSnapshot = m_chat->queuedUserText();
        });

        m_chat->sendMessage(QStringLiteral("first"), {});

        QElapsedTimer wall;
        wall.start();
        while (cascadeSpy.count() < 2 && wall.elapsed() < 5000) {
            QTest::qWait(20);
        }

        QCOMPARE(queuedSnapshot, QStringLiteral("steer me"));

        QVERIFY2(cascadeSpy.count() >= 1, "no cascade completed");
        bool sawPreempt = false;
        for (const QList<QVariant>& args : cascadeSpy) {
            if (args.at(1).toString() == QStringLiteral("user_message_pending")) {
                sawPreempt = true;
                break;
            }
        }
        QVERIFY2(sawPreempt,
                 "the cascade did not force-finalize with the "
                 "user_message_pending preemption finishReason");

        QCOMPARE(m_chat->queuedUserText(), QString());
    }
};

QTEST_MAIN(TestUserMessagePreemption)
#include "test-user-message-preemption.moc"

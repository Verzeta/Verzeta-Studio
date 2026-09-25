// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/chat/request-builder.h"
#include "services/conversation-service.h"
#include "services/message-service.h"
#include "services/model-router.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QUuid>

class TestRequestBuilderCanvasPrompt : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<Chat::RequestBuilder> m_builder;

    QString m_convId;

  private slots:

    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        QStandardPaths::setTestModeEnabled(true);
    }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

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
        m_router = std::make_unique<ModelRouter>();
        m_builder = std::make_unique<Chat::RequestBuilder>(*m_convSvc, *m_msgSvc, *m_router);

        m_convId = m_convSvc->createConversation(QStringLiteral("Canvas prompt test"));
        QVERIFY(!m_convId.isEmpty());

        LlmConfig cfg;
        cfg.providerId = QStringLiteral("test");
        cfg.modelName = QStringLiteral("test-model");
        cfg.stream = true;
        cfg.contextWindow = 8192;
        cfg.maxTokens = 1024;
        m_convSvc->updateLlmConfig(m_convId, cfg);
    }

    void cleanup() {
        m_builder.reset();
        m_router.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    Chat::BuildRequestInputs baselineInputs() {
        Chat::BuildRequestInputs in;
        in.activeConvId = m_convId;
        in.inflightConvId = m_convId;
        in.requestId = 1;
        in.lastUserText = QStringLiteral("hi");
        return in;
    }

    void test_noCanvas_systemPromptDoesNotContainBlock() {
        const Chat::BuildResult r = m_builder->buildRequest(baselineInputs());
        QVERIFY2(r.success, qPrintable(r.errorReason));
        QVERIFY(!r.request.systemPrompt.contains(QStringLiteral("=== ACTIVE CANVAS ===")));
    }

    void test_canvasMetadata_appearsInSystemPrompt() {
        Chat::BuildRequestInputs in = baselineInputs();
        in.canvasFilename = QStringLiteral("config.json");
        in.canvasLanguage = QStringLiteral("json");
        in.canvasRevision = 7;
        in.canvasLineCount = 42;
        in.canvasByteSize = 1234;

        const Chat::BuildResult r = m_builder->buildRequest(in);
        QVERIFY2(r.success, qPrintable(r.errorReason));

        const QString sys = r.request.systemPrompt;
        QVERIFY(sys.contains(QStringLiteral("=== ACTIVE CANVAS ===")));
        QVERIFY(sys.contains(QStringLiteral("config.json")));
        QVERIFY(sys.contains(QStringLiteral("json")));
        QVERIFY(sys.contains(QStringLiteral("42")));
        QVERIFY(sys.contains(QStringLiteral("1234")));
        QVERIFY(sys.contains(QStringLiteral("revision:  7")));
        QVERIFY(sys.contains(QStringLiteral("read_canvas")));
        QVERIFY(sys.contains(QStringLiteral("edit_canvas")));
        QVERIFY(sys.contains(QStringLiteral("always read first")));
    }

    void test_canvasContent_isNotInSystemPrompt() {
        Chat::BuildRequestInputs in = baselineInputs();
        in.canvasFilename = QStringLiteral("secrets.json");
        in.canvasLanguage = QStringLiteral("json");
        in.canvasRevision = 1;
        in.canvasLineCount = 3;
        in.canvasByteSize = 50;

        const Chat::BuildResult r = m_builder->buildRequest(in);
        QVERIFY(r.success);
        QVERIFY(!r.request.systemPrompt.contains(
            QStringLiteral("HYPOTHETICAL_CANVAS_CONTENT_SENTINEL")));
    }
};

QTEST_MAIN(TestRequestBuilderCanvasPrompt)
#include "test-request-builder-canvas-prompt.moc"

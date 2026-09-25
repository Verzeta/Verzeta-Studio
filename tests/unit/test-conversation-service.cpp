// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "models/llm-config.h"
#include "services/conversation-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>

class TestConversationService : public QObject {
    Q_OBJECT

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        m_dbPath = dbPath;
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());
        m_svc = std::make_unique<ConversationService>(DbManager::instance());
    }

    void cleanup() {
        m_svc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void testCreateAndRetrieve() {
        const QString id = m_svc->createConversation(QStringLiteral("Test Conversation"));
        QVERIFY(!id.isEmpty());

        const auto conv = m_svc->getConversation(id);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->title, QStringLiteral("Test Conversation"));
        QCOMPARE(conv->id, id);
        QVERIFY(conv->folderId.isEmpty());
        QCOMPARE(conv->tokenTotal, 0);
    }


    void testListConversationsOrder() {
        const QString id1 = m_svc->createConversation(QStringLiteral("First"));
        QTest::qWait(5);
        const QString id2 = m_svc->createConversation(QStringLiteral("Second"));
        QTest::qWait(5);
        const QString id3 = m_svc->createConversation(QStringLiteral("Third"));

        const QList<Conversation> list = m_svc->listConversations();
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[0].id, id3);
        QCOMPARE(list[1].id, id2);
        QCOMPARE(list[2].id, id1);
    }


    void testRename() {
        const QString id = m_svc->createConversation(QStringLiteral("Original Title"));
        QVERIFY(m_svc->renameConversation(id, QStringLiteral("New Title")));

        const auto conv = m_svc->getConversation(id);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->title, QStringLiteral("New Title"));
    }


    void testDelete() {
        const QString id = m_svc->createConversation(QStringLiteral("To Delete"));
        QVERIFY(m_svc->deleteConversation(id));

        const auto conv = m_svc->getConversation(id);
        QVERIFY(!conv.has_value());
    }


    void testFolderOperations() {
        const QString folderId = m_svc->createFolder(QStringLiteral("Work Projects"));
        QVERIFY(!folderId.isEmpty());

        const QString convId = m_svc->createConversation(QStringLiteral("Project Chat"));

        {
            const auto list = m_svc->listConversations();
            QCOMPARE(list.size(), 1);
            const auto folderList = m_svc->listConversations(folderId);
            QCOMPARE(folderList.size(), 0);
        }

        QVERIFY(m_svc->moveToFolder(convId, folderId));

        {
            const auto rootList = m_svc->listConversations();
            QCOMPARE(rootList.size(), 0);
            const auto folderList = m_svc->listConversations(folderId);
            QCOMPARE(folderList.size(), 1);
            QCOMPARE(folderList[0].id, convId);
        }
    }


    void testNestedFolders() {
        const QString parentId = m_svc->createFolder(QStringLiteral("Parent"));
        const QString childId = m_svc->createFolder(QStringLiteral("Child"), parentId);
        QVERIFY(!childId.isEmpty());

        const auto topLevel = m_svc->listFolders();
        QCOMPARE(topLevel.size(), 1);
        QCOMPARE(topLevel[0].id, parentId);

        const auto children = m_svc->listFolders(parentId);
        QCOMPARE(children.size(), 1);
        QCOMPARE(children[0].id, childId);
    }


    void testLlmConfigUpdate() {
        const QString id = m_svc->createConversation(QStringLiteral("Config Test"));

        LlmConfig cfg;
        cfg.providerId = QStringLiteral("openai");
        cfg.modelName = QStringLiteral("gpt-4o");
        cfg.temperature = 0.5;
        cfg.maxTokens = 2048;

        QVERIFY(m_svc->updateLlmConfig(id, cfg));

        const auto conv = m_svc->getConversation(id);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->llmConfig[QStringLiteral("provider_id")].toString(),
                 QStringLiteral("openai"));
        QCOMPARE(conv->llmConfig[QStringLiteral("model_name")].toString(),
                 QStringLiteral("gpt-4o"));
        QVERIFY(qAbs(conv->llmConfig[QStringLiteral("temperature")].toDouble() - 0.5) < 0.001);
    }


    void testSystemPromptUpdate() {
        const QString id = m_svc->createConversation(QStringLiteral("Prompt Test"));
        const QString prompt = QStringLiteral("You are a helpful assistant specialized in C++.");

        QVERIFY(m_svc->updateSystemPrompt(id, prompt));

        const auto conv = m_svc->getConversation(id);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->systemPrompt, prompt);
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_svc;
};

QTEST_MAIN(TestConversationService)
#include "test-conversation-service.moc"

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/agent.h"
#include "models/conversation.h"
#include "models/db-manager.h"
#include "services/agent-registry.h"
#include "services/conversation-controller.h"
#include "services/conversation-service.h"
#include "services/model-router.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QUuid>

class TestProjectDirectChatFolder : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationController> m_convCtrl;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    QString createAgent(const QString& name) {
        Agent a;
        a.id = uuid();
        a.name = name;
        a.description = QStringLiteral("Test agent %1").arg(name);
        a.systemPrompt = QStringLiteral("You are %1.").arg(name);
        a.defaultPattern = QStringLiteral("direct");
        a.createdAt = QDateTime::currentDateTimeUtc();
        const QString id = m_agentRegistry->createAgent(a);
        Q_ASSERT(!id.isEmpty());
        return id;
    }

    QString makeProjectFolder(const QString& name) {
        const QString id = m_convSvc->createFolder(name);
        m_convSvc->updateFolderMetadata(id,
                                        QStringLiteral("project"),
                                        QStringLiteral("Test goal"),
                                        QStringLiteral("Test description"),
                                        {});
        return id;
    }

    bool listed(const QString& folderId, const QString& convId) {
        const QList<Conversation> rows = m_convSvc->listConversations(folderId);
        for (const Conversation& c : rows) {
            if (c.id == convId)
                return true;
        }
        return false;
    }

  private slots:
    void initTestCase() { QVERIFY(m_tempDir.isValid()); }

    void init() {
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_agentRegistry = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentRegistry->initialize();
        m_router = std::make_unique<ModelRouter>();
        m_convCtrl = std::make_unique<ConversationController>(*m_convSvc, *m_router);
    }

    void cleanup() {
        m_convCtrl.reset();
        m_router.reset();
        m_agentRegistry.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        if (!m_dbPath.isEmpty())
            QFile::remove(m_dbPath);
    }


    void test_directChatStampsProjectFolderId() {
        const QString folderId = makeProjectFolder(QStringLiteral("Acme Project"));

        const QString agentId = createAgent(QStringLiteral("Alice"));
        const QString alias = QStringLiteral("Alice");

        const QString convId = m_convCtrl->openDirectChatWithMember(folderId, agentId, alias);
        QVERIFY2(!convId.isEmpty(), "openDirectChatWithMember must return a valid id");

        const auto opt = m_convSvc->getConversation(convId);
        QVERIFY(opt.has_value());

        QCOMPARE(opt->folderId, folderId);

        QCOMPARE(opt->primaryAgentId, agentId);

        QCOMPARE(opt->title, QStringLiteral("Chat with @Alice"));

        QVERIFY2(listed(folderId, convId),
                 "Conversation must appear in listConversations(folderId)");

        QVERIFY2(!listed(QString(), convId), "Conversation must NOT appear at root level");
    }


    void test_directChatReuseReturnsSameIdAndStaysInProject() {
        const QString folderId = makeProjectFolder(QStringLiteral("Reuse Project"));
        const QString agentId = createAgent(QStringLiteral("Bob"));
        const QString alias = QStringLiteral("Bob");

        const QString first = m_convCtrl->openDirectChatWithMember(folderId, agentId, alias);
        QVERIFY(!first.isEmpty());

        const QString second = m_convCtrl->openDirectChatWithMember(folderId, agentId, alias);
        QCOMPARE(second, first);

        const auto opt = m_convSvc->getConversation(second);
        QVERIFY(opt.has_value());
        QCOMPARE(opt->folderId, folderId);

        const QList<Conversation> rows = m_convSvc->listConversations(folderId);
        int matchCount = 0;
        for (const Conversation& c : rows) {
            if (c.primaryAgentId == agentId && c.title == QStringLiteral("Chat with @Bob")) {
                ++matchCount;
            }
        }
        QCOMPARE(matchCount, 1);
    }


    void test_emptyAgentIdReturnsEmptyAndCreatesNoRow() {
        const QString folderId = makeProjectFolder(QStringLiteral("Empty Args Project"));
        const QString convId =
            m_convCtrl->openDirectChatWithMember(folderId, QString(), QStringLiteral("X"));
        QVERIFY(convId.isEmpty());
        QCOMPARE(m_convSvc->listConversations(folderId).size(), 0);
    }

    void test_emptyAliasReturnsEmptyAndCreatesNoRow() {
        const QString folderId = makeProjectFolder(QStringLiteral("Empty Alias Project"));
        const QString convId = m_convCtrl->openDirectChatWithMember(folderId, uuid(), QString());
        QVERIFY(convId.isEmpty());
        QCOMPARE(m_convSvc->listConversations(folderId).size(), 0);
    }


    void test_emptyFolderIdProducesRootLevelChat() {
        const QString agentId = createAgent(QStringLiteral("Charlie"));
        const QString convId =
            m_convCtrl->openDirectChatWithMember(QString(), agentId, QStringLiteral("Charlie"));
        QVERIFY(!convId.isEmpty());

        const auto opt = m_convSvc->getConversation(convId);
        QVERIFY(opt.has_value());
        QCOMPARE(opt->folderId, QString());
        QCOMPARE(opt->primaryAgentId, agentId);

        QVERIFY(listed(QString(), convId));
    }


    void test_sameAgentInTwoProjectsProducesIndependentConversations() {
        const QString folderA = makeProjectFolder(QStringLiteral("Project A"));
        const QString folderB = makeProjectFolder(QStringLiteral("Project B"));
        const QString agentId = createAgent(QStringLiteral("Dana"));
        const QString alias = QStringLiteral("Dana");

        const QString convA = m_convCtrl->openDirectChatWithMember(folderA, agentId, alias);
        const QString convB = m_convCtrl->openDirectChatWithMember(folderB, agentId, alias);
        QVERIFY(!convA.isEmpty());
        QVERIFY(!convB.isEmpty());
        QVERIFY2(convA != convB, "Different projects must produce different conv rows");

        const auto optA = m_convSvc->getConversation(convA);
        const auto optB = m_convSvc->getConversation(convB);
        QVERIFY(optA.has_value());
        QVERIFY(optB.has_value());
        QCOMPARE(optA->folderId, folderA);
        QCOMPARE(optB->folderId, folderB);

        QVERIFY(listed(folderA, convA));
        QVERIFY(!listed(folderA, convB));
        QVERIFY(listed(folderB, convB));
        QVERIFY(!listed(folderB, convA));
    }
};

QTEST_MAIN(TestProjectDirectChatFolder)
#include "test-project-direct-chat-folder.moc"

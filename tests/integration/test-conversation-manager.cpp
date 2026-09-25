// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "helpers/scripted-mock-provider.h"
#include "models/agent.h"
#include "models/conversation.h"
#include "models/db-manager.h"
#include "models/member.h"
#include "services/agent-registry.h"
#include "services/chat-controller.h"
#include "services/conversation-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/file-service.h"
#include "services/membership-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/task-gate-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

class TestConversationManager : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    std::unique_ptr<MembershipService> m_membership;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<ConversationController> m_convController;
    std::unique_ptr<TaskGateService> m_taskGateSvc;
    std::unique_ptr<ChatController> m_chat;
    QString m_agentAId;
    QString m_agentBId;

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
        m_agentRegistry = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentRegistry->initialize();
        m_membership = std::make_unique<MembershipService>(DbManager::instance());
        m_fileSvc = std::make_unique<FileService>();

        auto provider = std::make_unique<ScriptedMockProvider>();
        m_router->registerProvider(std::move(provider));
        m_router->setActiveProvider(QStringLiteral("mock"), QStringLiteral("mock-model"));

        m_convController = std::make_unique<ConversationController>(*m_convSvc, *m_router);
        m_convController->setFileService(m_fileSvc.get());
        m_convController->setMembershipService(m_membership.get());

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_chat->setMembershipService(m_membership.get());
        m_chat->setAgentRegistry(m_agentRegistry.get());
        m_chat->setFileService(m_fileSvc.get());

        m_taskGateSvc = std::make_unique<TaskGateService>(*m_msgSvc);
        m_chat->setTaskGateService(m_taskGateSvc.get());
        QObject::connect(m_convController.get(),
                         &ConversationController::conversationAboutToBeDeleted,
                         m_chat.get(),
                         &ChatController::onExternalConversationAboutToBeDeleted);
        QObject::connect(m_convController.get(),
                         &ConversationController::conversationDeleted,
                         m_chat.get(),
                         &ChatController::onExternalConversationDeleted);

        m_agentAId = createAgent(QStringLiteral("AgentA"));
        m_agentBId = createAgent(QStringLiteral("AgentB"));
    }

    void cleanup() {
        m_chat.reset();
        m_taskGateSvc.reset();
        m_convController.reset();
        m_fileSvc.reset();
        m_membership.reset();
        m_agentRegistry.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_newGroupConversation_emptyMembers_emitsErrorAndReturnsEmpty() {
        QSignalSpy errSpy(m_convController.get(), &ConversationController::errorOccurred);
        const QString id =
            m_convController->newGroupConversation(QStringLiteral("bad-group"), {}, QString());
        QVERIFY(id.isEmpty());
        QCOMPARE(errSpy.count(), 1);
        QVERIFY(errSpy.first().first().toString().contains(QStringLiteral("at least one member"),
                                                           Qt::CaseInsensitive));
        QCOMPARE(m_convSvc->listAllConversations().size(), 0);
    }

    void test_newGroupConversation_withMembers_createsGroupAndSwitches() {
        QVariantList members;
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_agentAId;
            m[QStringLiteral("alias")] = QStringLiteral("Alice");
            m[QStringLiteral("isCoordinator")] = true;
            members.append(m);
        }
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_agentBId;
            m[QStringLiteral("alias")] = QStringLiteral("Bob");
            m[QStringLiteral("isCoordinator")] = false;
            members.append(m);
        }

        const QString id =
            m_convController->newGroupConversation(QStringLiteral("SquadChat"), members, QString());
        QVERIFY(!id.isEmpty());
        m_chat->switchConversation(id);

        const auto conv = m_convSvc->getConversation(id);
        QVERIFY(conv.has_value());
        QVERIFY(conv->isGroup);

        const QList<Member> roster = m_membership->conversationMembers(id);
        QCOMPARE(roster.size(), 2);
        QSet<QString> aliases;
        for (const Member& m : roster)
            aliases.insert(m.alias);
        QVERIFY(aliases.contains(QStringLiteral("Alice")));
        QVERIFY(aliases.contains(QStringLiteral("Bob")));

        QVERIFY(!conv->llmConfig.isEmpty());

        QCOMPARE(m_chat->activeConversationId(), id);
    }

    void test_openDirectChatWithMember_reusesMatchingOneOnOne() {
        const QString folderId = m_convSvc->createFolder(QStringLiteral("proj"));
        QVERIFY(!folderId.isEmpty());

        const QString preId =
            m_convSvc->createConversation(QStringLiteral("Chat with @Alice"), folderId);
        QVERIFY(!preId.isEmpty());
        m_convSvc->updatePrimaryAgent(preId, m_agentAId);

        const int convCountBefore = m_convSvc->listConversations(folderId).size();

        const QString hit = m_convController->openDirectChatWithMember(
            folderId, m_agentAId, QStringLiteral("Alice"));
        QCOMPARE(hit, preId);
        m_chat->switchConversation(hit);

        QCOMPARE(m_convSvc->listConversations(folderId).size(), convCountBefore);
        QCOMPARE(m_chat->activeConversationId(), preId);
    }

    void test_openDirectChatWithMember_createsWhenNoMatch() {
        const QString folderId = m_convSvc->createFolder(QStringLiteral("proj"));
        QVERIFY(!folderId.isEmpty());

        const QString id =
            m_convController->openDirectChatWithMember(folderId, m_agentBId, QStringLiteral("Bob"));
        QVERIFY(!id.isEmpty());
        m_chat->switchConversation(id);

        const auto conv = m_convSvc->getConversation(id);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->title, QStringLiteral("Chat with @Bob"));
        QCOMPARE(conv->primaryAgentId, m_agentBId);
        QVERIFY(!conv->isGroup);
        QCOMPARE(m_chat->activeConversationId(), id);
    }

    void test_createGroupChatForProject_singleMember_emitsError() {
        const QString folderId = m_convSvc->createFolder(QStringLiteral("tinyproj"));
        QVERIFY(!folderId.isEmpty());
        m_convSvc->updateFolderMetadata(
            folderId, QStringLiteral("project"), QStringLiteral(""), QStringLiteral(""), {});
        m_membership->addProjectMember(folderId, m_agentAId, QStringLiteral("Alice"), true);

        QSignalSpy errSpy(m_convController.get(), &ConversationController::errorOccurred);
        const QString id = m_convController->createGroupChatForProject(folderId);
        QVERIFY(id.isEmpty());
        QCOMPARE(errSpy.count(), 1);
        QVERIFY(errSpy.first().first().toString().contains(QStringLiteral("at least two"),
                                                           Qt::CaseInsensitive));
    }

    void test_activeGroupMembers_returnsRosterWithAgentInfo() {
        QVariantList members;
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_agentAId;
            m[QStringLiteral("alias")] = QStringLiteral("Alice");
            m[QStringLiteral("isCoordinator")] = true;
            members.append(m);
        }
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_agentBId;
            m[QStringLiteral("alias")] = QStringLiteral("Bob");
            m[QStringLiteral("isCoordinator")] = false;
            members.append(m);
        }
        const QString id = m_convController->newGroupConversation(
            QStringLiteral("SquadChat2"), members, QString());
        QVERIFY(!id.isEmpty());
        m_chat->switchConversation(id);

        QVERIFY(m_convController->isGroup(m_chat->activeConversationId()));

        const QVariantList roster = m_convController->groupMembers(m_chat->activeConversationId());
        QCOMPARE(roster.size(), 2);

        QSet<QString> names;
        QSet<QString> aliases;
        for (const QVariant& v : roster) {
            const QVariantMap m = v.toMap();
            aliases.insert(m.value(QStringLiteral("alias")).toString());
            names.insert(m.value(QStringLiteral("name")).toString());
        }
        QVERIFY(aliases.contains(QStringLiteral("Alice")));
        QVERIFY(aliases.contains(QStringLiteral("Bob")));
        QVERIFY(names.contains(QStringLiteral("AgentA")));
        QVERIFY(names.contains(QStringLiteral("AgentB")));
    }

    void test_newGroupConversation_roundTripsMemberOverride() {
        QVariantList members;
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_agentAId;
            m[QStringLiteral("alias")] = QStringLiteral("Alice");
            m[QStringLiteral("isCoordinator")] = true;
            m[QStringLiteral("modelProvider")] = QStringLiteral("provider-x");
            m[QStringLiteral("modelName")] = QStringLiteral("model-x");
            m[QStringLiteral("allowedTools")] =
                QStringList{QStringLiteral("read_file"), QStringLiteral("write_file")};
            members.append(m);
        }
        {
            QVariantMap m;
            m[QStringLiteral("agentId")] = m_agentBId;
            m[QStringLiteral("alias")] = QStringLiteral("Bob");
            m[QStringLiteral("isCoordinator")] = false;
            members.append(m);
        }

        const QString id = m_convController->newGroupConversation(
            QStringLiteral("OverrideSquad"), members, QString());
        QVERIFY(!id.isEmpty());

        const QList<Member> roster = m_membership->conversationMembers(id);
        QCOMPARE(roster.size(), 2);
        Member alice, bob;
        for (const Member& mem : roster) {
            if (mem.alias == QStringLiteral("Alice"))
                alice = mem;
            else if (mem.alias == QStringLiteral("Bob"))
                bob = mem;
        }
        QCOMPARE(alice.modelProvider, QStringLiteral("provider-x"));
        QCOMPARE(alice.modelName, QStringLiteral("model-x"));
        QCOMPARE(alice.allowedTools.size(), 2);
        QVERIFY(alice.allowedTools.contains(QStringLiteral("read_file")));
        QVERIFY(alice.allowedTools.contains(QStringLiteral("write_file")));
        QVERIFY(bob.modelProvider.isEmpty());
        QVERIFY(bob.modelName.isEmpty());
        QVERIFY(bob.allowedTools.isEmpty());
    }

    void test_createGroupChatForProject_carriesMemberOverride() {
        const QString folderId = m_convSvc->createFolder(QStringLiteral("overrideproj"));
        QVERIFY(!folderId.isEmpty());
        m_convSvc->updateFolderMetadata(
            folderId, QStringLiteral("project"), QStringLiteral(""), QStringLiteral(""), {});

        QVERIFY(m_membership->addProjectMember(folderId,
                                               m_agentAId,
                                               QStringLiteral("Alice"),
                                               true,
                                               QStringLiteral("user"),
                                               QString(),
                                               QStringLiteral("provider-a"),
                                               QStringLiteral("model-a"),
                                               QStringList{QStringLiteral("read_file")}));
        QVERIFY(m_membership->addProjectMember(folderId,
                                               m_agentBId,
                                               QStringLiteral("Bob"),
                                               false,
                                               QStringLiteral("user"),
                                               QString(),
                                               QStringLiteral("provider-b"),
                                               QStringLiteral("model-b"),
                                               QStringList{}));

        const QString id = m_convController->createGroupChatForProject(folderId);
        QVERIFY(!id.isEmpty());

        const QList<Member> roster = m_membership->conversationMembers(id);
        QCOMPARE(roster.size(), 2);
        Member alice, bob;
        for (const Member& mem : roster) {
            if (mem.alias == QStringLiteral("Alice"))
                alice = mem;
            else if (mem.alias == QStringLiteral("Bob"))
                bob = mem;
        }
        QCOMPARE(alice.modelProvider, QStringLiteral("provider-a"));
        QCOMPARE(alice.modelName, QStringLiteral("model-a"));
        QCOMPARE(alice.allowedTools.size(), 1);
        QVERIFY(alice.allowedTools.contains(QStringLiteral("read_file")));
        QCOMPARE(bob.modelProvider, QStringLiteral("provider-b"));
        QCOMPARE(bob.modelName, QStringLiteral("model-b"));
        QVERIFY(bob.allowedTools.isEmpty());
    }
};

QTEST_MAIN(TestConversationManager)
#include "test-conversation-manager.moc"

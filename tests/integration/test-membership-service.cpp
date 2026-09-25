// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/member.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/membership-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QDateTime>
#include <QSqlDatabase>

class TestMembershipService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MembershipService> m_members;
    std::unique_ptr<AgentRegistry> m_agents;
    QString m_writerId;
    QString m_folderId;
    QString m_groupConvId;

    QString createAgent(const QString& name) {
        Agent a;
        a.name = name;
        a.description = name + QStringLiteral(" template");
        a.systemPrompt = QStringLiteral("You are %1.").arg(name);
        a.iconName = QStringLiteral("face-smile");
        a.defaultPattern = QStringLiteral("direct");
        a.createdAt = QDateTime::currentDateTimeUtc();
        return m_agents->createAgent(a);
    }

    static Member findAlias(const QList<Member>& members, const QString& alias) {
        for (const Member& m : members) {
            if (m.alias == alias)
                return m;
        }
        return {};
    }

  private slots:
    void init() {
        QVERIFY(m_dbDir.isValid());
        m_dbPath =
            m_dbDir.path() + QStringLiteral("/ms_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_members = std::make_unique<MembershipService>(DbManager::instance());
        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());

        m_writerId = createAgent(QStringLiteral("Writer"));
        QVERIFY(!m_writerId.isEmpty());

        m_folderId = m_convs->createFolder(QStringLiteral("Test Project"));
        QVERIFY(!m_folderId.isEmpty());
        QVERIFY(m_convs->updateFolderMetadata(m_folderId,
                                              QStringLiteral("project"),
                                              QStringLiteral("goal"),
                                              QStringLiteral("desc"),
                                              {}));

        m_groupConvId = m_convs->createConversation(QStringLiteral("Group Chat"));
        QVERIFY(!m_groupConvId.isEmpty());
    }

    void cleanup() {
        m_agents.reset();
        m_members.reset();
        m_convs.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_addProjectMember_persistsModelOverride() {
        QVERIFY(m_members->addProjectMember(
            m_folderId,
            m_writerId,
            QStringLiteral("Writer1"),
            false,
            QStringLiteral("user"),
            QString(),
            QStringLiteral("anthropic"),
            QStringLiteral("claude-x"),
            {QStringLiteral("read_file"), QStringLiteral("write_file")}));

        const Member m =
            findAlias(m_members->projectMembers(m_folderId), QStringLiteral("Writer1"));
        QVERIFY(m.isValid());
        QCOMPARE(m.modelProvider, QStringLiteral("anthropic"));
        QCOMPARE(m.modelName, QStringLiteral("claude-x"));
        QCOMPARE(m.allowedTools,
                 (QStringList{QStringLiteral("read_file"), QStringLiteral("write_file")}));
    }

    void test_addProjectMember_emptyOverride_isNoRestriction() {
        QVERIFY(m_members->addProjectMember(m_folderId, m_writerId, QStringLiteral("Writer1")));

        const Member m =
            findAlias(m_members->projectMembers(m_folderId), QStringLiteral("Writer1"));
        QVERIFY(m.isValid());
        QVERIFY(m.modelProvider.isEmpty());
        QVERIFY(m.modelName.isEmpty());
        QVERIFY(m.allowedTools.isEmpty());
    }

    void test_threeMembersOneTemplate_distinctOverrides() {
        QVERIFY(m_members->addProjectMember(m_folderId,
                                            m_writerId,
                                            QStringLiteral("Writer1"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QStringLiteral("ollama"),
                                            QStringLiteral("gemma"),
                                            {}));
        QVERIFY(m_members->addProjectMember(m_folderId,
                                            m_writerId,
                                            QStringLiteral("Writer2"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QStringLiteral("anthropic"),
                                            QStringLiteral("claude"),
                                            {}));
        QVERIFY(m_members->addProjectMember(m_folderId,
                                            m_writerId,
                                            QStringLiteral("Writer3"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QStringLiteral("openai"),
                                            QStringLiteral("gpt"),
                                            {}));

        const QList<Member> all = m_members->projectMembers(m_folderId);
        QCOMPARE(all.size(), 3);
        QCOMPARE(findAlias(all, QStringLiteral("Writer1")).modelProvider, QStringLiteral("ollama"));
        QCOMPARE(findAlias(all, QStringLiteral("Writer2")).modelProvider,
                 QStringLiteral("anthropic"));
        QCOMPARE(findAlias(all, QStringLiteral("Writer3")).modelProvider, QStringLiteral("openai"));
        QCOMPARE(findAlias(all, QStringLiteral("Writer1")).agentId, m_writerId);
        QCOMPARE(findAlias(all, QStringLiteral("Writer3")).agentId, m_writerId);
    }

    void test_setProjectMembers_roundTripsOverride() {
        Member w1;
        w1.containerId = m_folderId;
        w1.agentId = m_writerId;
        w1.alias = QStringLiteral("Writer1");
        w1.modelProvider = QStringLiteral("deepseek");
        w1.modelName = QStringLiteral("deepseek-chat");
        w1.allowedTools = {QStringLiteral("search_web")};
        Member w2;
        w2.containerId = m_folderId;
        w2.agentId = m_writerId;
        w2.alias = QStringLiteral("Writer2");

        QVERIFY(m_members->setProjectMembers(m_folderId, {w1, w2}));

        const QList<Member> all = m_members->projectMembers(m_folderId);
        QCOMPARE(all.size(), 2);
        const Member r1 = findAlias(all, QStringLiteral("Writer1"));
        QCOMPARE(r1.modelProvider, QStringLiteral("deepseek"));
        QCOMPARE(r1.modelName, QStringLiteral("deepseek-chat"));
        QCOMPARE(r1.allowedTools, (QStringList{QStringLiteral("search_web")}));
        const Member r2 = findAlias(all, QStringLiteral("Writer2"));
        QVERIFY(r2.modelProvider.isEmpty());
        QVERIFY(r2.allowedTools.isEmpty());
    }

    void test_setProjectMembersFromList_roundTripsOverrideAndProvenance() {
        QVERIFY(m_members->addProjectMember(m_folderId,
                                            m_writerId,
                                            QStringLiteral("Writer1"),
                                            false,
                                            QStringLiteral("agent"),
                                            QStringLiteral("some-coordinator-id"),
                                            QStringLiteral("gemini"),
                                            QStringLiteral("gemini-pro"),
                                            {QStringLiteral("read_file")}));

        const QVariantList roster = m_members->projectMembersList(m_folderId);
        QCOMPARE(roster.size(), 1);
        QVERIFY(m_members->setProjectMembersFromList(m_folderId, roster));

        const Member m =
            findAlias(m_members->projectMembers(m_folderId), QStringLiteral("Writer1"));
        QVERIFY(m.isValid());
        QCOMPARE(m.modelProvider, QStringLiteral("gemini"));
        QCOMPARE(m.modelName, QStringLiteral("gemini-pro"));
        QCOMPARE(m.allowedTools, (QStringList{QStringLiteral("read_file")}));
        QCOMPARE(m.addedByKind, QStringLiteral("agent"));
        QCOMPARE(m.addedByAgentId, QStringLiteral("some-coordinator-id"));
    }

    void test_projectMembersList_exposesOverrideFields() {
        QVERIFY(m_members->addProjectMember(m_folderId,
                                            m_writerId,
                                            QStringLiteral("Writer1"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QStringLiteral("openrouter"),
                                            QStringLiteral("auto"),
                                            {QStringLiteral("run_shell")}));

        const QVariantList roster = m_members->projectMembersList(m_folderId);
        QCOMPARE(roster.size(), 1);
        const QVariantMap m = roster.first().toMap();
        QCOMPARE(m.value(QStringLiteral("modelProvider")).toString(), QStringLiteral("openrouter"));
        QCOMPARE(m.value(QStringLiteral("modelName")).toString(), QStringLiteral("auto"));
        QCOMPARE(m.value(QStringLiteral("allowedTools")).toStringList(),
                 (QStringList{QStringLiteral("run_shell")}));
    }

    void test_updateProjectMemberModelOverride_updatesOneMemberOnly() {
        QVERIFY(m_members->addProjectMember(m_folderId,
                                            m_writerId,
                                            QStringLiteral("Writer1"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QStringLiteral("ollama"),
                                            QStringLiteral("gemma"),
                                            {}));
        QVERIFY(m_members->addProjectMember(m_folderId,
                                            m_writerId,
                                            QStringLiteral("Writer2"),
                                            false,
                                            QStringLiteral("user"),
                                            QString(),
                                            QStringLiteral("ollama"),
                                            QStringLiteral("gemma"),
                                            {}));

        QVERIFY(m_members->updateProjectMemberModelOverride(m_folderId,
                                                            QStringLiteral("Writer2"),
                                                            QStringLiteral("anthropic"),
                                                            QStringLiteral("claude-x"),
                                                            {QStringLiteral("read_file")}));

        const QList<Member> all = m_members->projectMembers(m_folderId);
        const Member r1 = findAlias(all, QStringLiteral("Writer1"));
        const Member r2 = findAlias(all, QStringLiteral("Writer2"));
        QCOMPARE(r2.modelProvider, QStringLiteral("anthropic"));
        QCOMPARE(r2.modelName, QStringLiteral("claude-x"));
        QCOMPARE(r2.allowedTools, (QStringList{QStringLiteral("read_file")}));
        QCOMPARE(r1.modelProvider, QStringLiteral("ollama"));
        QCOMPARE(r1.modelName, QStringLiteral("gemma"));
        QVERIFY(r1.allowedTools.isEmpty());
    }

    void test_updateProjectMemberModelOverride_noMatch_returnsFalse() {
        QVERIFY(m_members->addProjectMember(m_folderId, m_writerId, QStringLiteral("Writer1")));
        QVERIFY(!m_members->updateProjectMemberModelOverride(m_folderId,
                                                             QStringLiteral("NoSuchAlias"),
                                                             QStringLiteral("anthropic"),
                                                             QStringLiteral("claude-x"),
                                                             {}));
    }

    void test_conversationMember_overrideRoundTrip() {
        QVERIFY(m_members->addConversationMember(m_groupConvId,
                                                 m_writerId,
                                                 QStringLiteral("Writer1"),
                                                 false,
                                                 QStringLiteral("user"),
                                                 QString(),
                                                 QStringLiteral("deepseek"),
                                                 QStringLiteral("deepseek-chat"),
                                                 {QStringLiteral("search_web")}));

        Member m =
            findAlias(m_members->conversationMembers(m_groupConvId), QStringLiteral("Writer1"));
        QVERIFY(m.isValid());
        QCOMPARE(m.modelProvider, QStringLiteral("deepseek"));
        QCOMPARE(m.allowedTools, (QStringList{QStringLiteral("search_web")}));

        QVERIFY(m_members->updateConversationMemberModelOverride(m_groupConvId,
                                                                 QStringLiteral("Writer1"),
                                                                 QStringLiteral("openai"),
                                                                 QStringLiteral("gpt"),
                                                                 {}));

        m = findAlias(m_members->conversationMembers(m_groupConvId), QStringLiteral("Writer1"));
        QCOMPARE(m.modelProvider, QStringLiteral("openai"));
        QCOMPARE(m.modelName, QStringLiteral("gpt"));
        QVERIFY(m.allowedTools.isEmpty());
    }

    void test_setConversationMemberAlias_roundTrip() {
        const QString convId =
            m_convs->createConversation(QStringLiteral("Chat with @Writer1"), m_folderId);
        QVERIFY(!convId.isEmpty());

        auto before = m_convs->getConversation(convId);
        QVERIFY(before.has_value());
        QVERIFY(before->memberAlias.isEmpty());

        QVERIFY(m_convs->setConversationMemberAlias(convId, QStringLiteral("Writer1")));
        auto after = m_convs->getConversation(convId);
        QVERIFY(after.has_value());
        QCOMPARE(after->memberAlias, QStringLiteral("Writer1"));

        QVERIFY(m_convs->setConversationMemberAlias(convId, QString()));
        auto cleared = m_convs->getConversation(convId);
        QVERIFY(cleared.has_value());
        QVERIFY(cleared->memberAlias.isEmpty());
    }

    void test_normalizedAlias_helper() {
        QCOMPARE(MembershipService::normalizedAlias(QStringLiteral("@Engineer")),
                 QStringLiteral("Engineer"));
        QCOMPARE(MembershipService::normalizedAlias(QStringLiteral("  @@Lead Writer  ")),
                 QStringLiteral("Lead Writer"));
        QCOMPARE(MembershipService::normalizedAlias(QStringLiteral("Bob")), QStringLiteral("Bob"));
        QVERIFY(MembershipService::normalizedAlias(QStringLiteral(" @@ ")).isEmpty());
    }

    void test_addMember_stripsLeadingSigil_bothTables() {
        QVERIFY(m_members->addProjectMember(m_folderId, m_writerId, QStringLiteral("@Engineer")));
        QCOMPARE(
            findAlias(m_members->projectMembers(m_folderId), QStringLiteral("Engineer")).agentId,
            m_writerId);

        QVERIFY(
            m_members->addConversationMember(m_groupConvId, m_writerId, QStringLiteral("@@Robin")));
        QCOMPARE(findAlias(m_members->conversationMembers(m_groupConvId), QStringLiteral("Robin"))
                     .agentId,
                 m_writerId);

        QVERIFY(!m_members->addProjectMember(m_folderId, m_writerId, QStringLiteral("@")));
    }

    void test_aliasKeyedOps_sigilTolerant() {
        QVERIFY(m_members->addConversationMember(
            m_groupConvId, m_writerId, QStringLiteral("Engineer")));

        QCOMPARE(
            m_members->findConversationMemberByAlias(m_groupConvId, QStringLiteral("@Engineer"))
                .agentId,
            m_writerId);
        QVERIFY(m_members->setConversationCoordinator(m_groupConvId, QStringLiteral("@Engineer")));
        QVERIFY(findAlias(m_members->conversationMembers(m_groupConvId), QStringLiteral("Engineer"))
                    .isCoordinator);
        QVERIFY(m_members->removeConversationMember(m_groupConvId, QStringLiteral("@Engineer")));
        QVERIFY(m_members->conversationMembers(m_groupConvId).isEmpty());
    }

    void test_legacySigilRows_repairedOnConstruction() {
        QSqlQuery raw(DbManager::instance().db());
        raw.prepare(QStringLiteral("INSERT INTO project_members(folder_id, agent_id, alias, "
                                   "is_coordinator, joined_at, added_by_kind, added_by_agent_id, "
                                   "model_provider, model_name, allowed_tools) "
                                   "VALUES(?, ?, '@Engineer', 0, 1, 'agent', '', '', '', '')"));
        raw.addBindValue(m_folderId);
        raw.addBindValue(m_writerId);
        QVERIFY(raw.exec());
        QVERIFY(m_members->addProjectMember(m_folderId, m_writerId, QStringLiteral("Taken")));
        QSqlQuery raw2(DbManager::instance().db());
        raw2.prepare(QStringLiteral("INSERT INTO project_members(folder_id, agent_id, alias, "
                                    "is_coordinator, joined_at, added_by_kind, added_by_agent_id, "
                                    "model_provider, model_name, allowed_tools) "
                                    "VALUES(?, ?, '@Taken', 0, 1, 'agent', '', '', '', '')"));
        raw2.addBindValue(m_folderId);
        raw2.addBindValue(m_writerId);
        QVERIFY(raw2.exec());

        auto healed = std::make_unique<MembershipService>(DbManager::instance());
        const QList<Member> after = healed->projectMembers(m_folderId);
        QCOMPARE(findAlias(after, QStringLiteral("Engineer")).agentId, m_writerId);
        QVERIFY(findAlias(after, QStringLiteral("@Engineer")).agentId.isEmpty());
        QCOMPARE(findAlias(after, QStringLiteral("@Taken")).agentId, m_writerId);
        QCOMPARE(findAlias(after, QStringLiteral("Taken")).agentId, m_writerId);
    }
};

QTEST_MAIN(TestMembershipService)
#include "test-membership-service.moc"

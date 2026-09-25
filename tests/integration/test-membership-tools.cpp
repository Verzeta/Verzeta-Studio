// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/member.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/membership-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/tools/membership/membership-tool-deps.h"
#include "../../backend/tools/membership/membership-tools.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>

class TestMembershipTools : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MembershipService> m_members;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<Chat::CascadeController> m_cascade;
    Tools::MembershipToolDeps m_deps;

    QString m_pmAgentId;
    QString m_engineerId;
    QString m_designerId;
    QString m_folderId;
    QString m_convId;

    QString createAgent(const QString& name, const QString& desc, bool isCoord = false) {
        Agent a;
        a.name = name;
        a.description = desc;
        a.systemPrompt = "You are " + name;
        a.iconName = "face-smile";
        a.defaultPattern = "direct";
        a.isCoordinator = isCoord;
        a.createdAt = QDateTime::currentDateTime();
        return m_agents->createAgent(a);
    }

  private slots:
    void init() {
        QVERIFY(m_dbDir.isValid());
        const QString dbPath =
            m_dbDir.path() + QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        m_dbPath = dbPath;
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_router = std::make_unique<ModelRouter>();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_members = std::make_unique<MembershipService>(DbManager::instance());
        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_cascade = std::make_unique<Chat::CascadeController>(*m_convs, *m_router);

        m_pmAgentId = createAgent("Project Manager", "Coordinates work across the team.", true);
        m_engineerId = createAgent("Engineer", "Implements features.");
        m_designerId = createAgent("Visual Designer", "Designs UI and visual assets.");
        QVERIFY(!m_pmAgentId.isEmpty());
        QVERIFY(!m_engineerId.isEmpty());
        QVERIFY(!m_designerId.isEmpty());

        m_folderId = m_convs->createFolder("Test Project");
        QVERIFY(!m_folderId.isEmpty());
        QVERIFY(m_convs->updateFolderMetadata(
            m_folderId, "project", "Test project goal", "Test project description", {}));

        m_convId = m_convs->createConversation("Team Chat", m_folderId);
        QVERIFY(!m_convId.isEmpty());

        QVERIFY(m_members->addProjectMember(m_folderId, m_pmAgentId, "PM", true));
        QVERIFY(m_members->addProjectMember(m_folderId, m_engineerId, "Eng", false));

        m_deps.members = m_members.get();
        m_deps.agents = m_agents.get();
        m_deps.convs = m_convs.get();
        m_deps.cascade = m_cascade.get();
        m_deps.activeConvIdGetter = [this]() { return m_convId; };
    }

    void cleanup() {
        m_cascade.reset();
        m_agents.reset();
        m_members.reset();
        m_convs.reset();
        m_router.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void test_ListAgentTemplates_ReturnsAll() {
        Tools::ListAgentTemplatesTool tool(m_deps);
        const auto result = tool.invoke({}).toObject();
        QCOMPARE(result.value("total").toInt(), 3);
        const auto arr = result.value("templates").toArray();
        QCOMPARE(arr.size(), 3);
    }

    void test_ListAgentTemplates_QueryFiltersCaseInsensitive() {
        Tools::ListAgentTemplatesTool tool(m_deps);
        QJsonObject args;
        args.insert("query", "designer");
        const auto result = tool.invoke(args).toObject();
        const auto arr = result.value("templates").toArray();
        QCOMPARE(arr.size(), 1);
        QCOMPARE(arr.at(0).toObject().value("name").toString(), QStringLiteral("Visual Designer"));
    }


    void test_ListProjectMembers_ReturnsRoster() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::ListProjectMembersTool tool(m_deps);
        const auto result = tool.invoke({}).toObject();
        QCOMPARE(result.value("count").toInt(), 2);
        QCOMPARE(result.value("folder_id").toString(), m_folderId);
    }

    void test_ListProjectMembers_FailsOutsideProject() {
        const QString rootConv = m_convs->createConversation("Solo");
        m_deps.activeConvIdGetter = [rootConv]() { return rootConv; };
        m_cascade->setCurrentResponder("PM", m_pmAgentId);

        Tools::ListProjectMembersTool tool(m_deps);
        const auto result = tool.invoke({}).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("project folder"));
    }


    void test_AddProjectMember_CoordinatorCanAdd_ByName() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Dez");
        args.insert("agent_name", "Visual Designer");
        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(result.value("alias").toString(), QStringLiteral("Dez"));
        QCOMPARE(result.value("agent_id").toString(), m_designerId);

        const auto roster = m_members->projectMembers(m_folderId);
        QCOMPARE(roster.size(), 3);
    }

    void test_AddProjectMember_GroupCaller_JoinsConversationToo() {
        const QString groupId = m_convs->createGroupConversation(
            QStringLiteral("Group Chat"), {m_pmAgentId}, m_folderId);
        QVERIFY(!groupId.isEmpty());
        QVERIFY(m_members->addConversationMember(groupId, m_pmAgentId, "PM", true));

        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::MembershipToolDeps deps = m_deps;
        deps.activeConvIdGetter = [groupId]() { return groupId; };
        Tools::AddProjectMemberTool tool(deps);

        QJsonObject args;
        args.insert("alias", "Dez");
        args.insert("agent_name", "Visual Designer");
        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("status").toString(), QStringLiteral("ok"));
        QVERIFY2(result.value("added_to_conversation").toBool(false),
                 "a group caller must join the member into the chat");
        QCOMPARE(result.value("conversation_id").toString(), groupId);

        QCOMPARE(m_members->projectMembers(m_folderId).size(), 3);
        const auto convRoster = m_members->conversationMembers(groupId);
        bool inConv = false;
        for (const Member& m : convRoster) {
            if (m.alias == QStringLiteral("Dez") && m.agentId == m_designerId) {
                inConv = true;
            }
        }
        QVERIFY2(inConv, "the new member must be routable in the group");
    }

    void test_AddProjectMember_SigiledAlias_StoredClean() {
        const QString groupId = m_convs->createGroupConversation(
            QStringLiteral("Group Chat"), {m_pmAgentId}, m_folderId);
        QVERIFY(!groupId.isEmpty());
        QVERIFY(m_members->addConversationMember(groupId, m_pmAgentId, "PM", true));

        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::MembershipToolDeps deps = m_deps;
        deps.activeConvIdGetter = [groupId]() { return groupId; };
        Tools::AddProjectMemberTool tool(deps);

        QJsonObject args;
        args.insert("alias", "@Engineer");
        args.insert("agent_name", "Visual Designer");
        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("alias").toString(), QStringLiteral("Engineer"));

        bool cleanInProject = false;
        for (const Member& m : m_members->projectMembers(m_folderId)) {
            QVERIFY2(!m.alias.startsWith(QLatin1Char('@')), "no stored alias may carry the sigil");
            if (m.alias == QStringLiteral("Engineer"))
                cleanInProject = true;
        }
        QVERIFY(cleanInProject);
        bool cleanInConv = false;
        for (const Member& m : m_members->conversationMembers(groupId)) {
            QVERIFY(!m.alias.startsWith(QLatin1Char('@')));
            if (m.alias == QStringLiteral("Engineer"))
                cleanInConv = true;
        }
        QVERIFY(cleanInConv);

        QJsonObject dup;
        dup.insert("alias", "@Engineer");
        dup.insert("agent_name", "Visual Designer");
        const auto dupResult = tool.invoke(dup).toObject();
        QVERIFY(dupResult.contains("error"));
        QVERIFY(dupResult.value("error").toString().contains(QStringLiteral("already"),
                                                             Qt::CaseInsensitive));
    }

    void test_AddProjectMember_NonGroupCaller_ProjectOnly() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Dez");
        args.insert("agent_name", "Visual Designer");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(!result.contains("error"));
        QCOMPARE(result.value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(result.value("added_to_conversation").toBool(true), false);
        QCOMPARE(m_members->conversationMembers(m_convId).size(), 0);
    }

    void test_AddProjectMember_CoordinatorCanAdd_ById() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Dez");
        args.insert("agent_id", m_designerId);
        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("agent_id").toString(), m_designerId);
    }

    void test_AddProjectMember_NonCoordinatorRejected() {
        m_cascade->setCurrentResponder("Eng", m_engineerId);
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Dez");
        args.insert("agent_name", "Visual Designer");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("coordinator"));

        QCOMPARE(m_members->projectMembers(m_folderId).size(), 2);
    }

    void test_AddProjectMember_AliasCollisionRejected() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Eng");
        args.insert("agent_name", "Visual Designer");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("already taken"));
    }

    void test_AddProjectMember_UnknownTemplateRejected() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Mystery");
        args.insert("agent_name", "Nonexistent Template");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("No agent template"));
    }

    void test_AddProjectMember_BothIdAndNameRejected() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Dez");
        args.insert("agent_id", m_designerId);
        args.insert("agent_name", "Visual Designer");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("not both"));
    }

    void test_AddProjectMember_OutsideProjectFolderRejected() {
        const QString rootConv = m_convs->createConversation("Solo");
        m_deps.activeConvIdGetter = [rootConv]() { return rootConv; };
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Dez");
        args.insert("agent_name", "Visual Designer");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
    }


    void test_RemoveProjectMember_UserAddedRejected() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::RemoveProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Eng");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("added by the user"));
        QCOMPARE(m_members->projectMembers(m_folderId).size(), 2);
    }

    void test_RemoveProjectMember_AgentAddedAllowed() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool addTool(m_deps);
        QJsonObject addArgs;
        addArgs.insert("alias", "Dez");
        addArgs.insert("agent_name", "Visual Designer");
        QVERIFY2(!addTool.invoke(addArgs).toObject().contains("error"), "agent-add should succeed");
        QCOMPARE(m_members->projectMembers(m_folderId).size(), 3);

        Tools::RemoveProjectMemberTool removeTool(m_deps);
        QJsonObject rmArgs;
        rmArgs.insert("alias", "Dez");
        const auto result = removeTool.invoke(rmArgs).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(m_members->projectMembers(m_folderId).size(), 2);
    }

    void test_AddProjectMember_TagsRowAsAgentAdded() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool addTool(m_deps);
        QJsonObject addArgs;
        addArgs.insert("alias", "Dez");
        addArgs.insert("agent_name", "Visual Designer");
        const auto result = addTool.invoke(addArgs).toObject();
        QVERIFY(!result.contains("error"));
        QCOMPARE(result.value("added_by_kind").toString(), QStringLiteral("agent"));
        QCOMPARE(result.value("added_by_agent_id").toString(), m_pmAgentId);

        const auto roster = m_members->projectMembers(m_folderId);
        bool found = false;
        for (const Member& m : roster) {
            if (m.alias == "Dez") {
                QCOMPARE(m.addedByKind, QStringLiteral("agent"));
                QCOMPARE(m.addedByAgentId, m_pmAgentId);
                found = true;
            }
        }
        QVERIFY(found);
    }

    void test_ListProjectMembers_ExposesRemovableFlag() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool addTool(m_deps);
        QJsonObject addArgs;
        addArgs.insert("alias", "Dez");
        addArgs.insert("agent_name", "Visual Designer");
        QVERIFY(!addTool.invoke(addArgs).toObject().contains("error"));

        Tools::ListProjectMembersTool listTool(m_deps);
        const auto result = listTool.invoke({}).toObject();
        const auto arr = result.value("members").toArray();
        QCOMPARE(arr.size(), 3);
        for (const auto& v : arr) {
            const auto row = v.toObject();
            const QString alias = row.value("alias").toString();
            const bool removable = row.value("removable_by_agent").toBool();
            if (alias == "Dez") {
                QVERIFY2(removable, "agent-added row should be removable_by_agent=true");
            } else {
                QVERIFY2(!removable,
                         qPrintable(QStringLiteral("seeded user-added row %1 "
                                                   "should be removable_by_agent=false")
                                        .arg(alias)));
            }
        }
    }

    void test_RemoveProjectMember_NonCoordinatorRejected() {
        m_cascade->setCurrentResponder("Eng", m_engineerId);
        Tools::RemoveProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "PM");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("coordinator"));
    }

    void test_RemoveProjectMember_SelfRemovalRejected() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::RemoveProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "PM");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("themselves"));
    }

    void test_RemoveProjectMember_AgentAddedCoordinatorLastCoordinatorRejected() {
        m_cascade->setCurrentResponder("PM", m_pmAgentId);
        Tools::AddProjectMemberTool addTool(m_deps);
        QJsonObject addArgs;
        addArgs.insert("alias", "Dez");
        addArgs.insert("agent_name", "Visual Designer");
        addArgs.insert("is_coordinator", true);
        QVERIFY(!addTool.invoke(addArgs).toObject().contains("error"));

        Tools::RemoveProjectMemberTool removeTool(m_deps);
        QJsonObject rmDez;
        rmDez.insert("alias", "Dez");
        const auto r = removeTool.invoke(rmDez).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
    }
};

QTEST_MAIN(TestMembershipTools)
#include "test-membership-tools.moc"

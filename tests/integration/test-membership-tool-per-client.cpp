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

class TestMembershipToolPerClient : public QObject {
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

    QString m_localPmAgentId;
    QString m_wirePmAgentId;
    QString m_wireEngAgentId;
    QString m_designerId;
    QString m_localFolderId;
    QString m_wireFolderId;
    QString m_localConvId;
    QString m_wireConvId;

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

        m_localPmAgentId = createAgent("LOCAL_PM", "Local-side coordinator", true);
        m_wirePmAgentId = createAgent("WIRE_PM", "Wire-side coordinator", true);
        m_wireEngAgentId = createAgent("WIRE_Engineer", "Wire-side non-coordinator", false);
        m_designerId = createAgent("Visual Designer", "Designs UI", false);
        QVERIFY(!m_localPmAgentId.isEmpty());
        QVERIFY(!m_wirePmAgentId.isEmpty());
        QVERIFY(!m_wireEngAgentId.isEmpty());
        QVERIFY(!m_designerId.isEmpty());

        m_localFolderId = m_convs->createFolder("Local Project");
        m_wireFolderId = m_convs->createFolder("Wire Project");
        QVERIFY(!m_localFolderId.isEmpty());
        QVERIFY(!m_wireFolderId.isEmpty());
        QVERIFY(m_convs->updateFolderMetadata(
            m_localFolderId, "project", "Local goal", "Local desc", {}));
        QVERIFY(
            m_convs->updateFolderMetadata(m_wireFolderId, "project", "Wire goal", "Wire desc", {}));

        m_localConvId = m_convs->createConversation("Local Team", m_localFolderId);
        m_wireConvId = m_convs->createConversation("Wire Team", m_wireFolderId);
        QVERIFY(!m_localConvId.isEmpty());
        QVERIFY(!m_wireConvId.isEmpty());

        QVERIFY(m_members->addProjectMember(m_localFolderId, m_localPmAgentId, "LOCAL_PM", true));

        QVERIFY(m_members->addProjectMember(m_wireFolderId, m_wirePmAgentId, "WIRE_PM", true));
        QVERIFY(
            m_members->addProjectMember(m_wireFolderId, m_wireEngAgentId, "WIRE_Engineer", false));

        m_deps.members = m_members.get();
        m_deps.agents = m_agents.get();
        m_deps.convs = m_convs.get();
        m_deps.cascade = m_cascade.get();
        m_deps.activeConvIdGetter = [this]() { return m_localConvId; };
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


    void test_addMember_adversarial_wireNonCoordinator_localCoordinator_REJECTS() {
        m_cascade->setCurrentResponder("LOCAL_PM", m_localPmAgentId);

        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Mallory");
        args.insert("agent_name", "Visual Designer");
        args.insert("__caller_conv_id", m_wireConvId);
        args.insert("__caller_agent_id", m_wireEngAgentId);
        args.insert("__caller_agent_alias", QStringLiteral("WIRE_Engineer"));

        const auto result = tool.invoke(args).toObject();
        QVERIFY2(result.contains("error"),
                 "Coordinator gate must REJECT a wire-side non-coordinator's "
                 "add_project_member even when the LOCAL cascade has a "
                 "coordinator seated.");

        const QString err = result.value("error").toString();
        QVERIFY2(
            err.contains("coordinator", Qt::CaseInsensitive),
            qPrintable(QStringLiteral("Error message should reference coordinator gate, got: %1")
                           .arg(err)));

        const auto roster = m_members->projectMembers(m_wireFolderId);
        for (const Member& m : roster) {
            QVERIFY2(m.alias != "Mallory", "Mallory must NOT have been added — gate was bypassed");
        }
        QCOMPARE(roster.size(), 2);
    }


    void test_addMember_wireCoordinator_succeeds_whenLocalIdle() {
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Mallory");
        args.insert("agent_name", "Visual Designer");
        args.insert("__caller_conv_id", m_wireConvId);
        args.insert("__caller_agent_id", m_wirePmAgentId);
        args.insert("__caller_agent_alias", QStringLiteral("WIRE_PM"));

        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"),
                 qPrintable(QStringLiteral("Wire-side coordinator add must succeed; got error: %1")
                                .arg(result.value("error").toString())));
        QCOMPARE(result.value("status").toString(), QStringLiteral("ok"));

        const auto roster = m_members->projectMembers(m_wireFolderId);
        QCOMPARE(roster.size(), 3);
        bool foundMallory = false;
        for (const Member& m : roster) {
            if (m.alias == "Mallory") {
                foundMallory = true;
                break;
            }
        }
        QVERIFY(foundMallory);
    }


    void test_addMember_fallsBackToCascade_whenArgsEmpty() {
        m_cascade->setCurrentResponder("LOCAL_PM", m_localPmAgentId);

        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Dez");
        args.insert("agent_name", "Visual Designer");

        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));

        const auto roster = m_members->projectMembers(m_localFolderId);
        QCOMPARE(roster.size(), 2);
    }


    void test_addMember_argsConvId_overridesCapturedGetter() {
        m_cascade->setCurrentResponder("LOCAL_PM", m_localPmAgentId);

        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Eve");
        args.insert("agent_name", "Visual Designer");
        args.insert("__caller_conv_id", m_wireConvId);
        args.insert("__caller_agent_id", m_wirePmAgentId);
        args.insert("__caller_agent_alias", QStringLiteral("WIRE_PM"));

        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));

        const auto wireRoster = m_members->projectMembers(m_wireFolderId);
        const auto localRoster = m_members->projectMembers(m_localFolderId);
        bool foundEveInWire = false;
        bool foundEveInLocal = false;
        for (const Member& m : wireRoster)
            if (m.alias == "Eve")
                foundEveInWire = true;
        for (const Member& m : localRoster)
            if (m.alias == "Eve")
                foundEveInLocal = true;
        QVERIFY(foundEveInWire);
        QVERIFY2(!foundEveInLocal, "Eve must NOT be in LOCAL folder — args conv id selects WIRE");
    }


    void test_addMember_errorsCleanly_whenBothEmpty() {
        Tools::AddProjectMemberTool tool(m_deps);
        QJsonObject args;
        args.insert("alias", "Ghost");
        args.insert("agent_name", "Visual Designer");

        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        const QString err = result.value("error").toString();
        QVERIFY2(
            err.contains("responder", Qt::CaseInsensitive) ||
                err.contains("cascade", Qt::CaseInsensitive),
            qPrintable(QStringLiteral("Expected 'no responder identity' error, got: %1").arg(err)));
    }
};

QTEST_MAIN(TestMembershipToolPerClient)
#include "test-membership-tool-per-client.moc"

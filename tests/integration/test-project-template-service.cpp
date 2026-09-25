// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/membership-service.h"
#include "../../backend/services/project-template-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>
#include <QVariantMap>

class TestProjectTemplateService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_dir;
    QString m_dbPath;
    QString m_userTemplateDir;
    int m_testSeq = 0;
    std::unique_ptr<ConversationService> m_conv;
    std::unique_ptr<MembershipService> m_membership;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ProjectTemplateService> m_svc;

  private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        m_dbPath =
            m_dir.path() + QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        m_userTemplateDir = m_dir.path() + QStringLiteral("/user-templates-%1").arg(++m_testSeq);

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_conv = std::make_unique<ConversationService>(DbManager::instance());
        m_membership = std::make_unique<MembershipService>(DbManager::instance());
        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();

        m_svc = std::make_unique<ProjectTemplateService>(
            *m_conv, *m_membership, *m_agents, DbManager::instance(), m_userTemplateDir);
    }

    void cleanup() {
        m_svc.reset();
        m_agents.reset();
        m_membership.reset();
        m_conv.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_templates_loadsAllEightBundled() {
        const QVariantList all = m_svc->templates();
        QCOMPARE(all.size(), 8);

        for (const QVariant& v : all) {
            const QVariantMap row = v.toMap();
            QVERIFY2(!row.value("id").toString().isEmpty(), "template missing id");
            QVERIFY2(!row.value("name").toString().isEmpty(), "template missing name");
            QVERIFY2(!row.value("category").toString().isEmpty(), "template missing category");
            QVERIFY2(!row.value("coordinator").toString().isEmpty(),
                     "template missing coordinator");
            QVERIFY2(row.value("members").toList().size() >= 1,
                     "template should have at least one non-coordinator member");
        }
    }

    void test_templates_coverEveryFilterCategory() {
        const QVariantList all = m_svc->templates();
        QSet<QString> categoriesSeen;
        for (const QVariant& v : all) {
            categoriesSeen.insert(v.toMap().value("category").toString());
        }
        QVERIFY(categoriesSeen.contains(QStringLiteral("discovery")));
        QVERIFY(categoriesSeen.contains(QStringLiteral("engineering")));
        QVERIFY(categoriesSeen.contains(QStringLiteral("marketing")));
        QVERIFY(categoriesSeen.contains(QStringLiteral("sales")));
        QVERIFY(categoriesSeen.contains(QStringLiteral("exec")));
    }

    void test_templateById_unknownId_returnsEmpty() {
        const QVariantMap row = m_svc->templateById(QStringLiteral("does-not-exist"));
        QVERIFY(row.isEmpty());
    }

    void test_templateById_knownId_returnsFullRow() {
        const QVariantMap row = m_svc->templateById(QStringLiteral("research-brief"));
        QVERIFY(!row.isEmpty());
        QCOMPARE(row.value("id").toString(), QStringLiteral("research-brief"));
        QCOMPARE(row.value("category").toString(), QStringLiteral("discovery"));
        QCOMPARE(row.value("coordinator").toString(), QStringLiteral("Project Manager"));
    }

    void test_createFromTemplate_happyPath_writesFolderAndMembers() {
        QSignalSpy spy(m_svc.get(), &ProjectTemplateService::projectCreatedFromTemplate);
        QVERIFY(spy.isValid());

        const QString folderId =
            m_svc->createProjectFromTemplate(QStringLiteral("research-brief"), {});

        QVERIFY2(!folderId.isEmpty(), "createProjectFromTemplate returned empty");

        const auto folderOpt = m_conv->getFolder(folderId);
        QVERIFY(folderOpt.has_value());
        QCOMPARE(folderOpt->folderType, QStringLiteral("project"));
        QVERIFY2(!folderOpt->goal.isEmpty(), "goal not propagated");

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT alias, is_coordinator FROM project_members "
                                 "WHERE folder_id = ? ORDER BY alias"));
        q.addBindValue(folderId);
        QVERIFY(q.exec());
        QStringList aliases;
        int coordCount = 0;
        while (q.next()) {
            aliases.append(q.value(0).toString());
            if (q.value(1).toInt() != 0)
                ++coordCount;
        }
        QCOMPARE(aliases.size(), 4);
        QCOMPARE(coordCount, 1);
        QVERIFY(aliases.contains(QStringLiteral("Project Manager")));
        QVERIFY(aliases.contains(QStringLiteral("Researcher")));
        QVERIFY(aliases.contains(QStringLiteral("Analyst")));
        QVERIFY(aliases.contains(QStringLiteral("Reviewer")));

        QCOMPARE(spy.count(), 1);
        const auto args = spy.takeFirst();
        QCOMPARE(args.at(0).toString(), folderId);
        QCOMPARE(args.at(1).toString(), QStringLiteral("Research Brief"));
        QCOMPARE(args.at(2).toInt(), 4);
    }

    void test_createFromTemplate_customisations_override() {
        const QVariantMap cust = {
            {QStringLiteral("name"), QStringLiteral("Custom Project Name")},
            {QStringLiteral("goal"), QStringLiteral("Custom goal text")},
        };
        const QString folderId =
            m_svc->createProjectFromTemplate(QStringLiteral("technical-spec"), cust);
        QVERIFY(!folderId.isEmpty());

        const auto folderOpt = m_conv->getFolder(folderId);
        QVERIFY(folderOpt.has_value());
        QCOMPARE(folderOpt->name, QStringLiteral("Custom Project Name"));
        QCOMPARE(folderOpt->goal, QStringLiteral("Custom goal text"));
    }

    void test_createFromTemplate_editedRoster_agentIdAndOverrides() {
        const QString pmId = m_agents->getAgentByName(QStringLiteral("Project Manager")).id;
        const QString resId = m_agents->getAgentByName(QStringLiteral("Researcher")).id;
        QVERIFY(!pmId.isEmpty());
        QVERIFY(!resId.isEmpty());

        QVariantList members;
        members.append(QVariantMap{
            {QStringLiteral("agentId"), pmId},
            {QStringLiteral("alias"), QStringLiteral("Lead")},
            {QStringLiteral("isCoordinator"), true},
        });
        members.append(QVariantMap{
            {QStringLiteral("agentId"), resId},
            {QStringLiteral("alias"), QStringLiteral("Digger")},
            {QStringLiteral("isCoordinator"), false},
            {QStringLiteral("modelProvider"), QStringLiteral("openrouter")},
            {QStringLiteral("modelName"), QStringLiteral("openrouter/free")},
            {QStringLiteral("allowedTools"),
             QStringList{QStringLiteral("read_file"), QStringLiteral("search_web")}},
        });

        const QVariantMap cust = {
            {QStringLiteral("members"), members},
        };
        const QString folderId =
            m_svc->createProjectFromTemplate(QStringLiteral("research-brief"), cust);
        QVERIFY2(!folderId.isEmpty(), "edited-roster create should succeed");

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT model_provider, model_name, allowed_tools "
                                 "FROM project_members WHERE folder_id = ? AND alias = ?"));
        q.addBindValue(folderId);
        q.addBindValue(QStringLiteral("Digger"));
        QVERIFY(q.exec());
        QVERIFY2(q.next(), "override member row not found");
        QCOMPARE(q.value(0).toString(), QStringLiteral("openrouter"));
        QCOMPARE(q.value(1).toString(), QStringLiteral("openrouter/free"));
        const QString toolsJson = q.value(2).toString();
        QVERIFY2(toolsJson.contains(QStringLiteral("read_file")) &&
                     toolsJson.contains(QStringLiteral("search_web")),
                 "allowed_tools not persisted");

        QSqlQuery q2(DbManager::instance().db());
        q2.prepare(QStringLiteral("SELECT model_provider, model_name FROM project_members "
                                  "WHERE folder_id = ? AND alias = ?"));
        q2.addBindValue(folderId);
        q2.addBindValue(QStringLiteral("Lead"));
        QVERIFY(q2.exec());
        QVERIFY(q2.next());
        QVERIFY(q2.value(0).toString().isEmpty());
        QVERIFY(q2.value(1).toString().isEmpty());
    }

    void test_createFromTemplate_rollback_onUnknownAgentTemplate() {
        QSignalSpy spy(m_svc.get(), &ProjectTemplateService::projectCreatedFromTemplate);
        QVERIFY(spy.isValid());

        QSqlQuery before(DbManager::instance().db());
        QVERIFY(before.exec(QStringLiteral("SELECT COUNT(*) FROM folders")));
        QVERIFY(before.next());
        const int foldersBefore = before.value(0).toInt();

        QVERIFY(before.exec(QStringLiteral("SELECT COUNT(*) FROM project_members")));
        QVERIFY(before.next());
        const int membersBefore = before.value(0).toInt();

        QVariantMap cust;
        QVariantList members;
        QVariantMap badMember;
        badMember.insert(QStringLiteral("alias"), QStringLiteral("Ghost"));
        badMember.insert(QStringLiteral("agentTemplate"), QStringLiteral("No Such Agent Template"));
        badMember.insert(QStringLiteral("isCoordinator"), true);
        members.append(badMember);
        cust.insert(QStringLiteral("members"), members);

        const QString folderId =
            m_svc->createProjectFromTemplate(QStringLiteral("research-brief"), cust);

        QVERIFY2(folderId.isEmpty(), "createProjectFromTemplate should return empty on failure");

        QSqlQuery after(DbManager::instance().db());
        QVERIFY(after.exec(QStringLiteral("SELECT COUNT(*) FROM folders")));
        QVERIFY(after.next());
        QCOMPARE(after.value(0).toInt(), foldersBefore);

        QVERIFY(after.exec(QStringLiteral("SELECT COUNT(*) FROM project_members")));
        QVERIFY(after.next());
        QCOMPARE(after.value(0).toInt(), membersBefore);

        QCOMPARE(spy.count(), 0);
    }

    void test_templateRoster_resolvesNamesToAgentIds() {
        const QVariantList roster = m_svc->templateRoster(QStringLiteral("research-brief"));
        QCOMPARE(roster.size(), 4);

        int coordCount = 0;
        for (const QVariant& rv : roster) {
            const QVariantMap r = rv.toMap();
            QVERIFY2(!r.value(QStringLiteral("agentId")).toString().isEmpty(),
                     "roster row missing agentId");
            QVERIFY(!r.value(QStringLiteral("alias")).toString().isEmpty());
            if (r.value(QStringLiteral("isCoordinator")).toBool())
                ++coordCount;
        }
        QCOMPARE(coordCount, 1);

        QVERIFY(m_svc->templateRoster(QStringLiteral("no-such-template")).isEmpty());
    }

    void test_createFromTemplate_unknownTemplateId_returnsEmpty() {
        const QString result =
            m_svc->createProjectFromTemplate(QStringLiteral("no-such-template"), {});
        QVERIFY(result.isEmpty());
    }


    QVariantMap sampleEdits(const QString& name) {
        const QString resId = m_agents->getAgentByName(QStringLiteral("Researcher")).id;
        QVariantList members;
        members.append(QVariantMap{
            {"agentId", resId},
            {"alias", QStringLiteral("R")},
            {"isCoordinator", true},
        });
        QVariantMap edits;
        edits.insert("name", name);
        edits.insert("members", members);
        return edits;
    }

    void test_landingTemplates_isBuiltinsOnly_whenNoUserTemplates() {
        const QVariantList landing = m_svc->landingTemplates();
        QCOMPARE(landing.size(), 8);
        for (const QVariant& v : landing) {
            QCOMPARE(v.toMap().value("isUserSaved").toBool(), false);
        }
    }

    void test_saveAsNewTemplate_appendsToCatalogNotLanding() {
        QSignalSpy spy(m_svc.get(), &ProjectTemplateService::catalogChanged);
        QVERIFY(spy.isValid());

        const QString newId = m_svc->saveAsNewTemplate(
            QStringLiteral("research-brief"), sampleEdits(QStringLiteral("My Saved Template")));
        QVERIFY2(!newId.isEmpty(), "saveAsNewTemplate returned empty");
        QVERIFY(newId.startsWith(QStringLiteral("user-")));
        QCOMPARE(spy.count(), 1);

        QCOMPARE(m_svc->templates().size(), 9);
        const QVariantMap row = m_svc->templateById(newId);
        QVERIFY(!row.isEmpty());
        QCOMPARE(row.value("isUserSaved").toBool(), true);
        QCOMPARE(row.value("name").toString(), QStringLiteral("My Saved Template"));

        QVERIFY(QFile::exists(m_userTemplateDir + QStringLiteral("/") + newId +
                              QStringLiteral(".json")));

        QCOMPARE(m_svc->landingTemplates().size(), 8);
    }

    void test_saveAsNewTemplate_emptyName_fails() {
        QVariantMap edits = sampleEdits(QString());
        QVERIFY(m_svc->saveAsNewTemplate(QString(), edits).isEmpty());
        QCOMPARE(m_svc->templates().size(), 8);
    }

    void test_userTemplate_pinning_addsAndRemovesFromLanding() {
        const QString id =
            m_svc->saveAsNewTemplate(QString(), sampleEdits(QStringLiteral("Pinnable")));
        QVERIFY(!id.isEmpty());
        QCOMPARE(m_svc->landingTemplates().size(), 8);

        QVERIFY(m_svc->setTemplatePinned(id, true));
        QCOMPARE(m_svc->landingTemplates().size(), 9);
        QCOMPARE(m_svc->templateById(id).value("isPinned").toBool(), true);

        QVERIFY(m_svc->setTemplatePinned(id, false));
        QCOMPARE(m_svc->landingTemplates().size(), 8);
        QCOMPARE(m_svc->templateById(id).value("isPinned").toBool(), false);
    }

    void test_setTemplatePinned_rejectsBuiltin() {
        QVERIFY(!m_svc->setTemplatePinned(QStringLiteral("research-brief"), true));
        QCOMPARE(m_svc->landingTemplates().size(), 8);
    }

    void test_deleteUserTemplate_removesRowAndFile() {
        const QString id =
            m_svc->saveAsNewTemplate(QString(), sampleEdits(QStringLiteral("Throwaway")));
        QVERIFY(!id.isEmpty());
        QCOMPARE(m_svc->templates().size(), 9);
        const QString file = m_userTemplateDir + QStringLiteral("/") + id + QStringLiteral(".json");
        QVERIFY(QFile::exists(file));

        QVERIFY(m_svc->deleteUserTemplate(id));
        QCOMPARE(m_svc->templates().size(), 8);
        QVERIFY(m_svc->templateById(id).isEmpty());
        QVERIFY(!QFile::exists(file));

        QVERIFY(!m_svc->templateById(QStringLiteral("research-brief")).isEmpty());
        QVERIFY(!m_svc->templateById(QStringLiteral("quarterly-business-review")).isEmpty());
    }

    void test_deleteUserTemplate_rejectsBuiltin() {
        QVERIFY(!m_svc->deleteUserTemplate(QStringLiteral("research-brief")));
        QCOMPARE(m_svc->templates().size(), 8);
    }

    void test_userTemplates_loadedFromDiskOnConstruction() {
        const QString id =
            m_svc->saveAsNewTemplate(QString(), sampleEdits(QStringLiteral("Persisted")));
        QVERIFY(!id.isEmpty());
        QVERIFY(m_svc->setTemplatePinned(id, true));

        ProjectTemplateService fresh(
            *m_conv, *m_membership, *m_agents, DbManager::instance(), m_userTemplateDir);
        QCOMPARE(fresh.templates().size(), 9);
        QVERIFY(!fresh.templateById(id).isEmpty());
        QCOMPARE(fresh.landingTemplates().size(), 9);
        QCOMPARE(fresh.templateById(id).value("isPinned").toBool(), true);
    }

    void test_createBlankProject_emptyTemplateId_buildsFromCustomisations() {
        const QString pmId = m_agents->getAgentByName(QStringLiteral("Project Manager")).id;
        QVERIFY(!pmId.isEmpty());

        QVariantList members;
        members.append(QVariantMap{
            {"agentId", pmId},
            {"alias", QStringLiteral("Lead")},
            {"isCoordinator", true},
        });
        QVariantMap cust;
        cust.insert("name", QStringLiteral("Blank Built Project"));
        cust.insert("goal", QStringLiteral("from scratch"));
        cust.insert("members", members);

        const QString folderId = m_svc->createProjectFromTemplate(QString(), cust);
        QVERIFY2(!folderId.isEmpty(), "blank-project create should succeed");

        const auto folderOpt = m_conv->getFolder(folderId);
        QVERIFY(folderOpt.has_value());
        QCOMPARE(folderOpt->folderType, QStringLiteral("project"));
        QCOMPARE(folderOpt->name, QStringLiteral("Blank Built Project"));
        QCOMPARE(folderOpt->goal, QStringLiteral("from scratch"));
    }

    void test_templateRoster_userTemplate_resolvesByAgentId() {
        const QString pmId = m_agents->getAgentByName(QStringLiteral("Project Manager")).id;
        const QString resId = m_agents->getAgentByName(QStringLiteral("Researcher")).id;
        QVariantList members;
        members.append(QVariantMap{
            {"agentId", pmId}, {"alias", QStringLiteral("Boss")}, {"isCoordinator", true}});
        members.append(QVariantMap{
            {"agentId", resId}, {"alias", QStringLiteral("Helper")}, {"isCoordinator", false}});
        QVariantMap edits;
        edits.insert("name", QStringLiteral("Roster Test"));
        edits.insert("members", members);
        const QString id = m_svc->saveAsNewTemplate(QString(), edits);
        QVERIFY(!id.isEmpty());

        const QVariantList roster = m_svc->templateRoster(id);
        QCOMPARE(roster.size(), 2);
        int coord = 0;
        for (const QVariant& rv : roster) {
            const QVariantMap r = rv.toMap();
            QVERIFY(!r.value("agentId").toString().isEmpty());
            QVERIFY(!r.value("agentName").toString().isEmpty());
            if (r.value("isCoordinator").toBool())
                ++coord;
        }
        QCOMPARE(coord, 1);
    }

    void test_saveAsNewTemplate_usesEditsGeometryAndHue() {
        QVariantMap edits = sampleEdits(QStringLiteral("Designed"));
        edits.insert("geometryKind", QStringLiteral("spiral"));
        edits.insert("baseHue", 45);
        const QString id = m_svc->saveAsNewTemplate(QString(), edits);
        QVERIFY(!id.isEmpty());
        const QVariantMap row = m_svc->templateById(id);
        QCOMPARE(row.value("geometryKind").toString(), QStringLiteral("spiral"));
        QCOMPARE(row.value("baseHue").toInt(), 45);
    }
};

QTEST_MAIN(TestProjectTemplateService)
#include "test-project-template-service.moc"

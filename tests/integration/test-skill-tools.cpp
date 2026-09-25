// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/skill-service.h"
#include "../../backend/tools/skills/discover-skills-tool.h"
#include "../../backend/tools/skills/read-skill-file-tool.h"
#include "../../backend/tools/skills/read-skill-tool.h"
#include "../../backend/tools/skills/skill-tool-deps.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSqlDatabase>

class TestSkillTools : public QObject {
    Q_OBJECT

  private:
    static void writeFile(const QString& abs, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
    }
    void seedAndApprove(const QString& id, const QString& desc = "Test") {
        const QString folder = m_srcDir->path() + "/" + id;
        const QString fm = QStringLiteral("---\nname: %1\ndescription: %2\nversion: 1.0.0\n---\n"
                                          "Body of %1.\n")
                               .arg(id, desc);
        writeFile(folder + "/SKILL.md", fm.toUtf8());
        writeFile(folder + "/extras/note.txt", "Helper note for " + id.toUtf8() + ".\n");
        QVERIFY(m_skills->importSkillFolder(folder).isEmpty());
        QVERIFY(m_skills->approveSkill(id));
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

        m_appData = std::make_unique<QTemporaryDir>();
        m_srcDir = std::make_unique<QTemporaryDir>();
        m_router = std::make_unique<ModelRouter>();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_skills = std::make_unique<SkillService>(*m_convs);
        m_skills->setAppDataRootForTesting(m_appData->path());
        m_skills->initialize();
        m_cascade = std::make_unique<Chat::CascadeController>(*m_convs, *m_router);

        m_convId = m_convs->createConversation("c");
        m_cascade->setCurrentResponder("Alice", "agent-x");

        seedAndApprove("twitter-campaign-planner",
                       "Plan and draft platform-specific campaign posts.");
        seedAndApprove("github-issue-triage", "Sort inbound GitHub issues by severity and label.");

        m_deps.skills = m_skills.get();
        m_deps.convs = m_convs.get();
        m_deps.cascade = m_cascade.get();
        m_deps.activeConvIdGetter = [this]() { return m_convId; };
    }

    void cleanup() {
        m_cascade.reset();
        m_skills.reset();
        m_convs.reset();
        m_router.reset();
        m_srcDir.reset();
        m_appData.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void test_DiscoverSkillsReturnsApproved() {
        Tools::DiscoverSkillsTool t(m_deps);
        const auto result = t.invoke({}).toObject();
        QCOMPARE(result.value("total").toInt(), 2);
        const auto arr = result.value("skills").toArray();
        QCOMPARE(arr.size(), 2);
    }

    void test_DiscoverSkillsQueryFilter() {
        Tools::DiscoverSkillsTool t(m_deps);
        QJsonObject args;
        args.insert("query", "github");
        const auto result = t.invoke(args).toObject();
        const auto arr = result.value("skills").toArray();
        QCOMPARE(arr.size(), 1);
        QCOMPARE(arr.at(0).toObject().value("id").toString(),
                 QStringLiteral("github-issue-triage"));
    }

    void test_DiscoverSkillsDisabledWhenExposeOnly() {
        QVERIFY(m_skills->setPreferredSkills(
            "conversation_1to1", m_convId, {"twitter-campaign-planner"}));
        QVERIFY(m_skills->setExposeOnlyPreferred("conversation_1to1", m_convId, true));
        Tools::DiscoverSkillsTool t(m_deps);
        const auto result = t.invoke({}).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("disabled"));
    }


    void test_ReadSkillReturnsContent() {
        Tools::ReadSkillTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "twitter-campaign-planner");
        const auto result = t.invoke(args).toObject();
        QVERIFY(!result.contains("error"));
        const QString content = result.value("content").toString();
        QVERIFY(content.contains("twitter-campaign-planner"));
        QVERIFY(content.contains("Body of"));
        QVERIFY(content.contains("authoritative steps"));
        QVERIFY(content.contains("Follow them in order"));
    }

    void test_ReadSkillRejectsBlocked() {
        QVERIFY(m_skills->blockSkill("twitter-campaign-planner"));
        Tools::ReadSkillTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "twitter-campaign-planner");
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
    }

    void test_ReadSkillRejectsNonPreferredWhenExposeOnly() {
        QVERIFY(m_skills->setPreferredSkills(
            "conversation_1to1", m_convId, {"twitter-campaign-planner"}));
        QVERIFY(m_skills->setExposeOnlyPreferred("conversation_1to1", m_convId, true));
        Tools::ReadSkillTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "github-issue-triage");
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("exposeOnly"));
    }


    void test_ReadSkillFileSupportingFile() {
        Tools::ReadSkillFileTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "twitter-campaign-planner");
        args.insert("relative_path", "extras/note.txt");
        const auto result = t.invoke(args).toObject();
        QVERIFY(!result.contains("error"));
        QVERIFY(result.value("content").toString().contains("Helper note"));
    }

    void test_ReadSkillFilePathTraversal() {
        Tools::ReadSkillFileTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "twitter-campaign-planner");
        args.insert("relative_path", "../etc/passwd");
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("invalid"));
    }

    void test_ReadSkillFileBinaryDetection() {
        const QString folder = m_srcDir->path() + "/twitter-campaign-planner";
        const QString installRoot = m_skills->installPathFor("twitter-campaign-planner");
        QVERIFY(!installRoot.isEmpty());
        QFile bin(installRoot + "/extras/blob.bin");
        QVERIFY(bin.open(QIODevice::WriteOnly));
        bin.write("hello\0world", 11);
        bin.close();
        Tools::ReadSkillFileTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "twitter-campaign-planner");
        args.insert("relative_path", "extras/blob.bin");
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("binary"));
    }

    void test_NoActiveResponderRefusesAll() {
        Tools::SkillToolDeps emptyDeps = m_deps;
        emptyDeps.activeConvIdGetter = []() { return QString(); };
        Tools::DiscoverSkillsTool d(emptyDeps);
        Tools::ReadSkillTool r(emptyDeps);
        Tools::ReadSkillFileTool f(emptyDeps);
        const auto rd = d.invoke({}).toObject();
        QJsonObject ra;
        ra.insert("skill_id", "twitter-campaign-planner");
        const auto rr = r.invoke(ra).toObject();
        QJsonObject rfArgs;
        rfArgs.insert("skill_id", "twitter-campaign-planner");
        rfArgs.insert("relative_path", "extras/note.txt");
        const auto rf = f.invoke(rfArgs).toObject();
        QVERIFY(rd.contains("error"));
        QVERIFY(rr.contains("error"));
        QVERIFY(rf.contains("error"));
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    QString m_convId;
    Tools::SkillToolDeps m_deps;
    std::unique_ptr<QTemporaryDir> m_appData;
    std::unique_ptr<QTemporaryDir> m_srcDir;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<SkillService> m_skills;
    std::unique_ptr<Chat::CascadeController> m_cascade;
};

QTEST_MAIN(TestSkillTools)
#include "test-skill-tools.moc"

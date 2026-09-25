// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/tool-calling-schema.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/skill-service.h"
#include "../../backend/services/tool-service.h"
#include "../../backend/tools/skills/get-skill-status-tool.h"
#include "../../backend/tools/skills/skill-tool-deps.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>

class TestGetSkillStatusTool : public QObject {
    Q_OBJECT

  private:
    static void writeFile(const QString& abs, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
    }
    void seedSkill(const QString& id, const QString& desc, const QStringList& declaredTools = {}) {
        const QString folder = m_srcDir->path() + "/" + id;
        QString fm =
            QStringLiteral("---\nname: %1\ndescription: %2\nversion: 1.0.0\n").arg(id, desc);
        if (!declaredTools.isEmpty()) {
            fm += QStringLiteral("tools: [%1]\n").arg(declaredTools.join(QStringLiteral(", ")));
        }
        fm += QStringLiteral("---\nBody.\n");
        writeFile(folder + "/SKILL.md", fm.toUtf8());
        QVERIFY(m_skills->importSkillFolder(folder).isEmpty());
        QVERIFY(m_skills->approveSkill(id));
    }
    void registerStub(const QString& name) {
        ToolSchema sch;
        sch.name = name;
        sch.description = QStringLiteral("stub");
        ToolHandler h = [](const QJsonObject&) -> QJsonValue { return QJsonObject{}; };
        m_tools->registerTool(sch, h);
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
        m_tools = std::make_unique<ToolService>();

        m_convId = m_convs->createConversation("c");
        m_cascade->setCurrentResponder("Alice", "agent-x");

        m_deps.skills = m_skills.get();
        m_deps.convs = m_convs.get();
        m_deps.cascade = m_cascade.get();
        m_deps.tools = m_tools.get();
        m_deps.activeConvIdGetter = [this]() { return m_convId; };
    }

    void cleanup() {
        m_tools.reset();
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

    void test_Name_Stable() {
        Tools::GetSkillStatusTool t(m_deps);
        QCOMPARE(t.name(), QStringLiteral("get_skill_status"));
    }

    void test_MissingSkillId_ReturnsError() {
        Tools::GetSkillStatusTool t(m_deps);
        QJsonObject args;
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("skill_id"));
    }

    void test_UnknownSkill_ReportsNotInstalledNotReady() {
        Tools::GetSkillStatusTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "no-such-skill");
        const auto result = t.invoke(args).toObject();
        QVERIFY(!result.contains("error"));
        QCOMPARE(result.value("installed").toBool(), false);
        QCOMPARE(result.value("approved").toBool(), false);
        QCOMPARE(result.value("ready").toBool(), false);
        QCOMPARE(result.value("declared_tools").toArray().size(), 0);
        QCOMPARE(result.value("missing_tools").toArray().size(), 0);
    }

    void test_SkillWithAllToolsAvailable_ReportsReady() {
        seedSkill("read-only-skill", "Reads files.", {"read_file", "list_files"});
        registerStub("read_file");
        registerStub("list_files");

        Tools::GetSkillStatusTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "read-only-skill");
        const auto result = t.invoke(args).toObject();
        QCOMPARE(result.value("installed").toBool(), true);
        QCOMPARE(result.value("approved").toBool(), true);
        QCOMPARE(result.value("ready").toBool(), true);
        const QJsonArray declared = result.value("declared_tools").toArray();
        QCOMPARE(declared.size(), 2);
        QCOMPARE(result.value("missing_tools").toArray().size(), 0);
        QCOMPARE(result.value("available_tools").toArray().size(), 2);
    }

    void test_SkillWithMissingTool_ReportsBlocked() {
        seedSkill("shell-skill", "Runs shell.", {"run_shell", "read_file"});
        registerStub("read_file");

        Tools::GetSkillStatusTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "shell-skill");
        const auto result = t.invoke(args).toObject();
        QCOMPARE(result.value("installed").toBool(), true);
        QCOMPARE(result.value("approved").toBool(), true);
        QCOMPARE(result.value("ready").toBool(), false);
        const QJsonArray missing = result.value("missing_tools").toArray();
        QCOMPARE(missing.size(), 1);
        QCOMPARE(missing.at(0).toString(), QStringLiteral("run_shell"));
        const QJsonArray available = result.value("available_tools").toArray();
        QCOMPARE(available.size(), 1);
        QCOMPARE(available.at(0).toString(), QStringLiteral("read_file"));
    }

    void test_SkillWithNoDeclaredTools_IsTriviallyReady() {
        seedSkill("no-tools-skill", "Just prose.");

        Tools::GetSkillStatusTool t(m_deps);
        QJsonObject args;
        args.insert("skill_id", "no-tools-skill");
        const auto result = t.invoke(args).toObject();
        QCOMPARE(result.value("installed").toBool(), true);
        QCOMPARE(result.value("approved").toBool(), true);
        QCOMPARE(result.value("ready").toBool(), true);
        QCOMPARE(result.value("declared_tools").toArray().size(), 0);
        QCOMPARE(result.value("missing_tools").toArray().size(), 0);
    }

    void test_RunsOnMainThread_True() {
        Tools::GetSkillStatusTool t(m_deps);
        QVERIFY(t.runsOnMainThread());
    }

    void test_ToolServiceNull_ReturnsError() {
        Tools::SkillToolDeps deps = m_deps;
        deps.tools = nullptr;
        Tools::GetSkillStatusTool t(deps);
        QJsonObject args;
        args.insert("skill_id", "anything");
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("tool registry"));
    }

    void test_ParametersShape() {
        Tools::GetSkillStatusTool t(m_deps);
        const auto params = t.parameters();
        QCOMPARE(params.size(), 1);
        QCOMPARE(params.at(0).name, QStringLiteral("skill_id"));
        QVERIFY(params.at(0).required);
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<QTemporaryDir> m_appData;
    std::unique_ptr<QTemporaryDir> m_srcDir;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<SkillService> m_skills;
    std::unique_ptr<Chat::CascadeController> m_cascade;
    std::unique_ptr<ToolService> m_tools;
    Tools::SkillToolDeps m_deps;
};

QTEST_MAIN(TestGetSkillStatusTool)
#include "test-get-skill-status-tool.moc"

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

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSqlDatabase>

class TestSkillToolPerClient : public QObject {
    Q_OBJECT

  private:
    static void writeFile(const QString& abs, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
    }

    void seedAndApprove(const QString& id) {
        const QString folder = m_srcDir->path() + "/" + id;
        const QByteArray fm = QByteArray("---\n"
                                         "name: ") +
                              id.toUtf8() +
                              "\n"
                              "description: Skill " +
                              id.toUtf8() +
                              "\n"
                              "version: 1.0.0\n---\nBody for " +
                              id.toUtf8() + ".\n";
        writeFile(folder + "/SKILL.md", fm);
        QVERIFY(m_skillSvc->importSkillFolder(folder).isEmpty());
        QVERIFY(m_skillSvc->approveSkill(id));
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
        QVERIFY(m_appData->isValid());
        QVERIFY(m_srcDir->isValid());

        m_router = std::make_unique<ModelRouter>();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_skillSvc = std::make_unique<SkillService>(*m_convs);
        m_skillSvc->setAppDataRootForTesting(m_appData->path());
        m_skillSvc->initialize();
        m_cascade = std::make_unique<Chat::CascadeController>(*m_convs, *m_router);

        seedAndApprove(QStringLiteral("skill-alpha"));

        m_localConvId = m_convs->createConversation("Local Conv");
        m_wireConvId = m_convs->createConversation("Wire Conv");
        QVERIFY(!m_localConvId.isEmpty());
        QVERIFY(!m_wireConvId.isEmpty());

        m_deps.skills = m_skillSvc.get();
        m_deps.convs = m_convs.get();
        m_deps.cascade = m_cascade.get();
        const QString local = m_localConvId;
        m_deps.activeConvIdGetter = [local]() { return local; };
    }

    void cleanup() {
        m_cascade.reset();
        m_skillSvc.reset();
        m_convs.reset();
        m_router.reset();
        m_srcDir.reset();
        m_appData.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void test_discoverSkills_argsConvIdUsed_whenGetterEmpty() {
        m_deps.activeConvIdGetter = []() { return QString(); };

        Tools::DiscoverSkillsTool tool(m_deps);
        QJsonObject args;
        args.insert("__caller_conv_id", m_wireConvId);

        const auto r = tool.invoke(args).toObject();
        QVERIFY2(!r.contains("error"),
                 qPrintable(QStringLiteral("args should have resolved conv id; got error: %1")
                                .arg(r.value("error").toString())));
    }


    void test_readSkill_argsConvIdOverridesCapturedGetter() {
        Tools::ReadSkillTool tool(m_deps);
        QJsonObject args;
        args.insert("skill_id", QStringLiteral("skill-alpha"));
        args.insert("__caller_conv_id", m_wireConvId);

        const auto r = tool.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("skill_id").toString(), QStringLiteral("skill-alpha"));
    }


    void test_readSkill_fallsBackToCapturedGetter_whenArgsEmpty() {
        Tools::ReadSkillTool tool(m_deps);
        QJsonObject args;
        args.insert("skill_id", QStringLiteral("skill-alpha"));

        const auto r = tool.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("skill_id").toString(), QStringLiteral("skill-alpha"));
    }


    void test_discoverSkills_errorsCleanly_whenBothEmpty() {
        m_deps.activeConvIdGetter = []() { return QString(); };

        Tools::DiscoverSkillsTool tool(m_deps);
        QJsonObject args;

        const auto r = tool.invoke(args).toObject();
        QVERIFY(r.contains("error"));
        QVERIFY(r.value("error").toString().contains("responder", Qt::CaseInsensitive));
    }


    void test_readSkillFile_argsConvIdOverridesCapturedGetter() {
        Tools::ReadSkillFileTool tool(m_deps);
        QJsonObject args;
        args.insert("skill_id", QStringLiteral("skill-alpha"));
        args.insert("relative_path", QStringLiteral("SKILL.md"));
        args.insert("__caller_conv_id", m_wireConvId);

        const auto r = tool.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    std::unique_ptr<QTemporaryDir> m_appData;
    std::unique_ptr<QTemporaryDir> m_srcDir;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<SkillService> m_skillSvc;
    std::unique_ptr<Chat::CascadeController> m_cascade;
    Tools::SkillToolDeps m_deps;
    QString m_localConvId;
    QString m_wireConvId;
};

QTEST_MAIN(TestSkillToolPerClient)
#include "test-skill-tool-per-client.moc"

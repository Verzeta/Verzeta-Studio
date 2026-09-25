// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/conversation.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/skill-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>

class TestSkillService : public QObject {
    Q_OBJECT

  private:
    static void writeFile(const QString& abs, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
    }

    void seedSkillFolder(const QString& folder, const QString& id, const QString& body = "Body.") {
        const QByteArray fm = QByteArray("---\n"
                                         "name: ") +
                              id.toUtf8() +
                              "\n"
                              "description: Test skill " +
                              id.toUtf8() +
                              "\n"
                              "tags: [test]\n"
                              "version: 1.0.0\n"
                              "---\n" +
                              body.toUtf8() + "\n";
        writeFile(folder + "/SKILL.md", fm);
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

        m_appDataDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_appDataDir->isValid());
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_svc = std::make_unique<SkillService>(*m_convs);
        m_svc->setAppDataRootForTesting(m_appDataDir->path());
        m_svc->initialize();

        m_sourceDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_sourceDir->isValid());
    }

    void cleanup() {
        m_svc.reset();
        m_convs.reset();
        m_sourceDir.reset();
        m_appDataDir.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void test_ImportPlacesSkillUnreviewed() {
        const QString folder = m_sourceDir->path() + "/twitter";
        seedSkillFolder(folder, "twitter-campaign-planner");
        const QString err = m_svc->importSkillFolder(folder);
        QVERIFY2(err.isEmpty(), qPrintable(err));

        QCOMPARE(m_svc->installedCount(), 1);
        QCOMPARE(m_svc->approvedCount(), 0);

        const Skill s = m_svc->skillById("twitter-campaign-planner");
        QVERIFY(s.isValid());
        QCOMPARE(s.reviewState, QStringLiteral("unreviewed"));
        QVERIFY(QFile::exists(m_appDataDir->path() +
                              "/skills/installed/twitter-campaign-planner/SKILL.md"));
    }

    void test_ApproveAndBlockRoundTripThroughManifest() {
        const QString folder = m_sourceDir->path() + "/x";
        seedSkillFolder(folder, "approve-test");
        QVERIFY(m_svc->importSkillFolder(folder).isEmpty());
        QVERIFY(m_svc->approveSkill("approve-test"));
        QCOMPARE(m_svc->skillById("approve-test").reviewState, QStringLiteral("approved"));

        m_svc->shutdown();
        m_svc = std::make_unique<SkillService>(*m_convs);
        m_svc->setAppDataRootForTesting(m_appDataDir->path());
        m_svc->initialize();
        QCOMPARE(m_svc->approvedCount(), 1);
        QCOMPARE(m_svc->skillById("approve-test").reviewState, QStringLiteral("approved"));

        QVERIFY(m_svc->blockSkill("approve-test"));
        QCOMPARE(m_svc->skillById("approve-test").reviewState, QStringLiteral("blocked"));
    }

    void test_ReimportWithContentChangeResetsReview() {
        const QString folder = m_sourceDir->path() + "/y";
        seedSkillFolder(folder, "edit-test", "First body.");
        QVERIFY(m_svc->importSkillFolder(folder).isEmpty());
        QVERIFY(m_svc->approveSkill("edit-test"));

        seedSkillFolder(folder, "edit-test", "Second body, different bytes.");
        QVERIFY(m_svc->importSkillFolder(folder).isEmpty());
        QCOMPARE(m_svc->skillById("edit-test").reviewState, QStringLiteral("unreviewed"));
    }

    void test_RemoveCascades() {
        const QString folder = m_sourceDir->path() + "/z";
        seedSkillFolder(folder, "remove-test");
        QVERIFY(m_svc->importSkillFolder(folder).isEmpty());
        QVERIFY(m_svc->approveSkill("remove-test"));

        QVERIFY(m_svc->setPreferredSkills(
            "folder", "f-uuid", {QStringLiteral("remove-test"), QStringLiteral("other-id")}));

        QVERIFY(m_svc->removeSkill("remove-test"));
        QCOMPARE(m_svc->installedCount(), 0);

        const QStringList remaining = m_svc->preferredSkillsFor("folder", "f-uuid");
        QVERIFY(!remaining.contains("remove-test"));
        QCOMPARE(remaining.size(), 1);
        QCOMPARE(remaining.first(), QStringLiteral("other-id"));

        QVERIFY(!QDir(m_appDataDir->path() + "/skills/installed/remove-test").exists());
    }

    void test_BadFolderQuarantined() {
        const QString folder = m_sourceDir->path() + "/bad";
        QDir().mkpath(folder);
        const QString err = m_svc->importSkillFolder(folder);
        QVERIFY(!err.isEmpty());
        QCOMPARE(m_svc->installedCount(), 0);
        const QDir qDir(m_appDataDir->path() + "/skills/quarantine");
        QVERIFY(qDir.exists());
        const auto entries = qDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QVERIFY(!entries.isEmpty());
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    std::unique_ptr<QTemporaryDir> m_appDataDir;
    std::unique_ptr<QTemporaryDir> m_sourceDir;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<SkillService> m_svc;
};

QTEST_MAIN(TestSkillService)
#include "test-skill-service.moc"

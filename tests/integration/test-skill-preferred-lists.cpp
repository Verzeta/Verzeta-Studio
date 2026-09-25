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

class TestSkillPreferredLists : public QObject {
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
                              "version: 1.0.0\n---\nBody.\n";
        writeFile(folder + "/SKILL.md", fm);
        QVERIFY(m_svc->importSkillFolder(folder).isEmpty());
        QVERIFY(m_svc->approveSkill(id));
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

        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_svc = std::make_unique<SkillService>(*m_convs);
        m_svc->setAppDataRootForTesting(m_appData->path());
        m_svc->initialize();

        seedAndApprove("skill-a");
        seedAndApprove("skill-b");
        seedAndApprove("skill-c");
    }

    void cleanup() {
        m_svc.reset();
        m_convs.reset();
        m_srcDir.reset();
        m_appData.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_StandaloneConvUsesOwnPreferredList() {
        const QString convId = m_convs->createConversation("Standalone");
        QVERIFY(!convId.isEmpty());
        QVERIFY(m_svc->setPreferredSkills("conversation_1to1", convId, {"skill-b", "skill-a"}));

        const auto resolved = m_svc->resolveForConversation(convId);
        QCOMPARE(resolved.preferredSkillIds.size(), 2);
        QCOMPARE(resolved.preferredSkillIds[0], QStringLiteral("skill-b"));
        QCOMPARE(resolved.preferredSkillIds[1], QStringLiteral("skill-a"));
        QCOMPARE(resolved.exposeOnly, false);
    }

    void test_FolderListAppliesWithoutOverride() {
        const QString folderId = m_convs->createFolder("Project");
        QVERIFY(!folderId.isEmpty());
        const QString convId = m_convs->createConversation("In project", folderId);
        QVERIFY(!convId.isEmpty());

        QVERIFY(m_svc->setPreferredSkills("folder", folderId, {"skill-a", "skill-c"}));
        QVERIFY(m_svc->setPreferredSkills("conversation_1to1", convId, {"skill-b"}));

        const auto resolved = m_svc->resolveForConversation(convId);
        QCOMPARE(resolved.preferredSkillIds.size(), 2);
        QVERIFY(resolved.preferredSkillIds.contains("skill-a"));
        QVERIFY(resolved.preferredSkillIds.contains("skill-c"));
        QVERIFY(!resolved.preferredSkillIds.contains("skill-b"));
    }

    void test_ConvOverrideWins() {
        const QString folderId = m_convs->createFolder("Project");
        const QString convId = m_convs->createConversation("In project", folderId);

        QVERIFY(m_svc->setPreferredSkills("folder", folderId, {"skill-a"}));
        QVERIFY(m_svc->setPreferredSkills("conversation_1to1", convId, {"skill-b", "skill-c"}));
        QVERIFY(m_svc->setOverrideParentFolder(convId, true));

        const auto resolved = m_svc->resolveForConversation(convId);
        QCOMPARE(resolved.preferredSkillIds.size(), 2);
        QVERIFY(resolved.preferredSkillIds.contains("skill-b"));
        QVERIFY(resolved.preferredSkillIds.contains("skill-c"));
        QVERIFY(!resolved.preferredSkillIds.contains("skill-a"));
    }

    void test_UnreviewedSkillFilteredOutOfResolved() {
        const QString convId = m_convs->createConversation("c");
        const QString folder = m_srcDir->path() + "/skill-d";
        const QByteArray fm = "---\n"
                              "name: skill-d\n"
                              "description: Skill D unreviewed\n"
                              "version: 1.0.0\n---\nBody.\n";
        writeFile(folder + "/SKILL.md", fm);
        QVERIFY(m_svc->importSkillFolder(folder).isEmpty());

        QVERIFY(m_svc->setPreferredSkills("conversation_1to1", convId, {"skill-a", "skill-d"}));
        const auto resolved = m_svc->resolveForConversation(convId);
        QVERIFY(resolved.preferredSkillIds.contains("skill-a"));
        QVERIFY2(!resolved.preferredSkillIds.contains("skill-d"),
                 "unreviewed skill must be filtered out of resolved list");
    }

    void test_BlockedSkillFilteredOutOfResolved() {
        const QString convId = m_convs->createConversation("c");
        QVERIFY(m_svc->blockSkill("skill-b"));
        QVERIFY(m_svc->setPreferredSkills("conversation_1to1", convId, {"skill-a", "skill-b"}));
        const auto resolved = m_svc->resolveForConversation(convId);
        QVERIFY(resolved.preferredSkillIds.contains("skill-a"));
        QVERIFY(!resolved.preferredSkillIds.contains("skill-b"));
    }

    void test_UninstalledSkillFilteredOutOfResolved() {
        const QString convId = m_convs->createConversation("c");
        QVERIFY(m_svc->setPreferredSkills("conversation_1to1", convId, {"skill-a", "ghost"}));
        const auto resolved = m_svc->resolveForConversation(convId);
        QVERIFY(resolved.preferredSkillIds.contains("skill-a"));
        QVERIFY(!resolved.preferredSkillIds.contains("ghost"));
    }

    void test_ResolveForFolderUsesFolderRow() {
        const QString folderId = m_convs->createFolder("FolderHB");
        QVERIFY(m_svc->setPreferredSkills("folder", folderId, {"skill-c", "skill-b"}));
        QVERIFY(m_svc->setExposeOnlyPreferred("folder", folderId, true));
        const auto resolved = m_svc->resolveForFolder(folderId);
        QCOMPARE(resolved.preferredSkillIds.size(), 2);
        QCOMPARE(resolved.preferredSkillIds[0], QStringLiteral("skill-c"));
        QCOMPARE(resolved.preferredSkillIds[1], QStringLiteral("skill-b"));
        QCOMPARE(resolved.exposeOnly, true);
    }

    void test_PreferredOrderingPreservedEndToEnd() {
        const QString convId = m_convs->createConversation("c");
        QVERIFY(m_svc->setPreferredSkills(
            "conversation_1to1", convId, {"skill-c", "skill-a", "skill-b"}));
        const auto resolved = m_svc->resolveForConversation(convId);
        QCOMPARE(resolved.preferredSkillIds[0], QStringLiteral("skill-c"));
        QCOMPARE(resolved.preferredSkillIds[1], QStringLiteral("skill-a"));
        QCOMPARE(resolved.preferredSkillIds[2], QStringLiteral("skill-b"));
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    std::unique_ptr<QTemporaryDir> m_appData;
    std::unique_ptr<QTemporaryDir> m_srcDir;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<SkillService> m_svc;
};

QTEST_MAIN(TestSkillPreferredLists)
#include "test-skill-preferred-lists.moc"

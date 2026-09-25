// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/utils/skill-hash.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>

class TestSkillHash : public QObject {
    Q_OBJECT

  private:
    static void writeFile(const QString& abs, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
    }

  private slots:
    void test_DeterministicAcrossIdenticalFolders() {
        QTemporaryDir d1, d2;
        QVERIFY(d1.isValid());
        QVERIFY(d2.isValid());

        writeFile(d1.path() + "/SKILL.md", "alpha");
        writeFile(d1.path() + "/README.md", "beta");
        writeFile(d1.path() + "/scripts/run.sh", "gamma");

        writeFile(d2.path() + "/scripts/run.sh", "gamma");
        writeFile(d2.path() + "/SKILL.md", "alpha");
        writeFile(d2.path() + "/README.md", "beta");

        const QString h1 = SkillHash::computeFolderHash(d1.path());
        const QString h2 = SkillHash::computeFolderHash(d2.path());
        QVERIFY(!h1.isEmpty());
        QCOMPARE(h1.size(), 64);
        QCOMPARE(h1, h2);
    }

    void test_OneByteFlipChangesHash() {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        writeFile(d.path() + "/SKILL.md", "alpha");
        const QString h0 = SkillHash::computeFolderHash(d.path());
        writeFile(d.path() + "/SKILL.md", "Alpha");
        const QString h1 = SkillHash::computeFolderHash(d.path());
        QVERIFY(!h0.isEmpty());
        QVERIFY(!h1.isEmpty());
        QVERIFY(h0 != h1);
    }

    void test_AddedFileChangesHash() {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        writeFile(d.path() + "/SKILL.md", "alpha");
        const QString h0 = SkillHash::computeFolderHash(d.path());
        writeFile(d.path() + "/extra.txt", "beta");
        const QString h1 = SkillHash::computeFolderHash(d.path());
        QVERIFY(h0 != h1);
    }

    void test_RenameChangesHash() {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        writeFile(d.path() + "/foo.md", "alpha");
        const QString h0 = SkillHash::computeFolderHash(d.path());
        QFile::rename(d.path() + "/foo.md", d.path() + "/bar.md");
        const QString h1 = SkillHash::computeFolderHash(d.path());
        QVERIFY(h0 != h1);
    }

    void test_NonExistentReturnsEmpty() {
        QString err;
        const QString h =
            SkillHash::computeFolderHash("/this/path/should/not/exist/anywhere", &err);
        QCOMPARE(h, QString());
        QVERIFY(!err.isEmpty());
    }
};

QTEST_MAIN(TestSkillHash)
#include "test-skill-hash.moc"

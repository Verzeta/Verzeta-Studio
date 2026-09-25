// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/utils/skill-archive-extractor.h"

#include <QTemporaryDir>
#include <QtTest>

#include <KZip>
#include <QByteArray>
#include <QDir>
#include <QFile>

class TestSkillArchiveExtractor : public QObject {
    Q_OBJECT

  private:
    QString writeZip(QTemporaryDir& dir, const QString& name, std::function<void(KZip&)> writer) {
        const QString path = dir.path() + "/" + name;
        KZip zip(path);
        if (!zip.open(QIODevice::WriteOnly))
            return {};
        writer(zip);
        zip.close();
        return path;
    }

  private slots:
    void test_HappyPathExtraction() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString zip = writeZip(tmp, "ok.zip", [](KZip& z) {
            z.writeFile("SKILL.md", QByteArray("---\nname: x\n---\n"));
            z.writeFile("README.md", QByteArray("hello"));
        });
        QVERIFY(!zip.isEmpty());
        const QString staging = tmp.path() + "/staging";
        const auto r = SkillArchive::extract(zip, staging);
        QVERIFY2(r.success, qPrintable(r.error));
        QVERIFY(QFile::exists(staging + "/SKILL.md"));
        QVERIFY(QFile::exists(staging + "/README.md"));
        QCOMPARE(r.fileCount, 2);
    }

    void test_PathTraversalRejected() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString zip = writeZip(tmp, "bad.zip", [](KZip& z) {
            z.writeFile("SKILL.md", QByteArray("ok"));
            z.writeFile("../escape.txt", QByteArray("oops"));
        });
        const QString staging = tmp.path() + "/staging";
        const auto r = SkillArchive::extract(zip, staging);
        QVERIFY(!r.success);
        QVERIFY(r.error.contains("traversal") || r.error.contains("escapes"));
    }

    void test_FileCountCap() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString zip = writeZip(tmp, "many.zip", [](KZip& z) {
            for (int i = 0; i < 300; ++i) {
                z.writeFile(QStringLiteral("f%1.txt").arg(i), QByteArray("x"));
            }
        });
        const QString staging = tmp.path() + "/staging";
        const auto r = SkillArchive::extract(zip, staging);
        QVERIFY(!r.success);
        QVERIFY(r.error.contains("256") || r.error.contains("cap"));
    }

    void test_SingleFileSizeCap() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString zip = writeZip(tmp, "big.zip", [](KZip& z) {
            z.writeFile("big.bin", QByteArray(2 * 1024 * 1024, 'a'));
        });
        const QString staging = tmp.path() + "/staging";
        const auto r = SkillArchive::extract(zip, staging);
        QVERIFY(!r.success);
        QVERIFY(r.error.contains("1 MiB") || r.error.contains("cap"));
    }

    void test_TotalSizeCap_DecompressionBombGuard() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString zip = writeZip(tmp, "bomb.zip", [](KZip& z) {
            for (int i = 0; i < 10; ++i) {
                z.writeFile(QStringLiteral("f%1.bin").arg(i), QByteArray(1 * 1024 * 1024, 'a'));
            }
        });
        const QString staging = tmp.path() + "/staging";
        const auto r = SkillArchive::extract(zip, staging);
        QVERIFY(!r.success);
        QVERIFY(r.error.contains("8 MiB") || r.error.contains("decompression"));
    }
};

QTEST_MAIN(TestSkillArchiveExtractor)
#include "test-skill-archive-extractor.moc"

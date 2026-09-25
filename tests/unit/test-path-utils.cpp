// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "utils/path-utils.h"

#include <QTest>

#include <QObject>
#include <QUrl>

using Verzeta::PathUtils;

class TestPathUtils : public QObject {
    Q_OBJECT

  private:
    PathUtils u;

  private slots:
    void test_theOldConcatenation_swallowsTheWindowsDriveLetter() {
        const QUrl bad(QStringLiteral("file://") + QStringLiteral("C:/Users/a/models"));
        QCOMPARE(bad.host(), QStringLiteral("c"));
        QCOMPARE(bad.toString(), QStringLiteral("file://c/Users/a/models"));
        QVERIFY(bad.isValid());
        QVERIFY(bad.isLocalFile());
    }

    void test_fromLocalFile_preservesAWindowsDriveLetter() {
        const QUrl url = u.fromLocalFile(QStringLiteral("C:/Users/a/models"));
        QCOMPARE(url.toString(), QStringLiteral("file:///C:/Users/a/models"));
        QVERIFY(url.isLocalFile());
        QVERIFY(url.host().isEmpty());
    }

    void test_fromLocalFile_preservesAUncShare() {
        const QUrl url = u.fromLocalFile(QStringLiteral("//server/share/m.gguf"));
        QCOMPARE(url.host(), QStringLiteral("server"));
        QVERIFY(url.isLocalFile());
    }

    void test_theOldStrip_leavesALeadingSlashOnWindows() {
        const QString stripped =
            QString(QUrl(QStringLiteral("file:///C:/Users/a/m.gguf")).toString())
                .replace(QStringLiteral("file://"), QString());
        QCOMPARE(stripped, QStringLiteral("/C:/Users/a/m.gguf"));
    }

    void test_roundTrip_posixPath() {
        const QString p = QStringLiteral("/home/user/models/m.gguf");
        QCOMPARE(u.toLocalFile(u.fromLocalFile(p)), p);
    }

    void test_posixResultsAreIdenticalToTheOldStringSurgery() {
        const QString p = QStringLiteral("/home/user/.local/share/ragp-models");

        QCOMPARE(u.fromLocalFile(p).toString(), QStringLiteral("file://") + p);

        const QUrl url(QStringLiteral("file:///home/user/Downloads/m.gguf"));
        const QString oldWay =
            QString(url.toString()).replace(QStringLiteral("file://"), QString());
        QCOMPARE(u.toLocalFile(url), oldWay);
    }

    void test_roundTrip_pathNeedingPercentEncoding() {
        const QString p = QStringLiteral("/home/user/Verzeta Studio/a#b/m.gguf");
        const QUrl url = u.fromLocalFile(p);
        QVERIFY(url.toString().contains(QStringLiteral("%23")));
        QCOMPARE(u.toLocalFile(url), p);
    }

    void test_fromLocalFile_doesNotDoubleWrapAUrl() {
        const QUrl once = u.fromLocalFile(QStringLiteral("file:///home/user/m.gguf"));
        QCOMPARE(once.toString(), QStringLiteral("file:///home/user/m.gguf"));
        QVERIFY(once.isLocalFile());
    }

    void test_fromLocalFile_singleLetterSchemeIsADriveNotAScheme() {
        QCOMPARE(QUrl(QStringLiteral("C:/x")).scheme(), QStringLiteral("c"));
        QCOMPARE(u.fromLocalFile(QStringLiteral("C:/x")).toString(),
                 QStringLiteral("file:///C:/x"));
    }

    void test_emptyInputs() {
        QVERIFY(u.fromLocalFile(QString()).isEmpty());
        QVERIFY(u.toLocalFile(QUrl()).isEmpty());
    }

    void test_toLocalFile_refusesARemoteUrl() {
        QVERIFY(u.toLocalFile(QUrl(QStringLiteral("https://x.test/m.gguf"))).isEmpty());
    }
};

QTEST_MAIN(TestPathUtils)
#include "test-path-utils.moc"

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/utils/process-sandbox.h"
#include "../../backend/utils/write-guard.h"

#include <QTemporaryDir>
#include <QtTest>

class TestProcessSandbox : public QObject {
    Q_OBJECT

  private slots:
#ifndef Q_OS_WIN
    void test_pipeStillRuns() {
        ProcessSandbox sb;
        const auto r = sb.execute(QStringLiteral("echo hello | tr a-z A-Z"), 10000);
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.stdoutOutput.trimmed(), QStringLiteral("HELLO"));
    }

    void test_heredocPythonStillRuns() {
        if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty())
            QSKIP("python3 not installed");
        ProcessSandbox sb;
        const auto r = sb.execute(QStringLiteral("python3 - <<'EOF'\nprint(40 + 2)\nEOF"), 10000);
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.stdoutOutput.trimmed(), QStringLiteral("42"));
    }

    void test_workingDirectory() {
        QTemporaryDir dir;
        ProcessSandbox sb;
        const auto r = sb.execute(QStringLiteral("pwd"), 10000, dir.path());
        QCOMPARE(r.stdoutOutput.trimmed(), QFileInfo(dir.path()).canonicalFilePath());
    }

    void test_pipeIntoShellRefused() {
        QTemporaryDir dir;
        const QString marker = dir.filePath(QStringLiteral("ran"));
        ProcessSandbox sb;
        QSignalSpy blocked(&sb, &ProcessSandbox::commandBlocked);
        const auto r = sb.execute(QStringLiteral("echo 'touch %1' | sh").arg(marker), 10000);
        QCOMPARE(r.exitCode, -1);
        QVERIFY(r.stderrOutput.startsWith(QStringLiteral("Refused")));
        QCOMPARE(blocked.count(), 1);
        QVERIFY(!QFile::exists(marker));
    }

    void test_dynamicEvalRefused() {
        ProcessSandbox sb;
        const auto r = sb.execute(QStringLiteral("echo x; eval \"$HOME\""), 10000);
        QCOMPARE(r.exitCode, -1);
        QVERIFY(r.stderrOutput.contains(QStringLiteral("eval")));
    }

    void test_writeRestriction_offByDefault() {
        QTemporaryDir project;
        QTemporaryDir outside(QDir::homePath() + QStringLiteral("/.verzeta-guard-test-XXXXXX"));
        ProcessSandbox sb;
        QVERIFY(!sb.writeRestrictionActive());
        const QString target = outside.filePath(QStringLiteral("f"));
        const auto r =
            sb.execute(QStringLiteral("echo x > '%1'").arg(target), 10000, project.path());
        QCOMPARE(r.exitCode, 0);
        QVERIFY(QFile::exists(target));
    }

    void test_writeRestriction_on() {
        if (!Verzeta::WriteGuard::available())
            QSKIP("Landlock is not available on this system");
        QTemporaryDir project(QDir::homePath() + QStringLiteral("/.verzeta-guard-proj-XXXXXX"));
        QTemporaryDir outside(QDir::homePath() + QStringLiteral("/.verzeta-guard-test-XXXXXX"));
        QTemporaryDir granted(QDir::homePath() + QStringLiteral("/.verzeta-guard-extra-XXXXXX"));
        QTemporaryDir tmp;
        QFile readable(outside.filePath(QStringLiteral("readme")));
        QVERIFY(readable.open(QIODevice::WriteOnly));
        readable.write("visible");
        readable.close();

        ProcessSandbox sb;
        sb.setWriteRestriction(true, {granted.path()});
        QVERIFY(sb.writeRestrictionActive());

        auto run = [&](const QString& cmd) { return sb.execute(cmd, 10000, project.path()); };
        QCOMPARE(run(QStringLiteral("echo x > in-project")).exitCode, 0);
        QVERIFY(QFile::exists(project.filePath(QStringLiteral("in-project"))));
        QCOMPARE(run(QStringLiteral("echo x > '%1/t'").arg(tmp.path())).exitCode, 0);
        QCOMPARE(run(QStringLiteral("echo x > '%1/g'").arg(granted.path())).exitCode, 0);
        QCOMPARE(run(QStringLiteral("echo x > /dev/null")).exitCode, 0);

        const auto blocked = run(QStringLiteral("echo x > '%1/nope'").arg(outside.path()));
        QVERIFY(blocked.exitCode != 0);
        QVERIFY(!QFile::exists(outside.filePath(QStringLiteral("nope"))));

        const auto read = run(QStringLiteral("cat '%1'").arg(readable.fileName()));
        QCOMPARE(read.stdoutOutput, QStringLiteral("visible"));

        sb.setWriteRestriction(false, {});
        QCOMPARE(run(QStringLiteral("echo x > '%1/again'").arg(outside.path())).exitCode, 0);
    }

    void test_allowListRefusalUnchanged() {
        ProcessSandbox sb;
        const auto r = sb.execute(QStringLiteral("definitely-not-a-program --x"), 10000);
        QCOMPARE(r.exitCode, -1);
        QVERIFY(r.stderrOutput.contains(QStringLiteral("is not in the allow-list")));
    }
#endif

    void test_helperOrdinaryCommand() {
        QVERIFY(ProcessSandbox::uncheckableCodeReason(QStringLiteral("git status && npm test"))
                    .isEmpty());
    }
};

QTEST_MAIN(TestProcessSandbox)
#include "test-process-sandbox.moc"

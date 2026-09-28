// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/utils/process-sandbox.h"

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

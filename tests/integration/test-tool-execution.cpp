// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "utils/process-sandbox.h"

#include <QTest>
#include <QTimer>

#include <QProcess>
#include <QSignalSpy>


class TestToolExecution : public QObject {
    Q_OBJECT

  private:
    ProcessSandbox* m_sandbox = nullptr;

  private slots:

    void init() { m_sandbox = new ProcessSandbox(this); }

    void cleanup() {
        delete m_sandbox;
        m_sandbox = nullptr;
    }

    void test_allowedCommand_echoHello() {
        const ProcessSandbox::CommandResult res =
            m_sandbox->execute(QStringLiteral("echo hello"), 5000);

        QVERIFY(!res.timedOut);
        QCOMPARE(res.exitCode, 0);
        QVERIFY2(res.stdoutOutput.trimmed() == QStringLiteral("hello"),
                 qPrintable(QStringLiteral("stdout: ") + res.stdoutOutput));
    }

    void test_blockedCommand_emitsCommandBlocked() {
        QSignalSpy blockedSpy(m_sandbox, &ProcessSandbox::commandBlocked);

        const ProcessSandbox::CommandResult res =
            m_sandbox->execute(QStringLiteral("rm -rf /"), 5000);

        QCOMPARE(blockedSpy.count(), 1);

        const QString blockedCmd = blockedSpy[0][0].toString();
        QCOMPARE(blockedCmd, QStringLiteral("rm -rf /"));

        QCOMPARE(res.exitCode, -1);
        QVERIFY(!res.timedOut);
    }

    void test_timeout_timedOutFlagSet() {
        QStringList extList = m_sandbox->allowList();
        extList.append(QStringLiteral("sleep"));
        m_sandbox->setAllowList(extList);

        const ProcessSandbox::CommandResult res =
            m_sandbox->execute(QStringLiteral("sleep 60"), 500);

        QVERIFY(res.timedOut);
        QCOMPARE(res.exitCode, -1);
    }

    void test_exitCode_nonZeroForFalse() {
        QStringList extList = m_sandbox->allowList();
        extList.append(QStringLiteral("false"));
        m_sandbox->setAllowList(extList);

        const ProcessSandbox::CommandResult res = m_sandbox->execute(QStringLiteral("false"), 5000);

        QVERIFY(!res.timedOut);
        QVERIFY(res.exitCode != 0);
    }

    void test_stderrCapture_lsNonexistent() {
        const ProcessSandbox::CommandResult res =
            m_sandbox->execute(QStringLiteral("ls /path/that/does/not/exist/verzeta"), 5000);

        QVERIFY(!res.timedOut);
        QVERIFY(res.exitCode != 0);

        const bool hasError = res.stderrOutput.contains(QStringLiteral("No such")) ||
                              res.stderrOutput.contains(QStringLiteral("cannot")) ||
                              res.stderrOutput.contains(QStringLiteral("error")) ||
                              res.stdoutOutput.contains(QStringLiteral("No such"));
        QVERIFY2(hasError,
                 qPrintable(QStringLiteral("stderr: ") + res.stderrOutput +
                            QStringLiteral(" stdout: ") + res.stdoutOutput));
    }

    void test_asyncOutput_seqEmitsFiveLines() {
        QSignalSpy lineSpy(m_sandbox, &ProcessSandbox::outputLine);
        QSignalSpy finishedSpy(m_sandbox, &ProcessSandbox::commandFinished);

        m_sandbox->executeAsync(QStringLiteral("seq 1 5"), 5000);

        QVERIFY(finishedSpy.wait(5000));

        QVERIFY2(lineSpy.count() >= 5,
                 qPrintable(
                     QStringLiteral("Expected 5 outputLine signals, got %1").arg(lineSpy.count())));

        QStringList lines;
        for (int i = 0; i < lineSpy.count(); ++i) {
            lines.append(lineSpy[i][0].toString());
        }
        QVERIFY(lines.contains(QStringLiteral("1")));
        QVERIFY(lines.contains(QStringLiteral("5")));
    }

    void test_kill_asyncProcessTerminated() {
        QStringList extList = m_sandbox->allowList();
        extList.append(QStringLiteral("sleep"));
        m_sandbox->setAllowList(extList);

        QSignalSpy finishedSpy(m_sandbox, &ProcessSandbox::commandFinished);

        m_sandbox->executeAsync(QStringLiteral("sleep 60"), 30000);

        QTest::qWait(100);

        m_sandbox->kill();

        if (finishedSpy.isEmpty()) {
            QVERIFY(finishedSpy.wait(3000));
        }
        QVERIFY(!finishedSpy.isEmpty());
    }
};

QTEST_MAIN(TestToolExecution)
#include "test-tool-execution.moc"

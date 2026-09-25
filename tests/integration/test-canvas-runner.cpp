// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/canvas-console-model.h"
#include "models/db-manager.h"
#include "services/canvas-runner.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"
#include "services/file-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

class TestCanvasRunner : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<CanvasService> m_canvas;
    std::unique_ptr<CanvasConsoleModel> m_console;
    std::unique_ptr<CanvasRunner> m_runner;

    static bool hasBwrap() {
        return !QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty();
    }
    static bool hasPython() {
        return !QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty();
    }
    static bool hasBash() {
        return !QStandardPaths::findExecutable(QStringLiteral("bash")).isEmpty();
    }
    static bool sandboxUsable() { return CanvasRunner::probeSandboxBlocker().isEmpty(); }

    bool waitForFinish(int timeoutMs = 5000) {
        if (!m_runner->isRunning())
            return true;
        QElapsedTimer t;
        t.start();
        while (m_runner->isRunning() && t.elapsed() < timeoutMs) {
            QTest::qWait(20);
        }
        return !m_runner->isRunning();
    }

    int countRowsOfKind(const QString& k) const {
        int n = 0;
        for (int i = 0; i < m_console->rowCount(); ++i) {
            if (m_console->data(m_console->index(i, 0), CanvasConsoleModel::KindRole).toString() ==
                k) {
                ++n;
            }
        }
        return n;
    }
    QString rowText(int row) const {
        return m_console->data(m_console->index(row, 0), CanvasConsoleModel::TextRole).toString();
    }
    QString rowKind(int row) const {
        return m_console->data(m_console->index(row, 0), CanvasConsoleModel::KindRole).toString();
    }
    bool consoleHasTextContaining(const QString& needle) const {
        for (int i = 0; i < m_console->rowCount(); ++i)
            if (rowText(i).contains(needle))
                return true;
        return false;
    }

    QString seedCanvas(const QString& filename, const QString& language, const QString& content) {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Run Test"));
        Q_ASSERT(!convId.isEmpty());
        const QString canvasId = m_canvas->openCanvas(convId, filename, language, content);
        Q_ASSERT(!canvasId.isEmpty());
        return convId;
    }

  private slots:

    void initTestCase() { QVERIFY(m_tempDir.isValid()); }

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_fileSvc = std::make_unique<FileService>();
        m_canvas = std::make_unique<CanvasService>(DbManager::instance());
        m_canvas->setConversationService(m_convSvc.get());
        m_canvas->setFileService(m_fileSvc.get());

        m_console = std::make_unique<CanvasConsoleModel>();
        m_runner = std::make_unique<CanvasRunner>();
        m_runner->setCanvasService(m_canvas.get());
        m_runner->setFileService(m_fileSvc.get());
        m_runner->setConsoleModel(m_console.get());
    }

    void cleanup() {
        m_runner.reset();
        m_console.reset();
        m_canvas.reset();
        m_fileSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_python_helloPrintsAndExitsZero() {
        if (!hasBwrap())
            QSKIP("bwrap not on PATH");
        if (!sandboxUsable())
            QSKIP("kernel blocks unprivileged user namespaces");
        if (!hasPython())
            QSKIP("python3 not on PATH");

        const QString convId = seedCanvas(QStringLiteral("hello.py"),
                                          QStringLiteral("python"),
                                          QStringLiteral("print('hello-from-canvas')\n"));

        QVERIFY(m_runner->runActiveCanvas(convId));
        QVERIFY(waitForFinish());

        QCOMPARE(rowKind(0), QStringLiteral("system"));
        QVERIFY(rowText(0).contains(QStringLiteral("hello.py")));
        bool foundStdout = false;
        for (int i = 0; i < m_console->rowCount(); ++i) {
            if (rowKind(i) == QStringLiteral("stdout") &&
                rowText(i) == QStringLiteral("hello-from-canvas")) {
                foundStdout = true;
                break;
            }
        }
        QVERIFY2(foundStdout, "stdout row 'hello-from-canvas' missing");

        const int last = m_console->rowCount() - 1;
        QCOMPARE(rowKind(last), QStringLiteral("exit-ok"));
        QVERIFY(rowText(last).contains(QStringLiteral("Done")));
    }


    void test_pythonFlags_areUnbufferedAndIsolated() {
        const QStringList f = CanvasRunner::pythonFlags();
        QVERIFY2(f.contains(QStringLiteral("-u")),
                 "-u missing: an input() prompt would never reach the console");
        QVERIFY(f.contains(QStringLiteral("-I")));
        QVERIFY(f.contains(QStringLiteral("-B")));
        QVERIFY(f.contains(QStringLiteral("-E")));
    }

    void test_sendInput_whenNotRunning_isSafeNoOp() {
        QVERIFY(!m_runner->isRunning());
        m_runner->sendInput(QStringLiteral("ignored"));
        m_runner->sendEof();
        QVERIFY(!m_runner->isRunning());
    }

    void test_python_interactiveInput_promptShownThenAnswered() {
        if (!hasPython())
            QSKIP("python3 not on PATH");

        const QString convId =
            seedCanvas(QStringLiteral("ask.py"),
                       QStringLiteral("python"),
                       QStringLiteral("name = input('Name: ')\nprint('hi', name)\n"));

        QVERIFY(m_runner->runActiveCanvas(convId));

        QTRY_VERIFY_WITH_TIMEOUT(consoleHasTextContaining(QStringLiteral("Name:")), 5000);
        QVERIFY2(m_runner->isRunning(), "script should still be blocked awaiting input");

        m_runner->sendInput(QStringLiteral("bob"));
        QVERIFY(waitForFinish());

        QVERIFY2(consoleHasTextContaining(QStringLiteral("hi bob")),
                 "script did not receive the answer on stdin");
        bool foundEcho = false;
        for (int i = 0; i < m_console->rowCount(); ++i) {
            if (rowKind(i) == QStringLiteral("input") &&
                rowText(i).contains(QStringLiteral("bob"))) {
                foundEcho = true;
                break;
            }
        }
        QVERIFY2(foundEcho, "typed input was not echoed as an 'input' row");

        const int last = m_console->rowCount() - 1;
        QCOMPARE(rowKind(last), QStringLiteral("exit-ok"));
    }


    void test_python_error_emitsLastStderrLineOnly() {
        if (!hasBwrap())
            QSKIP("bwrap not on PATH");
        if (!sandboxUsable())
            QSKIP("kernel blocks unprivileged user namespaces");
        if (!hasPython())
            QSKIP("python3 not on PATH");

        const QString convId = seedCanvas(QStringLiteral("bad.py"),
                                          QStringLiteral("python"),
                                          QStringLiteral("def f(:\n    return 1\n"));

        QVERIFY(m_runner->runActiveCanvas(convId));
        QVERIFY(waitForFinish());

        QCOMPARE(countRowsOfKind(QStringLiteral("stderr")), 1);

        const int last = m_console->rowCount() - 1;
        QCOMPARE(rowKind(last), QStringLiteral("exit-err"));
        QVERIFY(rowText(last).contains(QStringLiteral("Exit ")));
    }


    void test_bash_helloPrintsAndExitsZero() {
        if (!hasBwrap())
            QSKIP("bwrap not on PATH");
        if (!sandboxUsable())
            QSKIP("kernel blocks unprivileged user namespaces");
        if (!hasBash())
            QSKIP("bash not on PATH");

        const QString convId = seedCanvas(
            QStringLiteral("hi.sh"), QStringLiteral("bash"), QStringLiteral("echo hi-from-bash\n"));

        QVERIFY(m_runner->runActiveCanvas(convId));
        QVERIFY(waitForFinish());

        bool sawStdout = false;
        for (int i = 0; i < m_console->rowCount(); ++i) {
            if (rowKind(i) == QStringLiteral("stdout") &&
                rowText(i) == QStringLiteral("hi-from-bash")) {
                sawStdout = true;
                break;
            }
        }
        QVERIFY2(sawStdout, "expected stdout row 'hi-from-bash'");
        QCOMPARE(rowKind(m_console->rowCount() - 1), QStringLiteral("exit-ok"));
    }


    void test_sandboxBlocked_fallsThroughToDirectSubprocess() {
        if (!hasBwrap())
            QSKIP("bwrap not on PATH");
        if (sandboxUsable())
            QSKIP("sandbox is usable on this host");

        const QString convId = seedCanvas(
            QStringLiteral("hi.py"), QStringLiteral("python"), QStringLiteral("print('hi')\n"));

        QVERIFY(!m_runner->sandboxAvailable());

        QCOMPARE(m_runner->runActiveCanvas(convId), true);
        QSignalSpy finished(m_runner.get(), &CanvasRunner::runningChanged);
        QVERIFY(finished.wait(5000));
        while (m_runner->isRunning()) {
            QVERIFY(finished.wait(2000));
        }

        QVERIFY(m_console->rowCount() >= 2);
        QCOMPARE(rowKind(0), QStringLiteral("system"));
    }


    void test_unsupportedLanguage_refusedWithError() {
        const QString convId = seedCanvas(
            QStringLiteral("note.txt"), QStringLiteral("plaintext"), QStringLiteral("just words"));

        QSignalSpy spy(m_runner.get(), &CanvasRunner::errorOccurred);
        QCOMPARE(m_runner->runActiveCanvas(convId), false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(m_console->rowCount(), 3);
        QCOMPARE(rowKind(0), QStringLiteral("system"));
        QCOMPARE(rowKind(1), QStringLiteral("stderr"));
        QCOMPARE(rowKind(2), QStringLiteral("exit-err"));
    }


    void test_sendToIde_dispatchesToHandler() {
        const QString convId = seedCanvas(QStringLiteral("config.cpp"),
                                          QStringLiteral("cpp"),
                                          QStringLiteral("int main() { return 0; }\n"));

        QStringList captured;
        m_runner->setOpenUrlHandler([&captured](const QString& path) -> bool {
            captured.append(path);
            return true;
        });

        QVERIFY(m_runner->sendToIde(convId));
        QCOMPARE(captured.size(), 1);
        QVERIFY(captured.first().endsWith(QStringLiteral("config.cpp")));
        QVERIFY(QFileInfo::exists(captured.first()));
    }


    void test_sendToIde_emptyConversation_emitsError() {
        m_runner->setOpenUrlHandler([](const QString&) -> bool { return true; });
        QSignalSpy spy(m_runner.get(), &CanvasRunner::errorOccurred);
        QCOMPARE(m_runner->sendToIde(QString()), false);
        QCOMPARE(spy.count(), 1);
    }


    void test_supportedRunLanguages_includesPythonAndBash() {
        const QStringList supported = m_runner->supportedRunLanguages();
        QVERIFY(supported.contains(QStringLiteral("python")));
        QVERIFY(supported.contains(QStringLiteral("bash")));
    }

    void test_supportedIdeLanguages_includesCommonCodeLangs() {
        const QStringList supported = m_runner->supportedIdeLanguages();
        QVERIFY(supported.contains(QStringLiteral("cpp")));
        QVERIFY(supported.contains(QStringLiteral("javascript")));
        QVERIFY(supported.contains(QStringLiteral("typescript")));
    }
};

QTEST_MAIN(TestCanvasRunner)
#include "test-canvas-runner.moc"

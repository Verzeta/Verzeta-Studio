// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QString>

namespace {

QString locateHelperBinary() {
    const QString candidate = QDir(QCoreApplication::applicationDirPath())
                                  .absoluteFilePath(QStringLiteral("test-db-lock-helper"));
    if (QFileInfo::exists(candidate))
        return candidate;
#ifdef Q_OS_WIN
    const QString exe = candidate + QStringLiteral(".exe");
    if (QFileInfo::exists(exe))
        return exe;
#endif
    return QString();
}

}  // namespace

class TestDbLockRefusal : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/lock_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void cleanup() {
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void firstOpen_succeeds() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().isOpen());
    }

    void lockfile_is_createdAdjacentToDb() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        const QString lockPath = m_dbPath + QStringLiteral(".engine.lock");
        QVERIFY2(QFileInfo::exists(lockPath),
                 qPrintable(QStringLiteral("Lockfile not created at: %1").arg(lockPath)));
    }

    void close_releasesLockfile_so_reopenSucceeds() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));

        QVERIFY(DbManager::instance().open(m_dbPath));
    }

    void secondProcess_refusesWith_exitCode_2() {
        QVERIFY(DbManager::instance().open(m_dbPath));

        const QString helper = locateHelperBinary();
        if (helper.isEmpty()) {
            QSKIP("test-db-lock-helper binary not found alongside test "
                  "executable — check tests/CMakeLists.txt build wiring");
        }

        QProcess child;
        child.setProgram(helper);
        child.setArguments({m_dbPath});
        child.start();
        QVERIFY(child.waitForStarted(5000));
        QVERIFY(child.waitForFinished(15000));

        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 2);
    }

    void secondProcess_succeedsAfterFirstReleases() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));

        const QString helper = locateHelperBinary();
        if (helper.isEmpty()) {
            QSKIP("test-db-lock-helper binary not found alongside test "
                  "executable — check tests/CMakeLists.txt build wiring");
        }

        QProcess child;
        child.setProgram(helper);
        child.setArguments({m_dbPath});
        child.start();
        QVERIFY(child.waitForStarted(5000));
        QVERIFY(child.waitForFinished(15000));

        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
};

QTEST_GUILESS_MAIN(TestDbLockRefusal)
#include "test-db-lock-refusal.moc"

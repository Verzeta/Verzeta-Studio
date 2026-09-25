// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "models/db-manager.h"
#include "services/acn-service.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>

class TestAcnService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/acn_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
    }

    void cleanup() {
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_recordInsertsRow() {
        EmbeddingWorker worker;
        AcnService svc(DbManager::instance(), worker);
        QCOMPARE(svc.entryCount(), 0);
        const QString id =
            svc.record(QStringLiteral("The team agreed to ship the API in two phases"),
                       QStringLiteral("project:P1"));
        QVERIFY(!id.isEmpty());
        QCOMPARE(svc.entryCount(), 1);
    }

    void test_recordRejectsEmpty() {
        EmbeddingWorker worker;
        AcnService svc(DbManager::instance(), worker);
        QVERIFY(svc.record(QStringLiteral("  "), QStringLiteral("project:P1")).isEmpty());
        QVERIFY(svc.record(QStringLiteral("x"), QString()).isEmpty());
        QCOMPARE(svc.entryCount(), 0);
    }

    void test_recallFindsByToken() {
        EmbeddingWorker worker;
        AcnService svc(DbManager::instance(), worker);
        svc.record(QStringLiteral("decision: use postgres for the ledger"),
                   QStringLiteral("project:P1"),
                   QStringLiteral("decision"));
        svc.record(QStringLiteral("open question about the billing cycle"),
                   QStringLiteral("project:P1"),
                   QStringLiteral("open_question"));

        const auto hits = svc.recall(QStringLiteral("postgres"), {QStringLiteral("project:P1")}, 5);
        QCOMPARE(hits.size(), 1);
        QVERIFY(hits.first().text.contains(QStringLiteral("postgres")));
        QCOMPARE(hits.first().entryKind, QStringLiteral("decision"));
    }

    void test_recallScopeFilter() {
        EmbeddingWorker worker;
        AcnService svc(DbManager::instance(), worker);
        svc.record(QStringLiteral("widget spec lives in project one"),
                   QStringLiteral("project:P1"));
        svc.record(QStringLiteral("widget spec lives in project two"),
                   QStringLiteral("project:P2"));

        const auto a = svc.recall(QStringLiteral("widget"), {QStringLiteral("project:P1")}, 5);
        QCOMPARE(a.size(), 1);
        QVERIFY(a.first().text.contains(QStringLiteral("one")));
    }

    void test_recallPunctuationSafe() {
        EmbeddingWorker worker;
        AcnService svc(DbManager::instance(), worker);
        svc.record(QStringLiteral("use std::map in C++ for the index"),
                   QStringLiteral("project:P1"));
        const auto hits =
            svc.recall(QStringLiteral("C++ (index)!"), {QStringLiteral("project:P1")}, 5);
        QCOMPARE(hits.size(), 1);
    }

    void test_purgeScope() {
        EmbeddingWorker worker;
        AcnService svc(DbManager::instance(), worker);
        svc.record(QStringLiteral("alpha summary"), QStringLiteral("project:P1"));
        svc.record(QStringLiteral("beta summary"), QStringLiteral("project:P1"));
        svc.record(QStringLiteral("gamma summary"), QStringLiteral("project:P2"));
        QCOMPARE(svc.entryCount(), 3);

        QCOMPARE(svc.purgeScope(QStringLiteral("project:P1")), 2);
        QCOMPARE(svc.entryCount(), 1);
        QVERIFY(svc.recall(QStringLiteral("alpha"), {QStringLiteral("project:P1")}, 5).isEmpty());
        QCOMPARE(svc.recall(QStringLiteral("gamma"), {QStringLiteral("project:P2")}, 5).size(), 1);
        QCOMPARE(svc.purgeScope(QString()), 0);
    }
};

QTEST_MAIN(TestAcnService)
#include "test-acn-service.moc"

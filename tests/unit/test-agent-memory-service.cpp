// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "models/db-manager.h"
#include "services/agent-memory-service.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>

class TestAgentMemoryService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/aim_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

    void test_saveInsertsRow() {
        EmbeddingWorker worker;
        AgentMemoryService svc(DbManager::instance(), worker);
        QCOMPARE(svc.memoryCount(), 0);
        const QString id =
            svc.save(QStringLiteral("The user prefers dark mode"), QStringLiteral("agent:A"));
        QVERIFY(!id.isEmpty());
        QCOMPARE(svc.memoryCount(), 1);
    }

    void test_saveRejectsEmpty() {
        EmbeddingWorker worker;
        AgentMemoryService svc(DbManager::instance(), worker);
        QVERIFY(svc.save(QStringLiteral("   "), QStringLiteral("agent:A")).isEmpty());
        QVERIFY(svc.save(QStringLiteral("something"), QString()).isEmpty());
        QCOMPARE(svc.memoryCount(), 0);
    }

    void test_recallFindsByToken() {
        EmbeddingWorker worker;
        AgentMemoryService svc(DbManager::instance(), worker);
        svc.save(QStringLiteral("The deployment server is named atlas"), QStringLiteral("agent:A"));
        svc.save(QStringLiteral("Favourite colour is teal"), QStringLiteral("agent:A"));

        const auto hits = svc.recall(QStringLiteral("atlas"), {QStringLiteral("agent:A")}, 5);
        QCOMPARE(hits.size(), 1);
        QVERIFY(hits.first().text.contains(QStringLiteral("atlas")));
        QCOMPARE(hits.first().ownerScope, QStringLiteral("agent:A"));
        QCOMPARE(hits.first().kind, QStringLiteral("explicit"));
    }

    void test_recallScopeFilter() {
        EmbeddingWorker worker;
        AgentMemoryService svc(DbManager::instance(), worker);
        svc.save(QStringLiteral("widget calibration value is 42"), QStringLiteral("agent:A"));
        svc.save(QStringLiteral("widget calibration value is 99"), QStringLiteral("agent:B"));

        const auto a = svc.recall(QStringLiteral("widget"), {QStringLiteral("agent:A")}, 5);
        QCOMPARE(a.size(), 1);
        QVERIFY(a.first().text.contains(QStringLiteral("42")));

        const auto b = svc.recall(QStringLiteral("widget"), {QStringLiteral("agent:B")}, 5);
        QCOMPARE(b.size(), 1);
        QVERIFY(b.first().text.contains(QStringLiteral("99")));

        const auto both = svc.recall(
            QStringLiteral("widget"), {QStringLiteral("agent:A"), QStringLiteral("agent:B")}, 5);
        QCOMPARE(both.size(), 2);
    }

    void test_recallEmptyInputs() {
        EmbeddingWorker worker;
        AgentMemoryService svc(DbManager::instance(), worker);
        svc.save(QStringLiteral("anything"), QStringLiteral("agent:A"));
        QVERIFY(svc.recall(QStringLiteral("anything"), {}, 5).isEmpty());
        QVERIFY(svc.recall(QString(), {QStringLiteral("agent:A")}, 5).isEmpty());
        QVERIFY(
            svc.recall(QStringLiteral("!!! ??? @@@"), {QStringLiteral("agent:A")}, 5).isEmpty());
    }

    void test_recallPunctuationSafe() {
        EmbeddingWorker worker;
        AgentMemoryService svc(DbManager::instance(), worker);
        svc.save(QStringLiteral("use std::sort to sort a list in C++"), QStringLiteral("agent:A"));
        const auto hits = svc.recall(QStringLiteral("C++ (sort)!"), {QStringLiteral("agent:A")}, 5);
        QCOMPARE(hits.size(), 1);
    }

    void test_purgeScope() {
        EmbeddingWorker worker;
        AgentMemoryService svc(DbManager::instance(), worker);
        svc.save(QStringLiteral("alpha fact"), QStringLiteral("agent:A"));
        svc.save(QStringLiteral("beta fact"), QStringLiteral("agent:A"));
        svc.save(QStringLiteral("gamma fact"), QStringLiteral("conversation:C"));
        QCOMPARE(svc.memoryCount(), 3);

        QCOMPARE(svc.purgeScope(QStringLiteral("agent:A")), 2);
        QCOMPARE(svc.memoryCount(), 1);

        QVERIFY(svc.recall(QStringLiteral("alpha"), {QStringLiteral("agent:A")}, 5).isEmpty());
        QCOMPARE(svc.recall(QStringLiteral("gamma"), {QStringLiteral("conversation:C")}, 5).size(),
                 1);
        QCOMPARE(svc.purgeScope(QString()), 0);
    }
};

QTEST_MAIN(TestAgentMemoryService)
#include "test-agent-memory-service.moc"

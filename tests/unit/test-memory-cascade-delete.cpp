// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "models/agent.h"
#include "models/db-manager.h"
#include "services/agent-memory-service.h"
#include "services/agent-registry.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestMemoryCascadeDelete : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

    QString createAgent(AgentRegistry& reg, const QString& name) {
        Agent a;
        a.name = name;
        a.systemPrompt = QStringLiteral("You are %1.").arg(name);
        return reg.createAgent(a);
    }

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/mcd_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

    void test_deleteEmitsAgentDeleted() {
        AgentRegistry reg(DbManager::instance());
        const QString id = createAgent(reg, QStringLiteral("Atlas"));
        QVERIFY(!id.isEmpty());

        QSignalSpy spy(&reg, &AgentRegistry::agentDeleted);
        QVERIFY(reg.deleteAgent(id));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), id);
    }

    void test_cascadePurgesOnlyDeletedAgent() {
        AgentRegistry reg(DbManager::instance());
        EmbeddingWorker worker;
        AgentMemoryService mem(DbManager::instance(), worker);

        QObject::connect(&reg, &AgentRegistry::agentDeleted, &mem, [&mem](const QString& agentId) {
            mem.purgeScope(QStringLiteral("agent:%1").arg(agentId));
        });

        const QString a = createAgent(reg, QStringLiteral("Alice"));
        const QString b = createAgent(reg, QStringLiteral("Bob"));
        mem.save(QStringLiteral("Alice remembers the deploy key"),
                 QStringLiteral("agent:%1").arg(a));
        mem.save(QStringLiteral("Bob remembers the office wifi"),
                 QStringLiteral("agent:%1").arg(b));
        QCOMPARE(mem.memoryCount(), 2);

        QVERIFY(reg.deleteAgent(a));

        QCOMPARE(mem.memoryCount(), 1);
        QVERIFY(
            mem.recall(QStringLiteral("deploy"), {QStringLiteral("agent:%1").arg(a)}, 5).isEmpty());
        QCOMPARE(mem.recall(QStringLiteral("wifi"), {QStringLiteral("agent:%1").arg(b)}, 5).size(),
                 1);
    }
};

QTEST_MAIN(TestMemoryCascadeDelete)
#include "test-memory-cascade-delete.moc"

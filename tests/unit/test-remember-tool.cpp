// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "models/db-manager.h"
#include "services/agent-memory-service.h"
#include "tools/memory/remember-tool.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>

class TestRememberTool : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

    static QJsonObject
    saveArgs(const QString& text, const QString& agentId, const QString& convId) {
        QJsonObject a;
        a[QStringLiteral("text")] = text;
        if (!agentId.isEmpty())
            a[QStringLiteral("__caller_agent_id")] = agentId;
        if (!convId.isEmpty())
            a[QStringLiteral("__caller_conv_id")] = convId;
        return a;
    }

    static QString scopeOf(const QString& id) {
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT owner_scope FROM agent_memories WHERE id = ?"));
        q.addBindValue(id);
        return (q.exec() && q.next()) ? q.value(0).toString() : QString();
    }

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath =
            m_tempDir.path() + QStringLiteral("/rt_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

    void test_schema() {
        EmbeddingWorker worker;
        AgentMemoryService mem(DbManager::instance(), worker);
        Tools::RememberTool tool(mem);
        QCOMPARE(tool.name(), QStringLiteral("remember"));
        QVERIFY(tool.runsOnMainThread());
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 1);
        QCOMPARE(params.first().name, QStringLiteral("text"));
        QVERIFY(params.first().required);
    }

    void test_saveAgentScoped() {
        EmbeddingWorker worker;
        AgentMemoryService mem(DbManager::instance(), worker);
        Tools::RememberTool tool(mem);
        const QJsonObject r = tool.invoke(saveArgs(QStringLiteral("the API key rotates monthly"),
                                                   QStringLiteral("agentA"),
                                                   QStringLiteral("c1")))
                                  .toObject();
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        const QString id = r.value(QStringLiteral("id")).toString();
        QVERIFY(!id.isEmpty());
        QCOMPARE(mem.memoryCount(), 1);
        QCOMPARE(scopeOf(id), QStringLiteral("agent:agentA"));
    }

    void test_saveAgentlessConversationScoped() {
        EmbeddingWorker worker;
        AgentMemoryService mem(DbManager::instance(), worker);
        Tools::RememberTool tool(mem);
        const QJsonObject r = tool.invoke(saveArgs(QStringLiteral("user lives in Berlin"),
                                                   QString(),
                                                   QStringLiteral("convX")))
                                  .toObject();
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        QCOMPARE(scopeOf(r.value(QStringLiteral("id")).toString()),
                 QStringLiteral("conversation:convX"));
    }

    void test_noCallerIdentityErrors() {
        EmbeddingWorker worker;
        AgentMemoryService mem(DbManager::instance(), worker);
        Tools::RememberTool tool(mem);
        QVERIFY(tool.invoke(saveArgs(QStringLiteral("x"), QString(), QString()))
                    .toObject()
                    .contains(QStringLiteral("error")));
        QCOMPARE(mem.memoryCount(), 0);
    }

    void test_emptyTextErrors() {
        EmbeddingWorker worker;
        AgentMemoryService mem(DbManager::instance(), worker);
        Tools::RememberTool tool(mem);
        QVERIFY(tool.invoke(saveArgs(QStringLiteral("   "), QStringLiteral("agentA"), QString()))
                    .toObject()
                    .contains(QStringLiteral("error")));
        QCOMPARE(mem.memoryCount(), 0);
    }
};

QTEST_MAIN(TestRememberTool)
#include "test-remember-tool.moc"

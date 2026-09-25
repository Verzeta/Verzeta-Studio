// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/rag-service.h"
#include "services/search-service.h"
#include "tools/memory/search-messages-tool.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QStandardPaths>

class TestSearchMessagesTool : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    EmbeddingWorker* m_embWorker = nullptr;
    RagService* m_ragSvc = nullptr;
    SearchService* m_searchSvc = nullptr;

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        QVERIFY(m_tempDir.isValid());
        DbManager::instance().close();
        const QString dbPath = m_tempDir.filePath(QStringLiteral("smt.db"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_embWorker = new EmbeddingWorker(this);
        m_ragSvc = new RagService(DbManager::instance(), *m_embWorker, this);
        m_searchSvc = new SearchService(DbManager::instance(), *m_ragSvc, this);
    }

    void cleanup() {
        delete m_searchSvc;
        m_searchSvc = nullptr;
        delete m_ragSvc;
        m_ragSvc = nullptr;
        delete m_embWorker;
        m_embWorker = nullptr;
        DbManager::instance().close();
    }

    void test_contract() {
        Tools::SearchMessagesTool tool(*m_searchSvc);
        QCOMPARE(tool.name(), QStringLiteral("search_messages"));
        QCOMPARE(tool.runsOnMainThread(), true);
    }

    void test_parameters() {
        Tools::SearchMessagesTool tool(*m_searchSvc);
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params[0].name, QStringLiteral("query"));
        QCOMPARE(params[0].required, true);
        QCOMPARE(params[1].name, QStringLiteral("limit"));
        QCOMPARE(params[1].type, QStringLiteral("integer"));
        QCOMPARE(params[1].required, false);
    }

    void test_invoke_emptyQuery_returnsError() {
        Tools::SearchMessagesTool tool(*m_searchSvc);
        QJsonObject args;
        args[QStringLiteral("query")] = QString();
        QVERIFY(tool.invoke(args).toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_validQuery_coldIndex_returnsEmptyMatches() {
        Tools::SearchMessagesTool tool(*m_searchSvc);
        QJsonObject args;
        args[QStringLiteral("query")] = QStringLiteral("anything");
        const QJsonObject result = tool.invoke(args).toObject();
        QVERIFY(result.contains(QStringLiteral("matches")));
        QVERIFY(result[QStringLiteral("matches")].isArray());
        QCOMPARE(result[QStringLiteral("count")].toInt(), 0);
    }
};

QTEST_MAIN(TestSearchMessagesTool)
#include "test-search-messages-tool.moc"

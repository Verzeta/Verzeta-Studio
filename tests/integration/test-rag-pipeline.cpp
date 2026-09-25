// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/tool-calling-schema.h"
#include "models/db-manager.h"
#include "services/file-service.h"
#include "services/rag-service.h"
#include "services/tool-service.h"
#include "utils/process-sandbox.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>

class TestRagPipeline : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tmpDir;
    QString m_dbPath;

  private slots:

    void initTestCase() { QVERIFY(m_tmpDir.isValid()); }

    void init() {
        m_dbPath = m_tmpDir.filePath(
            QStringLiteral("rag_integ_%1.db").arg(QDateTime::currentMSecsSinceEpoch()));
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

    void test_toolService_listFiles_realDir_returnsKnownEntries() {
        const QString fileA = m_tmpDir.filePath(QStringLiteral("alpha.txt"));
        const QString fileB = m_tmpDir.filePath(QStringLiteral("beta.txt"));
        {
            QFile f(fileA);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("alpha content");
        }
        {
            QFile f(fileB);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("beta content");
        }

        ProcessSandbox sandbox;
        FileService fileSvc;
        ToolService svc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("path")] = m_tmpDir.path();

        const QJsonValue result = svc.invokeTool(QStringLiteral("list_files"), args);

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QVERIFY(!obj.contains(QStringLiteral("error")));
        QVERIFY(obj.contains(QStringLiteral("files")));

        const QJsonArray files = obj[QStringLiteral("files")].toArray();
        QVERIFY(files.size() >= 2);

        QStringList listedPaths;
        for (const QJsonValue& v : files) {
            listedPaths.append(v.toString());
        }
        QVERIFY(listedPaths.contains(fileA) ||
                listedPaths.contains(m_tmpDir.path() + QDir::separator() +
                                     QStringLiteral("alpha.txt")));
    }

    void test_ragService_emptyCorpus_withRealDb() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);

        QCOMPARE(svc.embeddingCount(), 0);
        QCOMPARE(svc.documentCount(), 0);

        const QList<RagChunk> chunks = svc.retrieve(QStringLiteral("test query"));
        QVERIFY(chunks.isEmpty());

        const QString base = QStringLiteral("You are a helpful assistant.");
        const QString result = svc.augmentPrompt(QStringLiteral("test"), base);
        QCOMPARE(result, base);
    }

    void test_toolSchema_threeFormats_structurallyDistinct() {
        ToolParameterSchema paramA;
        paramA.name = QStringLiteral("query");
        paramA.type = QStringLiteral("string");
        paramA.description = QStringLiteral("The search query");
        paramA.required = true;

        ToolParameterSchema paramB;
        paramB.name = QStringLiteral("limit");
        paramB.type = QStringLiteral("integer");
        paramB.description = QStringLiteral("Max results");
        paramB.required = false;

        ToolSchema schema;
        schema.name = QStringLiteral("search_web");
        schema.description = QStringLiteral("Search the web for information");
        schema.parameters = {paramA, paramB};

        const QJsonObject openAI = schema.toOpenAIFunction();
        const QJsonObject anthropic = schema.toAnthropicTool();
        const QJsonObject gemini = schema.toGeminiFunction();

        QCOMPARE(openAI[QStringLiteral("type")].toString(), QStringLiteral("function"));
        const QJsonObject oaFn = openAI[QStringLiteral("function")].toObject();
        QCOMPARE(oaFn[QStringLiteral("name")].toString(), schema.name);
        QCOMPARE(oaFn[QStringLiteral("parameters")].toObject()[QStringLiteral("type")].toString(),
                 QStringLiteral("object"));
        const QJsonArray oaRequired =
            oaFn[QStringLiteral("parameters")].toObject()[QStringLiteral("required")].toArray();
        QVERIFY(oaRequired.contains(QStringLiteral("query")));
        QVERIFY(!oaRequired.contains(QStringLiteral("limit")));

        QCOMPARE(anthropic[QStringLiteral("name")].toString(), schema.name);
        QVERIFY(anthropic.contains(QStringLiteral("input_schema")));
        QVERIFY(!anthropic.contains(QStringLiteral("parameters")));
        QCOMPARE(
            anthropic[QStringLiteral("input_schema")].toObject()[QStringLiteral("type")].toString(),
            QStringLiteral("object"));

        QCOMPARE(gemini[QStringLiteral("name")].toString(), schema.name);
        QVERIFY(gemini.contains(QStringLiteral("parameters")));
        const QJsonObject gemParams = gemini[QStringLiteral("parameters")].toObject();
        QCOMPARE(gemParams[QStringLiteral("type")].toString(), QStringLiteral("OBJECT"));
        const QJsonObject gemProps = gemParams[QStringLiteral("properties")].toObject();
        QCOMPARE(gemProps[QStringLiteral("query")].toObject()[QStringLiteral("type")].toString(),
                 QStringLiteral("STRING"));
        QCOMPARE(gemProps[QStringLiteral("limit")].toObject()[QStringLiteral("type")].toString(),
                 QStringLiteral("INTEGER"));
    }
};

QTEST_MAIN(TestRagPipeline)
#include "test-rag-pipeline.moc"

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/rag-service.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>

class TestRagService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

    void insertEmbedding(const QString& trackingId,
                         const QString& sourceId,
                         const QString& sourceType,
                         const QString& chunkText,
                         const QVector<float>& embedding) {
        QByteArray blob;
        blob.resize(embedding.size() * static_cast<int>(sizeof(float)));
        memcpy(blob.data(), embedding.constData(), static_cast<size_t>(blob.size()));

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO embeddings"
            "(id, source_id, source_type, chunk_text, embedding, model_used, created_at) "
            "VALUES(?,?,?,?,?,?,?)"));
        q.addBindValue(trackingId);
        q.addBindValue(sourceId);
        q.addBindValue(sourceType);
        q.addBindValue(chunkText);
        q.addBindValue(blob);
        q.addBindValue(QStringLiteral("text-embedding-3-small"));
        q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        QVERIFY(q.exec());
    }

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/rag_test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

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

    void test_retrieve_emptyCorpusReturnsEmpty() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);
        const QList<RagChunk> chunks = svc.retrieve(QStringLiteral("test query"));
        QVERIFY(chunks.isEmpty());
    }

    void test_documentCount_freshDbReturnsZero() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);
        QCOMPARE(svc.documentCount(), 0);
    }

    void test_embeddingCount_freshDbReturnsZero() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);
        QCOMPARE(svc.embeddingCount(), 0);
    }

    void test_v22_ownerScopeColumnDefaultAndIndex() {
        auto hasColumn = [](const QString& table, const QString& col) -> bool {
            QSqlQuery info(DbManager::instance().db());
            if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
                return false;
            while (info.next())
                if (info.value(1).toString() == col)
                    return true;
            return false;
        };
        QVERIFY(hasColumn(QStringLiteral("embeddings"), QStringLiteral("owner_scope")));
        QVERIFY(hasColumn(QStringLiteral("documents"), QStringLiteral("owner_scope")));

        QSqlQuery ins(DbManager::instance().db());
        QVERIFY(ins.exec(
            QStringLiteral("INSERT INTO embeddings(id, source_id, source_type, chunk_text, "
                           "embedding, model_used, created_at) "
                           "VALUES('e_v22','m1','message','hello', X'00000000', 'nomic', 0)")));
        QSqlQuery sel(DbManager::instance().db());
        QVERIFY(sel.exec(QStringLiteral("SELECT owner_scope FROM embeddings WHERE id='e_v22'")));
        QVERIFY(sel.next());
        QCOMPARE(sel.value(0).toString(), QStringLiteral("global"));

        QSqlQuery idx(DbManager::instance().db());
        QVERIFY(idx.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='index' "
                                        "AND name='idx_embeddings_scope'")));
        QVERIFY(idx.next());
    }

    void test_clearEmbeddings() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);

        auto ins = [](const QString& id, const QString& scope) {
            QSqlQuery q(DbManager::instance().db());
            q.prepare(
                QStringLiteral("INSERT INTO embeddings(id, source_id, source_type, chunk_text, "
                               "embedding, model_used, owner_scope, created_at) "
                               "VALUES(?, ?, 'message', 'x', X'00000000', 'm', ?, 0)"));
            q.addBindValue(id);
            q.addBindValue(id);
            q.addBindValue(scope);
            QVERIFY(q.exec());
        };
        ins(QStringLiteral("a1"), QStringLiteral("conversation:A"));
        ins(QStringLiteral("a2"), QStringLiteral("conversation:A"));
        ins(QStringLiteral("b1"), QStringLiteral("conversation:B"));
        QCOMPARE(svc.embeddingCount(), 3);

        QCOMPARE(svc.clearConversationEmbeddings(QStringLiteral("A")), 2);
        QCOMPARE(svc.embeddingCount(), 1);

        QCOMPARE(svc.clearAllEmbeddings(), 1);
        QCOMPARE(svc.embeddingCount(), 0);
    }

    void test_augmentPrompt_disabledReturnsBase() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);

        const QString base = QStringLiteral("You are a helpful assistant.");
        QCOMPARE(svc.augmentPrompt(QStringLiteral("query"), base), base);
    }

    void test_augmentPrompt_noEmbeddingsReturnsBase() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);

        const QString base = QStringLiteral("You are a helpful assistant.");
        QCOMPARE(svc.augmentPrompt(QStringLiteral("query"), base), base);
    }

    void test_embeddingCount_afterDirectInsert_increases() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);

        QCOMPARE(svc.embeddingCount(), 0);

        QVector<float> fakeVec(4, 0.25f);
        insertEmbedding(QStringLiteral("doc1::0::document"),
                        QStringLiteral("doc1"),
                        QStringLiteral("document"),
                        QStringLiteral("chunk text here"),
                        fakeVec);

        QCOMPARE(svc.embeddingCount(), 1);
    }

    void test_retrieve_disabled_returnsEmptyEvenWithEmbeddings() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);

        for (int i = 0; i < 3; ++i) {
            QVector<float> v(4, static_cast<float>(i + 1) / 4.0f);
            insertEmbedding(QStringLiteral("msg%1::0::message").arg(i),
                            QStringLiteral("msg%1").arg(i),
                            QStringLiteral("message"),
                            QStringLiteral("chunk content %1").arg(i),
                            v);
        }
        QCOMPARE(svc.embeddingCount(), 3);

        const QList<RagChunk> result = svc.retrieve(QStringLiteral("test query"));
        QVERIFY(result.isEmpty());
    }

    void test_augmentPrompt_enabledNoEmbeddings_returnsBase() {
        EmbeddingWorker worker;
        RagService svc(DbManager::instance(), worker);

        QCOMPARE(svc.embeddingCount(), 0);

        const QString base = QStringLiteral("You are a coding assistant.");
        const QString result = svc.augmentPrompt(QStringLiteral("how do I sort a list?"), base);

        QCOMPARE(result, base);
    }
};

QTEST_MAIN(TestRagService)
#include "test-rag-service.moc"

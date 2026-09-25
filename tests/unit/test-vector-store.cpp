// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QFile>
#include <QSqlError>
#include <QSqlQuery>

class TestVectorStore : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

  private slots:
    void init() {
        m_dbPath = m_tempDir.path() + QStringLiteral("/vec-test.db");
        DbManager::instance().close();
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
    }

    void cleanup() {
        DbManager::instance().close();
        QFile::remove(m_dbPath);
    }

    void test_freshConnectionUnavailable() {
        QVERIFY(!DbManager::instance().isVectorSearchAvailable());
    }

    void test_emptyPathGraceful() {
        QCOMPARE(DbManager::instance().loadVectorExtension(QString()), false);
        QVERIFY(!DbManager::instance().isVectorSearchAvailable());
    }

    void test_nonexistentPathGraceful() {
        QCOMPARE(
            DbManager::instance().loadVectorExtension(QStringLiteral("/nonexistent/path/vec0.so")),
            false);
        QVERIFY(!DbManager::instance().isVectorSearchAvailable());
    }

    void test_realLoadAndKnn() {
#ifdef VERZETA_VEC0_PATH
        const QString path = QStringLiteral(VERZETA_VEC0_PATH);
        if (path.isEmpty() || !QFile::exists(path)) {
            QSKIP("sqlite-vec loadable not available in this build");
        }
        const bool ok = DbManager::instance().loadVectorExtension(path);
        if (!ok) {
            QSKIP("vec0 did not load (sqlite version mismatch / unsupported "
                  "platform) — brute-force fallback path");
        }
        QVERIFY(DbManager::instance().isVectorSearchAvailable());

        QSqlQuery q(DbManager::instance().db());
        QVERIFY2(q.exec(QStringLiteral("CREATE VIRTUAL TABLE t_vec USING vec0(e float[3])")),
                 qPrintable(q.lastError().text()));
        QVERIFY(q.exec(
            QStringLiteral("INSERT INTO t_vec(rowid, e) VALUES (1, '[1,2,3]'), (2, '[9,9,9]')")));
        QVERIFY(q.exec(QStringLiteral("SELECT rowid FROM t_vec WHERE e MATCH '[1,2,3]' "
                                      "ORDER BY distance LIMIT 1")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
#else
        QSKIP("VERZETA_VEC0_PATH not defined — sqlite-vec not built into this "
              "configuration");
#endif
    }

    void test_filteredKnnJoinPattern() {
#ifdef VERZETA_VEC0_PATH
        const QString path = QStringLiteral(VERZETA_VEC0_PATH);
        if (path.isEmpty() || !QFile::exists(path)) {
            QSKIP("sqlite-vec loadable not available in this build");
        }
        if (!DbManager::instance().loadVectorExtension(path)) {
            QSKIP("vec0 did not load — brute-force fallback path");
        }

        QSqlQuery q(DbManager::instance().db());
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE emb(id TEXT PRIMARY KEY, chunk_text TEXT, "
                                      "model_used TEXT, owner_scope TEXT, embedding BLOB)")));
        QVERIFY(
            q.exec(QStringLiteral("INSERT INTO emb(id,chunk_text,model_used,owner_scope) VALUES "
                                  "('a','alpha','m1','global'),"
                                  "('b','bravo','m1','conversation:x'),"
                                  "('c','charlie','m2','global')")));
        QVERIFY2(q.exec(QStringLiteral("CREATE VIRTUAL TABLE vec_emb USING vec0("
                                       "embedding float[3] distance_metric=cosine, "
                                       "model_id text, owner_scope text)")),
                 qPrintable(q.lastError().text()));
        QVERIFY(q.exec(
            QStringLiteral("INSERT INTO vec_emb(rowid,embedding,model_id,owner_scope) "
                           "SELECT rowid,'[1,0,0]',model_used,owner_scope FROM emb WHERE id='a'")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO vec_emb(rowid,embedding,model_id,owner_scope) "
            "SELECT rowid,'[0.9,0.1,0]',model_used,owner_scope FROM emb WHERE id='b'")));
        QVERIFY(q.exec(
            QStringLiteral("INSERT INTO vec_emb(rowid,embedding,model_id,owner_scope) "
                           "SELECT rowid,'[9,9,9]',model_used,owner_scope FROM emb WHERE id='c'")));

        QVERIFY2(q.exec(QStringLiteral("SELECT e.id, v.distance "
                                       "FROM vec_emb v JOIN emb e ON e.rowid = v.rowid "
                                       "WHERE v.embedding MATCH '[1,0,0]' AND v.k = 5 "
                                       "AND v.model_id = 'm1' "
                                       "AND v.owner_scope IN ('global','conversation:x') "
                                       "ORDER BY v.distance")),
                 qPrintable(q.lastError().text()));
        QStringList ids;
        while (q.next()) {
            ids << q.value(0).toString();
        }
        QCOMPARE(ids.size(), 2);
        QCOMPARE(ids.first(), QStringLiteral("a"));
        QVERIFY(ids.contains(QStringLiteral("b")));
#else
        QSKIP("VERZETA_VEC0_PATH not defined — sqlite-vec not built into this "
              "configuration");
#endif
    }
};

QTEST_GUILESS_MAIN(TestVectorStore)
#include "test-vector-store.moc"

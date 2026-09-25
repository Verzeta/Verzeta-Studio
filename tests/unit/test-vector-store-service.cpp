// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "models/db-manager.h"
#include "services/vector-store.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVector>

class TestVectorStoreService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

    static QSqlDatabase db() { return DbManager::instance().db(); }

    qint64 insertRow(const QString& id,
                     const QVector<float>& v,
                     const QString& model,
                     const QString& scope) {
        QSqlQuery q(db());
        q.prepare(QStringLiteral("INSERT INTO vstest(id, embedding, model_used, owner_scope) "
                                 "VALUES(?, ?, ?, ?)"));
        q.addBindValue(id);
        q.addBindValue(VectorStore::serialize(v));
        q.addBindValue(model);
        q.addBindValue(scope);
        if (!q.exec()) {
            qWarning() << "insertRow failed:" << q.lastError().text();
            return -1;
        }
        return q.lastInsertId().toLongLong();
    }

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/vss_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

#ifdef VERZETA_VEC0_PATH
        DbManager::instance().loadVectorExtension(QStringLiteral(VERZETA_VEC0_PATH));
#endif

        QSqlQuery c(db());
        QVERIFY(c.exec(QStringLiteral("CREATE TABLE vstest("
                                      "id TEXT PRIMARY KEY, embedding BLOB, "
                                      "model_used TEXT, owner_scope TEXT)")));
    }

    void cleanup() {
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_serializeRoundTrip() {
        const QVector<float> v{1.5f, -2.25f, 0.0f, 42.0f};
        const QVector<float> back = VectorStore::deserialize(VectorStore::serialize(v));
        QCOMPARE(back, v);
        QVERIFY(VectorStore::deserialize(QByteArray()).isEmpty());
    }

    void test_cosineSimilarity() {
        const QVector<float> a{1.0f, 0.0f, 0.0f};
        const QVector<float> same{2.0f, 0.0f, 0.0f};
        const QVector<float> orth{0.0f, 1.0f, 0.0f};
        const QVector<float> opp{-1.0f, 0.0f, 0.0f};
        QVERIFY(qAbs(VectorStore::cosineSimilarity(a, same) - 1.0f) < 1e-5f);
        QVERIFY(qAbs(VectorStore::cosineSimilarity(a, orth) - 0.0f) < 1e-5f);
        QVERIFY(qAbs(VectorStore::cosineSimilarity(a, opp) + 1.0f) < 1e-5f);
        QCOMPARE(VectorStore::cosineSimilarity(a, QVector<float>{1.0f}), 0.0f);
        QCOMPARE(VectorStore::cosineSimilarity({}, {}), 0.0f);
    }

    void test_toJsonArray() {
        QCOMPARE(VectorStore::toJsonArray(QVector<float>{1.0f, 2.0f, 3.0f}),
                 QStringLiteral("[1,2,3]"));
        QCOMPARE(VectorStore::toJsonArray(QVector<float>{}), QStringLiteral("[]"));
    }

    void test_searchRanksNearestFirst() {
        VectorStore vs(
            DbManager::instance(), QStringLiteral("vstest"), QStringLiteral("vec_vstest"));
        const QString m = QStringLiteral("m1");
        const qint64 rx =
            insertRow(QStringLiteral("x"), {1.0f, 0.0f, 0.0f}, m, QStringLiteral("global"));
        const qint64 ry =
            insertRow(QStringLiteral("y"), {0.0f, 1.0f, 0.0f}, m, QStringLiteral("global"));
        const qint64 rz =
            insertRow(QStringLiteral("z"), {0.0f, 0.0f, 1.0f}, m, QStringLiteral("global"));
        QVERIFY(rx > 0 && ry > 0 && rz > 0);
        vs.upsert(rx, {1.0f, 0.0f, 0.0f}, m, QStringLiteral("global"));
        vs.upsert(ry, {0.0f, 1.0f, 0.0f}, m, QStringLiteral("global"));
        vs.upsert(rz, {0.0f, 0.0f, 1.0f}, m, QStringLiteral("global"));

        const auto hits = vs.search({0.1f, 0.9f, 0.0f}, m, {}, 3);
        QVERIFY(!hits.isEmpty());
        QCOMPARE(hits.first().rowid, ry);
        for (int i = 1; i < hits.size(); ++i) {
            QVERIFY(hits[i - 1].score >= hits[i].score);
        }
    }

    void test_searchScopeFilter() {
        VectorStore vs(
            DbManager::instance(), QStringLiteral("vstest"), QStringLiteral("vec_vstest"));
        const QString m = QStringLiteral("m1");
        const qint64 ra =
            insertRow(QStringLiteral("a"), {1.0f, 0.0f, 0.0f}, m, QStringLiteral("conversation:A"));
        const qint64 rb =
            insertRow(QStringLiteral("b"), {1.0f, 0.0f, 0.0f}, m, QStringLiteral("conversation:B"));
        QVERIFY(ra > 0 && rb > 0);
        vs.upsert(ra, {1.0f, 0.0f, 0.0f}, m, QStringLiteral("conversation:A"));
        vs.upsert(rb, {1.0f, 0.0f, 0.0f}, m, QStringLiteral("conversation:B"));

        const auto hits = vs.search({1.0f, 0.0f, 0.0f}, m, {QStringLiteral("conversation:A")}, 5);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.first().rowid, ra);
    }

    void test_searchModelFilter() {
        VectorStore vs(
            DbManager::instance(), QStringLiteral("vstest"), QStringLiteral("vec_vstest"));
        const qint64 r1 = insertRow(QStringLiteral("1"),
                                    {1.0f, 0.0f, 0.0f},
                                    QStringLiteral("m1"),
                                    QStringLiteral("global"));
        const qint64 r2 = insertRow(QStringLiteral("2"),
                                    {1.0f, 0.0f, 0.0f},
                                    QStringLiteral("m2"),
                                    QStringLiteral("global"));
        QVERIFY(r1 > 0 && r2 > 0);
        vs.upsert(r1, {1.0f, 0.0f, 0.0f}, QStringLiteral("m1"), QStringLiteral("global"));
        vs.upsert(r2, {1.0f, 0.0f, 0.0f}, QStringLiteral("m2"), QStringLiteral("global"));

        const auto hits = vs.search({1.0f, 0.0f, 0.0f}, QStringLiteral("m1"), {}, 5);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.first().rowid, r1);
    }

    void test_purgeScope() {
        VectorStore vs(
            DbManager::instance(), QStringLiteral("vstest"), QStringLiteral("vec_vstest"));
        const QString m = QStringLiteral("m1");
        insertRow(QStringLiteral("a1"), {1.0f, 0.0f, 0.0f}, m, QStringLiteral("agent:X"));
        insertRow(QStringLiteral("a2"), {0.0f, 1.0f, 0.0f}, m, QStringLiteral("agent:X"));
        insertRow(QStringLiteral("b1"), {0.0f, 0.0f, 1.0f}, m, QStringLiteral("agent:Y"));

        QCOMPARE(vs.purgeScope(QStringLiteral("agent:X")), 2);

        QSqlQuery q(db());
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM vstest")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        QCOMPARE(vs.purgeScope(QString()), 0);
    }

    void test_clearAll() {
        VectorStore vs(
            DbManager::instance(), QStringLiteral("vstest"), QStringLiteral("vec_vstest"));
        const QString m = QStringLiteral("m1");
        insertRow(QStringLiteral("a"), {1.0f, 0.0f, 0.0f}, m, QStringLiteral("global"));
        insertRow(QStringLiteral("b"), {0.0f, 1.0f, 0.0f}, m, QStringLiteral("global"));

        QCOMPARE(vs.clearAll(), 2);
        QSqlQuery q(db());
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM vstest")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }
};

QTEST_MAIN(TestVectorStoreService)
#include "test-vector-store-service.moc"

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "services/chat/memory-retriever.h"
#include "services/chat/memory-source.h"
#include "workers/embedding-worker.h"

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QVector>

class FakeSource : public Chat::IMemorySource {
  public:
    FakeSource(QString id, bool enabled, QList<Chat::MemoryHit> hits)
        : m_id(std::move(id)), m_enabled(enabled), m_hits(std::move(hits)) {}
    QString id() const override { return m_id; }
    bool isEnabled(const Chat::MemoryContext&) const override { return m_enabled; }
    bool usesEmbedding() const override { return false; }
    QList<Chat::MemoryHit>
    candidates(const Chat::MemoryContext&, const QVector<float>&, const QString&) override {
        return m_hits;
    }

  private:
    QString m_id;
    bool m_enabled;
    QList<Chat::MemoryHit> m_hits;
};

class TestMemoryRetriever : public QObject {
    Q_OBJECT

  private:
    static Chat::MemoryContext ctx(const QString& query) {
        Chat::MemoryContext c;
        c.conversationId = QStringLiteral("conv1");
        c.responderAgentId = QStringLiteral("agentA");
        c.queryText = query;
        return c;
    }

    static Chat::MemoryHit sem(const QString& src,
                               const QString& label,
                               const QString& text,
                               double score,
                               const QVector<float>& v) {
        Chat::MemoryHit h;
        h.sourceId = src;
        h.label = label;
        h.text = text;
        h.score = score;
        h.semantic = true;
        h.vector = v;
        return h;
    }

    static Chat::MemoryHit lex(const QString& src, const QString& label, const QString& text) {
        Chat::MemoryHit h;
        h.sourceId = src;
        h.label = label;
        h.text = text;
        h.semantic = false;
        return h;
    }

    static QVector<float> oneHot(int dim, int idx) {
        QVector<float> v(dim, 0.0f);
        v[idx] = 1.0f;
        return v;
    }

  private slots:

    void test_nothingEnabledReturnsZero() {
        EmbeddingWorker worker;
        Chat::MemoryRetriever r(worker);
        FakeSource off(QStringLiteral("aim"),
                       false,
                       {lex(QStringLiteral("aim"), QStringLiteral("you"), QStringLiteral("X"))});
        r.addSource(&off);
        QCOMPARE(r.beginRetrieve(ctx(QStringLiteral("hello"))), quint64(0));
    }

    void test_emptyQueryReturnsZero() {
        EmbeddingWorker worker;
        Chat::MemoryRetriever r(worker);
        FakeSource on(QStringLiteral("aim"),
                      true,
                      {lex(QStringLiteral("aim"), QStringLiteral("you"), QStringLiteral("X"))});
        r.addSource(&on);
        QCOMPARE(r.beginRetrieve(ctx(QStringLiteral("   "))), quint64(0));
    }

    void test_oneEnabledSourceEmitsUnifiedBlock() {
        EmbeddingWorker worker;
        Chat::MemoryRetriever r(worker);
        FakeSource on(QStringLiteral("aim"),
                      true,
                      {lex(QStringLiteral("aim"),
                           QStringLiteral("you"),
                           QStringLiteral("the user prefers metric units"))});
        r.addSource(&on);

        QSignalSpy spy(&r, &Chat::MemoryRetriever::retrievalReady);
        const quint64 rid = r.beginRetrieve(ctx(QStringLiteral("recall me")));
        QVERIFY(rid != 0);
        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toULongLong(), rid);
        const QString out = spy.first().at(1).toString();
        QVERIFY(out.contains(QStringLiteral("## Relevant memory")));
        QVERIFY(out.contains(QStringLiteral("the user prefers metric units")));
    }

    void test_disabledSourceExcluded() {
        EmbeddingWorker worker;
        Chat::MemoryRetriever r(worker);
        FakeSource on(
            QStringLiteral("aim"),
            true,
            {lex(QStringLiteral("aim"), QStringLiteral("you"), QStringLiteral("alpha entry"))});
        FakeSource off(
            QStringLiteral("rag"),
            false,
            {lex(QStringLiteral("rag"), QStringLiteral("context"), QStringLiteral("beta entry"))});
        r.addSource(&on);
        r.addSource(&off);

        QSignalSpy spy(&r, &Chat::MemoryRetriever::retrievalReady);
        QVERIFY(r.beginRetrieve(ctx(QStringLiteral("q"))) != 0);
        QVERIFY(spy.wait(2000));
        const QString out = spy.first().at(1).toString();
        QVERIFY(out.contains(QStringLiteral("alpha entry")));
        QVERIFY(!out.contains(QStringLiteral("beta entry")));
    }

    void test_relevanceFloorDropsIrrelevant() {
        EmbeddingWorker worker;
        Chat::MemoryRetriever r(worker);
        FakeSource s(QStringLiteral("acn"),
                     true,
                     {
                         sem(QStringLiteral("acn"),
                             QStringLiteral("team"),
                             QStringLiteral("highly relevant fact"),
                             0.80,
                             oneHot(4, 0)),
                         sem(QStringLiteral("acn"),
                             QStringLiteral("team"),
                             QStringLiteral("unrelated noise"),
                             0.10,
                             oneHot(4, 1)),
                     });
        r.addSource(&s);

        QSignalSpy spy(&r, &Chat::MemoryRetriever::retrievalReady);
        QVERIFY(r.beginRetrieve(ctx(QStringLiteral("q"))) != 0);
        QVERIFY(spy.wait(2000));
        const QString out = spy.first().at(1).toString();
        QVERIFY(out.contains(QStringLiteral("highly relevant fact")));
        QVERIFY(!out.contains(QStringLiteral("unrelated noise")));
    }

    void test_crossSourceDuplicateInjectedOnce() {
        EmbeddingWorker worker;
        Chat::MemoryRetriever r(worker);
        const QVector<float> v = oneHot(4, 2);
        FakeSource team(QStringLiteral("acn"),
                        true,
                        {
                            sem(QStringLiteral("acn"),
                                QStringLiteral("team"),
                                QStringLiteral("staging server is called atlas"),
                                0.85,
                                v),
                        });
        FakeSource rag(QStringLiteral("rag"),
                       true,
                       {
                           sem(QStringLiteral("rag"),
                               QStringLiteral("context"),
                               QStringLiteral("the staging server atlas"),
                               0.70,
                               v),
                       });
        r.addSource(&team);
        r.addSource(&rag);

        QSignalSpy spy(&r, &Chat::MemoryRetriever::retrievalReady);
        QVERIFY(r.beginRetrieve(ctx(QStringLiteral("q"))) != 0);
        QVERIFY(spy.wait(2000));
        const QString out = spy.first().at(1).toString();
        QCOMPARE(out.count(QStringLiteral("\n- ")), 1);
        QVERIFY(out.contains(QStringLiteral("staging server is called atlas")));
        QVERIFY(!out.contains(QStringLiteral("the staging server atlas")));
    }

    void test_oversizedInputIsCapped() {
        EmbeddingWorker worker;
        Chat::MemoryRetriever r(worker);
        QList<Chat::MemoryHit> many;
        for (int i = 0; i < 40; ++i) {
            many << sem(QStringLiteral("acn"),
                        QStringLiteral("team"),
                        QStringLiteral("entry %1 ").arg(i) + QString(600, QLatin1Char('z')),
                        0.9,
                        oneHot(40, i));
        }
        FakeSource big(QStringLiteral("acn"), true, many);
        r.addSource(&big);

        QSignalSpy spy(&r, &Chat::MemoryRetriever::retrievalReady);
        QVERIFY(r.beginRetrieve(ctx(QStringLiteral("q"))) != 0);
        QVERIFY(spy.wait(2000));
        const QString out = spy.first().at(1).toString();
        QVERIFY(out.startsWith(QStringLiteral("\n\n## Relevant memory\n")));
        QVERIFY(out.endsWith(QStringLiteral("\n")));
        QVERIFY(out.length() <= 4096);
        QVERIFY(out.count(QStringLiteral("\n- ")) <= 8);
    }

    void test_flattenMemoryEntryCapsAndFlattens() {
        const QString flat =
            Chat::flattenMemoryEntry(QStringLiteral("line one\nline two\nline three"));
        QCOMPARE(flat, QStringLiteral("line one line two line three"));

        const QString longText = QString(2000, QLatin1Char('x'));
        const QString capped = Chat::flattenMemoryEntry(longText);
        QVERIFY(capped.length() <= Chat::kMemoryEntryMaxChars);
        QVERIFY(capped.endsWith(QStringLiteral("…")));
    }
};

QTEST_MAIN(TestMemoryRetriever)
#include "test-memory-retriever.moc"

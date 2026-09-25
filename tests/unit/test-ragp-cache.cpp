// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/ragp/ragp-cache.h"
#include "services/ragp/ragp-types.h"

#include <QTest>
#include <QThread>

using Ragp::Cache;
using Ragp::Classification;
using Ragp::Intent;
using Ragp::Target;

class TestRagpCache : public QObject {
    Q_OBJECT

  private:
    static Classification
    makeClassification(const QString& alias, Intent intent, double confidence = 0.9) {
        Classification c;
        c.confidence = confidence;
        c.source = QStringLiteral("test-backend");
        Target t;
        t.alias = alias;
        t.intent = intent;
        c.targets.append(t);
        return c;
    }

  private slots:

    void test_emptyMiss() {
        Cache cache;
        Classification out;
        const bool hit =
            cache.lookup(QStringLiteral("any content"), {QStringLiteral("Alice")}, out);
        QVERIFY(!hit);
    }

    void test_storeAndHit() {
        Cache cache;
        const QString content = QStringLiteral("@Bob please review");
        const QStringList roster = {QStringLiteral("Alice"), QStringLiteral("Bob")};
        const Classification stored =
            makeClassification(QStringLiteral("Bob"), Intent::DELEGATE_RESPONSE);
        cache.store(content, roster, stored);

        Classification out;
        QVERIFY(cache.lookup(content, roster, out));
        QCOMPARE(out.targets.size(), 1);
        QCOMPARE(out.targets[0].alias, QStringLiteral("Bob"));
        QCOMPARE(out.targets[0].intent, Intent::DELEGATE_RESPONSE);
        QVERIFY(out.fromCache);
    }

    void test_differentContentSeparateEntries() {
        Cache cache;
        const QStringList roster = {QStringLiteral("A"), QStringLiteral("B")};
        cache.store(QStringLiteral("msg1"),
                    roster,
                    makeClassification(QStringLiteral("B"), Intent::DELEGATE_RESPONSE));
        cache.store(QStringLiteral("msg2"),
                    roster,
                    makeClassification(QStringLiteral("B"), Intent::ACKNOWLEDGMENT));

        QCOMPARE(cache.size(), 2);

        Classification out;
        QVERIFY(cache.lookup(QStringLiteral("msg1"), roster, out));
        QCOMPARE(out.targets[0].intent, Intent::DELEGATE_RESPONSE);
        QVERIFY(cache.lookup(QStringLiteral("msg2"), roster, out));
        QCOMPARE(out.targets[0].intent, Intent::ACKNOWLEDGMENT);
    }

    void test_rosterOrderIndependence() {
        Cache cache;
        const QString content = QStringLiteral("@Bob");
        cache.store(content,
                    {QStringLiteral("Alice"), QStringLiteral("Bob")},
                    makeClassification(QStringLiteral("Bob"), Intent::DELEGATE_RESPONSE));

        Classification out;
        QVERIFY(cache.lookup(content, {QStringLiteral("Bob"), QStringLiteral("Alice")}, out));
    }

    void test_differentRosterMiss() {
        Cache cache;
        const QString content = QStringLiteral("@Bob hi");
        cache.store(content,
                    {QStringLiteral("Alice"), QStringLiteral("Bob")},
                    makeClassification(QStringLiteral("Bob"), Intent::DELEGATE_RESPONSE));

        Classification out;
        QVERIFY(
            !cache.lookup(content,
                          {QStringLiteral("Alice"), QStringLiteral("Bob"), QStringLiteral("Carol")},
                          out));
    }

    void test_ttlExpiry() {
        Cache cache(256, 50);
        const QStringList roster = {QStringLiteral("A")};
        cache.store(QStringLiteral("content"),
                    roster,
                    makeClassification(QStringLiteral("A"), Intent::REFERENCE));

        Classification out;
        QVERIFY(cache.lookup(QStringLiteral("content"), roster, out));

        QThread::msleep(80);

        QVERIFY(!cache.lookup(QStringLiteral("content"), roster, out));
    }

    void test_capacityEviction() {
        Cache cache(3);
        const QStringList roster = {QStringLiteral("A")};
        cache.store(QStringLiteral("msg1"),
                    roster,
                    makeClassification(QStringLiteral("A"), Intent::REFERENCE));
        QThread::msleep(5);
        cache.store(QStringLiteral("msg2"),
                    roster,
                    makeClassification(QStringLiteral("A"), Intent::REFERENCE));
        QThread::msleep(5);
        cache.store(QStringLiteral("msg3"),
                    roster,
                    makeClassification(QStringLiteral("A"), Intent::REFERENCE));
        QThread::msleep(5);
        cache.store(QStringLiteral("msg4"),
                    roster,
                    makeClassification(QStringLiteral("A"), Intent::REFERENCE));

        QCOMPARE(cache.size(), 3);
        Classification out;
        QVERIFY(!cache.lookup(QStringLiteral("msg1"), roster, out));
        QVERIFY(cache.lookup(QStringLiteral("msg2"), roster, out));
        QVERIFY(cache.lookup(QStringLiteral("msg3"), roster, out));
        QVERIFY(cache.lookup(QStringLiteral("msg4"), roster, out));
    }

    void test_clear() {
        Cache cache;
        cache.store(QStringLiteral("a"),
                    {QStringLiteral("X")},
                    makeClassification(QStringLiteral("X"), Intent::REFERENCE));
        QCOMPARE(cache.size(), 1);
        cache.clear();
        QCOMPARE(cache.size(), 0);
        Classification out;
        QVERIFY(!cache.lookup(QStringLiteral("a"), {QStringLiteral("X")}, out));
    }

    void test_overwrite() {
        Cache cache;
        const QString content = QStringLiteral("msg");
        const QStringList roster = {QStringLiteral("A")};
        cache.store(content, roster, makeClassification(QStringLiteral("A"), Intent::REFERENCE));
        cache.store(
            content, roster, makeClassification(QStringLiteral("A"), Intent::DELEGATE_TASK));

        QCOMPARE(cache.size(), 1);
        Classification out;
        QVERIFY(cache.lookup(content, roster, out));
        QCOMPARE(out.targets[0].intent, Intent::DELEGATE_TASK);
    }
};

QTEST_MAIN(TestRagpCache)
#include "test-ragp-cache.moc"

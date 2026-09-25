// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/services/chat/provider-scheduler.h"

#include <QtTest>

#include <memory>

class TestProviderScheduler : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<Chat::ProviderScheduler> m_sched;

    static void pumpEvents(int ms = 30) { QTest::qWait(ms); }

  private slots:
    void init() { m_sched = std::make_unique<Chat::ProviderScheduler>(); }
    void cleanup() { m_sched.reset(); }

    void test_differentProviders_runInParallel() {
        bool grantedA = m_sched->acquire(QStringLiteral("provA"), QStringLiteral("runA"), [] {});
        bool grantedB = m_sched->acquire(QStringLiteral("provB"), QStringLiteral("runB"), [] {});
        QVERIFY2(grantedA, "provider A did not grant immediately");
        QVERIFY2(grantedB, "provider B did not grant immediately");
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provA")), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provB")), 1);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provA")), 0);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provB")), 0);
        QVERIFY(m_sched->anyInFlight());

        m_sched->release(QStringLiteral("provA"));
        m_sched->release(QStringLiteral("provB"));
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provA")), 0);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provB")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_sameProvider_serializes_thenAutoDispatch() {
        bool first = m_sched->acquire(QStringLiteral("prov"), QStringLiteral("run1"), [] {});
        QVERIFY2(first, "first acquire on idle provider must grant");

        bool secondDispatched = false;
        bool second = m_sched->acquire(
            QStringLiteral("prov"), QStringLiteral("run2"), [&] { secondDispatched = true; });
        QVERIFY2(!second, "second acquire on busy provider must queue");
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("prov")), 1);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("prov")), 1);

        pumpEvents();
        QVERIFY2(!secondDispatched, "queued waiter dispatched before the slot was released");

        m_sched->release(QStringLiteral("prov"));
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("prov")), 1);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("prov")), 0);
        QVERIFY2(!secondDispatched, "grant must be deferred (not fired synchronously in release)");

        pumpEvents();
        QVERIFY2(secondDispatched, "queued waiter did not auto-dispatch after release");

        m_sched->release(QStringLiteral("prov"));
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("prov")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_release_dequeuesFifo() {
        QVERIFY(m_sched->acquire(QStringLiteral("p"), QStringLiteral("holder"), [] {}));

        QStringList order;
        m_sched->acquire(
            QStringLiteral("p"), QStringLiteral("w1"), [&] { order << QStringLiteral("w1"); });
        m_sched->acquire(
            QStringLiteral("p"), QStringLiteral("w2"), [&] { order << QStringLiteral("w2"); });
        m_sched->acquire(
            QStringLiteral("p"), QStringLiteral("w3"), [&] { order << QStringLiteral("w3"); });
        QCOMPARE(m_sched->queuedCount(QStringLiteral("p")), 3);

        m_sched->release(QStringLiteral("p"));
        pumpEvents();
        m_sched->release(QStringLiteral("p"));
        pumpEvents();
        m_sched->release(QStringLiteral("p"));
        pumpEvents();
        m_sched->release(QStringLiteral("p"));
        pumpEvents();

        QCOMPARE(order,
                 (QStringList{QStringLiteral("w1"), QStringLiteral("w2"), QStringLiteral("w3")}));
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("p")), 0);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("p")), 0);
    }

    void test_cancel_removesQueuedWaiter() {
        QVERIFY(m_sched->acquire(QStringLiteral("p"), QStringLiteral("holder"), [] {}));

        bool w1Fired = false, w2Fired = false;
        m_sched->acquire(QStringLiteral("p"), QStringLiteral("w1"), [&] { w1Fired = true; });
        m_sched->acquire(QStringLiteral("p"), QStringLiteral("w2"), [&] { w2Fired = true; });
        QCOMPARE(m_sched->queuedCount(QStringLiteral("p")), 2);

        QVERIFY2(m_sched->cancel(QStringLiteral("w1")),
                 "cancel did not report removing the queued waiter");
        QCOMPARE(m_sched->queuedCount(QStringLiteral("p")), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("p")), 1);

        QVERIFY(!m_sched->cancel(QStringLiteral("nope")));

        m_sched->release(QStringLiteral("p"));
        pumpEvents();
        QVERIFY2(!w1Fired, "cancelled waiter fired");
        QVERIFY2(w2Fired, "remaining waiter did not dispatch after cancel");

        m_sched->release(QStringLiteral("p"));
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("p")), 0);
    }

    void test_destroyedWhileQueued_handsSlotBack() {
        QVERIFY(m_sched->acquire(QStringLiteral("p"), QStringLiteral("holder"), [] {}));

        bool runAlive = true;
        m_sched->acquire(QStringLiteral("p"), QStringLiteral("dead"), [&] {
            if (!runAlive) {
                m_sched->release(QStringLiteral("p"));
                return;
            }
        });
        runAlive = false;

        m_sched->release(QStringLiteral("p"));
        pumpEvents();

        QCOMPARE(m_sched->inFlightCount(QStringLiteral("p")), 0);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("p")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_doubleRelease_neverUnderflows() {
        QVERIFY(m_sched->acquire(QStringLiteral("p"), QStringLiteral("r"), [] {}));
        m_sched->release(QStringLiteral("p"));
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("p")), 0);
        m_sched->release(QStringLiteral("p"));
        m_sched->release(QStringLiteral("never-acquired"));
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("p")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_duplicateEnqueue_rejected() {
        QVERIFY(m_sched->acquire(QStringLiteral("p"), QStringLiteral("holder"), [] {}));
        QVERIFY(!m_sched->acquire(QStringLiteral("p"), QStringLiteral("w"), [] {}));
        QVERIFY(!m_sched->acquire(QStringLiteral("p"), QStringLiteral("w"), [] {}));
        QCOMPARE(m_sched->queuedCount(QStringLiteral("p")), 1);
    }

    void test_setProviderLimit_raisesCapacity() {
        m_sched->setProviderLimit(QStringLiteral("p"), 2);
        QVERIFY(m_sched->acquire(QStringLiteral("p"), QStringLiteral("a"), [] {}));
        QVERIFY2(m_sched->acquire(QStringLiteral("p"), QStringLiteral("b"), [] {}),
                 "limit 2 should grant a 2nd concurrent slot");
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("p")), 2);
        bool cFired = false;
        QVERIFY(
            !m_sched->acquire(QStringLiteral("p"), QStringLiteral("c"), [&] { cFired = true; }));
        m_sched->setProviderLimit(QStringLiteral("p"), 3);
        pumpEvents();
        QVERIFY2(cFired, "raising the limit did not drain the queued waiter");
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("p")), 3);
    }
};

QTEST_MAIN(TestProviderScheduler)
#include "test-provider-scheduler.moc"

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/services/heartbeat-subagent-queue.h"

#include <QTest>

#include <QString>

class TestHeartbeatSubagentQueue : public QObject {
    Q_OBJECT

  private slots:
    void test_emptyQueue_popReturnsNullopt() {
        HeartbeatSubagentQueue q;
        QVERIFY(q.isEmpty());
        QVERIFY(!q.popNext().has_value());
    }

    void test_enqueueRequiresNonEmptyIds() {
        HeartbeatSubagentQueue q;
        HeartbeatQueueEntry e1;
        QCOMPARE(q.enqueue(e1), HeartbeatSubagentQueue::EnqueueResult::DroppedOverflow);
        QVERIFY(q.isEmpty());
    }

    void test_fifoOrdering() {
        HeartbeatSubagentQueue q;
        for (int i = 0; i < 5; ++i) {
            HeartbeatQueueEntry e;
            e.configId = QStringLiteral("cfg-%1").arg(i);
            e.runId = QStringLiteral("run-%1").arg(i);
            QCOMPARE(q.enqueue(e), HeartbeatSubagentQueue::EnqueueResult::Accepted);
        }
        QCOMPARE(q.size(), 5);

        for (int i = 0; i < 5; ++i) {
            const auto popped = q.popNext();
            QVERIFY(popped.has_value());
            QCOMPARE(popped->configId, QStringLiteral("cfg-%1").arg(i));
        }
        QVERIFY(q.isEmpty());
    }

    void test_perConfigThrottle() {
        HeartbeatSubagentQueue q;
        const QString cfg = QStringLiteral("cfg-A");

        HeartbeatQueueEntry e;
        e.configId = cfg;
        e.runId = QStringLiteral("run-1");
        QCOMPARE(q.enqueue(e), HeartbeatSubagentQueue::EnqueueResult::Accepted);

        e.runId = QStringLiteral("run-2");
        QCOMPARE(q.enqueue(e), HeartbeatSubagentQueue::EnqueueResult::DroppedThrottled);
        QCOMPARE(q.countQueuedForConfig(cfg), 1);

        const auto popped = q.popNext();
        QVERIFY(popped.has_value());
        QCOMPARE(popped->runId, QStringLiteral("run-1"));
        q.markInflight(cfg);

        e.runId = QStringLiteral("run-3");
        QCOMPARE(q.enqueue(e), HeartbeatSubagentQueue::EnqueueResult::Accepted);
        e.runId = QStringLiteral("run-4");
        QCOMPARE(q.enqueue(e), HeartbeatSubagentQueue::EnqueueResult::DroppedThrottled);
    }

    void test_softCapBackpressure() {
        HeartbeatSubagentQueue q;
        q.setSoftCap(3);

        for (int i = 0; i < 3; ++i) {
            HeartbeatQueueEntry e;
            e.configId = QStringLiteral("cfg-%1").arg(i);
            e.runId = QStringLiteral("run-%1").arg(i);
            QCOMPARE(q.enqueue(e), HeartbeatSubagentQueue::EnqueueResult::Accepted);
        }
        HeartbeatQueueEntry e;
        e.configId = QStringLiteral("cfg-3");
        e.runId = QStringLiteral("run-3");
        QCOMPARE(q.enqueue(e), HeartbeatSubagentQueue::EnqueueResult::DroppedOverflow);
        QCOMPARE(q.size(), 3);
    }

    void test_dropQueuedForConfig_removesAllForConfig() {
        HeartbeatSubagentQueue q;
        for (int i = 0; i < 5; ++i) {
            HeartbeatQueueEntry e;
            e.configId = (i % 2 == 0) ? QStringLiteral("cfg-A") : QStringLiteral("cfg-B");
            e.runId = QStringLiteral("run-%1").arg(i);
            q.enqueue(e);
        }
        const int aBefore = q.countQueuedForConfig(QStringLiteral("cfg-A"));
        const int dropped = q.dropQueuedForConfig(QStringLiteral("cfg-A"));
        QCOMPARE(dropped, aBefore);
        QCOMPARE(q.countQueuedForConfig(QStringLiteral("cfg-A")), 0);
    }

    void test_inflightTrackingFlags() {
        HeartbeatSubagentQueue q;
        QVERIFY(!q.isInflight(QStringLiteral("cfg-X")));
        q.markInflight(QStringLiteral("cfg-X"));
        QVERIFY(q.isInflight(QStringLiteral("cfg-X")));
        q.markNotInflight(QStringLiteral("cfg-X"));
        QVERIFY(!q.isInflight(QStringLiteral("cfg-X")));
    }
};

QTEST_MAIN(TestHeartbeatSubagentQueue)
#include "test-heartbeat-subagent-queue.moc"

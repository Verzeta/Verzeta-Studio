// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/history-budgeter.h"

#include <QTest>

#include <QDateTime>
#include <QElapsedTimer>
#include <QUuid>

class TestHistoryBudgeter : public QObject {
    Q_OBJECT

  private:
    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    static Message makeMsg(const QString& role,
                           const QString& content,
                           const QString& finishReason = {},
                           int seqMs = 0) {
        Message m;
        m.id = uuid();
        m.conversationId = QStringLiteral("conv-1");
        m.role = role;
        m.content = content;
        m.finishReason = finishReason;
        m.createdAt = QDateTime::fromMSecsSinceEpoch(1000 + seqMs);
        return m;
    }

  private slots:

    void test_emptyInput() {
        const auto r = HistoryBudgeter::selectForBudget({}, 1000);
        QVERIFY(r.ok);
        QCOMPARE(r.messages.size(), 0);
        QCOMPARE(r.totalDbRows, 0);
    }

    void test_smallHistoryFitsEntirely() {
        QList<Message> msgs;
        msgs.append(makeMsg(QStringLiteral("user"), QStringLiteral("hi"), {}, 1));
        msgs.append(makeMsg(QStringLiteral("assistant"), QStringLiteral("hello"), {}, 2));

        const auto r = HistoryBudgeter::selectForBudget(msgs, 10000);
        QVERIFY(r.ok);
        QCOMPARE(r.messages.size(), 2);
        QCOMPARE(r.includedRows, 2);
        QCOMPARE(r.droppedRows, 0);
    }

    void test_budgetDropsOldestKeepsNewest() {
        QList<Message> msgs;
        for (int i = 0; i < 20; ++i) {
            const QString role =
                (i % 2 == 0) ? QStringLiteral("user") : QStringLiteral("assistant");
            msgs.append(
                makeMsg(role,
                        QString(QStringLiteral("message number %1 with some content")).arg(i),
                        {},
                        i));
        }

        const auto r = HistoryBudgeter::selectForBudget(msgs, 100);
        QVERIFY(r.ok);
        QVERIFY(r.messages.size() > 0);
        QVERIFY(r.messages.size() < 20);

        QCOMPARE(r.messages.last().id, msgs.last().id);

        for (int i = 1; i < r.messages.size(); ++i) {
            QVERIFY(r.messages[i].createdAt >= r.messages[i - 1].createdAt);
        }
    }

    void test_newestUserMessageAlwaysIncluded() {
        QList<Message> msgs;
        msgs.append(makeMsg(QStringLiteral("user"), QStringLiteral("old question"), {}, 1));
        msgs.append(makeMsg(QStringLiteral("assistant"), QStringLiteral("old answer"), {}, 2));
        msgs.append(makeMsg(QStringLiteral("user"), QStringLiteral("new question"), {}, 3));

        const int tightBudget = HistoryBudgeter::estimateMessageTokens(msgs[2]) + 5;
        const auto r = HistoryBudgeter::selectForBudget(msgs, tightBudget);
        QVERIFY(r.ok);

        bool foundNewUser = false;
        for (const Message& m : r.messages) {
            if (m.content == QStringLiteral("new question")) {
                foundNewUser = true;
            }
        }
        QVERIFY2(foundNewUser, "newest user message must always be included");
    }

    void test_newestUserExceedsBudget_returnsFalse() {
        QList<Message> msgs;
        msgs.append(makeMsg(QStringLiteral("user"), QString(5000, QLatin1Char('x')), {}, 1));

        const auto r = HistoryBudgeter::selectForBudget(msgs, 10);
        QVERIFY(!r.ok);
        QCOMPARE(r.messages.size(), 0);
    }

    void test_toolCallGroupNeverSplit() {
        QList<Message> msgs;
        msgs.append(makeMsg(QStringLiteral("user"), QStringLiteral("write a file"), {}, 1));
        msgs.append(makeMsg(QStringLiteral("assistant"),
                            QStringLiteral("I'll do it:"),
                            QStringLiteral("tool_calls"),
                            2));
        msgs.append(makeMsg(QStringLiteral("tool"), QStringLiteral("{\"ok\":true}"), {}, 3));
        msgs.append(makeMsg(QStringLiteral("user"), QStringLiteral("thanks"), {}, 4));

        const int userTok = HistoryBudgeter::estimateMessageTokens(msgs[0]) +
                            HistoryBudgeter::estimateMessageTokens(msgs[3]);
        const auto r = HistoryBudgeter::selectForBudget(msgs, userTok + 5);
        QVERIFY(r.ok);

        bool hasToolWithoutAssistant = false;
        bool hasAssistant = false;
        for (const Message& m : r.messages) {
            if (m.role == QStringLiteral("tool"))
                hasToolWithoutAssistant = true;
            if (m.role == QStringLiteral("assistant"))
                hasAssistant = true;
        }
        if (hasToolWithoutAssistant && !hasAssistant) {
            QFAIL("tool row included without its parent assistant");
        }
    }

    void test_excludeMsgIdHonored() {
        QList<Message> msgs;
        msgs.append(makeMsg(QStringLiteral("user"), QStringLiteral("hi"), {}, 1));
        const Message streaming = makeMsg(QStringLiteral("assistant"), QStringLiteral(""), {}, 2);
        msgs.append(streaming);

        const auto r = HistoryBudgeter::selectForBudget(msgs, 10000, streaming.id);
        QVERIFY(r.ok);
        QCOMPARE(r.messages.size(), 1);
        QCOMPARE(r.messages[0].role, QStringLiteral("user"));
    }

    void test_outputIsAscendingOrder() {
        QList<Message> msgs;
        for (int i = 0; i < 50; ++i) {
            msgs.append(makeMsg((i % 2 == 0) ? QStringLiteral("user") : QStringLiteral("assistant"),
                                QStringLiteral("msg"),
                                {},
                                i));
        }

        const auto r = HistoryBudgeter::selectForBudget(msgs, 500);
        QVERIFY(r.ok);
        for (int i = 1; i < r.messages.size(); ++i) {
            QVERIFY(r.messages[i].createdAt >= r.messages[i - 1].createdAt);
        }
    }

    void test_estimateTokens() {
        QCOMPARE(HistoryBudgeter::estimateTokens(QString()), 0);
        QCOMPARE(HistoryBudgeter::estimateTokens(QStringLiteral("abc")), 1);
        QCOMPARE(HistoryBudgeter::estimateTokens(QStringLiteral("abcdef")), 2);
        QCOMPARE(HistoryBudgeter::estimateTokens(QStringLiteral("0123456789")), 4);
    }

    void test_estimateTokens_contentClass() {
        using CC = HistoryBudgeter::ContentClass;
        QCOMPARE(HistoryBudgeter::estimateTokens(QString(), CC::CodeOrJson), 0);
        QCOMPARE(HistoryBudgeter::estimateTokens(QStringLiteral("0123456789"), CC::Prose),
                 HistoryBudgeter::estimateTokens(QStringLiteral("0123456789")));
        const QString hundred = QString(100, QLatin1Char('x'));
        const int code = HistoryBudgeter::estimateTokens(hundred, CC::CodeOrJson);
        const int prose = HistoryBudgeter::estimateTokens(hundred, CC::Prose);
        QCOMPARE(code, 40);
        QCOMPARE(prose, 34);
        QVERIFY2(code >= prose, "CodeOrJson must never under-count relative to prose");
    }

    void test_zeroBudget() {
        QList<Message> msgs;
        msgs.append(makeMsg(QStringLiteral("user"), QStringLiteral("hi"), {}, 1));
        const auto r = HistoryBudgeter::selectForBudget(msgs, 0);
        QCOMPARE(r.messages.size(), 0);
    }

    void test_scalePerformance() {
        QList<Message> msgs;
        msgs.reserve(10000);
        for (int i = 0; i < 10000; ++i) {
            msgs.append(makeMsg((i % 2 == 0) ? QStringLiteral("user") : QStringLiteral("assistant"),
                                QStringLiteral("A message with some typical content here"),
                                {},
                                i));
        }

        QElapsedTimer timer;
        timer.start();
        const auto r = HistoryBudgeter::selectForBudget(msgs, 2000);
        const qint64 elapsed = timer.elapsed();

        QVERIFY(r.ok);
        QVERIFY(r.messages.size() > 0);
        QVERIFY(r.messages.size() < 10000);
        QVERIFY2(elapsed < 50,
                 qPrintable(QStringLiteral("took %1ms, expected <50ms").arg(elapsed)));
    }

    void test_noGapsInOutput() {
        QList<Message> msgs;
        for (int i = 0; i < 30; ++i) {
            msgs.append(makeMsg((i % 2 == 0) ? QStringLiteral("user") : QStringLiteral("assistant"),
                                QStringLiteral("content"),
                                {},
                                i));
        }

        const auto r = HistoryBudgeter::selectForBudget(msgs, 200);
        QVERIFY(r.ok);
        if (r.messages.size() < 30 && r.messages.size() > 0) {
            const QString lastIncludedId = r.messages.last().id;
            QCOMPARE(lastIncludedId, msgs.last().id);
        }
    }

    void test_minimumHistoryFloor_reclaimsFromReservation() {
        QCOMPARE(HistoryBudgeter::applyMinimumHistoryFloor(5000, 4096), 5000);
        QCOMPARE(
            HistoryBudgeter::applyMinimumHistoryFloor(HistoryBudgeter::kMinHistoryBudget, 4096),
            HistoryBudgeter::kMinHistoryBudget);
        QCOMPARE(HistoryBudgeter::applyMinimumHistoryFloor(189, 4096),
                 HistoryBudgeter::kMinHistoryBudget);
        QCOMPARE(HistoryBudgeter::applyMinimumHistoryFloor(-3000, 4096),
                 -3000 + (4096 - HistoryBudgeter::kMinOutputReservation));
        QCOMPARE(
            HistoryBudgeter::applyMinimumHistoryFloor(200, HistoryBudgeter::kMinOutputReservation),
            200);
        QCOMPARE(HistoryBudgeter::applyMinimumHistoryFloor(200, 1024), 200);
    }
};

QTEST_MAIN(TestHistoryBudgeter)
#include "test-history-budgeter.moc"

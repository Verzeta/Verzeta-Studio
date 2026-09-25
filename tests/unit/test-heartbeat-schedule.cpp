// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/utils/heartbeat-schedule.h"

#include <QTest>

#include <QDateTime>

class TestHeartbeatSchedule : public QObject {
    Q_OBJECT

  private slots:
    void test_emptyInput_isValidEmpty() {
        const auto s = parseHeartbeatSchedule(QString());
        QVERIFY(s.valid);
        QCOMPARE(s.kind, HeartbeatSchedule::Empty);
    }

    void test_hourly_parsesAndFiresOnNextTopOfHour() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@hourly"));
        QVERIFY(s.valid);
        QCOMPARE(s.kind, HeartbeatSchedule::Hourly);

        const QDateTime now(QDate(2026, 5, 2), QTime(14, 23, 17));
        const QDateTime next = nextFireTime(s, now);
        QCOMPARE(next, QDateTime(QDate(2026, 5, 2), QTime(15, 0)));
    }

    void test_daily_parsesAndFiresAtSpecifiedTime() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@daily at 09:00"));
        QVERIFY(s.valid);
        QCOMPARE(s.kind, HeartbeatSchedule::Daily);
        QCOMPARE(s.hour, 9);
        QCOMPARE(s.minute, 0);

        QDateTime now(QDate(2026, 5, 2), QTime(7, 0));
        QCOMPARE(nextFireTime(s, now), QDateTime(QDate(2026, 5, 2), QTime(9, 0)));

        now = QDateTime(QDate(2026, 5, 2), QTime(11, 0));
        QCOMPARE(nextFireTime(s, now), QDateTime(QDate(2026, 5, 3), QTime(9, 0)));
    }

    void test_daily_rejectsMissingTime() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@daily"));
        QVERIFY(!s.valid);
        QVERIFY(!s.errorMessage.isEmpty());
    }

    void test_daily_rejectsBadTime() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@daily at 25:00"));
        QVERIFY(!s.valid);
    }

    void test_weeklyBare_parsesToMondayMidnight() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@weekly"));
        QVERIFY(s.valid);
        QCOMPARE(s.kind, HeartbeatSchedule::Weekly);
        QCOMPARE(s.dayOfWeek, int(Qt::Monday));
        QCOMPARE(s.hour, 0);
        QCOMPARE(s.minute, 0);

        const QDateTime now(QDate(2026, 5, 6), QTime(12, 0));
        const QDateTime next = nextFireTime(s, now);
        QCOMPARE(next.date().dayOfWeek(), int(Qt::Monday));
        QVERIFY(next > now);
    }

    void test_weekly_onDayAtTime() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@weekly on FRI at 17:00"));
        QVERIFY(s.valid);
        QCOMPARE(s.dayOfWeek, int(Qt::Friday));
        QCOMPARE(s.hour, 17);
        QCOMPARE(s.minute, 0);
    }

    void test_weekly_rejectsBadDay() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@weekly on FOO at 17:00"));
        QVERIFY(!s.valid);
    }

    void test_interval_parsesAndFires() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@interval 15"));
        QVERIFY(s.valid);
        QCOMPARE(s.kind, HeartbeatSchedule::Interval);
        QCOMPARE(s.intervalMinutes, 15);

        const QDateTime now(QDate(2026, 5, 2), QTime(10, 0));
        QCOMPARE(nextFireTime(s, now), QDateTime(QDate(2026, 5, 2), QTime(10, 15)));
        const QDateTime anchor(QDate(2026, 5, 2), QTime(9, 50));
        QCOMPARE(nextFireTime(s, now, anchor), QDateTime(QDate(2026, 5, 2), QTime(10, 5)));
    }

    void test_interval_missedFireAmnesty() {
        const auto s = parseHeartbeatSchedule(QStringLiteral("@interval 5"));
        const QDateTime now(QDate(2026, 5, 2), QTime(10, 0));
        const QDateTime anchorOldYesterday(QDate(2026, 5, 1), QTime(9, 0));
        QCOMPARE(nextFireTime(s, now, anchorOldYesterday), now);
    }

    void test_interval_rejectsBelowFiveMinutes() {
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("@interval 4")).valid);
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("@interval 1")).valid);
        QVERIFY(parseHeartbeatSchedule(QStringLiteral("@interval 5")).valid);
    }

    void test_interval_rejectsNonInteger() {
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("@interval abc")).valid);
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("@interval")).valid);
    }

    void test_rejectsLegacyShorthand() {
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("5m")).valid);
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("1h")).valid);
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("24h")).valid);
    }

    void test_rejectsCron() {
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("0 9 * * MON")).valid);
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("*/5 * * * *")).valid);
    }

    void test_rejectsRandomGarbage() {
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("frobnicate")).valid);
        QVERIFY(!parseHeartbeatSchedule(QStringLiteral("@daily")).valid);
    }

    void test_emptySchedule_neverFires() {
        const auto s = parseHeartbeatSchedule(QString());
        QVERIFY(s.valid);
        const QDateTime next = nextFireTime(s, QDateTime(QDate(2026, 5, 2), QTime(10, 0)));
        QVERIFY(!next.isValid());
    }

    void test_firesIn24h_hourly() {
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QStringLiteral("@hourly"))), 24);
    }

    void test_firesIn24h_daily() {
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QStringLiteral("@daily at 09:00"))), 1);
    }

    void test_firesIn24h_weekly_clampedToOne() {
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QStringLiteral("@weekly"))), 1);
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QStringLiteral("@weekly on FRI at 17:00"))), 1);
    }

    void test_firesIn24h_interval5_yields288() {
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QStringLiteral("@interval 5"))), 288);
    }

    void test_firesIn24h_interval15_yields96() {
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QStringLiteral("@interval 15"))), 96);
    }

    void test_firesIn24h_interval60_yields24() {
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QStringLiteral("@interval 60"))), 24);
    }

    void test_firesIn24h_intervalLargerThan24h_floorsToZero() {
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QStringLiteral("@interval 1500"))), 0);
    }

    void test_firesIn24h_emptyAndInvalid_yieldZero() {
        QCOMPARE(firesIn24h(parseHeartbeatSchedule(QString())), 0);
        const HeartbeatSchedule invalid = parseHeartbeatSchedule(QStringLiteral("garbage"));
        QVERIFY(!invalid.valid);
        QCOMPARE(firesIn24h(invalid), 0);
    }
};

QTEST_MAIN(TestHeartbeatSchedule)
#include "test-heartbeat-schedule.moc"

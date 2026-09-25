// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-schedule.cpp
 * @brief Implementation of the heartbeat schedule parser and
 *        next-fire-time computation.  Pure function: no QObject,
 *        no global state, no Qt event loop.  Designed to be
 *        exercised by tests with deterministic injected clocks.
 * @layer Utility
 * @dependencies Qt6::Core
 */


#include "heartbeat-schedule.h"

#include <QTime>

#include <QRegularExpression>
#include <QStringList>

namespace {

/**
 * @brief Parse "HH:MM" 24-hour time string into a QTime.
 * @return Valid QTime on success; invalid on failure.
 *
 * Accepts strict zero-padded form ("09:05") to keep the parser
 * simple and the schedule string canonical. Loose forms like "9:5"
 * are NOT accepted, because round-tripping through a single canonical
 * representation makes diffs against config rows easier.
 */
QTime parseHHMM(const QString& s) {
    static const QRegularExpression rx(QStringLiteral("^([0-2][0-9]):([0-5][0-9])$"));
    const auto m = rx.match(s);
    if (!m.hasMatch()) {
        return QTime();
    }
    bool okH = false, okM = false;
    const int h = m.captured(1).toInt(&okH);
    const int mi = m.captured(2).toInt(&okM);
    if (!okH || !okM)
        return QTime();
    if (h >= 24)
        return QTime();  // 23:59 max
    return QTime(h, mi);
}

/**
 * @brief Convert MON / TUE / ... / SUN to Qt::DayOfWeek (1..7).
 * @return 0 on parse failure (caller flags error).
 */
int parseDayOfWeek(const QString& raw) {
    const QString s = raw.trimmed().toUpper();
    if (s == QStringLiteral("MON"))
        return Qt::Monday;
    if (s == QStringLiteral("TUE"))
        return Qt::Tuesday;
    if (s == QStringLiteral("WED"))
        return Qt::Wednesday;
    if (s == QStringLiteral("THU"))
        return Qt::Thursday;
    if (s == QStringLiteral("FRI"))
        return Qt::Friday;
    if (s == QStringLiteral("SAT"))
        return Qt::Saturday;
    if (s == QStringLiteral("SUN"))
        return Qt::Sunday;
    return 0;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// parseHeartbeatSchedule
// ---------------------------------------------------------------------------

HeartbeatSchedule parseHeartbeatSchedule(const QString& raw) {
    HeartbeatSchedule out;
    const QString s = raw.trimmed();

    // Empty input — valid, represents "manual fire only".
    if (s.isEmpty()) {
        out.kind = HeartbeatSchedule::Empty;
        out.valid = true;
        return out;
    }

    // -----------------------------------------------------------------
    // @interval N — N is a positive integer, in MINUTES, with the
    // 5-minute hard floor enforced here. Format: "@interval 15".
    // -----------------------------------------------------------------
    if (s.startsWith(QStringLiteral("@interval"), Qt::CaseInsensitive)) {
        // Strip the "@interval" token, then read the integer.
        const QString tail = s.mid(QStringLiteral("@interval").size()).trimmed();
        if (tail.isEmpty()) {
            out.errorMessage = QStringLiteral(
                "@interval requires an integer minute count (e.g. \"@interval 15\")");
            return out;
        }
        bool ok = false;
        const int n = tail.toInt(&ok);
        if (!ok) {
            out.errorMessage = QStringLiteral("@interval expected an integer (got '%1')").arg(tail);
            return out;
        }
        if (n < kHeartbeatMinIntervalMinutes) {
            out.errorMessage = QStringLiteral("interval must be at least %1 minutes")
                                   .arg(kHeartbeatMinIntervalMinutes);
            return out;
        }
        out.kind = HeartbeatSchedule::Interval;
        out.intervalMinutes = n;
        out.valid = true;
        return out;
    }

    // -----------------------------------------------------------------
    // @hourly — top of every hour.
    // -----------------------------------------------------------------
    if (s.compare(QStringLiteral("@hourly"), Qt::CaseInsensitive) == 0) {
        out.kind = HeartbeatSchedule::Hourly;
        out.valid = true;
        return out;
    }

    // -----------------------------------------------------------------
    // @daily at HH:MM — once a day at the specified local-time HH:MM.
    // -----------------------------------------------------------------
    {
        static const QRegularExpression rxDaily(QStringLiteral("^@daily\\s+at\\s+(\\S+)$"),
                                                QRegularExpression::CaseInsensitiveOption);
        const auto m = rxDaily.match(s);
        if (m.hasMatch()) {
            const QTime t = parseHHMM(m.captured(1));
            if (!t.isValid()) {
                out.errorMessage =
                    QStringLiteral("@daily at HH:MM — could not parse '%1' as 24-hour HH:MM")
                        .arg(m.captured(1));
                return out;
            }
            out.kind = HeartbeatSchedule::Daily;
            out.hour = t.hour();
            out.minute = t.minute();
            out.valid = true;
            return out;
        }
    }

    // -----------------------------------------------------------------
    // @weekly                          — Monday 00:00 (default)
    // @weekly on DAY at HH:MM          — DAY HH:MM
    // (also accept the partial forms "@weekly on DAY" → DAY 00:00 for
    //  ergonomics, though the canonical doc uses both clauses).
    // -----------------------------------------------------------------
    if (s.compare(QStringLiteral("@weekly"), Qt::CaseInsensitive) == 0) {
        out.kind = HeartbeatSchedule::Weekly;
        out.dayOfWeek = Qt::Monday;
        out.hour = 0;
        out.minute = 0;
        out.valid = true;
        return out;
    }
    {
        static const QRegularExpression rxWeeklyFull(
            QStringLiteral("^@weekly\\s+on\\s+(\\S+)\\s+at\\s+(\\S+)$"),
            QRegularExpression::CaseInsensitiveOption);
        const auto m = rxWeeklyFull.match(s);
        if (m.hasMatch()) {
            const int dow = parseDayOfWeek(m.captured(1));
            if (dow == 0) {
                out.errorMessage = QStringLiteral("@weekly on DAY — '%1' is not a valid day "
                                                  "(MON/TUE/WED/THU/FRI/SAT/SUN)")
                                       .arg(m.captured(1));
                return out;
            }
            const QTime t = parseHHMM(m.captured(2));
            if (!t.isValid()) {
                out.errorMessage =
                    QStringLiteral("@weekly ... at HH:MM — could not parse '%1' as 24-hour HH:MM")
                        .arg(m.captured(2));
                return out;
            }
            out.kind = HeartbeatSchedule::Weekly;
            out.dayOfWeek = dow;
            out.hour = t.hour();
            out.minute = t.minute();
            out.valid = true;
            return out;
        }
    }
    {
        static const QRegularExpression rxWeeklyDayOnly(QStringLiteral("^@weekly\\s+on\\s+(\\S+)$"),
                                                        QRegularExpression::CaseInsensitiveOption);
        const auto m = rxWeeklyDayOnly.match(s);
        if (m.hasMatch()) {
            const int dow = parseDayOfWeek(m.captured(1));
            if (dow == 0) {
                out.errorMessage = QStringLiteral("@weekly on DAY — '%1' is not a valid day "
                                                  "(MON/TUE/WED/THU/FRI/SAT/SUN)")
                                       .arg(m.captured(1));
                return out;
            }
            out.kind = HeartbeatSchedule::Weekly;
            out.dayOfWeek = dow;
            out.hour = 0;
            out.minute = 0;
            out.valid = true;
            return out;
        }
    }

    // -----------------------------------------------------------------
    // Anything else — explicitly reject the plain interval shorthand
    // ("5m" / "1h" / "24h") so users get a clear error pointing at
    // the named formats. Also catches cron expressions.
    // -----------------------------------------------------------------
    out.errorMessage = QStringLiteral("unrecognised schedule '%1' — must be one of: "
                                      "@hourly | @daily at HH:MM | @weekly[ on DAY at HH:MM] | "
                                      "@interval N (N >= %2 minutes)")
                           .arg(raw)
                           .arg(kHeartbeatMinIntervalMinutes);
    return out;
}

// ---------------------------------------------------------------------------
// nextFireTime
// ---------------------------------------------------------------------------

QDateTime
nextFireTime(const HeartbeatSchedule& schedule, const QDateTime& now, const QDateTime& lastFireAt) {
    if (!schedule.valid || schedule.kind == HeartbeatSchedule::Empty) {
        return QDateTime();
    }
    if (!now.isValid()) {
        return QDateTime();
    }

    switch (schedule.kind) {
        case HeartbeatSchedule::Empty:
            return QDateTime();

        case HeartbeatSchedule::Interval: {
            if (lastFireAt.isValid()) {
                QDateTime t = lastFireAt.addSecs(qint64(schedule.intervalMinutes) * 60);
                if (t < now) {
                    return now;
                }
                return t;
            }
            return now.addSecs(qint64(schedule.intervalMinutes) * 60);
        }

        case HeartbeatSchedule::Hourly: {
            // Next "xx:00" strictly after `now`. If `now` is exactly on the
            // hour, fire one hour later (we don't fire AT now — too fragile
            // for tests that compare strictly).
            QDateTime t = now;
            t.setTime(QTime(t.time().hour(), 0));  // truncate to top of hour
            t = t.addSecs(60 * 60);                // strictly next hour
            return t;
        }

        case HeartbeatSchedule::Daily: {
            // Next HH:MM strictly after `now`. If today's HH:MM is still
            // ahead, fire today; otherwise fire tomorrow.
            QDateTime today = now;
            today.setTime(QTime(schedule.hour, schedule.minute));
            if (today > now) {
                return today;
            }
            return today.addDays(1);
        }

        case HeartbeatSchedule::Weekly: {
            QDateTime t = now;
            t.setTime(QTime(schedule.hour, schedule.minute));

            const int currentDow = t.date().dayOfWeek();
            int delta = schedule.dayOfWeek - currentDow;
            if (delta < 0) {
                delta += 7;
            }
            if (delta == 0 && t <= now) {
                // Same day-of-week as today, but the time has already
                // passed — fire next week.
                delta = 7;
            }
            return t.addDays(delta);
        }
    }
    return QDateTime();  // unreachable
}

// ---------------------------------------------------------------------------
// firesIn24h
// ---------------------------------------------------------------------------

int firesIn24h(const HeartbeatSchedule& schedule) {
    if (!schedule.valid) {
        return 0;
    }
    switch (schedule.kind) {
        case HeartbeatSchedule::Empty:
            return 0;
        case HeartbeatSchedule::Hourly:
            return 24;
        case HeartbeatSchedule::Daily:
            return 1;
        case HeartbeatSchedule::Weekly:
            // A weekly schedule fires once per 7 days. The daily cap
            // formula sums these into a per-conversation daily cap; we
            // round up to 1 (rather than 0) so the cap allows the
            // scheduled weekly post to actually go through. Truncating
            // to zero would silently rate-limit every weekly fire.
            return 1;
        case HeartbeatSchedule::Interval: {
            if (schedule.intervalMinutes <= 0)
                return 0;
            return (24 * 60) / schedule.intervalMinutes;
        }
    }
    return 0;  // unreachable
}

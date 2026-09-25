// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-schedule.h
 * @brief Heartbeat schedule parser.  Four named formats only (no
 *        cron, no unfettered shorthand):
 *
 *          \@hourly                        fires at xx:00 every hour
 *          \@daily at HH:MM                fires once a day at the time
 *          \@weekly                        fires Monday 00:00
 *          \@weekly on DAY at HH:MM        fires DAY HH:MM (DAY ∈
 *                                         MON/TUE/WED/THU/FRI/SAT/SUN)
 *          \@interval N                    fires every N minutes, N ≥ 5
 *
 *        Anything else (cron, "5m"/"1h" shorthand, "\@interval 4",
 *        malformed) is rejected at parse time.
 *
 *        Pure free function with injected clock. It accepts a "now"
 *        QDateTime so tests can drive deterministic schedules without
 *        sleeping. Returns the next QDateTime the schedule would fire
 *        AFTER (not at) `now`, taking the optional `lastFireAt` anchor
 *        into account so an interval schedule fires at the right offset
 *        from its previous run.
 *
 * @layer Utility (pure function)
 * @dependencies Qt6::Core
 */


#pragma once

#include <QDateTime>
#include <QString>

/**
 * @brief Result of parsing a schedule string. On parse failure, valid
 *        is false and errorMessage carries a user-displayable string;
 *        on success, nextFireTime() interprets the parsed form.
 *
 * The struct is opaque-ish: callers use parseHeartbeatSchedule()
 * to obtain one and nextFireTime() to compute the next firing instant.
 */
struct HeartbeatSchedule {
    /** @brief Schedule form, one per supported syntax. */
    enum Kind {
        Empty,     ///< "": no schedule. nextFireTime always returns an invalid QDateTime.
        Interval,  ///< \@interval N: fires every N minutes from lastFireAt (or now if no anchor).
        Hourly,    ///< \@hourly: fires at the top of every hour.
        Daily,     ///< \@daily at HH:MM: fires once a day at the local-time HH:MM.
        Weekly,    ///< \@weekly[ on DAY] [at HH:MM]: fires once a week. Defaults: MON 00:00.
    };

    Kind kind = Empty;     ///< Parsed schedule form.
    bool valid = false;    ///< True when the string parsed.
    QString errorMessage;  ///< User-readable reason when valid is false.

    /// Interval kind: minutes between runs; at least
    /// kHeartbeatMinIntervalMinutes.
    int intervalMinutes = 0;

    int hour = 0;    ///< Daily and Weekly: local hour, 0 to 23.
    int minute = 0;  ///< Daily and Weekly: local minute, 0 to 59.

    /// Weekly kind: day of week, Qt::Monday (1) to Qt::Sunday (7).
    int dayOfWeek = 1;
};

/**
 * @brief Hard floor: minimum minutes for an `\@interval N` schedule.
 *        Five minutes keeps queue backpressure manageable under
 *        realistic multi-agent configurations.
 */
inline constexpr int kHeartbeatMinIntervalMinutes = 5;

/**
 * @brief Parses a heartbeat schedule string.
 * @param raw The schedule string from heartbeat_configs.schedule.
 * @return HeartbeatSchedule with valid=true on success, or valid=false
 *         + errorMessage describing why it could not parse.
 *
 * Empty input is valid (kind=Empty) and represents "manual fire only".
 * The parser is total: never throws, never aborts; on any unrecognised
 * input it returns valid=false with an error.
 *
 * @complexity O(N) in the input length. Pure function: no Qt event
 *             loop, no QObject, no global state.
 */
HeartbeatSchedule parseHeartbeatSchedule(const QString& raw);

/**
 * @brief Computes the next firing instant after `now`, given an
 *        optional `lastFireAt` anchor.
 * @param schedule Parsed schedule (valid==true).
 * @param now      Current local time (caller supplies it, typically
 *                 QDateTime::currentDateTime() in production, or a
 *                 test-injected fake clock).
 * @param lastFireAt Optional previous-fire timestamp. Empty / invalid
 *                 means "never fired yet"; the schedule fires from
 *                 `now`.
 * @return The next QDateTime the schedule would fire (strictly AFTER
 *         `now` for aligned schedules, or AT `lastFireAt + interval`
 *         for interval schedules, clamped to `now` if that timestamp
 *         is in the past, so missed fires are not replayed).
 *         Returns an invalid QDateTime when schedule.kind == Empty.
 *
 * @complexity O(1) (or O(7) for weekly day-of-week wrap).
 */
QDateTime nextFireTime(const HeartbeatSchedule& schedule,
                       const QDateTime& now,
                       const QDateTime& lastFireAt = {});

/**
 * @brief Counts how many times this schedule would
 *        fire in any 24-hour window. Pure function; ignores
 *        last_fire_at and time-of-day alignment because the answer
 *        is window-independent for these formats:
 *
 *          \@hourly         → 24
 *          \@daily          → 1
 *          \@weekly         → 0 (a 24h window catches one weekly fire
 *                              only if you start in the right hour;
 *                              the practical "uniform per-day"
 *                              accounting treats this
 *                              as 1 ÷ 7 ≈ 0, rounded up to 1 to keep
 *                              the cap viable)
 *          \@interval N     → 1440 / N (rounded down)
 *          Empty           → 0
 *
 *        Used to derive the intelligent cap suggestion for
 *        the per-conversation auto-surface daily limit. The
 *        suggested cap = sum over enabled-HB configs of
 *        min(firesIn24h, max_runs_per_day), then capped at 24.
 *
 * @param schedule Parsed schedule (valid==true).
 * @return Estimated fire count per 24h. 0 for invalid / empty.
 *
 * @complexity O(1).
 */
int firesIn24h(const HeartbeatSchedule& schedule);

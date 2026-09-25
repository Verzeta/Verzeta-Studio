// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file heartbeat-report.h
 * @brief POD for one row in the `heartbeat_reports` table.
 *
 *        Each row records a single Tier-1 subagent invocation: the
 *        proactive run that produces the structured TITLE / RESULTS /
 *        SUMMARY report consumed by the Tier-2 reviewer.
 * @layer Data Model
 * @dependencies Qt6::Core, Qt6::Sql.
 */


#pragma once

#include <QDateTime>
#include <QString>

class QSqlRecord;

/**
 * @brief Outcome enum string constants for heartbeat_reports.outcome.
 *        Mirrors HeartbeatFireOutcome:: in heartbeat-config.h; the
 *        report row records the same outcome that updates the config's
 *        last_fire_outcome anchor, so the two stay in sync.
 */
namespace HeartbeatReportOutcome {
/// Written when the run is recorded at dispatch; replaced when it ends.
inline const QString kPending = QStringLiteral("pending");
/// The run finished and its title, body and summary were stored.
inline const QString kSuccess = QStringLiteral("success");
/// The run failed, for example because its config was deleted before it
/// started. The reason is in HeartbeatReport::error.
inline const QString kError = QStringLiteral("error");
/// The run exceeded its wall-clock timeout and its request was cancelled.
inline const QString kTimeout = QStringLiteral("timeout");
/// The config had already run maxRunsPerDay times in the past 24
/// hours, so this run was not dispatched.
inline const QString kRateLimited = QStringLiteral("rate_limited");
/// The dispatch queue dropped the run because it was throttled or full.
inline const QString kQueueOverflow = QStringLiteral("queue_overflow");
/// The run was cancelled, either while still queued or in flight (for
/// example at shutdown).
inline const QString kCancelled = QStringLiteral("cancelled");
/// Reserved. Not currently set by any code path.
inline const QString kSkippedBusy = QStringLiteral("skipped_busy");
}  // namespace HeartbeatReportOutcome

/**
 * @brief parent_review_status enum string constants.
 *
 * Orthogonal to surface_status: every report is tracked on both axes
 * independently.
 */
namespace HeartbeatReviewStatus {
/// Tier 2 has not run yet. Transient.
inline const QString kPending = QStringLiteral("pending");
/// Tier 2 ran and decided to post or to skip.
inline const QString kReviewed = QStringLiteral("reviewed");
/// A pre-Tier-2 condition blocked review, such as the gate being off or
/// the target conversation missing. The manual override in the overlay
/// is still available.
inline const QString kNotReviewableYet = QStringLiteral("not_reviewable_yet");
}  // namespace HeartbeatReviewStatus

/**
 * @brief surface_status enum string constants.
 *
 * The manual-override path transitions skipped or pending rows to
 * posted_manual or dismissed_by_user.
 */
namespace HeartbeatSurfaceStatus {
/// No surface decision yet.
inline const QString kPending = QStringLiteral("pending");
/// Tier-2 review surfaced this report.
inline const QString kPostedAuto = QStringLiteral("posted_auto");
/// The user chose "Post anyway" in the overlay.
inline const QString kPostedManual = QStringLiteral("posted_manual");
/// Tier 2 ran and the agent returned [SKIP].
inline const QString kSkippedByAgent = QStringLiteral("skipped_by_agent");
/// The conversation's allow_heartbeat_auto_surface gate is off.
inline const QString kSkippedByGate = QStringLiteral("skipped_by_gate");
/// The conversation's daily auto-surface cap was reached.
inline const QString kSkippedByRateLimit = QStringLiteral("skipped_by_rate_limit");
/// The user chose "Dismiss" in the overlay.
inline const QString kDismissedByUser = QStringLiteral("dismissed_by_user");
}  // namespace HeartbeatSurfaceStatus

/**
 * @brief Immutable snapshot of one heartbeat_reports row.
 */
struct HeartbeatReport {
    QString id;        ///< UUID v4 primary key (also the runId).
    QString configId;  ///< References heartbeat_configs.id (CASCADE on delete).

    QDateTime startedAt;    ///< When the run started or was recorded.
    QDateTime completedAt;  ///< Default-constructed (invalid) until the run finishes.

    QString outcome;  ///< See HeartbeatReportOutcome:: constants.

    /// Parsed from the subagent's structured output (TITLE: line).
    QString title;
    /// Parsed from the subagent's structured output (RESULTS: section).
    QString body;
    /// Parsed from the subagent's structured output (SUMMARY: section).
    /// Tier-2 review uses this as the basis for a 1-3 sentence chat post.
    QString summary;

    /// Tier-2 review state; see HeartbeatReviewStatus.
    QString parentReviewStatus = HeartbeatReviewStatus::kPending;
    /// Whether and how the report reached the chat; see
    /// HeartbeatSurfaceStatus.
    QString surfaceStatus = HeartbeatSurfaceStatus::kPending;

    /// If the report was surfaced (auto or manual), the resulting
    /// messages.id. Empty when not posted.
    QString surfacedMessageId;

    /// Populated when outcome == "error" or "timeout".
    QString error;

    /**
     * @brief Reports whether the row is well-formed enough to act on.
     * @returns True iff `id` and `configId` are both non-empty.
     */
    bool isValid() const { return !id.isEmpty() && !configId.isEmpty(); }

    /**
     * @brief Constructs a HeartbeatReport from a SELECT * row.
     * @param record  Row returned by a query against `heartbeat_reports`.
     * @returns Fully-populated report. Fields the row left NULL come
     *          back default-constructed.
     */
    static HeartbeatReport fromSqlRecord(const QSqlRecord& record);
};

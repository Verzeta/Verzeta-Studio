// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file heartbeat-config.h
 * @brief POD for one row in the `heartbeat_configs` table.
 *
 *        A heartbeat configuration is owned by an (agent, scope, alias)
 *        tuple, NOT by an agent template alone. Multiple aliases of the
 *        same agent in the same group chat get distinct config rows
 *        (e.g. `@ResearcherNorthAmerica` vs `@ResearcherTikTok`).
 * @layer Data Model
 * @dependencies Qt6::Core, Qt6::Sql.
 */


#pragma once

#include <QDateTime>
#include <QString>

class QSqlRecord;

/**
 * @brief Scope-type discriminator for heartbeat_configs.
 *
 * Stored in the schema as the literal strings 'conversation_1to1',
 * 'conversation_group', 'folder' (see db-manager.cpp applySchemaV10).
 * Folder covers ANY folder-like scope (folder_type ∈ {'regular',
 * 'project', 'organization'}); UI presentation distinguishes them via
 * folders.folder_type, but the heartbeat-side scope_type collapses
 * all three under 'folder'.
 */
enum class HeartbeatScopeType {
    Conversation1to1,   ///< 1:1 chat. scope_id = conversations.id. alias = "".
    ConversationGroup,  ///< Group chat. scope_id = conversations.id. alias = membership alias.
    Folder,  ///< Folder/project/organization. scope_id = folders.id. alias = project_members.alias.
};

/**
 * @brief Convert scope-type enum to its schema string.
 * @param t Scope type.
 * @return One of the literal CHECK-constraint strings.
 */
QString heartbeatScopeTypeToString(HeartbeatScopeType t);

/**
 * @brief Convert schema string back to scope-type enum.
 * @param s Schema string.
 * @return Matching enum, or HeartbeatScopeType::Conversation1to1 on
 *         unknown input (caller should not rely on this; the schema CHECK
 *         constraint should make malformed strings impossible).
 */
HeartbeatScopeType heartbeatScopeTypeFromString(const QString& s);

/**
 * @brief Outcome string of the most recent fire attempt for a config.
 *        Mirrors the heartbeat_reports.outcome enum so the scheduler
 *        anchor (last_fire_outcome) records why a fire was not a
 *        successful dispatch even when no full report row was persisted.
 *
 * Stored as literal strings:
 *   "success" | "error" | "timeout" | "rate_limited" |
 *   "queue_overflow" | "cancelled" | "skipped_busy"
 * The constants below are exposed to avoid stringly-typed code in the
 * service layer. Schema column has no CHECK constraint so unknown
 * strings are tolerated (forward-compatible).
 */
namespace HeartbeatFireOutcome {
/// The last fire ran and finished with a stored report.
inline const QString kSuccess = QStringLiteral("success");
/// The last fire failed.
inline const QString kError = QStringLiteral("error");
/// The last fire exceeded its wall-clock timeout.
inline const QString kTimeout = QStringLiteral("timeout");
/// The last fire was refused because maxRunsPerDay was already reached
/// for the past 24 hours.
inline const QString kRateLimited = QStringLiteral("rate_limited");
/// The dispatch queue dropped the last fire because it was throttled or
/// full.
inline const QString kQueueOverflow = QStringLiteral("queue_overflow");
/// The last fire was cancelled, either while queued or in flight.
inline const QString kCancelled = QStringLiteral("cancelled");
/// Reserved. Not currently set by any code path.
inline const QString kSkippedBusy = QStringLiteral("skipped_busy");
}  // namespace HeartbeatFireOutcome

/**
 * @brief Immutable snapshot of one heartbeat_configs row.
 */
struct HeartbeatConfig {
    QString id;       ///< UUID v4 primary key.
    QString agentId;  ///< References agents.id (CASCADE on delete).
    /// Which kind of scope scopeId refers to.
    HeartbeatScopeType scopeType = HeartbeatScopeType::Conversation1to1;
    QString scopeId;  ///< Polymorphic; see scopeType.
    QString alias;    ///< "" for 1:1; non-empty for group/folder.

    bool enabled = false;                     ///< Master switch.
    QString schedule;                         ///< One of the 4 named formats.
    QString goal;                             ///< Standing instruction (the routine's WHAT).
    QString surfaceCriteria;                  ///< When to share (empty falls back to goal).
    int maxRunsPerDay = 24;                   ///< Cost-cap.
    QString autoSurfaceTargetConversationId;  ///< Folder-scope only; "" = overlay-only.
    bool selfConfigAllowed = false;           ///< Tier-B grant.

    QString lastFireAt;       ///< Qt::ISODateWithMs string (or empty if never fired).
    QString lastFireOutcome;  ///< See HeartbeatFireOutcome:: constants.

    QDateTime createdAt;  ///< When the row was created.
    QDateTime updatedAt;  ///< Last write to the row, including each fire.

    /**
     * @brief Reports whether the row is well-formed enough to act on.
     * @returns True iff `id`, `agentId`, and `scopeId` are all non-empty.
     */
    bool isValid() const { return !id.isEmpty() && !agentId.isEmpty() && !scopeId.isEmpty(); }

    /**
     * @brief Constructs a HeartbeatConfig from a SELECT * row.
     * @param record Record from a query against heartbeat_configs.
     * @return Populated struct.
     */
    static HeartbeatConfig fromSqlRecord(const QSqlRecord& record);
};

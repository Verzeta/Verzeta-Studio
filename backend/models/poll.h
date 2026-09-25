// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file poll.h
 * @brief POD types backing the polls subsystem.
 *
 *        Three rows-as-structs that mirror the polls schema. PollService
 *        is the only owner; consumers see read-only copies plus
 *        QVariantMap projections for QML / wire surfaces.
 * @layer Data Access (POD only)
 * @dependencies Qt6::Core.
 */


#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

/**
 * @brief One row of the `polls` table.
 */
struct Poll {
    QString id;              ///< UUID primary key.
    QString conversationId;  ///< Conversation the poll belongs to.
    QString creatorKind;     ///< "agent" | "user"
    QString creatorAlias;    ///< alias on the cascade for agents; "user" for the human
    QString question;        ///< The question being voted on.
    QString mode;            ///< "single" | "multi" (v1) [reserved: "ranked"]
    QString status;          ///< "open" | "closed"
    QDateTime createdAt;     ///< When the poll was created.
    QDateTime closesAt;      ///< null when no auto-close deadline
    QDateTime closedAt;      ///< set on close, else null

    /**
     * @brief Reports whether the row carries a non-empty primary key.
     * @returns True iff `id` is non-empty.
     */
    bool isValid() const { return !id.isEmpty(); }

    /**
     * @brief Returns whether the poll currently accepts votes.
     * @returns True iff `status` is "open".
     */
    bool isOpen() const { return status == QStringLiteral("open"); }
};

/**
 * @brief One row of the `poll_options` table.
 */
struct PollOption {
    QString id;        ///< UUID primary key.
    QString pollId;    ///< Poll the option belongs to.
    int ordering = 0;  ///< 0-based row order within the poll
    QString text;      ///< Option label.
};

/**
 * @brief One row of the `poll_votes` table.
 */
struct PollVote {
    QString id;          ///< UUID primary key.
    QString pollId;      ///< Poll the vote was cast in.
    QString voterKind;   ///< "agent" | "user"
    QString voterAlias;  ///< cascade alias for agents; "user" for the human
    QString optionId;    ///< Option voted for.
    int rank = -1;       ///< -1 when non-ranked; reserved
    QDateTime castAt;    ///< When the vote was cast.
};

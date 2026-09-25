// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file subagent-run.h
 * @brief Data model for an agent-spawned sub-agent run.
 *        Maps 1:1 to the `subagent_runs` table (schema v21).
 * @layer Data Access
 * @dependencies Qt6::Core
 */

#pragma once

#include <QString>
#include <QStringList>

/**
 * @brief One sub-agent run: a scoped task an agent delegated to a
 *        private worker whose transcript lives OUTSIDE the parent
 *        conversation (run-scoped `subagent_messages`).
 *
 * Lifecycle: queued → running → done | failed | cancelled.
 */
struct SubagentRun {
    QString id;                  ///< UUID v4 primary key
    QString conversationId;      ///< Parent conversation
    QString parentMsgId;         ///< Assistant message that spawned the run
    QString requesterAlias;      ///< Parent agent's alias
    QString task;                ///< The delegated task text
    QStringList toolsWhitelist;  ///< Tool names the sub-agent may call
    QString providerId;          ///< Resolved provider for the run
    QString modelName;           ///< Resolved model for the run
    QString status =
        QStringLiteral("queued");  ///< "queued", "running", "done", "failed" or "cancelled".
    QString resultText;            ///< Final report (terminal reply)
    QString failReason;            ///< Cause when status == failed
    int totalTokens = 0;           ///< Stored column; not currently written, so always 0.
    qint64 createdAtMs = 0;        ///< Creation time, ms since epoch.
    qint64 startedAtMs = 0;        ///< 0 until dispatch
    qint64 finishedAtMs = 0;       ///< 0 until terminal

    /**
     * @brief Whether the run reached a final state.
     * @returns true for done / failed / cancelled.
     */
    bool terminal() const {
        return status == QStringLiteral("done") || status == QStringLiteral("failed") ||
               status == QStringLiteral("cancelled");
    }
};

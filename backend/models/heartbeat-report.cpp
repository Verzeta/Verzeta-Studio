// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file heartbeat-report.cpp
 * @brief Implementation of HeartbeatReport's SQL-record deserialiser.
 * @layer Data Model
 * @dependencies Qt6::Sql.
 */


#include "heartbeat-report.h"

#include <QSqlRecord>
#include <QVariant>

HeartbeatReport HeartbeatReport::fromSqlRecord(const QSqlRecord& record) {
    HeartbeatReport r;
    r.id = record.value(QStringLiteral("id")).toString();
    r.configId = record.value(QStringLiteral("config_id")).toString();

    {
        const QString s = record.value(QStringLiteral("started_at")).toString();
        if (!s.isEmpty()) {
            r.startedAt = QDateTime::fromString(s, Qt::ISODateWithMs);
        }
    }
    {
        const QString s = record.value(QStringLiteral("completed_at")).toString();
        if (!s.isEmpty()) {
            r.completedAt = QDateTime::fromString(s, Qt::ISODateWithMs);
        }
    }

    r.outcome = record.value(QStringLiteral("outcome")).toString();
    r.title = record.value(QStringLiteral("title")).toString();
    r.body = record.value(QStringLiteral("body")).toString();
    r.summary = record.value(QStringLiteral("summary")).toString();
    r.parentReviewStatus = record.value(QStringLiteral("parent_review_status")).toString();
    r.surfaceStatus = record.value(QStringLiteral("surface_status")).toString();
    r.surfacedMessageId = record.value(QStringLiteral("surfaced_message_id")).toString();
    r.error = record.value(QStringLiteral("error")).toString();
    return r;
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file job-context.cpp
 * @brief JobContext factory + meta-type registration.
 * @layer Data Access (POD model)
 * @dependencies Qt6::Core (QUuid for jobId generation, QDateTime for
 *               createdAt stamping).
 */


#include "job-context.h"

#include <QUuid>

JobContext JobContext::makeNew(const QString& convId,
                               const QString& prompt,
                               const QString& alias,
                               const QString& agentId) {
    JobContext ctx;
    ctx.jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ctx.conversationId = convId;
    ctx.requestingAlias = alias;
    ctx.requestingAgentId = agentId;
    ctx.prompt = prompt;
    ctx.createdAt = QDateTime::currentDateTimeUtc();
    return ctx;
}

// Static initializer that registers JobContext as a Qt meta-type at
// program startup. Required for `emit signal(JobContext)` to marshal
// the value across a Qt::QueuedConnection (image-service.cpp connects
// the worker on a different thread). The integer return value is
// captured to give the global a definition lifetime — it is not
// referenced afterwards.
namespace {
const int kJobContextMetaTypeId = qRegisterMetaType<JobContext>("JobContext");
}

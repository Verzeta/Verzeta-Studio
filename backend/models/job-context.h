// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file job-context.h
 * @brief Immutable per-job attribution context for async backend
 *        outputs. Stamped at enqueue time and echoed back unchanged on
 *        worker completion. Attribution NEVER comes from a mutable
 *        service-side "active conversation" field.
 * @layer Data Access (POD model)
 * @dependencies Qt6::Core only (QString, QDateTime, QUuid, QMetaType).
 */


#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>

/**
 * @brief Immutable attribution context for one async background job.
 *
 * Every field is final after construction; copy is the only mutation
 * pattern. The service builds one at enqueue, hands it to the worker on
 * the request signal, and the worker echoes it back unchanged on the
 * result signal. The service's completion slot reads attribution
 * (conversation id, prompt text) from the echoed context, never from a
 * mutable member field.
 */
struct JobContext {
    /// Per-job correlation key. Tells apart two jobs with the same
    /// conversation and prompt; used by the audit log.
    QString jobId;
    /// Conversation the result belongs to. It stays fixed from enqueue
    /// to completion, so a result is never attributed to whichever
    /// conversation is active when the job finishes.
    QString conversationId;
    /// Group member alias that asked for the job, kept even if the
    /// cascade has moved on. Empty for the Generate Image dialog and
    /// 1:1 chats.
    QString requestingAlias;
    /// Agent template id of the requester; empty when the caller is not
    /// an agent. The audit log uses it for the actor fields.
    QString requestingAgentId;
    /// Verbatim prompt (image description or speech text), used to
    /// compose the saved message on completion.
    QString prompt;
    QDateTime createdAt;  ///< UTC creation time, for ordering and audit.

    /**
     * @brief True when the context names a job.
     * @returns True iff both `jobId` and `conversationId` are non-empty.
     *          Empty / default-constructed instances are falsy.
     *
     * Returning a zero-init context to the service slot is structurally
     * impossible under the worker-echo contract, but the guard documents
     * the contract and lets defensive code short-circuit cleanly.
     */
    bool isValid() const { return !jobId.isEmpty() && !conversationId.isEmpty(); }

    /**
     * @brief Factory: mints a fresh job id + stamps now(). The alias and
     *        agentId default to empty for callers that don't know them
     *        (e.g. QML's `generateImageFromMap` invoked from the
     *        Generate-Image dialog).
     * @param convId  Conversation the job's output belongs to.
     * @param prompt  Prompt the job was started with.
     * @param alias   Alias of the requesting member; empty when unknown.
     * @param agentId Id of the requesting agent; empty when unknown.
     * @returns A context with a new UUID job id and createdAt set to the
     *          current UTC time.
     */
    static JobContext makeNew(const QString& convId,
                              const QString& prompt,
                              const QString& alias = QString(),
                              const QString& agentId = QString());
};

// Register so queued connections can marshal JobContext across thread
// boundaries (ImageService/AudioService → worker thread → service slot).
Q_DECLARE_METATYPE(JobContext)

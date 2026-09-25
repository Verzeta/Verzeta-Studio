// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file streaming-manager.cpp
 * @brief Implementation of the Chat::StreamingManager collaborator.
 *        Each method is a thin wrapper over MessageService paired
 *        with local-state bookkeeping.
 * @layer Service (Chat subsystem)
 * @dependencies MessageService, utils/logger.h,
 *               utils/thread-discipline.h.
 *
 * Every public method is main-thread-only and asserts the invariant
 * via VERZETA_ASSERT_MAIN_THREAD(). No shared mutable state, no
 * mutexes; the whole surface is serialised by Qt's main-thread
 * event loop.
 */

#include "streaming-manager.h"

#include "../../services/message-service.h"
#include "../../utils/logger.h"
#include "../../utils/thread-discipline.h"

#include <QUuid>

namespace {
inline bool streamTraceEnabled() {
    static const bool s_enabled = qEnvironmentVariableIntValue("VERZETA_STREAM_TRACE") > 0;
    return s_enabled;
}
}  // namespace


namespace Chat {

StreamingManager::StreamingManager(MessageService& msgSvc, QObject* parent)
    : QObject(parent), m_msgSvc(msgSvc) {}

StreamingManager::~StreamingManager() = default;

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool StreamingManager::isStreaming() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return !m_msgId.isEmpty();
}

QString StreamingManager::msgId() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_msgId;
}

QString StreamingManager::content() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_content;
}

QString StreamingManager::thinkingContent() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_thinking;
}

void StreamingManager::clearThinking() {
    VERZETA_ASSERT_MAIN_THREAD();
    m_thinking.clear();
}

bool StreamingManager::wasPersisted() const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_persisted;
}

bool StreamingManager::hasInMessageService() const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_msgId.isEmpty())
        return false;
    return m_msgSvc.hasStreamingMessage(m_msgId);
}

// ---------------------------------------------------------------------------
// State mutations
// ---------------------------------------------------------------------------

void StreamingManager::begin(const QString& convId,
                             const QString& role,
                             const QString& agentId,
                             const QString& memberAlias) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (!m_msgId.isEmpty()) {
        // Calling begin() while a placeholder is still open means the
        // previous placeholder leaked past the caller's cleanup path.
        // Log loudly and overwrite state so the new stream at least
        // works. Silent no-op would mask real control-flow bugs.
        qCWarning(verzetaUi) << "StreamingManager::begin called while a placeholder is "
                                "already open. Previous msgId="
                             << m_msgId << "content chars=" << m_content.size()
                             << "persisted=" << m_persisted
                             << "— overwriting state without calling abort "
                                "(caller must have cleaned up via finalize/abort/"
                                "resetWithoutAbort before calling begin).";
    }

    m_msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_content.clear();
    m_thinking.clear();
    m_persisted = false;

    m_msgSvc.beginStreamingMessage(convId, m_msgId, role, agentId, memberAlias);

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    if (streamTraceEnabled()) {
        qCInfo(verzetaUi) << "TRACE: SM-BEGIN msgId=" << m_msgId.left(8)
                          << " convId=" << convId.left(8) << " role=" << role
                          << " alias=" << memberAlias;
    }
    // === END STREAM-TRACE DEBUG ===

    emit streamBegan(m_msgId);
}

void StreamingManager::appendChunk(const QString& delta) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_msgId.isEmpty()) {
        qCWarning(verzetaUi) << "StreamingManager::appendChunk called without an open "
                                "placeholder — delta dropped ("
                             << delta.size() << " chars).";
        return;
    }
    m_content += delta;
    m_msgSvc.appendStreamingChunk(m_msgId, delta);
    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    if (streamTraceEnabled()) {
        qCInfo(verzetaUi) << "TRACE: SM-CONTENT msgId=" << m_msgId.left(8)
                          << " delta=" << delta.size() << " cumulative=" << m_content.size();
    }
    // === END STREAM-TRACE DEBUG ===
}

void StreamingManager::appendThinkingChunk(const QString& delta) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (delta.isEmpty()) {
        // Common case — most providers emit zero thinking on most
        // chunks.  Silent no-op keeps the call site in
        // ChatController::onChunkReceived unconditional and cheap.
        return;
    }
    if (m_msgId.isEmpty()) {
        qCWarning(verzetaUi) << "StreamingManager::appendThinkingChunk called without an "
                                "open placeholder — thinking delta dropped ("
                             << delta.size() << " chars).";
        return;
    }
    // Pure in-memory accumulation.  Thinking does NOT flow through
    // MessageService's incremental delta-buffer path because the
    // QML message-bubble disclosure renders the captured content
    // only at finalise time (no live thinking-stream UI).
    m_thinking += delta;
    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    if (streamTraceEnabled()) {
        qCInfo(verzetaUi) << "TRACE: SM-THINKING msgId=" << m_msgId.left(8)
                          << " delta=" << delta.size() << " cumulative=" << m_thinking.size();
    }
    // === END STREAM-TRACE DEBUG ===
}

void StreamingManager::rewrite(const QString& newContent) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_msgId.isEmpty()) {
        qCWarning(verzetaUi) << "StreamingManager::rewrite called without an open "
                                "placeholder — ignored ("
                             << newContent.size() << " chars).";
        return;
    }
    m_content = newContent;
    m_msgSvc.rewriteStreamingContent(m_msgId, newContent);
}

bool StreamingManager::finalize(int tokens,
                                const QString& finishReason,
                                const QString& contentHtml,
                                const QString& modelUsed,
                                const QJsonObject& metadata) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_msgId.isEmpty()) {
        qCWarning(verzetaUi) << "StreamingManager::finalize called without an open "
                                "placeholder — ignored.";
        return false;
    }

    // Capture the id before clearing state so the signal carries it.
    const QString savedId = m_msgId;

    // Trim-to-empty discard rule for the thinking sidecar.  Whitespace-
    // only thinking (Qwen empty `<think></think>` blocks, leading /
    // trailing newlines from the inline hoister, etc.) is suppressed
    // here so no false-disclosure rows ever land in the DB.  A
    // non-empty trimmed buffer falls through to MessageService where
    // the row's `thinking_content` column is populated.
    const QString thinkingForRow = m_thinking.trimmed().isEmpty() ? QString() : m_thinking;

    // === BEGIN STREAM-TRACE DEBUG (remove after truncation bug diagnosed) ===
    if (streamTraceEnabled()) {
        qCInfo(verzetaUi).noquote()
            << "TRACE: SM-FINALIZE msgId=" << m_msgId.left(8) << " finish=" << finishReason
            << " contentChars=" << m_content.size() << " thinkingChars=" << m_thinking.size()
            << " thinkingPersistedChars=" << thinkingForRow.size();
        // Dump the FULL content + thinking to stderr so the operator can
        // see exactly what is about to land in the DB.  noquote() keeps
        // the body unescaped; the markers bracket the body so a log
        // viewer can extract it cleanly.
        qCInfo(verzetaUi).noquote()
            << "TRACE: SM-FINALIZE-CONTENT-BEGIN msgId=" << m_msgId.left(8) << "\n"
            << m_content << "\nTRACE: SM-FINALIZE-CONTENT-END msgId=" << m_msgId.left(8);
        if (!m_thinking.isEmpty()) {
            qCInfo(verzetaUi).noquote()
                << "TRACE: SM-FINALIZE-THINKING-BEGIN msgId=" << m_msgId.left(8) << "\n"
                << m_thinking << "\nTRACE: SM-FINALIZE-THINKING-END msgId=" << m_msgId.left(8);
        }
    }
    // === END STREAM-TRACE DEBUG ===

    const bool ok = m_msgSvc.finalizeStreamingMessage(
        m_msgId, tokens, finishReason, contentHtml, modelUsed, metadata, thinkingForRow);

    // wasPersisted() must reflect the DB outcome of the last stream,
    // not optimistic assumption. Update it BEFORE clearing the rest.
    m_persisted = ok;
    m_msgId.clear();
    m_content.clear();
    m_thinking.clear();

    emit streamFinalized(savedId, finishReason, ok);
    return ok;
}

void StreamingManager::abort(const QString& reason) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_msgId.isEmpty()) {
        // Silent no-op is correct here. Error paths sometimes cannot
        // tell whether a placeholder is open, and "already aborted"
        // is a valid terminal state — logging it as a warning would
        // add noise to every shutdown.
        return;
    }

    const QString savedId = m_msgId;

    // Retry / error paths may have already aborted or deleted the
    // placeholder upstream; only call into MessageService when it
    // still holds the row so the common case stays quiet.
    if (m_msgSvc.hasStreamingMessage(m_msgId)) {
        m_msgSvc.abortStreamingMessage(m_msgId);
    }

    m_msgId.clear();
    m_content.clear();
    m_thinking.clear();
    m_persisted = false;

    emit streamAborted(savedId, reason);
}

void StreamingManager::resetWithoutAbort() {
    VERZETA_ASSERT_MAIN_THREAD();
    m_msgId.clear();
    m_content.clear();
    m_thinking.clear();
    m_persisted = false;
}

}  // namespace Chat

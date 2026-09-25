// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file streaming-manager.h
 * @brief ChatController collaborator that owns the in-flight-stream
 *        state machine: the placeholder UUID, the accumulated
 *        content buffer, and the persistence flag for the assistant
 *        message currently being produced.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core, MessageService (non-owning reference),
 *               models/message.h (via MessageService).
 *
 * Wraps MessageService's streaming API (beginStreamingMessage,
 * appendStreamingChunk, rewriteStreamingContent,
 * finalizeStreamingMessage, abortStreamingMessage, hasStreamingMessage)
 * so the call sites live in one place. The QML-facing surface
 * (Q_PROPERTY / Q_INVOKABLE / signals) stays on ChatController;
 * this class carries none of that.
 *
 * Ownership: constructed in ChatController's constructor via
 *   m_streaming = std::make_unique<Chat::StreamingManager>(m_msgSvc, this);
 *
 * Threading: strictly main-thread. Every public method begins with
 *   VERZETA_ASSERT_MAIN_THREAD();
 */

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class MessageService;

namespace Chat {

/**
 * @brief Pure QObject collaborator owned by ChatController.
 *        Does not expose Q_PROPERTY / Q_INVOKABLE; all QML-facing
 *        surface stays on ChatController (facade-preservation
 *        invariant).
 *
 * Contract in one sentence: `isStreaming()` is true iff there is a
 * placeholder row currently accumulating deltas. `begin` starts one;
 * `appendChunk` / `rewrite` mutate its content; `finalize` persists
 * and clears state (setting `wasPersisted()==true` on success);
 * `abort` drops it and clears state; `resetWithoutAbort` clears
 * state WITHOUT touching MessageService (used for conversation
 * switches and retry paths where the row is being handled
 * separately).
 */
class StreamingManager : public QObject {
    Q_OBJECT

  public:
    /**
     * @param msgSvc Non-owning reference; must outlive this
     *               StreamingManager. ChatController owns both.
     * @param parent Qt parent (ChatController), providing backup destruction
     *               via the parent-chain behind std::unique_ptr
     *               member cleanup.
     */
    explicit StreamingManager(MessageService& msgSvc, QObject* parent = nullptr);
    ~StreamingManager() override;

    // -----------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------

    /**
     * @brief True iff a placeholder is currently open: begin() was
     *        called and neither finalize() nor abort() nor
     *        resetWithoutAbort() has cleared the state yet.
     * @returns true while a placeholder is mid-flight.
     */
    bool isStreaming() const;

    /**
     * @brief UUID of the current placeholder.
     * @returns Placeholder UUID, or empty string if none is in flight.
     */
    QString msgId() const;

    /**
     * @brief Accumulated content of the current placeholder.
     * @returns Current content, or empty when isStreaming()==false.
     */
    QString content() const;

    /**
     * @brief Accumulated thinking / reasoning content of the current
     *        placeholder.  Parallel buffer to `content()`, fed by
     *        provider parsers via `appendThinkingChunk` from the
     *        `LlmChunk::thinkingDelta` field.  Display-only; the
     *        value lands on `Message::thinkingContent` at finalise
     *        time and is never read by request-side code.
     * @returns Current thinking buffer, or empty when no thinking
     *          deltas have arrived (the common case for non-reasoning
     *          providers).
     */
    QString thinkingContent() const;

    /**
     * @brief Discard the accumulated thinking buffer for the in-flight
     *        stream. Used by the thinking-misroute recovery: when a
     *        finish=stop turn carried the user-facing reply in the
     *        thinking channel (content empty), the caller promotes that
     *        text to content via rewrite() and clears the sidecar here so
     *        the persisted row does not show the same text twice (bubble
     *        + disclosure).
     * @sideeffects Clears the parallel thinking buffer only; the content
     *              buffer and MessageService state are untouched.
     */
    void clearThinking();

    /**
     * @brief True iff the last-finalised stream was persisted (the
     *        MessageService DB insert returned true). The flag
     *        reflects the LAST stream only. It is cleared by
     *        begin() on every new stream and by resetWithoutAbort().
     *        Not affected by abort() (abort always drops the row).
     * @returns true if the last finalize() persisted successfully.
     */
    bool wasPersisted() const;

    /**
     * @brief Proxy for MessageService::hasStreamingMessage with the
     *        current placeholder id. True iff MessageService still has
     *        the placeholder buffered internally. Error and retry
     *        paths consult this before calling abortStreamingMessage
     *        to avoid a redundant call on a placeholder that has
     *        already been cleaned up elsewhere.
     * @returns true if MessageService still buffers the placeholder.
     */
    bool hasInMessageService() const;

    // -----------------------------------------------------------------
    // State mutations
    // -----------------------------------------------------------------

    /**
     * @brief Open a new placeholder with a freshly-generated UUID.
     *        Wraps MessageService::beginStreamingMessage. Any prior
     *        state is overwritten without calling abort on
     *        MessageService, so callers must clean up previous streams
     *        via finalize / abort / resetWithoutAbort before calling
     *        begin again. Calling begin() with state still set is a
     *        caller bug that is logged via qCWarning.
     *
     * @param convId      Conversation UUID.
     * @param role        Usually "assistant".
     * @param agentId     Optional agent attribution.
     * @param memberAlias Optional group-chat member alias.
     */
    void begin(const QString& convId,
               const QString& role,
               const QString& agentId = {},
               const QString& memberAlias = {});

    /**
     * @brief Append a delta. Updates content() in-place and calls
     *        MessageService::appendStreamingChunk. No-op if no
     *        placeholder is open (caller bug; logged).
     * @param delta New text chunk to append.
     */
    void appendChunk(const QString& delta);

    /**
     * @brief Append a thinking-channel delta into the parallel
     *        thinking buffer.  Updates `thinkingContent()` in-place.
     *        Does NOT touch the visible-content buffer or
     *        MessageService. Thinking is render-only and lands on
     *        the row at `finalize` time via the same path the
     *        finalize-side persistence uses.  No-op when no
     *        placeholder is open OR when the delta is empty
     *        (the common case for non-reasoning chunks).
     * @param delta Thinking-channel text to append.
     */
    void appendThinkingChunk(const QString& delta);

    /**
     * @brief Replace the full content. Used after sanitisation
     *        (LARP-prefix strip, think-block removal, task-status
     *        marker strip). Calls
     *        MessageService::rewriteStreamingContent. No-op if no
     *        placeholder is open (caller bug; logged).
     * @param newContent Replacement content for the placeholder.
     */
    void rewrite(const QString& newContent);

    /**
     * @brief Flush the placeholder to the DB as a permanent message.
     *        Wraps MessageService::finalizeStreamingMessage, updates
     *        wasPersisted() from the return value, then clears the
     *        in-memory state. After finalize returns isStreaming()
     *        is false. Emits streamFinalized with the saved msgId
     *        (captured before the state clear).
     * @param tokens       Total token count for the response (0 when
     *                     unreported).
     * @param finishReason Provider-reported finish reason.
     * @param contentHtml  Pre-rendered HTML version of the content,
     *                     persisted alongside the raw content.
     * @param modelUsed    Model name the response was generated by.
     * @param metadata     Arbitrary metadata blob persisted with the
     *                     message row.
     * @returns true iff the DB write succeeded.
     */
    bool finalize(int tokens,
                  const QString& finishReason,
                  const QString& contentHtml,
                  const QString& modelUsed,
                  const QJsonObject& metadata);

    /**
     * @brief Drop the placeholder without persisting. Calls
     *        MessageService::abortStreamingMessage only if
     *        MessageService still has the placeholder buffered;
     *        otherwise just clears the in-memory state. Emits
     *        streamAborted with the saved msgId. Safe to call when
     *        isStreaming() == false (silent no-op).
     * @param reason Human-readable reason recorded with the abort.
     */
    void abort(const QString& reason);

    /**
     * @brief Clear the in-memory state WITHOUT calling into
     *        MessageService. Used on conversation switches (old
     *        placeholder is abandoned but MessageService cleanup is
     *        handled by the switch path at its own layer) and on
     *        retry paths where the row has already been deleted /
     *        aborted via deleteMessage. Emits nothing.
     */
    void resetWithoutAbort();

  signals:
    /**
     * @brief Emitted after begin() has registered a new placeholder.
     * @param msgId UUID of the newly-opened placeholder.
     */
    void streamBegan(const QString& msgId);

    /**
     * @brief Emitted after finalize() has persisted (or attempted to
     *        persist) a placeholder.
     * @param msgId        UUID of the placeholder that was finalised.
     * @param finishReason Provider-reported finish reason.
     * @param ok           true if the DB write succeeded; false
     *                     otherwise. Matches finalize()'s return
     *                     value.
     */
    void streamFinalized(const QString& msgId, const QString& finishReason, bool ok);

    /**
     * @brief Emitted after abort() has dropped a placeholder.
     *        Observers (e.g. MessageListModel) use this to remove
     *        stuck typing indicators when a provider fails before
     *        any content arrived.
     * @param msgId  UUID of the placeholder that was dropped.
     * @param reason Human-readable reason recorded with the abort.
     */
    void streamAborted(const QString& msgId, const QString& reason);

  private:
    MessageService& m_msgSvc;  // non-owning
    QString m_msgId;
    QString m_content;
    /**
     * @brief Parallel buffer holding the captured reasoning content
     *        for the in-flight stream.  Reset alongside m_content
     *        on every state transition (begin / resetWithoutAbort)
     *        so the next placeholder starts clean.  Read by
     *        `thinkingContent()`; flushed to the row by `finalize()`
     *        via the trim-to-empty discard rule (empty after trim →
     *        not persisted; non-empty → written to
     *        `messages.thinking_content`).
     */
    QString m_thinking;
    bool m_persisted = false;
};

}  // namespace Chat

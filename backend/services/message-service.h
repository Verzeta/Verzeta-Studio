// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file message-service.h
 * @brief CRUD operations for chat messages, attachments, and tool calls
 *        within conversations. Also manages cumulative token counts.
 * @layer Service
 * @dependencies DbManager (Data Access)
 */

#pragma once

#include "../models/attachment.h"
#include "../models/db-manager.h"
#include "../models/message.h"
#include "../models/tool-call.h"

#include <QTimer>

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

/**
 * @brief Service for message-level persistence operations.
 *
 * Responsible for all CRUD on the messages, attachments, and tool_calls tables.
 * After inserting a message, it updates the conversation's token_total.
 *
 * Pagination uses LIMIT/OFFSET on the messages table, ordered by created_at ASC,
 * allowing the UI to load older messages lazily.
 *
 * Thread safety: Designed for use on the main thread only. All DB access uses
 * the named connection opened by DbManager::open().
 */
class MessageService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the MessageService.
     * @param db Reference to the open DbManager singleton.
     * @param parent Optional Qt parent.
     */
    explicit MessageService(DbManager& db, QObject* parent = nullptr);
    ~MessageService() override;

    // -----------------------------------------------------------------------
    // Message operations
    // -----------------------------------------------------------------------

    /**
     * @brief Inserts a new message into the database.
     *        Also updates the conversation's cumulative token_total.
     * @param msg Message struct. The id field should be a pre-generated UUID.
     * @return The message UUID on success, or empty string on failure.
     * @sideeffects Inserts into messages table, updates conversations.token_total.
     *              Emits messageAdded on success.
     */
    QString addMessage(const Message& msg);

    /**
     * @brief Auto-persist a SYSTEM-role intervening message DURING a
     *        tool-call sequence.
     *
     * **Walk-and-pair invariant** (request-builder.cpp:282-311): when
     * the LLM emits a `tool_calls` assistant message, the matching
     * `role=tool` result row(s) must be adjacent in history for the
     * pairing logic to recognise them. An intervening
     * `role=assistant` row between the two would terminate the
     * tool-window: the role=tool rows would be dropped as orphans
     * and the agent would never see its own successful tool result.
     *
     * Hard rule discovered while debugging the poll-card auto-persist
     * path: auto-persisted messages from tool invocations MUST be
     * role=system, NEVER role=assistant. role=assistant breaks
     * RequestBuilder's walk-and-pair: the tool result row is
     * orphaned and dropped, and the agent loops forever waiting for
     * its tool result.
     *
     * This contract is structural rather than caller-disciplined: any
     * service that needs to auto-persist a chat row during a tool
     * invoke MUST go through this helper. The helper does not accept
     * a `role` parameter; the role is `system` unconditionally. A
     * caller that needs a different role is using the wrong helper.
     *
     * @param convId   Conversation UUID. Empty input is rejected
     *                 (returns "").
     * @param content  Plain-text body of the message. Becomes
     *                 `messages.content` directly; the helper makes
     *                 no formatting assumptions.
     * @param metadata Optional `messages.metadata` JSON. Pass tags
     *                 like `produced_by`, `poll_id`, etc. here.
     *                 Defaults to empty.
     * @return The new message UUID on success, "" on failure.
     * @sideeffects Inserts via addMessage(); same signals fire.
     *              `messageAdded` will be emitted with the new id.
     *
     * NB: this helper is for **intervening** rows produced INSIDE a
     * tool-invoke window. Genuine async-after-cascade persists (e.g.
     * ImageService::onImageReady, where the worker completion
     * arrives long after the original tool returned and the cascade
     * has moved on) may continue to use `addMessage` directly with
     * role=assistant, because those rows are not inside a tool window so
     * walk-and-pair is not at risk.
     */
    QString addInterveningSystemMessage(const QString& convId,
                                        const QString& content,
                                        const QJsonObject& metadata = {});

    /**
     * @brief Retrieves ALL messages for a conversation ordered by created_at ASC.
     * @param convId UUID of the conversation.
     * @return Complete list of Message structs in ascending chronological order.
     *         For conversations with thousands of messages, this is still fast
     *         (<50ms for 10k rows on modern hardware) because SQLite sequential
     *         reads on an indexed column are very efficient.
     * @complexity O(n) where n = total messages in conversation.
     */
    QList<Message> getMessages(const QString& convId);

    /**
     * @brief Retrieves a single message by its UUID.
     * @param id Message UUID (the messages.id primary key).
     * @return The Message struct on hit; default-constructed Message
     *         (empty `id`) on miss. Never throws.
     * @complexity O(log N), primary-key indexed lookup.
     *
     * Required by HeartbeatSubagentService's stream-collision deferral
     * path: it subscribes to StreamingManager::streamFinalized(msgId,
     * ...) and resolves the msgId argument to its conversation_id by
     * looking up the persisted row. The signal carries no convId, so a
     * single-id lookup is the cleanest resolution path.
     */
    Message getMessage(const QString& id);

    /**
     * @brief Retrieves a page of messages for explicit pagination (UI scrollback).
     * @param convId UUID of the conversation.
     * @param limit Maximum number of messages to return.
     * @param offset Number of messages to skip from the beginning.
     * @return List of Message structs in ascending chronological order.
     * @complexity O(limit)
     */
    QList<Message> getMessagePage(const QString& convId, int limit, int offset);

    /**
     * @brief Retrieves the most recent N messages for a conversation.
     *        Uses DESC ordering internally and reverses to ascending.
     *        Performance is O(maxRows) regardless of total conversation size.
     * @param convId UUID of the conversation.
     * @param maxRows Maximum number of rows to load from the newest end.
     * @return List of Message structs in ascending chronological order.
     * @complexity O(maxRows), constant cost regardless of conversation size.
     */
    QList<Message> getRecentMessages(const QString& convId, int maxRows);

    /**
     * @brief Backs /flashmemory: deletes EVERY message in
     *        the conversation plus their tool_calls rows, in one
     *        transaction-shaped pair of bulk DELETEs (tool_calls
     *        first; messages second). The conversation row itself is
     *        untouched. Destructive; callers own the confirmation UX.
     * @param convId Conversation UUID.
     * @returns Number of message rows deleted, or -1 on SQL error.
     */
    int deleteAllForConversation(const QString& convId);

    /**
     * @brief Deletes a single message by ID.
     * @param id UUID of the message.
     * @return true if deletion succeeded and a row was actually removed.
     * @sideeffects Removes the message row. Cascade removes attached attachments
     *              and tool_calls. Emits messageDeleted on success.
     */
    bool deleteMessage(const QString& id);

    /**
     * @brief Marks an existing assistant message as the persisted
     *        outcome of an exhausted echo-retry round. Prepends an
     *        "[ECHO-FAILED]" sentinel line to the content so QML
     *        bubbles can render the row with a warning badge or
     *        strikethrough; also stamps `metadata.echo_failed=true`
     *        so structural binding sites can switch on the flag
     *        instead of pattern-matching the prefix.
     * @param id UUID of the message to mark.
     * @return true if the row existed and was rewritten.
     * @sideeffects Updates the messages.content / metadata columns.
     *              Emits messageContentUpdated so the model re-renders.
     */
    bool markMessageAsEchoFailed(const QString& id);

    /**
     * @brief Updates the pre-rendered HTML cache for a message.
     * @param id UUID of the message.
     * @param html Pre-rendered HTML string from MarkdownConverter.
     * @return true if update succeeded.
     * @sideeffects Updates content_html in messages table. Emits messageUpdated.
     */
    bool updateContentHtml(const QString& id, const QString& html);

    // -----------------------------------------------------------------------
    // Streaming API — in-memory buffer + signals. No DB writes until finalize.
    // -----------------------------------------------------------------------

    /**
     * @brief Starts tracking a streaming assistant message.
     *
     * The placeholder message is NOT written to the DB yet. It only lives
     * in the streaming buffer and is announced to the model via the
     * messageStreamingStarted() signal so MessageListModel can insert a
     * placeholder row. On finalizeStreamingMessage() the full message is
     * persisted to the DB in a single INSERT and messageAdded() fires.
     *
     * This means: streaming rows exist only in memory. If the user
     * switches conversations mid-stream, the buffer for the old chat
     * keeps accumulating and is flushed on finalize. The new chat shows
     * its own DB rows. No cross-contamination, no partial DB writes.
     *
     * @param convId   Conversation UUID.
     * @param msgId    Pre-generated UUID for the new assistant message.
     * @param role     Usually "assistant".
     * @param agentId  Optional agent attribution.
     * @param memberAlias Optional group-chat member alias.
     */
    void beginStreamingMessage(const QString& convId,
                               const QString& msgId,
                               const QString& role,
                               const QString& agentId = {},
                               const QString& memberAlias = {});

    /**
     * @brief Appends a delta chunk to a streaming message.
     *        Updates the in-memory buffer and emits messageContentStreamed.
     * @param msgId UUID of the streaming message.
     * @param delta New text to append.
     */
    void appendStreamingChunk(const QString& msgId, const QString& delta);

    /**
     * @brief Rewrites the full content of a streaming message.
     *        Used when ChatController sanitises the content (strips LARP
     *        prefixes, \<think\> blocks, task_state markers) before persist.
     *        Emits messageContentRewritten so the model can resync the row.
     * @param msgId      UUID of the streaming message.
     * @param newContent Sanitised full content.
     */
    void rewriteStreamingContent(const QString& msgId, const QString& newContent);

    /**
     * @brief Finalises a streaming message: flushes buffer to DB, emits
     *        messageAdded (so the model's "is streaming" flag flips to false
     *        and the row gets its permanent DB-backed data). The buffer is
     *        discarded.
     * @param msgId        UUID of the streaming message.
     * @param tokenCount   Final token count from provider.
     * @param finishReason "stop" | "length" | "tool_calls" | "error".
     * @param contentHtml  Pre-rendered HTML (optional; may be empty).
     * @param modelUsed    Model identifier used to generate the message.
     * @param metadata     Per-message metadata (elapsed_ms, finish_reason, ...).
     * @param thinkingContent Captured reasoning sidecar from the provider's
     *                     thinking channel.  Display-only: written verbatim
     *                     to `messages.thinking_content` and rendered by the
     *                     assistant-bubble disclosure, never read by any
     *                     code that builds a subsequent request.  Default
     *                     empty string preserves the original call
     *                     signature for sites that have no thinking
     *                     content to persist.
     * @return true on successful DB insert.
     */
    bool finalizeStreamingMessage(const QString& msgId,
                                  int tokenCount,
                                  const QString& finishReason,
                                  const QString& contentHtml,
                                  const QString& modelUsed,
                                  const QJsonObject& metadata,
                                  const QString& thinkingContent = {});

    /**
     * @brief Aborts a streaming message without persisting it. Emits
     *        messageStreamingAborted so the model can remove the
     *        placeholder.
     * @param msgId UUID of the streaming placeholder to discard.
     */
    void abortStreamingMessage(const QString& msgId);

    /**
     * @brief Relays a context-fill measurement onto the shared
     *        contextFillMeasured signal. Called by ChatController
     *        (any instance) after each request build; exists because
     *        signals cannot be emitted from outside the class.
     * @param convId  Conversation UUID the measurement belongs to.
     * @param percent Estimated context fill (0-100).
     */
    void notifyContextFillMeasured(const QString& convId, int percent) {
        emit contextFillMeasured(convId, percent);
    }

    /**
     * @brief Relays an agent's \@user mention onto the shared
     *        userMentioned signal so wire clients can raise platform
     *        notifications. Called by ChatController (any instance)
     *        when its cascade detects the mention.
     * @param convId Conversation UUID.
     * @param alias  Alias of the agent that mentioned the user.
     * @param text   The mentioning message's content.
     */
    void notifyUserMentioned(const QString& convId, const QString& alias, const QString& text) {
        emit userMentioned(convId, alias, text);
    }

    /**
     * @brief Returns whether a streaming message with the given id
     *        exists.
     * @param msgId UUID to query.
     * @returns true iff a streaming slot with this id is currently
     *          open.
     */
    bool hasStreamingMessage(const QString& msgId) const;

    /**
     * @brief Returns the current streaming buffer content for the
     *        given msgId.
     * @param msgId UUID of the streaming placeholder.
     * @returns Accumulated content, or empty string when no such
     *          streaming message exists.
     */
    QString streamingContent(const QString& msgId) const;

    /**
     * @brief Returns the conversation id a streaming message belongs
     *        to.
     * @param msgId UUID of the streaming placeholder.
     * @returns Conversation UUID, or empty string when the msgId is
     *          unknown.
     */
    QString streamingConversationId(const QString& msgId) const;

    /**
     * @brief Returns all active streaming messages for a conversation.
     *        Used by MessageListModel to rehydrate placeholders for
     *        the conversation being opened.
     * @param convId Conversation UUID to enumerate.
     * @returns List of partial Message structs (one per active
     *          streaming slot whose convId matches); empty when none
     *          are open.
     */
    QList<Message> streamingMessagesForConversation(const QString& convId) const;

    /**
     * @brief Aborts (and emits messageStreamingAborted for) every
     *        streaming slot whose conv id matches the given
     *        conversation. Called by ChatController when a
     *        conversation is deleted so no orphan streaming slots
     *        remain.
     * @param convId Conversation UUID whose streaming slots should be
     *               aborted.
     * @returns Number of slots aborted.
     */
    int abortStreamingMessagesForConversation(const QString& convId);

    /**
     * @brief Posts an ephemeral message to the UI without touching the
     *        DB. Used for slash-command output (/help, /showtools,
     *        etc.) that should appear in the chat stream but NOT be
     *        persisted. MessageListModel picks it up via
     *        messageEphemeralPosted and inserts an in-memory row. The
     *        row stays visible across setActiveConversation() calls
     *        because the model's reload path explicitly re-includes
     *        ephemeral rows from ephemeralMessagesForConversation().
     *        Call clearEphemeralForConversation(convId) to drop them.
     * @param msg Message struct (must have id, convId, role, content
     *            populated).
     */
    void postEphemeralMessage(const Message& msg);

    /**
     * @brief Drop every ephemeral message for the given conversation.
     *        Used by /clear to actually clear slash-command output
     *        bubbles from the chat view (otherwise they'd survive a
     *        MessageListModel::setActiveConversation reload because
     *        the model's reload path re-includes them). Emits
     *        messageEphemeralCleared so MessageListModel can refresh
     *        if the convId matches its active conversation.
     *
     *        DB-backed messages are NOT touched; this is purely an
     *        in-memory ephemeral-cache clear.
     * @param convId Conversation UUID whose ephemeral rows to drop.
     */
    void clearEphemeralForConversation(const QString& convId);

    /**
     * @brief Returns true if a conversation row with the given id
     *        exists. Used as a guard before DB writes that could hit
     *        an FK error on a deleted conversation.
     * @param convId Conversation UUID to test.
     * @returns true iff the row exists.
     */
    bool conversationExists(const QString& convId);

  public slots:
    /**
     * @brief Slot: called when ConversationService reports a
     *        conversation has been deleted. Aborts any streaming
     *        slots targeting that conversation so no orphan state
     *        survives.
     * @param convId UUID of the deleted conversation.
     */
    void onConversationDeleted(const QString& convId);

    /**
     * @brief Returns the cumulative token count for a conversation.
     * @param convId UUID of the conversation.
     * @return Sum of token_count across all messages in the conversation.
     * @complexity O(1); reads from pre-maintained conversations.token_total.
     */
    int getTokenTotal(const QString& convId);

    // -----------------------------------------------------------------------
    // Attachment operations
    // -----------------------------------------------------------------------

    /**
     * @brief Inserts a new attachment linked to a message.
     * @param attachment Attachment struct with a pre-generated UUID id.
     * @return The attachment UUID on success, or empty string on failure.
     * @sideeffects Inserts into attachments table.
     */
    QString addAttachment(const Attachment& attachment);

    /**
     * @brief Retrieves all attachments for a message.
     * @param messageId UUID of the message.
     * @return List of Attachment structs.
     * @complexity O(n) where n is the number of attachments on the message.
     */
    QList<Attachment> getAttachments(const QString& messageId);

    /**
     * @brief Deletes an attachment by ID.
     * @param id UUID of the attachment.
     * @return true if deletion succeeded.
     */
    bool deleteAttachment(const QString& id);

    // -----------------------------------------------------------------------
    // ToolCall operations
    // -----------------------------------------------------------------------

    /**
     * @brief Inserts a new tool call record linked to a message.
     * @param toolCall ToolCall struct with pre-generated UUID id and "pending" status.
     * @return The tool call UUID on success, or empty string on failure.
     * @sideeffects Inserts into tool_calls table. Emits toolCallAdded on success
     *              so ToolCallLogModel / ArtifactsModel pick up the new row.
     */
    QString addToolCall(const ToolCall& toolCall);

    /**
     * @brief Updates the result and status of a completed tool call.
     * @param id UUID of the tool call.
     * @param result JSON result value (object, array, or string).
     * @param status New status: "success" or "error".
     * @return true if update succeeded.
     * @sideeffects Updates result, status, and completed_at in tool_calls table.
     *              Emits toolCallUpdated on success.
     */
    bool updateToolCallResult(const QString& id, const QJsonValue& result, const QString& status);

    /**
     * @brief Fetches a single tool call by id.
     * @param id ToolCall UUID to look up.
     * @return The ToolCall struct, or an empty struct if not found.
     */
    ToolCall getToolCall(const QString& id);

    /**
     * @brief Retrieves all tool calls for a message.
     * @param messageId UUID of the message.
     * @return List of ToolCall structs.
     */
    QList<ToolCall> getToolCalls(const QString& messageId);

    /**
     * @brief Bulk variant of getToolCalls using a single SQL query against
     *        an IN clause built from @p messageIds. Used by
     *        RequestBuilder's assembleHistory so a 50-message history
     *        walk doesn't fan out into 50 separate SELECTs on the
     *        main thread per cascade turn.
     * @param messageIds List of message UUIDs whose tool calls to
     *                   fetch.
     * @returns Hash keyed by message_id; empty entries (messages
     *          with no tool calls) are omitted.
     */
    QHash<QString, QList<ToolCall>> getToolCallsForMessages(const QStringList& messageIds);

    /**
     * @brief Retrieves all tool calls associated with the same `turn_id`
     *        as the given message, optionally filtered to only those made
     *        by the same agent (when agentId is non-empty).
     *        Used by the per-turn UI chip to summarise the entire tool
     *        chain a single agent ran in service of one user prompt.
     * @param messageId    Anchor message; its turn_id is the grouping key.
     * @param agentId      Optional. If non-empty, restricts results to tool
     *                     calls anchored on messages with the same agent.
     * @return Tool calls in chronological order (started_at ASC).
     */
    QList<ToolCall> toolCallsForTurn(const QString& messageId, const QString& agentId = {});

    /**
     * @brief Retrieves every tool call across an entire conversation,
     *        ordered by started_at ASC. Used to populate the Tool Log
     *        panel from a clean source.
     * @param conversationId Conversation UUID to enumerate.
     * @returns Tool calls in chronological order; empty list when the
     *          conversation has no tool calls.
     */
    QList<ToolCall> toolCallsForConversation(const QString& conversationId);

  signals:
    /**
     * @brief Emitted when a message is persisted to a conversation.
     * @param convId Conversation UUID the message belongs to.
     * @param msgId  UUID of the newly-persisted message.
     */
    void messageAdded(const QString& convId, const QString& msgId);

    /**
     * @brief Emitted when a message's persisted fields change (html,
     *        tokens, ...).
     * @param convId Conversation UUID.
     * @param msgId  UUID of the updated message.
     */
    void messageUpdated(const QString& convId, const QString& msgId);

    /**
     * @brief Emitted when a message is deleted.
     * @param convId Conversation UUID.
     * @param msgId  UUID of the deleted message.
     */
    void messageDeleted(const QString& convId, const QString& msgId);

    // -----------------------------------------------------------------------
    // Streaming lifecycle signals. MessageListModel consumes these to
    // render live tokens without touching the DB.
    // -----------------------------------------------------------------------

    /**
     * @brief Emitted when a streaming placeholder is created.
     * @param convId      Conversation UUID.
     * @param msgId       UUID of the new placeholder row.
     * @param role        Role string (usually "assistant").
     * @param agentId     Optional agent attribution.
     * @param memberAlias Optional group-chat member alias.
     */
    void messageStreamingStarted(const QString& convId,
                                 const QString& msgId,
                                 const QString& role,
                                 const QString& agentId,
                                 const QString& memberAlias);

    /**
     * @brief Emitted for each streaming chunk appended.
     * @param convId Conversation UUID.
     * @param msgId  UUID of the streaming placeholder.
     * @param delta  New text appended.
     */
    void messageContentStreamed(const QString& convId, const QString& msgId, const QString& delta);

    /**
     * @brief Emitted when a streaming message's content is rewritten.
     * @param convId     Conversation UUID.
     * @param msgId      UUID of the streaming placeholder.
     * @param newContent Sanitised full content.
     */
    void
    messageContentRewritten(const QString& convId, const QString& msgId, const QString& newContent);

    /**
     * @brief Emitted when a streaming placeholder is discarded without
     *        persisting.
     * @param convId Conversation UUID.
     * @param msgId  UUID of the aborted placeholder.
     */
    void messageStreamingAborted(const QString& convId, const QString& msgId);

    /**
     * @brief A conversation's context-fill percentage was measured at
     *        request build. Emitted via notifyContextFillMeasured by
     *        whichever ChatController instance (local or wire-session)
     *        built the request. MessageService is the shared event
     *        spine the wire bridge subscribes to, so the gauge reaches
     *        every client regardless of which controller drove the
     *        turn.
     * @param convId  Conversation UUID the measurement belongs to.
     * @param percent Estimated context fill (0-100).
     */
    void contextFillMeasured(const QString& convId, int percent);

    /**
     * @brief An agent @-mentioned the user in a group chat. Relayed
     *        via notifyUserMentioned from whichever ChatController
     *        instance detected it; the wire bridge forwards it to
     *        clients as chat.user_mentioned for platform notifications.
     * @param convId Conversation UUID.
     * @param alias  Alias of the mentioning agent.
     * @param text   The mentioning message's content.
     */
    void userMentioned(const QString& convId, const QString& alias, const QString& text);

    /**
     * @brief Emitted when an ephemeral (non-persisted) message is
     *        posted.
     * @param convId Conversation UUID.
     * @param msgId  UUID of the ephemeral message.
     */
    void messageEphemeralPosted(const QString& convId, const QString& msgId);

    /**
     * @brief Emitted when ephemeral messages for a conversation are
     *        dropped (via clearEphemeralForConversation).
     *        MessageListModel listens on its active conversation to
     *        refresh the visible list.
     * @param convId Conversation UUID whose ephemeral rows were
     *               cleared.
     */
    void messageEphemeralCleared(const QString& convId);

    // -----------------------------------------------------------------------
    // Tool call table signals — consumed by ToolCallLogModel / ArtifactsModel.
    // -----------------------------------------------------------------------

    /**
     * @brief Emitted after a row is inserted into tool_calls.
     * @param convId Conversation UUID owning the tool call.
     * @param callId UUID of the new tool_call row.
     */
    void toolCallAdded(const QString& convId, const QString& callId);

    /**
     * @brief Emitted after a tool call's result / status changes.
     * @param convId Conversation UUID owning the tool call.
     * @param callId UUID of the updated tool_call row.
     */
    void toolCallUpdated(const QString& convId, const QString& callId);

  private:
    DbManager& m_db;

    /**
     * @brief In-memory streaming buffer with one entry per in-flight message.
     *        Destroyed on finalize or abort. Never hits the DB directly;
     *        the final insert happens in finalizeStreamingMessage().
     */
    struct StreamingSlot {
        QString convId;
        QString role;
        QString agentId;
        QString memberAlias;
        QString content;
        QDateTime startedAt;
    };
    QHash<QString, StreamingSlot> m_streamingSlots;

    /**
     * @brief Per-msgId pending delta buffer for chunk emission throttling.
     *        Inbound chunks update the slot's `content` immediately so the
     *        in-memory state is current; the messageContentStreamed signal
     *        is coalesced and emitted at most once per
     *        kStreamFlushIntervalMs to keep the QML render loop from
     *        thrashing during multi-agent cascades that produce 100+
     *        chunks/sec aggregated.
     */
    QHash<QString, QString> m_pendingDeltas;
    QTimer m_streamFlushTimer;
    static constexpr int kStreamFlushIntervalMs = 33;

  private slots:
    void flushPendingDeltas();

  private:
    /**
     * @brief In-memory cache of ephemeral messages per conversation.
     *        MessageListModel asks for them via
     *        ephemeralMessagesForConversation() when it reloads a
     *        conversation, so ephemeral output that was already shown
     *        stays visible when the user re-selects the chat.
     *        These never touch the DB. Cleared on conversation delete.
     */
    QHash<QString, QList<Message>> m_ephemeralMessages;

  public:
    /**
     * @brief Returns ephemeral (non-persisted) messages for the given
     *        conversation.
     * @param convId Conversation UUID to enumerate.
     * @returns List of ephemeral Message structs in post order; empty
     *          when the conversation has none.
     */
    QList<Message> ephemeralMessagesForConversation(const QString& convId) const;

    /**
     * @brief Helper that executes a prepared query and logs on failure.
     * @param q Prepared QSqlQuery.
     * @param context Label for error messages.
     * @return true if execution succeeded.
     */
    bool execQuery(QSqlQuery& q, const QString& context);
};

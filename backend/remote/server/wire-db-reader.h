// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-db-reader.h
 * @brief Read-only access to the host's SQLite database from inside
 *        the verzeta-remote process.
 *
 *        Used for ops where the host does NOT already expose a
 *        Q_INVOKABLE accessor. Direct SQL is the contract-approved
 *        alternative to "add a method to the host". The wire process
 *        opens its own read-only WAL connection to
 *        `verzeta-studio.db` and runs SELECTs directly. WAL allows
 *        multiple readers concurrently with the host's writer;
 *        SQLite's atomic page commits guarantee the wire never sees
 *        a torn write. We NEVER write through this connection;
 *        every state mutation goes via existing host Q_INVOKABLE
 *        methods over the bridge.
 *
 *        Threading: this object lives on the verzeta-remote main
 *        thread (the only thread that runs the QWebSocketServer).
 *        All methods are synchronous and run on the calling thread.
 *        SQLite WAL guarantees concurrent readers do not block each
 *        other; writes from the host's process are also non-blocking
 *        thanks to WAL.
 * @layer Service (presentation; remote-access read-only DB)
 * @dependencies Qt6::Core, Qt6::Sql.
 */

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

namespace Verzeta::Remote {

/**
 * @brief Read-only SQLite WAL reader the verzeta-remote daemon uses
 *        to answer queries the host bridge does not expose as a
 *        Q_INVOKABLE.
 */
class WireDbReader : public QObject {
    Q_OBJECT
  public:
    /**
     * @brief Constructs an unopened reader. Call open() before use.
     * @param parent  Optional Qt parent.
     */
    explicit WireDbReader(QObject* parent = nullptr);
    ~WireDbReader() override;

    /**
     * @brief Opens the host's SQLite database read-only.
     * @returns True when the connection opens; false when the file
     *          does not exist or cannot be opened.
     *
     * The path defaults to the host's `<AppData>/verzeta-studio.db`.
     * The wire process is expected to start AFTER the host (which
     * creates the file on first run), but if it starts before, this
     * returns false and the wire serves `host_offline` errors for
     * SQL-backed ops until the file appears.
     */
    bool open();

    /**
     * @brief Reports whether the connection is open.
     * @returns True after a successful open().
     */
    bool isOpen() const { return m_db.isOpen(); }

    /**
     * @brief Closes the connection and removes it from QSqlDatabase.
     *        Idempotent.
     */
    void close();

    // -----------------------------------------------------------------
    // Conversation reads
    // -----------------------------------------------------------------

    /**
     * @brief Lists all conversations newest-first.
     * @returns JSON array of `{id, title, folder_id, created_at,
     *          updated_at, system_prompt, primary_agent_id, is_group,
     *          is_pinned}` objects.
     */
    QJsonArray listConversations();

    /**
     * @brief Returns one conversation by id.
     * @param id  Conversation UUID.
     * @returns Conversation object in the same shape as
     *          listConversations() rows; empty object on miss.
     */
    QJsonObject conversationById(const QString& id);

    /**
     * @brief Returns the messages for a conversation, oldest-first.
     * @param convId  Conversation UUID.
     * @returns JSON array of `{id, conversation_id, role, content,
     *          created_at, finish_reason, agent_id, member_alias,
     *          turn_id, token_count, model_used}` objects.
     */
    QJsonArray messagesForConversation(const QString& convId);

    /**
     * @brief Returns a single message by id.
     * @param msgId  Message UUID.
     * @returns Message object in the same shape as
     *          messagesForConversation() rows; empty object on miss.
     *
     * Used to enrich `message.added` / `message.updated` wire events
     * with the full row snapshot.
     */
    QJsonObject messageById(const QString& msgId);

    // -----------------------------------------------------------------
    // Folder reads
    // -----------------------------------------------------------------

    /**
     * @brief Lists all folders (regular + project + organization).
     * @returns JSON array of `{id, name, parent_id, folder_type, goal,
     *          description, agent_ids, created_at}` objects.
     *          `agent_ids` is parsed from the stored JSON column.
     */
    QJsonArray listAllFolders();

    // -----------------------------------------------------------------
    // Per-conversation settings
    // -----------------------------------------------------------------

    /**
     * @brief Returns the per-conversation LLM config + system prompt.
     * @param convId  Conversation UUID.
     * @returns `{system_prompt, temperature, max_tokens, context_window,
     *          streaming, thinking, primary_agent_id}` parsed from
     *          `conversations.system_prompt` + `conversations.llm_config`
     *          (JSON). Empty object on miss.
     */
    QJsonObject conversationSettings(const QString& convId);

    // -----------------------------------------------------------------
    // Plans / tasks
    // -----------------------------------------------------------------

    /**
     * @brief Returns the plans associated with a conversation.
     * @param convId  Conversation UUID.
     * @returns JSON array of plan objects; each carries the plan
     *          metadata and an embedded array of plan_steps.
     */
    QJsonArray plansForConversation(const QString& convId);

    /**
     * @brief Returns one plan with its steps.
     * @param planId  Plan UUID.
     * @returns Plan object with embedded steps; empty object on miss.
     */
    QJsonObject planById(const QString& planId);

    /**
     * @brief Returns the parent plan id for a given step.
     * @param stepId  Step UUID.
     * @returns Plan UUID, or empty string on miss.
     *
     * Used by the wire process to enrich `step.updated` events with
     * the plan id Android needs to follow up via `plan.get`.
     */
    QString planIdForStep(const QString& stepId);

    // -----------------------------------------------------------------
    // Tool-call activity
    // -----------------------------------------------------------------

    /**
     * @brief Returns the tool calls for a conversation, ordered by
     *        message `created_at` and then `started_at`.
     * @param convId  Conversation UUID.
     * @returns JSON array of `{id, message_id, tool_name, arguments
     *          (parsed JSON), result (parsed JSON or string), status,
     *          started_at, completed_at, plan_step_id,
     *          conversation_id}` objects.
     */
    QJsonArray toolCallsForConversation(const QString& convId);

    /**
     * @brief Returns the tool calls for a single message.
     * @param messageId  Message UUID.
     * @returns JSON array of tool-call objects with the same shape as
     *          toolCallsForConversation() rows.
     */
    QJsonArray toolCallsForMessage(const QString& messageId);

    /**
     * @brief Returns the tool calls for an entire cascade turn.
     * @param turnMsgId  Message id whose turn_id selects the rows.
     * @param agentId    When non-empty, filters to that agent's calls
     *                   only.
     * @returns JSON array of tool-call objects.
     */
    QJsonArray toolCallsForTurn(const QString& turnMsgId, const QString& agentId);

    /**
     * @brief Returns one tool-call row by id.
     * @param callId  Tool-call UUID.
     * @returns Tool-call object; empty object on miss.
     */
    QJsonObject toolCallById(const QString& callId);

    // -----------------------------------------------------------------
    // Attachments
    // -----------------------------------------------------------------

    /**
     * @brief Returns all attachments for a message.
     * @param messageId  Message UUID.
     * @returns JSON array of `{id, message_id, type, filename,
     *          mime_type, data_path, size_bytes, created_at}` objects.
     *          `data_inline` is NOT included; call attachmentBytes()
     *          for the actual content.
     */
    QJsonArray attachmentsForMessage(const QString& messageId);

    /**
     * @brief Returns attachment metadata + base64 content.
     * @param attachmentId  Attachment UUID.
     * @returns `{id, filename, mime_type, content_base64, size_bytes}`
     *          on success, empty object on miss.
     *
     * Returns `data_inline` directly when present. When only
     * `data_path` is set, opens the file from the wire process (no
     * host involvement).
     */
    QJsonObject attachmentBytes(const QString& attachmentId);

    // -----------------------------------------------------------------
    // Artifacts (canvas_artifacts table)
    // -----------------------------------------------------------------

    /**
     * @brief Returns canvas artifacts for a conversation, history-style.
     * @param convId  Conversation UUID.
     * @returns JSON array of `{id, conversation_id, filename, language,
     *          revision, is_archived, source_msg_id, created_at,
     *          updated_at, content_size}` objects. `content` is NOT
     *          included; fetch via `canvas.read_slice` for in-place
     *          reads.
     */
    QJsonArray artifactsForConversation(const QString& convId);

    /**
     * @brief Returns full content of one artifact by `(conv_id, filename)`.
     * @param convId    Conversation UUID.
     * @param filename  Artifact filename within the conversation.
     * @returns `{filename, language, content_base64, size_bytes}` on
     *          success, empty object on miss. Artifact text is bounded at
     *          write time by kMaxContentBytes (see wire-protocol.h).
     */
    QJsonObject artifactBytes(const QString& convId, const QString& filename);

    /**
     * @brief Returns one `canvas_artifacts` row by id in the camelCase
     *        shape Android's `parseCanvasObject` expects.
     * @param canvasId  Canvas artifact UUID.
     * @returns Camel-cased canvas object matching the
     *          `CanvasService::canvasRowToMap` projection byte-for-byte,
     *          including the full canvas content body. Empty object on
     *          miss.
     *
     * Used to enrich the wire `canvas.opened` and `canvas.updated`
     * events so paired clients can populate their canvas state
     * directly from the event payload. Without this enrichment the
     * event carries only `{canvas_id, conv_id}` and the client cannot
     * reconstruct the canvas: Android's `parseCanvasObject` returns
     * null on the missing `id` field and the handler bails silently.
     */
    QJsonObject canvasArtifactById(const QString& canvasId);

    // -----------------------------------------------------------------
    // Settings table reads (raw key/value)
    // -----------------------------------------------------------------

    /**
     * @brief Reads a value from the `settings` key/value table.
     * @param key  Settings key.
     * @param def  Default returned when the key is absent.
     * @returns Stored value, or `def` when the key is missing.
     */
    QString settingsValue(const QString& key, const QString& def = QString());

    /**
     * @brief Returns one `heartbeat_configs` row by id in the camelCase
     *        shape Android's `parseHeartbeatConfigObject` expects.
     * @param configId  Heartbeat config UUID.
     * @returns Camel-cased heartbeat config object; empty object on miss.
     *
     * Used to enrich `heartbeat.config.changed` wire events.
     */
    QJsonObject heartbeatConfigById(const QString& configId);

  private:
    QString m_connectionName;
    QSqlDatabase m_db;
};

}  // namespace Verzeta::Remote

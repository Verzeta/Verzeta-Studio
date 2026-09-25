// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file message-service.cpp
 * @brief Implementation of message, attachment, and tool call CRUD.
 *        All queries use parameter binding to prevent SQL injection.
 * @layer Service
 * @dependencies DbManager (Data Access)
 */

#include "message-service.h"

#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the MessageService.
 * @param db Open DbManager singleton reference.
 * @param parent Qt parent for memory management.
 */
MessageService::MessageService(DbManager& db, QObject* parent) : QObject(parent), m_db(db) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_streamFlushTimer.setInterval(kStreamFlushIntervalMs);
    m_streamFlushTimer.setSingleShot(true);
    connect(&m_streamFlushTimer, &QTimer::timeout, this, &MessageService::flushPendingDeltas);
}

MessageService::~MessageService() {
    VERZETA_ASSERT_MAIN_THREAD();
    // Sanity check on shutdown: the streaming buffer should be empty
    // because every request lifecycle calls finalize or abort on its
    // slot. If something is still in-flight at destruction time it's
    // a lifecycle bug — emit abort signals so any surviving consumers
    // (in test harnesses, for example) get consistent state.
    if (!m_streamingSlots.isEmpty()) {
        qCWarning(verzetaDb) << "MessageService: destroying with" << m_streamingSlots.size()
                             << "orphaned streaming slots — aborting them";
        const auto ids = m_streamingSlots.keys();
        for (const QString& msgId : ids) {
            abortStreamingMessage(msgId);
        }
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/*
 * @brief Executes a prepared query and logs on failure.
 * @param q Prepared QSqlQuery to execute.
 * @param context Label for error messages.
 * @return true if execution succeeded.
 */
bool MessageService::execQuery(QSqlQuery& q, const QString& context) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!q.exec()) {
        qCWarning(verzetaDb) << context << "failed:" << q.lastError().text();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Message operations
// ---------------------------------------------------------------------------

QString MessageService::addInterveningSystemMessage(const QString& convId,
                                                    const QString& content,
                                                    const QJsonObject& metadata) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty()) {
        qCWarning(verzetaDb) << "addInterveningSystemMessage: empty convId — rejecting";
        return {};
    }

    Message msg;
    msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.conversationId = convId;
    // role=system is the ENTIRE point of this helper — not parameterised.
    // A caller that wants a different role must use addMessage directly,
    // and is responsible for making sure the row does not fall inside a
    // tool-call/tool-result pairing window (request-builder.cpp:282-311).
    msg.role = QStringLiteral("system");
    msg.content = content;
    msg.createdAt = QDateTime::currentDateTime();
    msg.finishReason = QStringLiteral("stop");
    msg.metadata = metadata;

    return addMessage(msg);
}

/*
 * @brief Inserts a message into the database.
 *        Updates conversation.token_total after insertion.
 * @param msg Message struct with pre-generated UUID id.
 * @return UUID on success, empty string on failure.
 * @sideeffects Inserts into messages, updates conversations.token_total.
 */
QString MessageService::addMessage(const Message& msg) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!msg.isValid()) {
        qCWarning(verzetaDb) << "addMessage: invalid message (empty id or unknown role)";
        return {};
    }

    // Guard: the target conversation must still exist. If it was deleted
    // between capture and persist, dropping the row is correct (and
    // avoids hitting an FK error that SQLite reports ambiguously).
    if (!conversationExists(msg.conversationId)) {
        qCWarning(verzetaDb) << "addMessage: conversation" << msg.conversationId
                             << "no longer exists — dropping message" << msg.id;
        return {};
    }

    const QString metaJson =
        msg.metadata.isEmpty()
            ? QString{}
            : QString::fromUtf8(QJsonDocument(msg.metadata).toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("INSERT INTO messages(id, conversation_id, role, content, content_html, "
                       "created_at, token_count, model_used, finish_reason, metadata, agent_id, "
                       "member_alias, turn_id, thinking_content) "
                       "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(msg.id);
    q.addBindValue(msg.conversationId);
    q.addBindValue(msg.role);
    // Qt6: null QString → QVariant::isNull() == true → SQLite binds as NULL.
    // Guard: always bind a non-null string for the NOT NULL content column.
    q.addBindValue(msg.content.isNull() ? QStringLiteral("") : msg.content);
    q.addBindValue(msg.contentHtml.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                             : QVariant(msg.contentHtml));
    q.addBindValue(msg.createdAt.isValid() ? msg.createdAt.toMSecsSinceEpoch()
                                           : QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(msg.tokenCount);
    q.addBindValue(msg.modelUsed.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                           : QVariant(msg.modelUsed));
    q.addBindValue(msg.finishReason.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                              : QVariant(msg.finishReason));
    q.addBindValue(metaJson.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                      : QVariant(metaJson));
    q.addBindValue(msg.agentId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                         : QVariant(msg.agentId));
    q.addBindValue(msg.memberAlias.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                             : QVariant(msg.memberAlias));
    q.addBindValue(msg.turnId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                        : QVariant(msg.turnId));
    // Schema v16+: thinking_content sidecar.  NOT NULL with DEFAULT ''
    // at the column level; bind a non-null QString here so a null
    // `msg.thinkingContent` (unset by older callers, by non-reasoning
    // model paths, or by the trim-to-empty discard rule) binds as ''
    // rather than tripping the constraint.  Display-only — RequestBuilder
    // and HistoryBudgeter never read this column.
    q.addBindValue(msg.thinkingContent.isNull() ? QStringLiteral("") : msg.thinkingContent);

    if (!execQuery(q, QStringLiteral("addMessage"))) {
        return {};
    }

    // Update cumulative token total on the conversation
    if (msg.tokenCount > 0) {
        QSqlQuery updateQ(m_db.db());
        updateQ.prepare(
            QStringLiteral("UPDATE conversations SET token_total = token_total + ?, updated_at = ? "
                           "WHERE id = ?"));
        updateQ.addBindValue(msg.tokenCount);
        updateQ.addBindValue(QDateTime::currentMSecsSinceEpoch());
        updateQ.addBindValue(msg.conversationId);
        execQuery(updateQ, QStringLiteral("addMessage/updateTokenTotal"));
    }

    if ((msg.role == QStringLiteral("user") || msg.role == QStringLiteral("assistant")) &&
        !msg.content.isEmpty()) {
        QSqlQuery ftsQ(m_db.db());
        ftsQ.prepare(
            QStringLiteral("INSERT INTO messages_fts(content, message_id, conversation_id) "
                           "VALUES(?, ?, ?)"));
        ftsQ.addBindValue(msg.content);
        ftsQ.addBindValue(msg.id);
        ftsQ.addBindValue(msg.conversationId);
        execQuery(ftsQ, QStringLiteral("addMessage/ftsInsert"));
    }

    emit messageAdded(msg.conversationId, msg.id);
    return msg.id;
}

/*
 * @brief Retrieves ALL messages for a conversation in chronological order.
 * @param convId Conversation UUID.
 * @return Complete list of Message structs in created_at ASC order.
 */
QList<Message> MessageService::getMessages(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM messages WHERE conversation_id = ? "
                             "ORDER BY created_at ASC"));
    q.addBindValue(convId);

    if (!execQuery(q, QStringLiteral("getMessages"))) {
        return {};
    }

    QList<Message> results;
    while (q.next()) {
        results.append(Message::fromSqlRecord(q.record()));
    }
    return results;
}

/*
 * @brief Retrieves a single message by its UUID.
 *        Required by HeartbeatSubagentService for
 *        msgId → convId resolution from StreamingManager::streamFinalized.
 * @param id Message UUID.
 * @return The Message struct on hit; default-constructed Message on miss.
 */
Message MessageService::getMessage(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (id.isEmpty()) {
        return {};
    }
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM messages WHERE id = ? LIMIT 1"));
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("getMessage"))) {
        return {};
    }

    if (!q.next()) {
        return {};
    }
    return Message::fromSqlRecord(q.record());
}

/*
 * @brief Retrieves a page of messages for explicit pagination.
 * @param convId Conversation UUID.
 * @param limit Maximum messages to return.
 * @param offset Messages to skip from the start.
 * @return List of Message structs in created_at ASC order.
 */
QList<Message> MessageService::getMessagePage(const QString& convId, int limit, int offset) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM messages WHERE conversation_id = ? "
                             "ORDER BY created_at ASC LIMIT ? OFFSET ?"));
    q.addBindValue(convId);
    q.addBindValue(limit);
    q.addBindValue(offset);

    if (!execQuery(q, QStringLiteral("getMessagePage"))) {
        return {};
    }

    QList<Message> results;
    while (q.next()) {
        results.append(Message::fromSqlRecord(q.record()));
    }
    return results;
}

int MessageService::deleteAllForConversation(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return -1;

    // tool_calls FK-references messages — delete children first.
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("DELETE FROM tool_calls WHERE message_id IN "
                                 "(SELECT id FROM messages WHERE conversation_id = ?)"));
        q.addBindValue(convId);
        if (!q.exec()) {
            qCWarning(verzetaDb) << "deleteAllForConversation: tool_calls DELETE failed:"
                                 << q.lastError().text();
            return -1;
        }
    }
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM messages WHERE conversation_id = ?"));
    q.addBindValue(convId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "deleteAllForConversation: messages DELETE failed:"
                             << q.lastError().text();
        return -1;
    }
    const int deleted = q.numRowsAffected();
    clearEphemeralForConversation(convId);
    qCDebug(verzetaDb) << "deleteAllForConversation: wiped" << deleted << "messages from"
                       << convId.left(8);
    return deleted;
}

/*
 * @brief Retrieves the most recent N messages, returned in ascending order.
 * @param convId Conversation UUID.
 * @param maxRows Maximum rows to load from the newest end.
 * @return List of Message structs in created_at ASC order (reversed from query).
 */
QList<Message> MessageService::getRecentMessages(const QString& convId, int maxRows) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM messages WHERE conversation_id = ? "
                             "ORDER BY created_at DESC LIMIT ?"));
    q.addBindValue(convId);
    q.addBindValue(maxRows);

    if (!execQuery(q, QStringLiteral("getRecentMessages"))) {
        return {};
    }

    QList<Message> results;
    while (q.next()) {
        results.append(Message::fromSqlRecord(q.record()));
    }
    // Reverse to ascending chronological order.
    std::reverse(results.begin(), results.end());
    return results;
}

/*
 * @brief Deletes a message by ID.
 * @param id Message UUID.
 * @return true if a row was deleted.
 */
bool MessageService::deleteMessage(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Retrieve conversation_id before deleting (for signal)
    QString convId;
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("SELECT conversation_id FROM messages WHERE id = ? LIMIT 1"));
        q.addBindValue(id);
        if (execQuery(q, QStringLiteral("deleteMessage/getConvId")) && q.next()) {
            convId = q.value(0).toString();
        }
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM messages WHERE id = ?"));
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("deleteMessage"))) {
        return false;
    }

    if (q.numRowsAffected() == 0) {
        return false;
    }

    if (!convId.isEmpty()) {
        emit messageDeleted(convId, id);
    }
    return true;
}

bool MessageService::markMessageAsEchoFailed(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();

    // Read the existing row so we can rewrite content + metadata
    // atomically. Skip if the row already carries the marker — the
    // method must be idempotent in case the cascade-controller
    // emits exhaustion twice for the same id under odd timing.
    QString convId;
    QString existingContent;
    QString existingMetadata;
    {
        QSqlQuery lookup(m_db.db());
        lookup.prepare(QStringLiteral("SELECT conversation_id, content, metadata "
                                      "FROM messages WHERE id = ? LIMIT 1"));
        lookup.addBindValue(id);
        if (!execQuery(lookup, QStringLiteral("markMessageAsEchoFailed/lookup")) ||
            !lookup.next()) {
            return false;
        }
        convId = lookup.value(0).toString();
        existingContent = lookup.value(1).toString();
        existingMetadata = lookup.value(2).toString();
    }

    static const QString kSentinel = QStringLiteral("[ECHO-FAILED — agent could not produce an "
                                                    "original reply after multiple retries]\n\n");
    if (existingContent.startsWith(kSentinel)) {
        // Idempotent — already marked.
        return true;
    }

    // Stamp the metadata flag (preserve any existing keys). Tolerate
    // an empty metadata cell on legacy rows.
    QJsonObject metaObj;
    if (!existingMetadata.isEmpty()) {
        const auto doc = QJsonDocument::fromJson(existingMetadata.toUtf8());
        if (doc.isObject())
            metaObj = doc.object();
    }
    metaObj[QStringLiteral("echo_failed")] = true;
    const QString metadataJson =
        QString::fromUtf8(QJsonDocument(metaObj).toJson(QJsonDocument::Compact));

    const QString newContent = kSentinel + existingContent;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE messages SET content = ?, metadata = ? WHERE id = ?"));
    q.addBindValue(newContent);
    q.addBindValue(metadataJson);
    q.addBindValue(id);
    if (!execQuery(q, QStringLiteral("markMessageAsEchoFailed/update"))) {
        return false;
    }
    if (!convId.isEmpty()) {
        emit messageUpdated(convId, id);
    }
    return true;
}

/*
 * @brief Updates the pre-rendered HTML cache for a message.
 *        Emits messageUpdated on success so the model can refresh the row.
 * @param id Message UUID.
 * @param html Pre-rendered HTML string.
 * @return true if update succeeded.
 */
bool MessageService::updateContentHtml(const QString& id, const QString& html) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Look up conversation id first so the signal can carry it
    QString convId;
    {
        QSqlQuery lookup(m_db.db());
        lookup.prepare(QStringLiteral("SELECT conversation_id FROM messages WHERE id = ? LIMIT 1"));
        lookup.addBindValue(id);
        if (execQuery(lookup, QStringLiteral("updateContentHtml/getConvId")) && lookup.next()) {
            convId = lookup.value(0).toString();
        }
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE messages SET content_html = ? WHERE id = ?"));
    q.addBindValue(html);
    q.addBindValue(id);
    if (!execQuery(q, QStringLiteral("updateContentHtml"))) {
        return false;
    }
    if (!convId.isEmpty()) {
        emit messageUpdated(convId, id);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Streaming API — in-memory buffer, signal-based UI, DB only on finalize
// ---------------------------------------------------------------------------

void MessageService::beginStreamingMessage(const QString& convId,
                                           const QString& msgId,
                                           const QString& role,
                                           const QString& agentId,
                                           const QString& memberAlias) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty() || msgId.isEmpty()) {
        qCWarning(verzetaDb) << "beginStreamingMessage: empty convId or msgId";
        return;
    }
    if (m_streamingSlots.contains(msgId)) {
        qCWarning(verzetaDb) << "beginStreamingMessage: msgId already streaming" << msgId;
        return;
    }
    // Guard: rejecting streams for deleted conversations prevents
    // accumulating content that can never be persisted.
    if (!conversationExists(convId)) {
        qCWarning(verzetaDb) << "beginStreamingMessage: conversation" << convId
                             << "does not exist — refusing to start stream" << msgId;
        return;
    }
    StreamingSlot slot;
    slot.convId = convId;
    slot.role = role;
    slot.agentId = agentId;
    slot.memberAlias = memberAlias;
    slot.startedAt = QDateTime::currentDateTimeUtc();
    m_streamingSlots.insert(msgId, slot);
    emit messageStreamingStarted(convId, msgId, role, agentId, memberAlias);
}

void MessageService::appendStreamingChunk(const QString& msgId, const QString& delta) {
    VERZETA_ASSERT_MAIN_THREAD();
    auto it = m_streamingSlots.find(msgId);
    if (it == m_streamingSlots.end() || delta.isEmpty())
        return;
    it->content += delta;
    m_pendingDeltas[msgId] += delta;
    if (!m_streamFlushTimer.isActive()) {
        m_streamFlushTimer.start();
    }
}

void MessageService::flushPendingDeltas() {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_pendingDeltas.isEmpty())
        return;
    QHash<QString, QString> drained;
    drained.swap(m_pendingDeltas);
    for (auto it = drained.constBegin(); it != drained.constEnd(); ++it) {
        const QString& msgId = it.key();
        const QString& delta = it.value();
        if (delta.isEmpty())
            continue;
        const auto sit = m_streamingSlots.constFind(msgId);
        if (sit == m_streamingSlots.constEnd())
            continue;
        emit messageContentStreamed(sit->convId, msgId, delta);
    }
}

void MessageService::rewriteStreamingContent(const QString& msgId, const QString& newContent) {
    VERZETA_ASSERT_MAIN_THREAD();
    auto pit = m_pendingDeltas.find(msgId);
    if (pit != m_pendingDeltas.end()) {
        if (!pit.value().isEmpty()) {
            const auto sit = m_streamingSlots.constFind(msgId);
            if (sit != m_streamingSlots.constEnd()) {
                emit messageContentStreamed(sit->convId, msgId, pit.value());
            }
        }
        m_pendingDeltas.erase(pit);
    }
    auto it = m_streamingSlots.find(msgId);
    if (it == m_streamingSlots.end())
        return;
    if (it->content == newContent)
        return;
    it->content = newContent;
    emit messageContentRewritten(it->convId, msgId, newContent);
}

bool MessageService::finalizeStreamingMessage(const QString& msgId,
                                              int tokenCount,
                                              const QString& finishReason,
                                              const QString& contentHtml,
                                              const QString& modelUsed,
                                              const QJsonObject& metadata,
                                              const QString& thinkingContent) {
    VERZETA_ASSERT_MAIN_THREAD();
    auto pit = m_pendingDeltas.find(msgId);
    if (pit != m_pendingDeltas.end()) {
        if (!pit.value().isEmpty()) {
            const auto sit = m_streamingSlots.constFind(msgId);
            if (sit != m_streamingSlots.constEnd()) {
                emit messageContentStreamed(sit->convId, msgId, pit.value());
            }
        }
        m_pendingDeltas.erase(pit);
    }
    auto it = m_streamingSlots.find(msgId);
    if (it == m_streamingSlots.end()) {
        qCWarning(verzetaDb) << "finalizeStreamingMessage: no streaming slot for" << msgId;
        return false;
    }
    const StreamingSlot slot = *it;
    m_streamingSlots.erase(it);

    Message msg;
    msg.id = msgId;
    msg.conversationId = slot.convId;
    msg.role = slot.role;
    msg.content = slot.content;
    msg.contentHtml = contentHtml;
    msg.createdAt = slot.startedAt.isValid() ? slot.startedAt : QDateTime::currentDateTimeUtc();
    msg.tokenCount = tokenCount;
    msg.modelUsed = modelUsed;
    msg.finishReason = finishReason;
    msg.metadata = metadata;
    msg.agentId = slot.agentId;
    msg.memberAlias = slot.memberAlias;
    // Display-only reasoning sidecar.  StreamingManager applies the
    // trim-to-empty discard rule before calling us, so an empty
    // value here means "no thinking to persist" and the NOT NULL
    // DEFAULT '' on the column handles the bind cleanly via
    // addMessage's null-guard.
    msg.thinkingContent = thinkingContent;

    const bool ok = !addMessage(msg).isEmpty();
    if (!ok) {
        // addMessage failed (conversation deleted, FK violation, disk
        // error, ...). The slot is already gone from the buffer, so
        // the UI row is now an orphan with isStreaming=true. Emit the
        // abort signal so MessageListModel drops the placeholder row
        // cleanly instead of leaving a stuck "typing" bubble.
        qCWarning(verzetaDb) << "finalizeStreamingMessage: addMessage failed for" << msgId
                             << "— emitting abort to clear placeholder";
        emit messageStreamingAborted(slot.convId, msgId);
    }
    return ok;
}

void MessageService::abortStreamingMessage(const QString& msgId) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_pendingDeltas.remove(msgId);
    auto it = m_streamingSlots.find(msgId);
    if (it == m_streamingSlots.end())
        return;
    const QString convId = it->convId;
    m_streamingSlots.erase(it);
    emit messageStreamingAborted(convId, msgId);
}

bool MessageService::hasStreamingMessage(const QString& msgId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_streamingSlots.contains(msgId);
}

QString MessageService::streamingContent(const QString& msgId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto it = m_streamingSlots.constFind(msgId);
    if (it == m_streamingSlots.constEnd())
        return {};
    return it->content;
}

QString MessageService::streamingConversationId(const QString& msgId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto it = m_streamingSlots.constFind(msgId);
    if (it == m_streamingSlots.constEnd())
        return {};
    return it->convId;
}

QList<Message> MessageService::streamingMessagesForConversation(const QString& convId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<Message> out;
    for (auto it = m_streamingSlots.constBegin(); it != m_streamingSlots.constEnd(); ++it) {
        if (it->convId != convId)
            continue;
        Message m;
        m.id = it.key();
        m.conversationId = it->convId;
        m.role = it->role;
        m.content = it->content;
        m.createdAt = it->startedAt;
        m.agentId = it->agentId;
        m.memberAlias = it->memberAlias;
        out.append(m);
    }
    return out;
}

void MessageService::postEphemeralMessage(const Message& msg) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (msg.conversationId.isEmpty() || msg.id.isEmpty()) {
        qCWarning(verzetaDb) << "postEphemeralMessage: empty convId or msgId";
        return;
    }
    m_ephemeralMessages[msg.conversationId].append(msg);
    emit messageEphemeralPosted(msg.conversationId, msg.id);
}

void MessageService::clearEphemeralForConversation(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return;
    auto it = m_ephemeralMessages.find(convId);
    if (it == m_ephemeralMessages.end() || it.value().isEmpty()) {
        return;
    }
    it.value().clear();
    emit messageEphemeralCleared(convId);
}

QList<Message> MessageService::ephemeralMessagesForConversation(const QString& convId) const {
    return m_ephemeralMessages.value(convId);
}

int MessageService::abortStreamingMessagesForConversation(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return 0;

    // Collect matching ids first so we don't mutate the hash while iterating.
    QStringList targets;
    for (auto it = m_streamingSlots.constBegin(); it != m_streamingSlots.constEnd(); ++it) {
        if (it->convId == convId)
            targets.append(it.key());
    }
    for (const QString& msgId : targets) {
        abortStreamingMessage(msgId);
    }
    return targets.size();
}

bool MessageService::conversationExists(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT 1 FROM conversations WHERE id = ? LIMIT 1"));
    q.addBindValue(convId);
    if (!execQuery(q, QStringLiteral("conversationExists")))
        return false;
    return q.next();
}

void MessageService::onConversationDeleted(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    const int aborted = abortStreamingMessagesForConversation(convId);
    if (aborted > 0) {
        qCInfo(verzetaDb) << "MessageService: aborted" << aborted
                          << "streaming slots after conversation delete" << convId;
    }
    // Drop ephemeral rows so memory doesn't accumulate.
    m_ephemeralMessages.remove(convId);
}

/**
 * @brief Returns the cumulative token count for a conversation.
 * @param convId Conversation UUID.
 * @return token_total from conversations table, or 0 on error.
 */
int MessageService::getTokenTotal(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT token_total FROM conversations WHERE id = ? LIMIT 1"));
    q.addBindValue(convId);

    if (!execQuery(q, QStringLiteral("getTokenTotal")) || !q.next()) {
        return 0;
    }
    return q.value(0).toInt();
}

// ---------------------------------------------------------------------------
// Attachment operations
// ---------------------------------------------------------------------------

/**
 * @brief Inserts an attachment into the database.
 * @param attachment Attachment struct with pre-generated UUID id.
 * @return UUID on success, empty string on failure.
 */
QString MessageService::addAttachment(const Attachment& attachment) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!attachment.isValid()) {
        qCWarning(verzetaDb) << "addAttachment: invalid attachment";
        return {};
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO attachments(id, message_id, type, filename, mime_type, "
                             "data_path, data_inline, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(attachment.id);
    q.addBindValue(attachment.messageId);
    q.addBindValue(attachment.type);
    q.addBindValue(attachment.filename);
    q.addBindValue(attachment.mimeType.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                                 : QVariant(attachment.mimeType));
    q.addBindValue(attachment.dataPath.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                                 : QVariant(attachment.dataPath));
    q.addBindValue(attachment.dataInline.isEmpty() ? QVariant(QMetaType(QMetaType::QByteArray))
                                                   : QVariant(attachment.dataInline));
    q.addBindValue(attachment.createdAt.isValid() ? attachment.createdAt.toMSecsSinceEpoch()
                                                  : QDateTime::currentMSecsSinceEpoch());

    if (!execQuery(q, QStringLiteral("addAttachment"))) {
        return {};
    }

    return attachment.id;
}

/**
 * @brief Retrieves all attachments for a message.
 * @param messageId Message UUID.
 * @return List of Attachment structs.
 */
QList<Attachment> MessageService::getAttachments(const QString& messageId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("SELECT * FROM attachments WHERE message_id = ? ORDER BY created_at ASC"));
    q.addBindValue(messageId);

    if (!execQuery(q, QStringLiteral("getAttachments"))) {
        return {};
    }

    QList<Attachment> results;
    while (q.next()) {
        results.append(Attachment::fromSqlRecord(q.record()));
    }
    return results;
}

/**
 * @brief Deletes an attachment by ID.
 * @param id Attachment UUID.
 * @return true if a row was deleted.
 */
bool MessageService::deleteAttachment(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM attachments WHERE id = ?"));
    q.addBindValue(id);
    return execQuery(q, QStringLiteral("deleteAttachment")) && q.numRowsAffected() > 0;
}

// ---------------------------------------------------------------------------
// ToolCall operations
// ---------------------------------------------------------------------------

/**
 * @brief Inserts a tool call record linked to a message.
 * @param toolCall ToolCall struct with pre-generated UUID id.
 * @return UUID on success, empty string on failure.
 */
QString MessageService::addToolCall(const ToolCall& toolCall) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!toolCall.isValid()) {
        qCWarning(verzetaDb) << "addToolCall: invalid tool call";
        return {};
    }

    const QString argsJson =
        QString::fromUtf8(QJsonDocument(toolCall.arguments).toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO tool_calls(id, message_id, tool_name, arguments, status, "
                             "started_at, plan_step_id) "
                             "VALUES(?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(toolCall.id);
    q.addBindValue(toolCall.messageId);
    q.addBindValue(toolCall.toolName);
    q.addBindValue(argsJson);
    q.addBindValue(toolCall.status);
    q.addBindValue(toolCall.startedAt.isValid() ? QVariant(toolCall.startedAt.toMSecsSinceEpoch())
                                                : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(toolCall.planStepId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                                 : QVariant(toolCall.planStepId));

    if (!execQuery(q, QStringLiteral("addToolCall"))) {
        return {};
    }

    // Look up the parent message's conversation id so the signal carries
    // enough to route to a filtered model (one conv per tool log view).
    QString convId;
    {
        QSqlQuery lookup(m_db.db());
        lookup.prepare(QStringLiteral("SELECT conversation_id FROM messages WHERE id = ? LIMIT 1"));
        lookup.addBindValue(toolCall.messageId);
        if (execQuery(lookup, QStringLiteral("addToolCall/convLookup")) && lookup.next()) {
            convId = lookup.value(0).toString();
        }
    }
    emit toolCallAdded(convId, toolCall.id);
    return toolCall.id;
}

/**
 * @brief Updates the result and status of a completed tool call.
 * @param id Tool call UUID.
 * @param result JSON result value.
 * @param status "success" or "error".
 * @return true if update succeeded.
 */
bool MessageService::updateToolCallResult(const QString& id,
                                          const QJsonValue& result,
                                          const QString& status) {
    VERZETA_ASSERT_MAIN_THREAD();
    QJsonDocument resultDoc;
    if (result.isObject()) {
        resultDoc = QJsonDocument(result.toObject());
    } else if (result.isArray()) {
        resultDoc = QJsonDocument(result.toArray());
    }

    const QString resultJson = resultDoc.isEmpty()
                                   ? result.toString()
                                   : QString::fromUtf8(resultDoc.toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE tool_calls SET result = ?, status = ?, completed_at = ? WHERE id = ?"));
    q.addBindValue(resultJson);
    q.addBindValue(status);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    if (!execQuery(q, QStringLiteral("updateToolCallResult"))) {
        return false;
    }

    // Resolve the conversation id for the signal payload.
    QString convId;
    {
        QSqlQuery lookup(m_db.db());
        lookup.prepare(QStringLiteral("SELECT m.conversation_id FROM tool_calls tc "
                                      "JOIN messages m ON m.id = tc.message_id "
                                      "WHERE tc.id = ? LIMIT 1"));
        lookup.addBindValue(id);
        if (execQuery(lookup, QStringLiteral("updateToolCallResult/convLookup")) && lookup.next()) {
            convId = lookup.value(0).toString();
        }
    }
    emit toolCallUpdated(convId, id);
    return true;
}

ToolCall MessageService::getToolCall(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM tool_calls WHERE id = ? LIMIT 1"));
    q.addBindValue(id);
    if (!execQuery(q, QStringLiteral("getToolCall")) || !q.next()) {
        return {};
    }
    return ToolCall::fromSqlRecord(q.record());
}

/**
 * @brief Retrieves all tool calls for a message.
 * @param messageId Message UUID.
 * @return List of ToolCall structs.
 */
QList<ToolCall> MessageService::getToolCalls(const QString& messageId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("SELECT * FROM tool_calls WHERE message_id = ? ORDER BY started_at ASC"));
    q.addBindValue(messageId);

    if (!execQuery(q, QStringLiteral("getToolCalls"))) {
        return {};
    }

    QList<ToolCall> results;
    while (q.next()) {
        results.append(ToolCall::fromSqlRecord(q.record()));
    }
    return results;
}

QHash<QString, QList<ToolCall>>
MessageService::getToolCallsForMessages(const QStringList& messageIds) {
    VERZETA_ASSERT_MAIN_THREAD();
    QHash<QString, QList<ToolCall>> out;
    if (messageIds.isEmpty())
        return out;

    QStringList placeholders;
    placeholders.reserve(messageIds.size());
    for (int i = 0; i < messageIds.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
    }
    const QString sql = QStringLiteral("SELECT * FROM tool_calls WHERE message_id IN (%1) "
                                       "ORDER BY started_at ASC")
                            .arg(placeholders.join(QLatin1Char(',')));

    QSqlQuery q(m_db.db());
    q.prepare(sql);
    for (const QString& id : messageIds)
        q.addBindValue(id);
    if (!execQuery(q, QStringLiteral("getToolCallsForMessages"))) {
        return out;
    }
    while (q.next()) {
        const ToolCall tc = ToolCall::fromSqlRecord(q.record());
        out[tc.messageId].append(tc);
    }
    return out;
}

/**
 * @brief Returns every tool call whose anchor assistant message belongs
 *        to the same `turn_id` as the message identified by `messageId`,
 *        optionally constrained to a single responding agent.
 *
 * Used by the per-turn UI tool-chain chip so that 10 internal tool
 * iterations across multiple LLM round-trips collapse into one summary
 * card in the chat. The grouping key is `(messages.turn_id,
 * messages.agent_id)`, kept denormalized for query locality.
 */
QList<ToolCall> MessageService::toolCallsForTurn(const QString& messageId, const QString& agentId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<ToolCall> results;
    if (messageId.isEmpty())
        return results;

    QString sql = QStringLiteral("SELECT tc.* FROM tool_calls tc "
                                 "JOIN messages m ON m.id = tc.message_id "
                                 "WHERE m.turn_id = (SELECT turn_id FROM messages WHERE id = ?) "
                                 "  AND m.turn_id IS NOT NULL ");

    if (!agentId.isEmpty()) {
        sql += QStringLiteral("AND m.agent_id = ? ");
    }
    sql += QStringLiteral("ORDER BY tc.started_at ASC");

    QSqlQuery q(m_db.db());
    q.prepare(sql);
    q.addBindValue(messageId);
    if (!agentId.isEmpty()) {
        q.addBindValue(agentId);
    }

    if (!execQuery(q, QStringLiteral("toolCallsForTurn"))) {
        return results;
    }
    while (q.next()) {
        results.append(ToolCall::fromSqlRecord(q.record()));
    }
    return results;
}

/**
 * @brief Returns every tool call across an entire conversation,
 *        ordered chronologically. Used by the Tool Log panel to
 *        populate from a single source.
 */
QList<ToolCall> MessageService::toolCallsForConversation(const QString& conversationId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<ToolCall> results;
    if (conversationId.isEmpty())
        return results;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT tc.* FROM tool_calls tc "
                             "JOIN messages m ON m.id = tc.message_id "
                             "WHERE m.conversation_id = ? "
                             "ORDER BY tc.started_at ASC"));
    q.addBindValue(conversationId);

    if (!execQuery(q, QStringLiteral("toolCallsForConversation"))) {
        return results;
    }
    while (q.next()) {
        results.append(ToolCall::fromSqlRecord(q.record()));
    }
    return results;
}

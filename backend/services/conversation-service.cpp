// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-service.cpp
 * @brief Implementation of conversation and folder CRUD operations.
 *        All queries use parameter binding to prevent SQL injection.
 * @layer Service
 * @dependencies DbManager (Data Access)
 */

#include "conversation-service.h"

#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <algorithm>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

// ---------------------------------------------------------------------------
// Folder serialization
// ---------------------------------------------------------------------------

QJsonObject Folder::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("name")] = name;
    obj[QStringLiteral("parent_id")] = parentId;
    obj[QStringLiteral("created_at")] = createdAt.toMSecsSinceEpoch();
    obj[QStringLiteral("folder_type")] = folderType;
    obj[QStringLiteral("goal")] = goal;
    obj[QStringLiteral("description")] = description;
    QJsonArray agentsArr;
    for (const QString& a : agentIds)
        agentsArr.append(a);
    obj[QStringLiteral("agent_ids")] = agentsArr;
    return obj;
}

Folder Folder::fromJson(const QJsonObject& json) {
    Folder f;
    f.id = json[QStringLiteral("id")].toString();
    f.name = json[QStringLiteral("name")].toString();
    f.parentId = json[QStringLiteral("parent_id")].toString();
    f.createdAt = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(json[QStringLiteral("created_at")].toDouble()));
    f.folderType = json[QStringLiteral("folder_type")].toString(QStringLiteral("regular"));
    f.goal = json[QStringLiteral("goal")].toString();
    f.description = json[QStringLiteral("description")].toString();
    const QJsonArray agentsArr = json[QStringLiteral("agent_ids")].toArray();
    for (const QJsonValue& v : agentsArr)
        f.agentIds.append(v.toString());
    return f;
}

Folder Folder::fromSqlRecord(const QSqlRecord& record) {
    Folder f;
    f.id = record.value(QStringLiteral("id")).toString();
    f.name = record.value(QStringLiteral("name")).toString();
    f.parentId = record.value(QStringLiteral("parent_id")).toString();
    f.createdAt =
        QDateTime::fromMSecsSinceEpoch(record.value(QStringLiteral("created_at")).toLongLong());

    // v2 fields — guard against older schema versions
    if (record.contains(QStringLiteral("folder_type"))) {
        const QString ft = record.value(QStringLiteral("folder_type")).toString();
        if (!ft.isEmpty())
            f.folderType = ft;
    }
    if (record.contains(QStringLiteral("goal"))) {
        f.goal = record.value(QStringLiteral("goal")).toString();
    }
    if (record.contains(QStringLiteral("description"))) {
        f.description = record.value(QStringLiteral("description")).toString();
    }
    if (record.contains(QStringLiteral("agent_ids"))) {
        const QString agentsJson = record.value(QStringLiteral("agent_ids")).toString();
        if (!agentsJson.isEmpty()) {
            const QJsonArray arr = QJsonDocument::fromJson(agentsJson.toUtf8()).array();
            for (const QJsonValue& v : arr)
                f.agentIds.append(v.toString());
        }
    }
    return f;
}

// ---------------------------------------------------------------------------
// ConversationService
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the ConversationService.
 * @param db Open DbManager singleton reference.
 * @param parent Qt parent for memory management.
 */
ConversationService::ConversationService(DbManager& db, QObject* parent)
    : QObject(parent), m_db(db) {
    VERZETA_ASSERT_MAIN_THREAD();
}

// -----------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------

/**
 * @brief Executes a prepared query and emits an error log on failure.
 * @param q Prepared QSqlQuery to execute.
 * @param context Human-readable label for error messages.
 * @return true if q.exec() succeeded.
 */
bool ConversationService::execQuery(QSqlQuery& q, const QString& context) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!q.exec()) {
        qCWarning(verzetaDb) << context << "failed:" << q.lastError().text();
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------
// Conversation operations
// -----------------------------------------------------------------------

/*
 * @brief Creates a new conversation.
 * @param title Display title.
 * @param folderId Optional folder UUID.
 * @return UUID of created conversation, or empty on failure.
 * @sideeffects Inserts into conversations table.
 */
QString ConversationService::createConversation(const QString& title, const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("INSERT INTO conversations(id, title, folder_id, created_at, updated_at) "
                       "VALUES(?, ?, ?, ?, ?)"));
    q.addBindValue(id);
    q.addBindValue(title);
    q.addBindValue(folderId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                      : QVariant(folderId));
    q.addBindValue(now);
    q.addBindValue(now);

    if (!execQuery(q, QStringLiteral("createConversation"))) {
        return {};
    }

    qCInfo(verzetaDb) << "Created conversation" << id << title;
    emit conversationCreated(id);
    return id;
}

QString ConversationService::createGroupConversation(const QString& title,
                                                     const QStringList& agentIds,
                                                     const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (agentIds.isEmpty())
        return {};

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QJsonArray arr;
    for (const QString& a : agentIds)
        arr.append(a);
    const QString agentsJson = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("INSERT INTO conversations(id, title, folder_id, created_at, updated_at, "
                       "is_group, group_agent_ids) VALUES(?, ?, ?, ?, ?, 1, ?)"));
    q.addBindValue(id);
    q.addBindValue(title);
    q.addBindValue(folderId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                      : QVariant(folderId));
    q.addBindValue(now);
    q.addBindValue(now);
    q.addBindValue(agentsJson);

    if (!execQuery(q, QStringLiteral("createGroupConversation"))) {
        return {};
    }

    qCInfo(verzetaDb) << "Created group conversation" << id << title << "with" << agentIds.size()
                      << "agents";
    emit conversationCreated(id);
    return id;
}

bool ConversationService::updateGroupAgents(const QString& convId, const QStringList& agentIds) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return false;

    QJsonArray arr;
    for (const QString& a : agentIds)
        arr.append(a);
    const QString agentsJson = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE conversations SET group_agent_ids = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(agentsJson);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(convId);

    if (!execQuery(q, QStringLiteral("updateGroupAgents"))) {
        return false;
    }

    emit conversationUpdated(convId);
    return true;
}

/*
 * @brief Deletes a conversation (cascade deletes messages, attachments, tool_calls).
 * @param id UUID of the conversation.
 * @return true if deletion succeeded.
 */
bool ConversationService::deleteConversation(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM conversations WHERE id = ?"));
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("deleteConversation"))) {
        return false;
    }

    if (q.numRowsAffected() == 0) {
        qCWarning(verzetaDb) << "deleteConversation: no row deleted for id" << id;
        return false;
    }

    emit conversationDeleted(id);
    return true;
}

/*
 * @brief Renames a conversation.
 * @param id UUID of the conversation.
 * @param title New title.
 * @return true if update succeeded.
 */
bool ConversationService::renameConversation(const QString& id, const QString& title) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (title.isEmpty()) {
        qCWarning(verzetaDb) << "renameConversation: title must not be empty";
        return false;
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE conversations SET title = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(title);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("renameConversation"))) {
        return false;
    }

    emit conversationUpdated(id);
    return true;
}

bool ConversationService::clearAuxiliaryContent(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return false;
    bool ok = true;
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("DELETE FROM canvas_artifacts WHERE conversation_id = ?"));
        q.addBindValue(convId);
        if (!execQuery(q, QStringLiteral("clearAuxiliaryContent.canvas"))) {
            ok = false;
        }
    }
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("UPDATE conversations SET token_total = 0 WHERE id = ?"));
        q.addBindValue(convId);
        if (!execQuery(q, QStringLiteral("clearAuxiliaryContent.tokens"))) {
            ok = false;
        }
    }
    return ok;
}

/*
 * @brief Moves a conversation to a folder (or root if folderId is empty).
 * @param convId UUID of the conversation.
 * @param folderId Target folder UUID, or empty for root.
 * @return true if update succeeded.
 */
bool ConversationService::moveToFolder(const QString& convId, const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("UPDATE conversations SET folder_id = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(folderId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                      : QVariant(folderId));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(convId);

    if (!execQuery(q, QStringLiteral("moveToFolder"))) {
        return false;
    }

    emit conversationFolderChanged(convId);
    emit conversationUpdated(convId);
    return true;
}

/*
 * @brief Lists conversations in a folder or at root level.
 * @param folderId If empty, returns root conversations (folder_id IS NULL).
 * @return List of Conversation structs ordered by updated_at DESC.
 */
QList<Conversation> ConversationService::listConversations(const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());

    if (folderId.isEmpty()) {
        q.prepare(QStringLiteral(
            "SELECT * FROM conversations WHERE folder_id IS NULL ORDER BY updated_at DESC"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT * FROM conversations WHERE folder_id = ? ORDER BY updated_at DESC"));
        q.addBindValue(folderId);
    }

    if (!execQuery(q, QStringLiteral("listConversations"))) {
        return {};
    }

    QList<Conversation> results;
    while (q.next()) {
        results.append(Conversation::fromSqlRecord(q.record()));
    }
    return results;
}

/**
 * @brief Lists all conversations across all folders.
 * @return Full list ordered by updated_at DESC.
 */
QList<Conversation> ConversationService::listAllConversations() {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM conversations ORDER BY updated_at DESC"));

    if (!execQuery(q, QStringLiteral("listAllConversations"))) {
        return {};
    }

    QList<Conversation> results;
    while (q.next()) {
        results.append(Conversation::fromSqlRecord(q.record()));
    }
    return results;
}

/*
 * @brief Retrieves a single conversation by ID.
 * @param id UUID of the conversation.
 * @return Conversation in std::optional, or std::nullopt if not found.
 */
std::optional<Conversation> ConversationService::getConversation(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM conversations WHERE id = ? LIMIT 1"));
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("getConversation"))) {
        return std::nullopt;
    }

    if (q.next()) {
        return Conversation::fromSqlRecord(q.record());
    }
    return std::nullopt;
}

/*
 * @brief Updates the LLM configuration for a conversation.
 * @param id UUID of the conversation.
 * @param config New LlmConfig.
 * @return true if update succeeded.
 */
bool ConversationService::updateLlmConfig(const QString& id, const LlmConfig& config) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString configJson =
        QString::fromUtf8(QJsonDocument(config.toJson()).toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("UPDATE conversations SET llm_config = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(configJson);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("updateLlmConfig"))) {
        return false;
    }

    emit conversationUpdated(id);
    return true;
}

/*
 * @brief Patches only the heartbeat-gate fields on a
 *        conversation's llm_config JSON. Reads the existing config,
 *        mutates the two keys, writes via updateLlmConfig.
 * @param id        Conversation UUID.
 * @param allow     Gate value.
 * @param maxPerDay Daily cap (clamped to >= 1).
 * @return true on success.
 */
bool ConversationService::setHeartbeatAutoSurface(const QString& id, bool allow, int maxPerDay) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto existing = getConversation(id);
    if (!existing) {
        return false;
    }
    LlmConfig cfg = LlmConfig::fromJson(existing->llmConfig);
    cfg.allowHeartbeatAutoSurface = allow;
    cfg.autoSurfaceMaxPerDay = std::max(1, maxPerDay);
    return updateLlmConfig(id, cfg);
}

/*
 * @brief Updates the system prompt for a conversation.
 * @param id UUID of the conversation.
 * @param prompt New system prompt text.
 * @return true if update succeeded.
 */
bool ConversationService::updateSystemPrompt(const QString& id, const QString& prompt) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("UPDATE conversations SET system_prompt = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(prompt);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("updateSystemPrompt"))) {
        return false;
    }

    emit conversationUpdated(id);
    return true;
}

bool ConversationService::updatePrimaryAgent(const QString& id, const QString& agentId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE conversations SET primary_agent_id = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(agentId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(agentId));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("updatePrimaryAgent"))) {
        return false;
    }

    emit conversationUpdated(id);
    return true;
}

/**
 * @brief Updates the updated_at timestamp on a conversation.
 * @param id UUID of the conversation.
 * @return true if update succeeded.
 */
bool ConversationService::touchConversation(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE conversations SET updated_at = ? WHERE id = ?"));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    if (!execQuery(q, QStringLiteral("touchConversation"))) {
        return false;
    }
    // Emit conversationUpdated so the sidebar model re-reads updated_at,
    // bumps the row to the top of its parent's children list via
    // beginMoveRows, and re-renders the timestamp subtitle. This
    // signal-propagation chain is how the sidebar stays in "most
    // recently updated first" order without polling.
    emit conversationUpdated(id);
    return true;
}

bool ConversationService::setConversationPinned(const QString& id, bool pinned) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (id.isEmpty())
        return false;

    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("UPDATE conversations SET is_pinned = ?, updated_at = ? WHERE id = ?"));
    // SQLite stores INTEGER 0/1 — bind as int, not bool, so the column
    // value is unambiguous across the Qt SQL driver's type coercion.
    q.addBindValue(pinned ? 1 : 0);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("setConversationPinned"))) {
        return false;
    }
    if (q.numRowsAffected() == 0) {
        qCWarning(verzetaDb) << "setConversationPinned: no row matched id" << id;
        return false;
    }

    qCInfo(verzetaDb) << "Conversation" << id << "is_pinned ->" << (pinned ? "true" : "false");

    // Same signal as renameConversation / moveToFolder / touchConversation
    // so every existing model subscriber (ConversationListModel,
    // SidebarFlatModel) picks up the change via row-level dataChanged
    // / beginMoveRows. No new signal contract is introduced.
    emit conversationUpdated(id);
    return true;
}

bool ConversationService::setConversationMemberAlias(const QString& id,
                                                     const QString& memberAlias) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (id.isEmpty())
        return false;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE conversations SET member_alias = ? WHERE id = ?"));
    q.addBindValue(memberAlias.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                         : QVariant(memberAlias));
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("setConversationMemberAlias"))) {
        return false;
    }
    if (q.numRowsAffected() == 0) {
        qCWarning(verzetaDb) << "setConversationMemberAlias: no row matched id" << id;
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------
// Folder operations
// -----------------------------------------------------------------------

/*
 * @brief Creates a new folder.
 * @param name Folder display name.
 * @param parentId Parent folder UUID, or empty for top-level.
 * @return UUID of created folder, or empty on failure.
 */
QString ConversationService::createFolder(const QString& name, const QString& parentId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (name.isEmpty()) {
        qCWarning(verzetaDb) << "createFolder: name must not be empty";
        return {};
    }

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("INSERT INTO folders(id, name, parent_id, created_at) VALUES(?, ?, ?, ?)"));
    q.addBindValue(id);
    q.addBindValue(name);
    q.addBindValue(parentId.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                      : QVariant(parentId));
    q.addBindValue(now);

    if (!execQuery(q, QStringLiteral("createFolder"))) {
        return {};
    }

    emit folderCreated(id);
    return id;
}

/*
 * @brief Deletes a folder. Contained conversations are moved to root.
 * @param id UUID of the folder.
 * @return true if deletion succeeded.
 */
bool ConversationService::deleteFolder(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    // First move all conversations in this folder to root
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("UPDATE conversations SET folder_id = NULL WHERE folder_id = ?"));
        q.addBindValue(id);
        execQuery(q, QStringLiteral("deleteFolder/orphanConversations"));
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM folders WHERE id = ?"));
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("deleteFolder"))) {
        return false;
    }

    emit folderDeleted(id);
    return true;
}

/*
 * @brief Renames a folder.
 * @param id UUID of the folder.
 * @param name New display name.
 * @return true if update succeeded.
 */
bool ConversationService::renameFolder(const QString& id, const QString& name) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (name.isEmpty()) {
        return false;
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE folders SET name = ? WHERE id = ?"));
    q.addBindValue(name);
    q.addBindValue(id);
    return execQuery(q, QStringLiteral("renameFolder"));
}

/*
 * @brief Lists folders at a given nesting level.
 * @param parentId If empty, returns top-level folders.
 * @return List of Folder structs ordered by name ASC.
 */
QList<Folder> ConversationService::listFolders(const QString& parentId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());

    if (parentId.isEmpty()) {
        q.prepare(
            QStringLiteral("SELECT * FROM folders WHERE parent_id IS NULL ORDER BY name ASC"));
    } else {
        q.prepare(QStringLiteral("SELECT * FROM folders WHERE parent_id = ? ORDER BY name ASC"));
        q.addBindValue(parentId);
    }

    if (!execQuery(q, QStringLiteral("listFolders"))) {
        return {};
    }

    QList<Folder> results;
    while (q.next()) {
        results.append(Folder::fromSqlRecord(q.record()));
    }
    return results;
}

std::optional<Folder> ConversationService::getFolder(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (id.isEmpty())
        return std::nullopt;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM folders WHERE id = ? LIMIT 1"));
    q.addBindValue(id);
    if (!execQuery(q, QStringLiteral("getFolder")) || !q.next()) {
        return std::nullopt;
    }
    return Folder::fromSqlRecord(q.record());
}

QList<Folder> ConversationService::folderChainForConversation(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<Folder> chain;
    const auto conv = getConversation(convId);
    if (!conv.has_value() || conv->folderId.isEmpty())
        return chain;

    QString currentId = conv->folderId;
    int safety = 32;  // prevent infinite loops on cyclic data
    while (!currentId.isEmpty() && safety-- > 0) {
        const auto f = getFolder(currentId);
        if (!f.has_value())
            break;
        chain.append(*f);
        currentId = f->parentId;
    }
    return chain;
}

QString ConversationService::projectOrgAncestorId(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QList<Folder> chain = folderChainForConversation(convId);
    for (const Folder& f : chain) {
        if (f.folderType == QStringLiteral("project") ||
            f.folderType == QStringLiteral("organization")) {
            return f.id;
        }
    }
    return QString();
}

bool ConversationService::folderAcnEnabled(const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (folderId.isEmpty())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT acn_enabled FROM folders WHERE id = ?"));
    q.addBindValue(folderId);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() != 0;
    }
    return true;  // default ON when the row/column is unexpectedly absent
}

bool ConversationService::setFolderAcnEnabled(const QString& folderId, bool enabled) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (folderId.isEmpty())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE folders SET acn_enabled = ? WHERE id = ?"));
    q.addBindValue(enabled ? 1 : 0);
    q.addBindValue(folderId);
    if (!execQuery(q, QStringLiteral("setFolderAcnEnabled"))) {
        return false;
    }
    emit folderUpdated(folderId);
    return true;
}

bool ConversationService::updateFolderMetadata(const QString& id,
                                               const QString& folderType,
                                               const QString& goal,
                                               const QString& description,
                                               const QStringList& agentIds) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (id.isEmpty())
        return false;

    // Serialize agentIds to JSON
    QJsonArray arr;
    for (const QString& a : agentIds)
        arr.append(a);
    const QString agentsJson =
        agentIds.isEmpty() ? QString()
                           : QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE folders SET folder_type = ?, goal = ?, description = ?, "
                             "agent_ids = ? WHERE id = ?"));
    q.addBindValue(folderType.isEmpty() ? QStringLiteral("regular") : folderType);
    q.addBindValue(goal.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(goal));
    q.addBindValue(description.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                         : QVariant(description));
    q.addBindValue(agentsJson.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                        : QVariant(agentsJson));
    q.addBindValue(id);

    if (!execQuery(q, QStringLiteral("updateFolderMetadata"))) {
        return false;
    }

    emit folderUpdated(id);
    return true;
}

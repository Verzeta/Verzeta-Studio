// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation.h
 * @brief Data model representing a single conversation session.
 *        Provides JSON and SQL record serialization/deserialization.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#pragma once

#include "llm-config.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Forward declaration
class QSqlRecord;

/**
 * @brief Immutable data model for a conversation session.
 *
 * Fields map directly to the `conversations` table schema.
 * All timestamps are stored internally as QDateTime (UTC) and serialized
 * as Unix milliseconds when stored in SQLite or JSON.
 */
struct Conversation {
    QString id;                 ///< UUID v4 primary key
    QString title;              ///< Display title shown in sidebar
    QString folderId;           ///< References folders.id; empty if in root
    QDateTime createdAt;        ///< UTC creation timestamp
    QDateTime updatedAt;        ///< UTC last-modified timestamp
    QString systemPrompt;       ///< Per-conversation LLM system prompt
    QJsonObject llmConfig;      ///< {model, provider, temperature, max_tokens, stream, thinking}
    int tokenTotal = 0;         ///< Cumulative token count across all messages
    QString primaryAgentId;     ///< References agents.id; empty for legacy/no-agent conversations
    bool isGroup = false;       ///< true for group chat with multiple agents
    QStringList groupAgentIds;  ///< Agent UUIDs in the group (isGroup==true)
    /**
     * @brief Sidebar pin flag (schema v9). When true, the
     *        row appears in the new "Pinned" sidebar section IN ADDITION
     *        TO its natural section (project / standalone group / direct
     *        agent / plain). Pinning duplicates, it does not move.
     *        Default false; toggled via ConversationService::setConversationPinned.
     */
    bool isPinned = false;

    /**
     * @brief For a project/org 1:1
     *        direct chat, the project member alias this conversation is
     *        the 1:1 channel for. Empty for group chats, plain chats,
     *        and non-project 1:1 chats.
     *
     *        Makes the project-1:1 → member binding first-class instead
     *        of encoding it in the "Chat with @Alias" title string
     *        (which the user can rename). RequestBuilder resolves the
     *        member's per-member model override via
     *        (folderId, memberAlias) → project_members. Resolution is
     *        tolerant: a stale alias (member removed/renamed) simply
     *        falls through to the conversation default, so it can never
     *        break dispatch.
     */
    QString memberAlias;

    /**
     * @brief Serializes this conversation to a JSON object.
     * @return QJsonObject with all fields; timestamps as Unix ms integers.
     */
    QJsonObject toJson() const;

    /**
     * @brief Deserializes a conversation from a JSON object.
     * @param json JSON object with the expected conversation fields.
     * @return Populated Conversation struct. Missing fields use default values.
     */
    static Conversation fromJson(const QJsonObject& json);

    /**
     * @brief Constructs a Conversation from a QSqlRecord row.
     * @param record SQL record from a SELECT on the conversations table.
     * @return Populated Conversation struct.
     */
    static Conversation fromSqlRecord(const QSqlRecord& record);

    /**
     * @brief Returns true if this conversation has a valid (non-empty) ID.
     * @return true if id is non-empty.
     */
    bool isValid() const { return !id.isEmpty(); }

    /**
     * @brief Reads the Tier-2 auto-surface gate from `llmConfig`.
     * @returns True iff `llm_config["allow_heartbeat_auto_surface"]` is
     *          present and true; false when the key is absent
     *          (default-OFF semantics for legacy conversations).
     */
    bool allowHeartbeatAutoSurface() const;

    /**
     * @brief Reads the per-conversation daily cap on auto-surfaced
     *        heartbeat posts from `llmConfig`.
     * @returns The cap stored on the conversation, or 1 when the key
     *          is absent (safe default).
     */
    int heartbeatAutoSurfaceMaxPerDay() const;
};

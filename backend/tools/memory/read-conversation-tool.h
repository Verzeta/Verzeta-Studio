// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-conversation-tool.h
 * @brief `read_conversation` tool: loads the most recent messages
 *        from a specific conversation, with a hard cap so a single
 *        invocation cannot overrun the context window.
 * @layer Service (Tool subsystem)
 * @dependencies MessageService (non-owning reference),
 *               std::function<QString()> for the active-conversation
 *               id lookup, backend/tools/itool.h.
 *
 * The tool accepts "current" as a special conversation_id value; it
 * resolves to whichever conversation is currently active in the UI,
 * obtained via the injected getter. The getter abstraction lets the
 * tool stay decoupled from ChatController's internals.
 *
 * Threading: runsOnMainThread() returns true. MessageService::getRecentMessages
 * touches SQLite; the main-thread gate applies.
 */
#pragma once

#include "../itool.h"

#include <functional>

class MessageService;

namespace Tools {

/** @brief ITool implementation for `read_conversation`. */
class ReadConversationTool : public ITool {
  public:
    /**
     * @param msgService            Non-owning reference; must outlive this tool.
     * @param activeConvIdGetter    Callable returning the UUID of the
     *                              conversation currently active in the
     *                              UI. Invoked at most once per tool call.
     *                              Must never be null.
     */
    ReadConversationTool(MessageService& msgService, std::function<QString()> activeConvIdGetter);

    /**
     * @brief Canonical tool name.
     * @returns "read_conversation".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the recent-messages behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `conversation_id` (required) and `limit`
     *          (optional int).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because MessageService::getRecentMessages touches SQLite.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Read the recent messages of the requested conversation.
     * @param args JSON object with:
     *               - "conversation_id" (required, string): target UUID,
     *                 or the literal "current" / empty string to resolve
     *                 to the active conversation.
     *               - "limit" (optional, integer): 1–100; default 20.
     *                 Clamped identically to the pre-extraction lambda.
     * @return On success, {"conversationId", "messages": [...],
     *         "count", "totalInDb"}. Each message object has id / role /
     *         optional "from" (member alias) / content / createdAt.
     *         Content is truncated to 2000 chars per message with a
     *         " …[truncated]" suffix on overflow.
     *         On no active conversation AND no explicit id, returns
     *         {"error": "no active conversation"}.
     * @complexity O(limit) SQLite query + linear content trim.
     * @sideeffects Read-only SQLite query.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    MessageService& m_msgService;
    std::function<QString()> m_activeConvIdGetter;
};

}  // namespace Tools

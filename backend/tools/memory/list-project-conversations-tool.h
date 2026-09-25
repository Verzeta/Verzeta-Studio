// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file list-project-conversations-tool.h
 * @brief `list_project_conversations` tool: enumerates sibling
 *        conversations inside the same project/organization folder
 *        as the active conversation, so an agent can discover
 *        adjacent threads without having to grep the entire history.
 * @layer Service (Tool subsystem)
 * @dependencies ConversationService (non-owning reference),
 *               std::function<QString()> for the active-conversation
 *               id lookup, backend/tools/itool.h.
 *
 * Threading: runsOnMainThread() returns true. ConversationService
 * touches SQLite; the main-thread gate applies.
 */
#pragma once

#include "../itool.h"

#include <functional>

class ConversationService;

namespace Tools {

/** @brief ITool implementation for `list_project_conversations`. */
class ListProjectConversationsTool : public ITool {
  public:
    /**
     * @param convService           Non-owning reference; must outlive this tool.
     * @param activeConvIdGetter    Callable returning the UUID of the
     *                              conversation currently active in the UI.
     *                              Must never be null.
     */
    ListProjectConversationsTool(ConversationService& convService,
                                 std::function<QString()> activeConvIdGetter);

    /**
     * @brief Canonical tool name.
     * @returns "list_project_conversations".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the project-siblings enumeration.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Empty list, because the tool takes no parameters.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because ConversationService touches SQLite.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Enumerate sibling conversations inside the same project.
     * @param args Ignored; the tool takes no parameters.
     * @return On success, {"projectFolderId": \<uuid\>,
     *         "conversations": [ {id, title, isGroup, updatedAt}, ... ],
     *         "count": \<int\>}. The active conversation itself is
     *         excluded from the list.
     *         On no active conversation, {"error": "no active conversation"}.
     *         On active conversation not inside a project folder,
     *         {"error": "current chat is not inside a project"}.
     * @complexity O(N) in siblings.
     * @sideeffects Read-only SQLite queries (folder chain + sibling list).
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    ConversationService& m_convService;
    std::function<QString()> m_activeConvIdGetter;
};

}  // namespace Tools

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file discover-skills-tool.h
 * @brief `discover_skills` tool: paginated lookup over every approved
 *        and non-blocked skill in the application library.
 *
 *        Returns a structured error when the active scope's
 *        `exposeOnly=true` flag forbids open discovery.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, ConversationService, CascadeController
 *               (via SkillToolDeps).
 */


#pragma once

#include "../itool.h"
#include "skill-tool-deps.h"

namespace Tools {

/**
 * @brief ITool implementation for the `discover_skills` built-in.
 */
class DiscoverSkillsTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of SkillService / ConversationService /
     *              CascadeController references.
     */
    explicit DiscoverSkillsTool(const SkillToolDeps& deps) : m_deps(deps) {}

    /**
     * @brief Canonical tool name.
     * @returns "discover_skills".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the skills-discovery behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for optional `query`, `tags`, `page`, and
     *          `page_size`.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because SkillService is main-thread-only.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Enumerate skills available to the active scope.
     * @param args  Optional `query`, `tags`, pagination args.
     * @returns Paginated `{"skills": [...], "page": ..., "total": ...}`
     *          or a structured error when discovery is forbidden.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    SkillToolDeps m_deps;
};

}  // namespace Tools

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-skill-tool.h
 * @brief `read_skill` tool: returns the full SKILL.md content for an
 *        approved skill.
 *
 *        When the active scope has `exposeOnly=true`, the tool refuses
 *        unless the requested skill_id is in the preferred list.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, ConversationService, CascadeController
 *               (via SkillToolDeps).
 */


#pragma once

#include "../itool.h"
#include "skill-tool-deps.h"

namespace Tools {

/**
 * @brief ITool implementation for the `read_skill` built-in.
 */
class ReadSkillTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of skill-tool dependencies.
     */
    explicit ReadSkillTool(const SkillToolDeps& deps) : m_deps(deps) {}

    /**
     * @brief Canonical tool name.
     * @returns "read_skill".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the skill-read behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `skill_id` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because SkillService is main-thread-only.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Read the full SKILL.md for the requested id.
     * @param args  JSON object with required `skill_id`.
     * @returns `{"skill_id": ..., "content": ...}` on success, or a
     *          structured error when the skill is missing, blocked,
     *          or excluded by `exposeOnly`.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    SkillToolDeps m_deps;
};

}  // namespace Tools

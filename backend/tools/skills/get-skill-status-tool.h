// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file get-skill-status-tool.h
 * @brief `get_skill_status` tool: reports whether a given skill is
 *        ready to execute on the current platform.
 *
 *        Cross-references the skill's frontmatter `declared_tools`
 *        against the ToolService's actual registered tool surface.
 *
 *        Use case: skill X declares `run_shell` in its frontmatter; on
 *        Android `run_shell` refuses. The AVAILABLE SKILLS prompt
 *        layer tags such skills `[BLOCKED: missing run_shell]` for
 *        the agent, but the agent (or a downstream tool) may want to
 *        confirm before deciding what to do. This tool returns the
 *        full diagnostic.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, ToolService (via SkillToolDeps).
 */


#pragma once

#include "../itool.h"
#include "skill-tool-deps.h"

namespace Tools {

/**
 * @brief ITool implementation for the `get_skill_status` built-in.
 */
class GetSkillStatusTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of skill-tool dependencies (must include
     *              `tools`).
     */
    explicit GetSkillStatusTool(const SkillToolDeps& deps) : m_deps(deps) {}

    /**
     * @brief Canonical tool name.
     * @returns "get_skill_status".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the skill-status diagnostic.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `skill_id` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because both SkillService and ToolService are main-thread-only.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Report whether the requested skill is ready to execute.
     * @param args  JSON object with required `skill_id`.
     * @returns `{"skill_id": ..., "ready": <bool>, "missing_tools": [...]}`,
     *          or a structured error when the skill is unknown.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    SkillToolDeps m_deps;
};

}  // namespace Tools

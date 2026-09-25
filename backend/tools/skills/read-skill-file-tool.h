// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-skill-file-tool.h
 * @brief `read_skill_file` tool: reads a supporting text file inside
 *        an approved skill folder.
 *
 *        Hard caps: path containment, no symlinks, 256 KiB on-disk
 *        limit, NUL-byte binary detection, 32 KiB LLM-return truncation.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, ConversationService, CascadeController
 *               (via SkillToolDeps).
 */


#pragma once

#include "../itool.h"
#include "skill-tool-deps.h"

namespace Tools {

/**
 * @brief ITool implementation for the `read_skill_file` built-in.
 */
class ReadSkillFileTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of skill-tool dependencies.
     */
    explicit ReadSkillFileTool(const SkillToolDeps& deps) : m_deps(deps) {}

    /**
     * @brief Canonical tool name.
     * @returns "read_skill_file".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the skill-file-read behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `skill_id` (required) and `relative_path`
     *          (required).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because SkillService is main-thread-only.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Read the requested supporting file from the skill folder.
     * @param args  JSON object with `skill_id` and `relative_path`.
     * @returns `{"content": ..., "truncated": <bool>}` on success, or
     *          a structured error when the path escapes containment,
     *          is a symlink, exceeds 256 KiB, or fails binary detection.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    SkillToolDeps m_deps;
};

}  // namespace Tools

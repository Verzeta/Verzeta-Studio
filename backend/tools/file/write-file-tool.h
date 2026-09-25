// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file write-file-tool.h
 * @brief `write_file` tool: writes text content into the currently
 *        active conversation- or project-scoped directory managed by
 *        FileService.
 * @layer Service (Tool subsystem)
 * @dependencies FileService (non-owning reference),
 *               backend/tools/itool.h.
 *
 * FileService owns the choice of destination directory via
 * setActiveConversation() / setActiveProjectContext(), so this tool
 * never embeds absolute paths. Only the suggested basename and the
 * content are arguments; FileService::saveGeneratedFile picks the
 * final absolute path and performs any directory-creation work.
 *
 * Threading: runsOnMainThread() returns false.
 */
#pragma once

#include "../itool.h"

class FileService;

namespace Tools {

/** @brief ITool implementation for the `write_file` built-in. */
class WriteFileTool : public ITool {
  public:
    /**
     * @param fileService Non-owning reference used for every write.
     *                    Must outlive this tool.
     */
    explicit WriteFileTool(FileService& fileService);

    /**
     * @brief Canonical tool name.
     * @returns "write_file".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the write-file behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `filename` (required) and `content` (required).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns false, because FileService::saveGeneratedFile is safe off the main thread.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Write text content to a file under the active directory.
     * @param args JSON object with:
     *               - "filename" (required, string): basename; path
     *                 resolution is handled by FileService.
     *               - "content"  (required, string): text body.
     * @return On success, {"path": \<absolute output path\>,
     *         "written": \<character count\>}.
     *         On missing field, {"error": "filename and content are required"}.
     *         On filesystem failure, {"error": "Failed to write file: ..."}.
     * @complexity O(N) in content length.
     * @sideeffects Creates or overwrites a file under the active
     *              conversation/project directory chosen by
     *              FileService. Creates parent directories as needed.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    FileService& m_fileService;
};

}  // namespace Tools

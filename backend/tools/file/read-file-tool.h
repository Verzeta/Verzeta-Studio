// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-file-tool.h
 * @brief `read_file` tool: reads a file's content through
 *        FileService and returns it to the LLM as a string.
 * @layer Service (Tool subsystem)
 * @dependencies FileService (non-owning reference),
 *               backend/tools/itool.h.
 *
 * FileService enforces whatever path-safety / sandboxing it applies
 * to reads; the tool body does no additional validation.
 *
 * Threading: runsOnMainThread() returns false. FileService::readFileContent
 * is documented as safe to call off the main thread.
 */
#pragma once

#include "../itool.h"

class FileService;

namespace Tools {

/** @brief ITool implementation for the `read_file` built-in. */
class ReadFileTool : public ITool {
  public:
    /**
     * @param fileService Non-owning reference used for every read.
     *                    Must outlive this tool.
     */
    explicit ReadFileTool(FileService& fileService);

    /**
     * @brief Canonical tool name.
     * @returns "read_file".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the read-file behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `path` (required) and `max_lines` (optional int).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns false, because FileService::readFileContent is safe off the main thread.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Read the file at the given path.
     * @param args JSON object with:
     *               - "path"      (required, string): absolute path.
     *               - "max_lines" (optional, integer): soft cap on
     *                 lines. Internally enforced as a byte cap of
     *                 (max_lines × 200). Default 200 lines.
     * @return On success, {"path": \<input\>, "content": \<utf8 string\>,
     *         "size": \<bytes read\>}.
     *         On empty path, {"error": "path is required"}.
     *         On unreadable file, {"error": "Could not read file: ..."}.
     * @complexity O(N) in bytes read, bounded by max_lines × 200.
     * @sideeffects None (read-only).
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    FileService& m_fileService;
};

}  // namespace Tools

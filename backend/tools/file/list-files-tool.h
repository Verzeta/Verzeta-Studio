// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file list-files-tool.h
 * @brief `list_files` tool: enumerates the entries of a directory,
 *        optionally recursively, and returns their absolute paths.
 * @layer Service (Tool subsystem)
 * @dependencies backend/tools/itool.h.
 *
 * The tool queries the filesystem through QDir / QDirIterator. It
 * holds no external service reference; it is stateless and safe to
 * share or instantiate per-registration.
 *
 * Threading: runsOnMainThread() returns false. QDir is reentrant;
 * each invocation constructs its own iterator.
 */
#pragma once

#include "../itool.h"

class FileService;

namespace Tools {

/** @brief ITool implementation for the `list_files` built-in. */
class ListFilesTool : public ITool {
  public:
    /** @param fileService Used to resolve relative paths against the
     *                     active project directory, mirroring
     *                     WriteFileTool / ReadFileTool. */
    explicit ListFilesTool(FileService& fileService);

    /**
     * @brief Canonical tool name.
     * @returns "list_files".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the directory-listing behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `path` (required) and `recursive` (optional bool).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns false, because QDir is reentrant and safe on a worker thread.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief List the directory's entries.
     * @param args  JSON object with `path` (required) and optional
     *              `recursive` (default false).
     * @returns On success, `{"path": ..., "entries": [<abs paths>]}`.
     *          On missing/empty path, `{"error": "path is required"}`.
     *          On unreadable directory, `{"error": "..."}`.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    FileService& m_fileService;
};

}  // namespace Tools

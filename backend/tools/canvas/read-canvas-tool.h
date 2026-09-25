// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-canvas-tool.h
 * @brief `read_canvas` tool: returns a 1-based line range of the
 *        conversation's active canvas content.
 *
 *        The agent always reads via this tool instead of recalling
 *        content from memory: the system prompt's ACTIVE CANVAS block
 *        is metadata-only (filename / language / lines / size /
 *        revision), so this tool is the only legitimate way for the
 *        agent to see canvas content.
 * @layer Service (Tool subsystem)
 * @dependencies CanvasService (non-owning ref) + an active-conversation
 *               getter (the LLM tool call has no conversation context).
 *
 * Threading: runsOnMainThread() returns true because the tool touches DB via CanvasService.
 */

#pragma once

#include "../itool.h"

#include <functional>

class CanvasService;

namespace Tools {

/** @brief ITool implementation for the `read_canvas` built-in. */
class ReadCanvasTool : public ITool {
  public:
    /**
     * @param canvasSvc          Non-owning reference to CanvasService.
     *                           Must outlive this tool.
     * @param activeConvIdGetter Returns the UI-active conversation id
     *                           at invocation time. The LLM's tool
     *                           call carries no conversation context.
     */
    explicit ReadCanvasTool(CanvasService& canvasSvc, std::function<QString()> activeConvIdGetter);

    /**
     * @brief Canonical tool name.
     * @returns "read_canvas".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the canvas-read behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `start_line` (optional int) and
     *          `end_line` (optional int).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because it touches SQLite via CanvasService.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Read a 1-based line range from the active canvas.
     * @param args JSON object with:
     *               - "start_line" (optional, integer, default 1):
     *                 first line to include, 1-based.
     *               - "end_line"   (optional, integer, default -1):
     *                 last line to include, 1-based; -1 = end of file.
     * @return On success: {"start_line": \<int\>, "end_line": \<int\>,
     *                      "content": \<string\>, "total_lines": \<int\>}
     *         On no active canvas: {"error": "No active canvas"}.
     *         On invalid range: {"error": "Invalid line range"}.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    CanvasService& m_canvasSvc;
    std::function<QString()> m_activeConvIdGetter;
};

}  // namespace Tools

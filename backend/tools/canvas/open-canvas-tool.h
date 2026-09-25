// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file open-canvas-tool.h
 * @brief `open_canvas` tool: the agent opens a file (or replaces its
 *        content with a fresh revision) in the conversation's Canvas
 *        panel.
 * @layer Service (Tool subsystem)
 * @dependencies CanvasService (non-owning ref) + a getter for the
 *               UI-active conversation id (the LLM's tool call has no
 *               conversation context of its own; it operates on the
 *               conversation the chat session is currently in).
 *
 * Threading: runsOnMainThread() returns true because CanvasService writes
 * SQLite via DbManager and mutates FileService global state for the
 * disk-mirror write, both of which require the main thread.
 */

#pragma once

#include "../itool.h"

#include <functional>

class CanvasService;

namespace Tools {

/** @brief ITool implementation for the `open_canvas` built-in. */
class OpenCanvasTool : public ITool {
  public:
    /**
     * @param canvasSvc          Non-owning reference to CanvasService.
     *                           Must outlive this tool.
     * @param activeConvIdGetter Returns the UI-active conversation id
     *                           at invocation time. Captured by value
     *                           into the body. Used because the LLM's
     *                           tool call doesn't carry conversation
     *                           context.
     */
    explicit OpenCanvasTool(CanvasService& canvasSvc, std::function<QString()> activeConvIdGetter);

    /**
     * @brief Canonical tool name.
     * @returns "open_canvas".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the canvas-open / revise behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `filename` (required), `language`
     *          (required), `content` (required), and `source_msg_id`
     *          (optional).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because CanvasService writes SQLite + mutates FileService
     *          global state for the disk-mirror write.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Open or revise a canvas.
     * @param args JSON object with:
     *               - "filename" (required, string): basename + extension.
     *               - "language" (required, string): language tag for
     *                 syntax highlighting (e.g. "json", "python",
     *                 "markdown", "plaintext").
     *               - "content"  (required, string): full file content.
     *               - "source_msg_id" (optional, string): the assistant
     *                 message id that produced this canvas. Reserved
     *                 for the future auto-promote audit path; not
     *                 consumed by the current implementation.
     * @return On success: {"canvas_id": \<UUID\>, "filename": \<name\>,
     *                      "language": \<lang\>, "lines": \<line count\>}
     *         On invalid args: {"error": "..."}
     *         On service failure: {"error": "Failed to open canvas: ..."}
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    CanvasService& m_canvasSvc;
    std::function<QString()> m_activeConvIdGetter;
};

}  // namespace Tools

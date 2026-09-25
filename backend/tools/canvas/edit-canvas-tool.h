// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file edit-canvas-tool.h
 * @brief `edit_canvas` tool: replaces the active canvas's full content.
 *
 *        The agent always passes the FULL new content; the current
 *        implementation does not support delta / line-range edits.
 * @layer Service (Tool subsystem)
 * @dependencies CanvasService (non-owning ref) + an active-conversation
 *               getter.
 *
 * Threading: runsOnMainThread() returns true because the tool touches DB + FileService
 * (disk mirror) via CanvasService.
 */

#pragma once

#include "../itool.h"

#include <functional>

class CanvasService;

namespace Tools {

/** @brief ITool implementation for the `edit_canvas` built-in. */
class EditCanvasTool : public ITool {
  public:
    /**
     * @param canvasSvc          Non-owning reference to CanvasService.
     *                           Must outlive this tool.
     * @param activeConvIdGetter Returns the UI-active conversation id
     *                           at invocation time.
     */
    explicit EditCanvasTool(CanvasService& canvasSvc, std::function<QString()> activeConvIdGetter);

    /**
     * @brief Canonical tool name.
     * @returns "edit_canvas".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the full-content replace behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `new_content` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because it touches DB + FileService disk mirror via CanvasService.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Replace the active canvas's content.
     * @param args JSON object with:
     *               - "new_content" (required, string): full file body.
     * @return On success: {"canvas_id": \<UUID\>, "revision": \<int\>,
     *                      "lines": \<int\>, "byte_size": \<int\>}
     *         On no active canvas: {"error": "No active canvas"}.
     *         On service failure: {"error": "Failed to edit canvas"}.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    CanvasService& m_canvasSvc;
    std::function<QString()> m_activeConvIdGetter;
};

}  // namespace Tools

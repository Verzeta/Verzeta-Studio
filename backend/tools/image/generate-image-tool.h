// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file generate-image-tool.h
 * @brief LLM-callable image-generation tool. Wraps ImageService with
 *        the agent's caller-identity flow.
 *
 *        Returns synchronously with `status="queued"`; the actual
 *        image lands as an assistant message + image attachment via
 *        the auto-persist path in ImageService::onImageReady. The
 *        agent is told this is async, so its next reply should NOT
 *        assume the image is already visible.
 *
 *        Backend selection: defaults to OpenAI DALL-E. If the user
 *        has configured a local Stable-Diffusion CLI path in Settings,
 *        the tool prefers local. The agent does not get to override
 *        the backend; that decision is user policy.
 * @layer Service (Tool subsystem)
 * @dependencies ImageService, SettingsService.
 */


#pragma once

#include "../itool.h"
#include "image-tool-deps.h"

namespace Tools {

/**
 * @brief ITool implementation for the `generate_image` built-in tool.
 */
class GenerateImageTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of ImageService + SettingsService references.
     *              Both must outlive the tool.
     */
    explicit GenerateImageTool(const ImageToolDeps& deps);

    /**
     * @brief Canonical tool name.
     * @returns "generate_image".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the image-generation behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `prompt` (required) and `size` (optional).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because it touches ImageService which writes through SQLite.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Queue an image-generation job and return immediately.
     * @param args  JSON object with `prompt` (required) and `size` (optional).
     * @returns `{"status": "queued", "jobId": \<uuid\>}` on success,
     *          `{"error": ...}` on a missing prompt or service failure.
     *
     * The actual image arrives later as an assistant message + image
     * attachment; the agent must not assume it is visible in its
     * immediate next reply.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    ImageToolDeps m_deps;
};

}  // namespace Tools

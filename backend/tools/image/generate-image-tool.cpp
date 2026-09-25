// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file generate-image-tool.cpp
 * @brief Implementation of the `generate_image` tool body.
 * @layer Service (Tool subsystem)
 * @dependencies ImageService, SettingsService.
 */


#include "generate-image-tool.h"

#include "../../services/image-provider-registry.h"
#include "../../services/image-service.h"
#include "../../services/settings-service.h"

#include <QJsonObject>

namespace Tools {

namespace {

QJsonObject makeError(const QString& message) {
    QJsonObject e;
    e[QStringLiteral("error")] = message;
    return e;
}

}  // namespace

GenerateImageTool::GenerateImageTool(const ImageToolDeps& deps) : m_deps(deps) {}

QString GenerateImageTool::name() const {
    return QStringLiteral("generate_image");
}

QString GenerateImageTool::description() const {
    return QStringLiteral("Generate an image from a text description. The image is saved "
                          "as an attachment in the active conversation and will appear "
                          "inline as an assistant message a moment after this tool returns "
                          "— THIS CALL IS ASYNC. Do NOT claim the image is visible in your "
                          "next reply unless the user confirms it. Use for visual "
                          "deliverables (mockups, diagrams, illustrations). The active "
                          "backend (DALL-E, local Stable Diffusion CLI, a self-hosted "
                          "OpenAI-compatible HTTP server, or Automatic1111) is chosen "
                          "automatically from the user's settings.");
}

QList<ToolParameterSchema> GenerateImageTool::parameters() const {
    ToolParameterSchema prompt;
    prompt.name = QStringLiteral("prompt");
    prompt.type = QStringLiteral("string");
    prompt.description =
        QStringLiteral("Text description of the desired image. Be specific — style, "
                       "subject, composition, mood.");
    prompt.required = true;

    ToolParameterSchema size;
    size.name = QStringLiteral("size");
    size.type = QStringLiteral("string");
    size.description =
        QStringLiteral("Image dimensions. DALL-E accepts: 1024x1024 (default), "
                       "1024x1792 (portrait), 1792x1024 (landscape). Local SD ignores "
                       "this and uses its CLI defaults.");
    size.required = false;

    ToolParameterSchema quality;
    quality.name = QStringLiteral("quality");
    quality.type = QStringLiteral("string");
    quality.description =
        QStringLiteral("DALL-E quality: \"standard\" (default) or \"hd\". Local SD "
                       "ignores this.");
    quality.required = false;

    return {prompt, size, quality};
}

bool GenerateImageTool::runsOnMainThread() const {
    // ImageService is main-thread (its worker thread is internal); the
    // tool invocation enqueues the request and returns. We're firmly
    // main-thread for the SettingsService reads + the generateImage
    // call.
    return true;
}

QJsonValue GenerateImageTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("Image generation is unavailable in this build."));
    }
    // Args-injected conv id wins over the captured-LOCAL getter so
    // wire-side per-client cascades generate images attached to
    // THEIR conversation, not the LOCAL CC's.
    QString convId = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (convId.isEmpty())
        convId = m_deps.activeConvIdGetter();
    if (convId.isEmpty()) {
        return makeError(QStringLiteral("Image generation requires an active conversation."));
    }
    const QString prompt = args.value(QStringLiteral("prompt")).toString().trimmed();
    if (prompt.isEmpty()) {
        return makeError(QStringLiteral("prompt is required and non-empty."));
    }

    // Resolve the active provider from the registry. Empty / invalid
    // means no provider is configured — the agent gets a clear error
    // pointing at the Image Generation settings page.
    const ActiveImageConfig active = m_deps.registry->activeConfig();
    if (!active.valid) {
        return makeError(
            QStringLiteral("Image generation is not configured. Ask the user to open "
                           "Settings -> Image Generation, add an image provider, and set "
                           "it active."));
    }

    ImageGenConfig cfg;
    cfg.endpointShape = active.endpointShape;
    cfg.baseUrl = active.baseUrl;
    cfg.model = active.model;
    cfg.apiKey = active.apiKey;
    cfg.authLocation =
        active.authLocation.isEmpty() ? QStringLiteral("header") : active.authLocation;
    cfg.authHeaderName = active.authHeaderName;
    cfg.authValuePrefix = active.authValuePrefix;
    cfg.authQueryParam = active.authQueryParam;
    cfg.authBodyField = active.authBodyField;
    cfg.sdPath = active.sdPath;
    cfg.sdParams = active.extraParams;
    cfg.outputModalities =
        active.outputModalities.isEmpty() ? QStringLiteral("image") : active.outputModalities;

    // size / quality: caller override wins, then the provider's stored
    // value, then a sane default.
    const QString size = args.value(QStringLiteral("size")).toString();
    const QString quality = args.value(QStringLiteral("quality")).toString();
    cfg.size = !size.isEmpty()
                   ? size
                   : (active.size.isEmpty() ? QStringLiteral("1024x1024") : active.size);
    cfg.quality = !quality.isEmpty()
                      ? quality
                      : (active.quality.isEmpty() ? QStringLiteral("standard") : active.quality);

    m_deps.image->generateImage(convId, prompt, cfg);

    QJsonObject result;
    result[QStringLiteral("status")] = QStringLiteral("queued");
    result[QStringLiteral("provider")] = active.displayName;
    result[QStringLiteral("backend")] = cfg.endpointShape;
    result[QStringLiteral("message")] =
        QStringLiteral("Image generation started. The image will appear as a separate "
                       "assistant message in this conversation within a few seconds. "
                       "Do not assume it is already visible.");
    return result;
}

}  // namespace Tools

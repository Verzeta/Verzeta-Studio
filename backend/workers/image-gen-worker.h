// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file image-gen-worker.h
 * @brief Background worker for image generation via DALL-E API
 *        or local Stable Diffusion CLI.
 * @layer Worker
 * @dependencies HttpClient (Utility), Qt6::Core (QProcess), JobContext (model)
 */


#pragma once

#include "../models/job-context.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

/**
 * @brief Generates images via DALL-E API or a local Stable Diffusion CLI.
 *
 * This worker is designed to run on a dedicated QThread. All public slots
 * should be invoked via Qt::QueuedConnection from the owning service.
 *
 * ## Signals / Slots threading model
 *
 * The worker lives on a background thread. `generateViaOpenAI()` and
 * `generateViaLocalSD()` are invoked via QueuedConnection. Results are
 * returned through `imageReady()` and `errorOccurred()` signals which
 * can be connected to main-thread receivers.
 *
 * ## JobContext threading
 *
 * The first parameter of every slot is the `JobContext` the service
 * stamped at enqueue. The slot stores it locally on the stack for the
 * duration of the call and emits it unchanged on every result signal.
 * Two overlapping calls each carry their own context; there is no
 * shared mutable state between in-flight jobs.
 */
class ImageGenWorker : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the worker.
     * @param parent Optional Qt parent.
     */
    explicit ImageGenWorker(QObject* parent = nullptr);

    /**
     * @brief Destroys the worker.
     */
    ~ImageGenWorker() override = default;

    /**
     * @brief Joins an API base URL with an endpoint path suffix, without
     *        duplicating the suffix when the base already ends with it.
     *
     * The image-provider Base URL field accepts either the API base
     * (e.g. "https://openrouter.ai/api/v1") or the full documented
     * endpoint (e.g. "https://openrouter.ai/api/v1/chat/completions",
     * the exact URL OpenRouter's docs show). Both must resolve to the
     * same endpoint: blindly concatenating @p pathSuffix onto the second
     * form yields ".../chat/completions/chat/completions" → 404. Trailing
     * slashes on @p baseUrl are stripped; the suffix match is
     * case-insensitive so the result is never doubled.
     *
     * @param baseUrl    The configured Base URL (trimmed, slashes stripped).
     * @param pathSuffix The endpoint path to ensure is present (must begin
     *                   with '/', e.g. "/chat/completions").
     * @returns The resolved endpoint URL string. When @p baseUrl already
     *          ends with @p pathSuffix it is returned unchanged (minus
     *          trailing slashes); otherwise @p pathSuffix is appended.
     */
    static QString joinEndpoint(const QString& baseUrl, const QString& pathSuffix);

  public slots:
    /**
     * @brief Generates an image via OpenAI DALL-E 3 API.
     *
     * Posts to https://api.openai.com/v1/images/generations with model=dall-e-3.
     * Parses the returned URL, downloads the image, and saves to
     * AppDataLocation/attachments/{uuid}.png before emitting imageReady().
     *
     * @param ctx     Per-job attribution context (echoed back on every emit).
     * @param prompt  Text description of the desired image.
     * @param size    Image dimensions: "1024x1024", "1024x1792", or "1792x1024".
     * @param quality Generation quality: "standard" or "hd".
     * @param apiKey  OpenAI API key (Bearer token).
     * @sideeffects HTTP POST to api.openai.com + HTTP GET for image download.
     *              Saves result PNG to AppDataLocation/attachments/.
     *              Emits progressUpdate(ctx, 50) after generation,
     *              progressUpdate(ctx, 100) after download.
     */
    void generateViaOpenAI(const JobContext& ctx,
                           const QString& prompt,
                           const QString& size,
                           const QString& quality,
                           const QString& apiKey);

    /**
     * @brief Generates an image via a local Stable Diffusion CLI executable.
     *
     * Spawns a QProcess running the SD CLI with:
     *   {sdPath} --prompt "{prompt}" --output {outputPath} --steps {steps}
     * Parses stdout for lines of the form "Step N/M" to emit progress updates.
     *
     * @param ctx      Per-job attribution context (echoed back on every emit).
     * @param prompt   Text description.
     * @param sdPath   Absolute path to the SD CLI executable.
     * @param params   Additional parameters (e.g., {"steps": 30, "seed": -1}).
     * @sideeffects Spawns QProcess. Emits progressUpdate(ctx, …) as steps complete.
     *              Emits imageReady(ctx, outputPath) on process exit code 0.
     */
    void generateViaLocalSD(const JobContext& ctx,
                            const QString& prompt,
                            const QString& sdPath,
                            const QJsonObject& params);

    /**
     * @brief Generates an image via a self-hosted server speaking the
     *        OpenAI Images API shape.
     *
     * Flow: POST \<url\>/images/generations with the same body OpenAI
     * cloud accepts ({model, prompt, size, quality, n, response_format}).
     * The server replies with `data[0].url` (download then save) OR
     * `data[0].b64_json` (decode and save directly). We request
     * `response_format=b64_json` so we never need a follow-up GET.
     *
     * @param ctx     Per-job attribution context (echoed back on every emit).
     * @param prompt  Text description.
     * @param url     Base URL (no trailing "/images/generations"; we
     *                append it here). Should include "/v1" if the
     *                server uses an OpenAI-style versioned path.
     * @param apiKey  Optional bearer token; omitted from headers when
     *                empty (self-hosted deployments are commonly
     *                unauthenticated).
     * @param size    "WxH" string passed through to the server.
     * @param quality "standard" / "hd", passed through; many servers
     *                ignore this field.
     */
    void generateViaOpenAICompat(const JobContext& ctx,
                                 const QString& prompt,
                                 const QString& url,
                                 const QString& apiKey,
                                 const QString& size,
                                 const QString& quality);

    /**
     * @brief Generates an image via an Automatic1111 / SD WebUI server's
     *        text-to-image endpoint.
     *
     * Flow: POST \<url\>/sdapi/v1/txt2img with an A1111-shaped body. The
     * server replies with `{ "images": [base64...], "info": "..." }`;
     * we decode `images[0]` and save it.
     *
     * @param ctx     Per-job attribution context (echoed back on every emit).
     * @param prompt  Text description.
     * @param url     Base URL (no trailing "/sdapi/v1/txt2img"; we
     *                append it here).
     * @param apiKey  Optional bearer token; omitted when empty.
     * @param size    "WxH" string. Parsed into A1111's separate
     *                width / height integer fields; falls back to
     *                512x512 on parse failure.
     * @param params  Extra fields merged into the request body:
     *                steps (int, default 30), cfg_scale, sampler_name,
     *                seed (int, -1 = random), negative_prompt (string).
     *                Anything not recognised here passes through to
     *                A1111 (e.g. hires_fix, batch_size).
     */
    void generateViaAutomatic1111(const JobContext& ctx,
                                  const QString& prompt,
                                  const QString& url,
                                  const QString& apiKey,
                                  const QString& size,
                                  const QJsonObject& params);

    /**
     * @brief Unified, registry-driven generation entry point.
     *
     * Routes by @p params["endpointShape"] to the matching handler:
     *   - "openai_images"     → POST {baseUrl}/images/generations
     *   - "a1111"             → POST {baseUrl}/sdapi/v1/txt2img
     *   - "local_cli"         → spawn the CLI at params["sdPath"]
     *   - "openai_chat_image" → POST {baseUrl}/chat/completions with
     *                           modalities ["image","text"] (OpenRouter
     *                           and other chat-image providers)
     *
     * Flexible auth is applied to all HTTP shapes: when
     * params["authQueryParam"] is non-empty the credential is appended
     * as that URL query parameter; otherwise it is sent as the header
     * params["authHeaderName"] with value
     * params["authValuePrefix"] + key.
     *
     * @param ctx    Per-job attribution context (echoed back on every emit).
     * @param prompt Text description of the desired image.
     * @param params Resolved config object. Keys: endpointShape, baseUrl,
     *               model, size, quality, apiKey, authHeaderName,
     *               authValuePrefix, authQueryParam, sdPath, extraParams.
     * @sideeffects HTTP request or QProcess spawn; saves the resulting
     *              image under AppDataLocation/attachments/. Emits
     *              progressUpdate / imageReady / errorOccurred, all
     *              echoing @p ctx.
     */
    void generate(const JobContext& ctx, const QString& prompt, const QJsonObject& params);

  signals:
    /**
     * @brief Generation progress (0–100).
     * @param ctx     Job context echoed unchanged from the inbound slot.
     * @param percent Completion percentage.
     */
    void progressUpdate(const JobContext& ctx, int percent);

    /**
     * @brief Emitted when image generation and download are complete.
     * @param ctx       Job context echoed unchanged from the inbound slot.
     * @param localPath Absolute path to the saved PNG file.
     */
    void imageReady(const JobContext& ctx, const QString& localPath);

    /**
     * @brief Emitted when an error occurs.
     * @param ctx   Job context echoed unchanged from the inbound slot.
     * @param error Human-readable error message.
     */
    void errorOccurred(const JobContext& ctx, const QString& error);
};

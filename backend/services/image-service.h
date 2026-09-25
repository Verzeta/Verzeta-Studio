// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file image-service.h
 * @brief Orchestrates image generation, storage, and vision analysis.
 *        Manages ImageGenWorker lifetime on a dedicated QThread.
 * @layer Service
 * @dependencies ImageGenWorker (Worker), ModelRouter (Service), FileService (Service)
 */


#pragma once

#include "../models/job-context.h"

#include <QThread>

#include <functional>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

class AuditService;
class ModelRouter;
class FileService;
class ImageGenWorker;
class MessageService;
class TaskObserver;
class TaskGateService;
class ImageProviderRegistry;

/**
 * @brief Configuration for image generation requests.
 *
 * The `backend` field selects the worker dispatch arm. Internal names
 * (worker-facing) differ from the user-facing names stored in
 * SettingsService::imageGenActiveBackend(); the generate_image tool
 * maps between them.
 */
struct ImageGenConfig {
    QString backend = QStringLiteral("openai");
    ///< Legacy worker arm selector: "openai" | "local_sd" |
    ///< "openai_compat" | "a1111". Consulted only when
    ///< `endpointShape` is empty (back-compat path).
    QString endpointShape;
    ///< Registry-driven dispatch selector, one of
    ///< "openai_images" | "a1111" | "local_cli" |
    ///< "openai_chat_image". When non-empty this takes priority
    ///< over `backend` and drives the unified worker dispatch.
    QString baseUrl;
    ///< Base URL for the HTTP endpoint shapes. The worker appends
    ///< the shape-specific path suffix
    ///< (/images/generations, /sdapi/v1/txt2img,
    ///< /chat/completions). Trailing slashes are tolerated.
    QString model;
    ///< Model id sent in the request body for the HTTP shapes
    ///< (e.g. "dall-e-3", "gpt-image-1",
    ///< "google/gemini-2.5-flash-image").
    QString size = QStringLiteral("1024x1024");
    ///< DALL-E image size; A1111 splits this into width x height.
    QString quality = QStringLiteral("standard");
    ///< "standard" | "hd"
    QString sdPath;
    ///< Absolute path to local SD CLI (local_cli only).
    QJsonObject sdParams;
    ///< Extra params: {"steps":30,"seed":-1}. Merged into the
    ///< A1111 / local-CLI request.
    QString apiKey;
    ///< Auth credential. Applied via the flexible-auth fields
    ///< below: as a header value, or as a query param.
    QString url;
    ///< Legacy alias for `baseUrl`, kept for the back-compat
    ///< `backend`-driven path. New callers set `baseUrl`.
    QString authLocation = QStringLiteral("header");
    ///< Where the credential is placed: "header" (default), "query",
    ///< or "body". Selects which of the fields below applies.
    QString authHeaderName = QStringLiteral("Authorization");
    ///< Header name carrying the credential (authLocation=="header").
    ///< Default "Authorization".
    QString authValuePrefix = QStringLiteral("Bearer ");
    ///< Prefix prepended to the credential in the header value.
    ///< Default "Bearer ".
    QString authQueryParam;
    ///< URL query parameter carrying the credential
    ///< (authLocation=="query"): `?<authQueryParam>=<key>`.
    QString authBodyField = QStringLiteral("api_key");
    ///< JSON body field carrying the credential
    ///< (authLocation=="body"): merged as {\<authBodyField\>: key}.
    QString outputModalities = QStringLiteral("image");
    ///< Requested output modalities for the chat-image shape:
    ///< "image" (image-only, default) or "image_text" (dual-output
    ///< models that also return text, e.g. Gemini). Ignored by the
    ///< non-chat-image shapes.
    QString sourceImagePath;
    ///< Refine/edit mode (chat-image shape only): absolute path to an
    ///< existing image sent to the model as a reference alongside the
    ///< prompt, so the model edits it instead of generating from
    ///< scratch. Empty for normal text-to-image generation.
};

/**
 * @brief Service for image generation and vision analysis.
 *
 * Owns one `ImageGenWorker` on a private `QThread`. The worker is started
 * in the constructor and stopped in the destructor. Callers invoke `generateImage()`
 * and `analyzeImage()` from the main thread; results arrive via Qt signals.
 *
 * ## QML Integration
 *
 * Exposed as `ImageService` context property via `AppController::registerTypes()`.
 * QML can call `generateImage()`, `analyzeImage()`, and `conversationImages()`.
 */
class ImageService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs ImageService and starts the worker thread.
     * @param router      ModelRouter for dispatching vision analysis requests.
     * @param fileService FileService for path resolution.
     * @param parent      Optional Qt parent.
     */
    explicit ImageService(ModelRouter& router, FileService& fileService, QObject* parent = nullptr);

    /**
     * @brief Stops the worker thread and destroys the service.
     */
    ~ImageService() override;

    /**
     * @brief Installs a resolver that returns the workspace directory for a
     *        conversation, resolved deterministically from its folder chain.
     *
     * Image completion is asynchronous, so filing must NOT rely on the
     * FileService's mutable "current" context (which is only set during an
     * agent turn; a UI-triggered refine has none, and the image lands in the
     * base projects dir). With this resolver, every image files into its own
     * conversation's project workspace. When unset, filing falls back to
     * FileService::activeProjectDir() (the legacy behaviour).
     *
     * @param resolver  convId -> absolute workspace directory.
     */
    void setWorkspaceDirResolver(std::function<QString(const QString&)> resolver);

    // -----------------------------------------------------------------------
    // Public API (callable from QML via Q_INVOKABLE)
    // -----------------------------------------------------------------------

    /**
     * @brief Requests image generation for the given conversation.
     * @param convId Conversation UUID (used to organise attachment
     *               storage).
     * @param prompt Image description text.
     * @param config Generation configuration (backend, size, quality,
     *               API key).
     * @sideeffects Dispatches to ImageGenWorker on background thread.
     *              Emits generationStarted(convId) immediately. Emits
     *              generationProgress, imageGenerated, or error when
     *              complete.
     *
     * Each call builds its own immutable JobContext, which the worker echoes
     * back on every result. Results are attributed from that context
     * (conversation and prompt), so overlapping calls always resolve to the
     * conversation that started them.
     */
    Q_INVOKABLE void
    generateImage(const QString& convId, const QString& prompt, const ImageGenConfig& config);

    /**
     * @brief QML-callable wrapper. The typed generateImage() takes
     *        an ImageGenConfig struct which QML cannot construct.
     *        This overload accepts a QVariantMap with keys matching
     *        the struct field names and forwards to the typed entry
     *        point. Used by ChatInputBar's Generate-Image dialog.
     *
     *        Recognised map keys: backend ("openai"|"local_sd"),
     *        size, quality, sdPath, sdParams (variantmap), apiKey.
     *        Any key omitted falls back to the ImageGenConfig
     *        default. apiKey defaults to
     *        SettingsService::apiKey("openai") when not supplied
     *        (typical caller path).
     * @param convId Conversation UUID for attachment association.
     * @param prompt Image description text.
     * @param config QML-friendly variant map of generation options.
     */
    Q_INVOKABLE void
    generateImageFromMap(const QString& convId, const QString& prompt, const QVariantMap& config);

    /**
     * @brief Generates an image using the ACTIVE provider resolved from
     *        the attached ImageProviderRegistry.
     *
     * Resolution: reads `ImageProviderRegistry::activeConfig()`, maps it
     * into an ImageGenConfig (endpointShape + baseUrl + model + flexible
     * auth + sdPath), then dispatches through the shape-based worker
     * path. The caller may override `size` and `quality` via @p
     * overrides; all other fields come from the active provider.
     *
     * @param convId    Conversation UUID for attachment association.
     * @param prompt    Image description text.
     * @param overrides Optional caller overrides. Recognised keys:
     *                  "size", "quality". Any other key is ignored.
     * @sideeffects Emits generationStarted(convId) immediately. Emits a
     *              clear error(convId, ...) and returns without
     *              dispatching when no registry is attached or no valid
     *              active provider is configured.
     */
    Q_INVOKABLE void generateImageFromActiveProvider(const QString& convId,
                                                     const QString& prompt,
                                                     const QVariantMap& overrides = {});

    /**
     * @brief Refines/edits an existing image with a new instruction using
     *        the active provider.
     *
     * Sends @p sourceImagePath to the model as a reference image alongside
     * @p prompt so the model edits it rather than generating from scratch.
     * Only the chat-image endpoint shape supports this (the model must
     * accept image input); other shapes emit a clear error. The refined
     * image arrives as a new assistant image message in @p convId (same
     * delivery path as generation).
     *
     * @param convId          Conversation to attach the refined image to.
     * @param sourceImagePath Absolute path to the image being refined.
     * @param prompt          Instruction describing the desired changes.
     * @sideeffects Emits generationStarted(convId) then, asynchronously,
     *              imageGenerated(convId, path) or error(convId, ...).
     */
    Q_INVOKABLE void
    refineImage(const QString& convId, const QString& sourceImagePath, const QString& prompt);

    /**
     * @brief Attach the ImageProviderRegistry that resolves the active
     *        image provider for generation.
     *
     *        AppController wires this once during initialize(), after
     *        both ImageService and ImageProviderRegistry exist. The
     *        pointer is non-owning; null disables the active-provider
     *        generate path (the typed generateImage() still works).
     * @param registry Non-owning pointer; pass nullptr to detach.
     */
    void setImageProviderRegistry(ImageProviderRegistry* registry);

    /**
     * @brief Attach a MessageService so generated images are
     *        auto-persisted as assistant messages with image
     *        attachments. Without this setter the service emits
     *        imageGenerated() only and no chat row is created.
     *
     *        AppController wires this once during initialize(). The
     *        pointer is non-owning; null disables auto-persist
     *        (image still saved on disk, signal still fires).
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setMessageService(MessageService* svc);

    /**
     * @brief Attach an AuditService so each completed
     *        image-generation job records an image_generated event
     *        to the activity_log. Non-owning; AppController wires
     *        this once during initialize(). When unset, the audit
     *        hook is a no-op (the image still persists + emits
     *        imageGenerated).
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setAuditService(AuditService* svc);

    /**
     * @brief Attach the TaskObserver so a completed image that was
     *        filed into the project workspace is ALSO recorded as a
     *        File artifact on the active plan (the timeline/artifact
     *        surfaces previously only saw the tool CALL, while the image
     *        completes asynchronously long after it). Non-owning;
     *        no-op when unset.
     * @param observer Observer; nullptr detaches.
     */
    void setTaskObserver(TaskObserver* observer);

    /**
     * @brief Attach the TaskGateService used to resolve the active
     *        plan id at image-completion time. Non-owning; no-op when
     *        unset (no artifact row is written without a plan).
     * @param gate Gate service; nullptr detaches.
     */
    void setTaskGateService(TaskGateService* gate);

    /**
     * @brief Attach a SettingsService for read-time defaults (OpenAI
     *        key, local SD path) used by the QML wrapper. Optional;
     *        the typed generateImage() does not consult settings.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setSettingsService(class SettingsService* svc);

    /**
     * @brief Sends an image to a vision-capable LLM for analysis.
     *
     * Reads the image file, encodes as base64, and constructs a multimodal
     * LLM request via ModelRouter. The response streams as normal text.
     *
     * @param convId     Conversation UUID.
     * @param imagePath  Absolute local path to the image file.
     * @param question   What to ask about the image.
     * @sideeffects Reads the file (max 10 MiB) and dispatches through
     *              ModelRouter::route(). The response streams through
     *              ModelRouter's chunkReceived and requestFinished
     *              signals, so callers connect to those; analysisReady()
     *              is not emitted. Emits error() when the image cannot be
     *              read.
     */
    Q_INVOKABLE void
    analyzeImage(const QString& convId, const QString& imagePath, const QString& question);

    /**
     * @brief Returns paths of all image attachments for a conversation.
     *
     * Scans AppDataLocation/attachments/ for PNG/JPG files whose names
     * are associated with the given conversation (stored as metadata in
     * m_conversationImages map at generation time).
     *
     * @param convId Conversation UUID.
     * @return List of absolute file paths, sorted by creation time (oldest first).
     */
    Q_INVOKABLE QStringList conversationImages(const QString& convId) const;

  signals:
    // -----------------------------------------------------------------------
    // Progress / result signals
    // -----------------------------------------------------------------------

    /** @brief Emitted when generation begins. */
    void generationStarted(const QString& convId);

    /**
     * @brief Emitted with progress updates (0-100).
     * @param convId  Conversation UUID.
     * @param percent Progress percentage in [0, 100].
     */
    void generationProgress(const QString& convId, int percent);

    /**
     * @brief Emitted when the generated image is ready.
     * @param convId    Conversation UUID.
     * @param imagePath Absolute path to the saved image file.
     */
    void imageGenerated(const QString& convId, const QString& imagePath);

    /**
     * @brief Emitted after an AGENT-requested generation completed and
     *        its chat row was persisted. The chat layer resumes the
     *        requesting agent with the finished image so the
     *        conversation continues instead of stalling on the async
     *        completion. Not emitted for user/QML-driven generations.
     * @param convId    Conversation the image belongs to.
     * @param alias     Requesting member alias (empty in 1:1).
     * @param agentId   Requesting agent template id (never empty).
     * @param imagePath Absolute path of the finished image (workspace
     *                  copy when filing succeeded, else the raw file).
     */
    void imageReadyForFollowUp(const QString& convId,
                               const QString& alias,
                               const QString& agentId,
                               const QString& imagePath);

    /**
     * @brief Declared for a completed vision analysis, but not currently
     *        emitted: analyzeImage() streams its response through
     *        ModelRouter instead.
     * @param convId      Conversation UUID.
     * @param description Full analysis text from the LLM.
     */
    void analysisReady(const QString& convId, const QString& description);

    /**
     * @brief Emitted on error.
     * @param convId  Conversation UUID.
     * @param message Human-readable error description.
     */
    void error(const QString& convId, const QString& message);

    // Internal cross-thread signals (invokeMethod targets on the
    // worker thread). Every signal threads a JobContext that the
    // worker echoes back unchanged on result emit; the service slot
    // reads attribution from the echoed context — never from a
    // mutable m_active* field. This guarantees attribution is
    // race-free across overlapping generateImage() calls.

    /**
     * @brief Worker dispatch: OpenAI DALL-E backend.
     * @param ctx     JobContext echoed back on result.
     * @param prompt  Image description text.
     * @param size    OpenAI size string ("1024x1024" etc.).
     * @param quality "standard" or "hd".
     * @param apiKey  Bearer token.
     */
    void requestOpenAI(const JobContext& ctx,
                       const QString& prompt,
                       const QString& size,
                       const QString& quality,
                       const QString& apiKey);

    /**
     * @brief Worker dispatch: local Stable Diffusion CLI backend.
     * @param ctx    JobContext echoed back on result.
     * @param prompt Image description text.
     * @param sdPath Absolute path to the local SD CLI.
     * @param params Extra params (steps, seed, etc.).
     */
    void requestLocalSD(const JobContext& ctx,
                        const QString& prompt,
                        const QString& sdPath,
                        const QJsonObject& params);

    /**
     * @brief Worker dispatch: OpenAI-compatible self-hosted HTTP
     *        backend.
     * @param ctx     JobContext echoed back on result.
     * @param prompt  Image description text.
     * @param url     Base URL of the self-hosted endpoint.
     * @param apiKey  Optional bearer token.
     * @param size    Image size string.
     * @param quality "standard" or "hd".
     */
    void requestOpenAICompat(const JobContext& ctx,
                             const QString& prompt,
                             const QString& url,
                             const QString& apiKey,
                             const QString& size,
                             const QString& quality);

    /**
     * @brief Worker dispatch: Automatic1111 / Forge HTTP backend.
     * @param ctx    JobContext echoed back on result.
     * @param prompt Image description text.
     * @param url    Base URL of the Automatic1111 endpoint.
     * @param apiKey Optional bearer token.
     * @param size   Image size string (split into width x height).
     * @param params Extra params (steps, seed, etc.).
     */
    void requestAutomatic1111(const JobContext& ctx,
                              const QString& prompt,
                              const QString& url,
                              const QString& apiKey,
                              const QString& size,
                              const QJsonObject& params);

    /**
     * @brief Worker dispatch: unified, registry-driven shape dispatch.
     *
     * Carries every resolved field in one queued QJsonObject so the
     * cross-thread connection uses only built-in metatypes (QJsonObject
     * is queueable; ImageGenConfig is not). The worker reads
     * `endpointShape` and routes to the matching handler.
     *
     * @param ctx    JobContext echoed back on result.
     * @param prompt Image description text.
     * @param params Resolved config object. Keys: endpointShape,
     *               baseUrl, model, size, quality, apiKey,
     *               authHeaderName, authValuePrefix, authQueryParam,
     *               sdPath, extraParams.
     */
    void requestGenerate(const JobContext& ctx, const QString& prompt, const QJsonObject& params);

  private slots:
    /**
     * @brief Worker result slot: image landed on disk.
     * @param ctx       JobContext echoed back by the worker.
     * @param localPath Absolute path to the saved image file.
     */
    void onImageReady(const JobContext& ctx, const QString& localPath);

    /**
     * @brief Worker result slot: generation failed.
     * @param ctx      JobContext echoed back by the worker.
     * @param errorMsg Human-readable error description.
     */
    void onWorkerError(const JobContext& ctx, const QString& errorMsg);

    /**
     * @brief Worker progress slot: percent complete update.
     * @param ctx     JobContext echoed back by the worker.
     * @param percent Progress percentage in [0, 100].
     */
    void onWorkerProgress(const JobContext& ctx, int percent);

  private:
    /**
     * @brief Persists a visible image-generation failure into the
     *        conversation (when a MessageService is attached) and emits
     *        the error() signal.
     *
     * Every image-error path (synchronous pre-dispatch validation
     * failures AND asynchronous worker failures) routes through here so
     * a failure is never silent (Rule 11). Generation is async and the
     * generate_image tool has already returned "queued", so a transient
     * toast alone leaves the user waiting for an image that will never
     * arrive; the persisted row makes the reason visible.
     *
     * The persisted role depends on WHEN this runs, because
     * RequestBuilder::assembleHistory walks each assistant[tool_calls]
     * row forward to its paired role=tool result and an intervening
     * role=assistant ENDS that window (orphaning the tool result → the
     * agent loops). A @p synchronous failure happens DURING the
     * generate_image tool's invoke(), before the tool result row exists,
     * so it MUST be a role=system intervening row
     * (addInterveningSystemMessage). An async worker failure arrives long
     * after the tool pair is persisted, so role=assistant is safe there
     * (and renders inline for the user, like onImageReady).
     *
     * @param convId      Owning conversation id (no message persisted if empty).
     * @param jobId       Job id for metadata correlation (may be empty for
     *                    pre-dispatch failures with no JobContext).
     * @param message     Human-readable failure reason.
     * @param synchronous true when called mid-tool-invoke (persist as
     *                    role=system); false for async worker failures
     *                    (persist as role=assistant).
     */
    void emitGenerationError(const QString& convId,
                             const QString& jobId,
                             const QString& message,
                             bool synchronous);

    ModelRouter& m_router;
    FileService& m_fileService;
    std::function<QString(const QString&)> m_workspaceDirResolver;
    ImageGenWorker* m_worker = nullptr;
    QThread* m_workerThread = nullptr;

    MessageService* m_msgService = nullptr;
    class SettingsService* m_settingsService = nullptr;

    // Image-provider registry — non-owning, AppController-set after
    // construction. Resolves the active provider for the
    // generateImageFromActiveProvider() path.
    ImageProviderRegistry* m_providerRegistry = nullptr;

    AuditService* m_auditService = nullptr;

    /** Optional artifact hooks (see setTaskObserver/setTaskGateService).
     *  Non-owning; both must be set AND a plan active for the File
     *  artifact row to be written. */
    TaskObserver* m_taskObserver = nullptr;
    TaskGateService* m_taskGate = nullptr;


    // Per-conversation image lists: convId → list of absolute paths.
    // Written from onImageReady's ctx.conversationId, NOT from a
    // mutable active-conv field.
    QMap<QString, QStringList> m_conversationImages;
};

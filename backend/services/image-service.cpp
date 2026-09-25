// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file image-service.cpp
 * @brief Implementation of image generation, vision analysis, and
 *        catalog management.
 * @layer Service
 * @dependencies ImageGenWorker, ModelRouter, FileService, JobContext,
 *               Qt6::Core, Qt6::Network.
 */


#include "image-service.h"

#include "../api/llm-interface.h"
#include "../models/activity-event.h"
#include "../models/attachment.h"
#include "../models/message.h"
#include "../services/audit-service.h"
#include "../services/file-service.h"
#include "../services/image-provider-registry.h"
#include "../services/message-service.h"
#include "../services/model-router.h"
#include "../services/settings-service.h"
#include "../utils/logger.h"
#include "../workers/image-gen-worker.h"
#include "task-gate-service.h"
#include "task-observer.h"

#include <QThread>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs ImageService and starts the worker thread.
 * @param router      ModelRouter for vision analysis dispatch.
 * @param fileService FileService for path helpers.
 * @param parent      Optional Qt parent.
 */
ImageService::ImageService(ModelRouter& router, FileService& fileService, QObject* parent)
    : QObject(parent)
    , m_router(router)
    , m_fileService(fileService)
    , m_worker(new ImageGenWorker)
    , m_workerThread(new QThread(this)) {
    m_worker->moveToThread(m_workerThread);

    connect(m_worker,
            &ImageGenWorker::progressUpdate,
            this,
            &ImageService::onWorkerProgress,
            Qt::QueuedConnection);

    connect(m_worker,
            &ImageGenWorker::imageReady,
            this,
            &ImageService::onImageReady,
            Qt::QueuedConnection);

    connect(m_worker,
            &ImageGenWorker::errorOccurred,
            this,
            &ImageService::onWorkerError,
            Qt::QueuedConnection);

    // Cross-thread invocation signals — now all carry JobContext as
    // first argument. Qt deduces the new signatures from the member
    // function pointers.
    connect(this,
            &ImageService::requestOpenAI,
            m_worker,
            &ImageGenWorker::generateViaOpenAI,
            Qt::QueuedConnection);

    connect(this,
            &ImageService::requestLocalSD,
            m_worker,
            &ImageGenWorker::generateViaLocalSD,
            Qt::QueuedConnection);

    connect(this,
            &ImageService::requestOpenAICompat,
            m_worker,
            &ImageGenWorker::generateViaOpenAICompat,
            Qt::QueuedConnection);

    connect(this,
            &ImageService::requestAutomatic1111,
            m_worker,
            &ImageGenWorker::generateViaAutomatic1111,
            Qt::QueuedConnection);

    // Unified registry-driven dispatch — carries every resolved field in
    // one queued QJsonObject; the worker routes by `endpointShape`.
    connect(this,
            &ImageService::requestGenerate,
            m_worker,
            &ImageGenWorker::generate,
            Qt::QueuedConnection);

    m_workerThread->start();
    qCInfo(verzetaUi) << "ImageService initialised";
}

/**
 * @brief Stops worker thread and frees worker.
 */
ImageService::~ImageService() {
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
    }
    delete m_worker;
}

void ImageService::setWorkspaceDirResolver(std::function<QString(const QString&)> resolver) {
    m_workspaceDirResolver = std::move(resolver);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/*
 * @brief Starts image generation for the given conversation.
 * @param convId Conversation UUID.
 * @param prompt Image description.
 * @param config Generation configuration.
 *
 * Builds a JobContext (immutable, per-job)
 * and threads it through the dispatch signal to the worker. The worker
 * echoes the same JobContext back on every result emit. The completion
 * slot reads attribution from ctx.conversationId / ctx.prompt, never
 * from m_active* (which no longer exists). Two overlapping calls each
 * carry their own context and resolve to the correct conversation
 * regardless of timing.
 */
void ImageService::generateImage(const QString& convId,
                                 const QString& prompt,
                                 const ImageGenConfig& config) {
    const JobContext ctx = JobContext::makeNew(convId, prompt);
    emit generationStarted(convId);

    // Registry-driven shape dispatch takes priority. When endpointShape
    // is set the unified worker path handles every shape (including the
    // new openai_chat_image / OpenRouter arm) with flexible auth, base
    // URL, and model. The legacy `backend` branch below is the
    // back-compat path for the typed-only callers (and existing tests).
    if (!config.endpointShape.isEmpty()) {
        const QString shape = config.endpointShape;
        const QString base = config.baseUrl.isEmpty() ? config.url : config.baseUrl;

        if (shape == QStringLiteral("local_cli")) {
            if (config.sdPath.isEmpty()) {
                emitGenerationError(convId,
                                    ctx.jobId,
                                    QStringLiteral("Local image CLI path not "
                                                   "configured for this provider."),
                                    /*synchronous=*/true);
                return;
            }
        } else if (shape == QStringLiteral("openai_images") || shape == QStringLiteral("a1111") ||
                   shape == QStringLiteral("openai_chat_image")) {
            if (base.isEmpty()) {
                emitGenerationError(convId,
                                    ctx.jobId,
                                    QStringLiteral("Image provider base URL is not "
                                                   "configured."),
                                    /*synchronous=*/true);
                return;
            }
        } else {
            emitGenerationError(convId,
                                ctx.jobId,
                                QStringLiteral("Unsupported image endpoint type: %1").arg(shape),
                                /*synchronous=*/true);
            return;
        }

        QJsonObject params;
        params.insert(QStringLiteral("endpointShape"), shape);
        params.insert(QStringLiteral("baseUrl"), base);
        params.insert(QStringLiteral("model"), config.model);
        params.insert(QStringLiteral("size"), config.size);
        params.insert(QStringLiteral("quality"), config.quality);
        params.insert(QStringLiteral("apiKey"), config.apiKey);
        params.insert(QStringLiteral("authLocation"), config.authLocation);
        params.insert(QStringLiteral("authHeaderName"), config.authHeaderName);
        params.insert(QStringLiteral("authValuePrefix"), config.authValuePrefix);
        params.insert(QStringLiteral("authQueryParam"), config.authQueryParam);
        params.insert(QStringLiteral("authBodyField"), config.authBodyField);
        params.insert(QStringLiteral("sdPath"), config.sdPath);
        params.insert(QStringLiteral("extraParams"), config.sdParams);
        params.insert(QStringLiteral("outputModalities"), config.outputModalities);
        params.insert(QStringLiteral("sourceImagePath"), config.sourceImagePath);
        emit requestGenerate(ctx, prompt, params);
        return;
    }

    if (config.backend == QStringLiteral("local_sd")) {
        if (config.sdPath.isEmpty()) {
            emitGenerationError(convId,
                                ctx.jobId,
                                QStringLiteral("Local SD path not configured"),
                                /*synchronous=*/true);
            return;
        }
        emit requestLocalSD(ctx, prompt, config.sdPath, config.sdParams);
    } else if (config.backend == QStringLiteral("openai_compat")) {
        if (config.url.isEmpty()) {
            emitGenerationError(convId,
                                ctx.jobId,
                                QStringLiteral("OpenAI-compatible image server "
                                               "URL not configured"),
                                /*synchronous=*/true);
            return;
        }
        emit requestOpenAICompat(
            ctx, prompt, config.url, config.apiKey, config.size, config.quality);
    } else if (config.backend == QStringLiteral("a1111")) {
        if (config.url.isEmpty()) {
            emitGenerationError(convId,
                                ctx.jobId,
                                QStringLiteral("Automatic1111 server URL not "
                                               "configured"),
                                /*synchronous=*/true);
            return;
        }
        emit requestAutomatic1111(
            ctx, prompt, config.url, config.apiKey, config.size, config.sdParams);
    } else {
        // Default: OpenAI DALL-E
        if (config.apiKey.isEmpty()) {
            emitGenerationError(convId,
                                ctx.jobId,
                                QStringLiteral("OpenAI API key not configured for image "
                                               "generation"),
                                /*synchronous=*/true);
            return;
        }
        emit requestOpenAI(ctx, prompt, config.size, config.quality, config.apiKey);
    }
}

void ImageService::generateImageFromMap(const QString& convId,
                                        const QString& prompt,
                                        const QVariantMap& config) {
    // Preferred path: resolve from the active registry provider. The
    // QML "Generate Image" dialog supplies only {size, quality}; every
    // other field (shape, base URL, model, auth) comes from the active
    // provider. A non-empty explicit "backend"/"endpointShape" override
    // falls through to the legacy resolution below for test/headless
    // callers that drive ImageService directly.
    if (m_providerRegistry && config.value(QStringLiteral("backend")).toString().isEmpty() &&
        config.value(QStringLiteral("endpointShape")).toString().isEmpty() &&
        config.value(QStringLiteral("url")).toString().isEmpty() &&
        config.value(QStringLiteral("sdPath")).toString().isEmpty()) {
        QVariantMap overrides;
        if (config.contains(QStringLiteral("size"))) {
            overrides.insert(QStringLiteral("size"), config.value(QStringLiteral("size")));
        }
        if (config.contains(QStringLiteral("quality"))) {
            overrides.insert(QStringLiteral("quality"), config.value(QStringLiteral("quality")));
        }
        generateImageFromActiveProvider(convId, prompt, overrides);
        return;
    }

    ImageGenConfig cfg;
    cfg.size = config.value(QStringLiteral("size"), QStringLiteral("1024x1024")).toString();
    cfg.quality = config.value(QStringLiteral("quality"), QStringLiteral("standard")).toString();

    if (config.contains(QStringLiteral("sdParams"))) {
        const QVariantMap raw = config.value(QStringLiteral("sdParams")).toMap();
        cfg.sdParams = QJsonObject::fromVariantMap(raw);
    }

    // Backend resolution. Explicit override wins; otherwise consult
    // SettingsService and translate the user-facing name to the
    // worker-internal name.
    QString explicitBackend = config.value(QStringLiteral("backend")).toString();
    QString userFacing;  // What the user picked in Settings → Image Gen.
    if (!explicitBackend.isEmpty()) {
        cfg.backend = explicitBackend;
    } else if (m_settingsService) {
        userFacing = m_settingsService->imageGenActiveBackend();
        if (userFacing == QStringLiteral("openai_dalle")) {
            cfg.backend = QStringLiteral("openai");
        } else if (userFacing == QStringLiteral("local_cli")) {
            cfg.backend = QStringLiteral("local_sd");
        } else if (userFacing == QStringLiteral("remote_openai_compat")) {
            cfg.backend = QStringLiteral("openai_compat");
        } else if (userFacing == QStringLiteral("remote_a1111")) {
            cfg.backend = QStringLiteral("a1111");
        } else {
            cfg.backend = QStringLiteral("openai");
        }
    } else {
        cfg.backend = QStringLiteral("openai");
    }

    // API key + URL + sdPath fallback by selected backend.
    cfg.apiKey = config.value(QStringLiteral("apiKey")).toString();
    cfg.url = config.value(QStringLiteral("url")).toString();
    cfg.sdPath = config.value(QStringLiteral("sdPath")).toString();

    if (m_settingsService) {
        if (cfg.backend == QStringLiteral("openai") && cfg.apiKey.isEmpty()) {
            cfg.apiKey = m_settingsService->apiKey(QStringLiteral("openai"));
        } else if (cfg.backend == QStringLiteral("local_sd") && cfg.sdPath.isEmpty()) {
            cfg.sdPath = m_settingsService->imageGenLocalSdPath();
        } else if (cfg.backend == QStringLiteral("openai_compat")) {
            if (cfg.url.isEmpty()) {
                cfg.url = m_settingsService->imageGenOpenAICompatUrl();
            }
            if (cfg.apiKey.isEmpty()) {
                cfg.apiKey = m_settingsService->imageGenOpenAICompatKey();
            }
        } else if (cfg.backend == QStringLiteral("a1111")) {
            if (cfg.url.isEmpty()) {
                cfg.url = m_settingsService->imageGenA1111Url();
            }
            if (cfg.apiKey.isEmpty()) {
                cfg.apiKey = m_settingsService->imageGenA1111Key();
            }
        }
    }

    generateImage(convId, prompt, cfg);
}

void ImageService::generateImageFromActiveProvider(const QString& convId,
                                                   const QString& prompt,
                                                   const QVariantMap& overrides) {
    if (!m_providerRegistry) {
        emit generationStarted(convId);
        emitGenerationError(convId,
                            QString(),
                            QStringLiteral("Image generation is unavailable in "
                                           "this session."),
                            /*synchronous=*/true);
        return;
    }

    const ActiveImageConfig active = m_providerRegistry->activeConfig();
    if (!active.valid) {
        emit generationStarted(convId);
        emitGenerationError(convId,
                            QString(),
                            QStringLiteral("No image provider is configured. Open "
                                           "Settings -> Image Generation, add a "
                                           "provider, and set it active."),
                            /*synchronous=*/true);
        return;
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
    // Size / quality default to the provider's stored values; the caller
    // may override either.
    cfg.size = overrides
                   .value(QStringLiteral("size"),
                          active.size.isEmpty() ? QStringLiteral("1024x1024") : active.size)
                   .toString();
    cfg.quality = overrides
                      .value(QStringLiteral("quality"),
                             active.quality.isEmpty() ? QStringLiteral("standard") : active.quality)
                      .toString();

    generateImage(convId, prompt, cfg);
}

void ImageService::refineImage(const QString& convId,
                               const QString& sourceImagePath,
                               const QString& prompt) {
    if (!m_providerRegistry) {
        emit generationStarted(convId);
        emitGenerationError(convId,
                            QString(),
                            QStringLiteral("Image refinement is unavailable in "
                                           "this session."),
                            /*synchronous=*/true);
        return;
    }
    const ActiveImageConfig active = m_providerRegistry->activeConfig();
    if (!active.valid) {
        emit generationStarted(convId);
        emitGenerationError(convId,
                            QString(),
                            QStringLiteral("No image provider is configured. Open "
                                           "Settings -> Image Generation, add a "
                                           "provider, and set it active."),
                            /*synchronous=*/true);
        return;
    }
    // Editing an existing image requires sending it back as a reference;
    // only the chat-image shape carries image input. Other shapes
    // (DALL-E images, A1111, local CLI) can't refine in this flow.
    if (active.endpointShape != QStringLiteral("openai_chat_image")) {
        emit generationStarted(convId);
        emitGenerationError(convId,
                            QString(),
                            QStringLiteral("Refine works only with a chat-image "
                                           "provider (one that accepts a reference "
                                           "image). Set one active in Settings -> "
                                           "Image Generation."),
                            /*synchronous=*/true);
        return;
    }
    if (sourceImagePath.isEmpty()) {
        emit generationStarted(convId);
        emitGenerationError(convId,
                            QString(),
                            QStringLiteral("Refine needs a source image."),
                            /*synchronous=*/true);
        return;
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
    cfg.sdParams = active.extraParams;
    cfg.outputModalities =
        active.outputModalities.isEmpty() ? QStringLiteral("image") : active.outputModalities;
    cfg.size = active.size.isEmpty() ? QStringLiteral("1024x1024") : active.size;
    cfg.quality = active.quality.isEmpty() ? QStringLiteral("standard") : active.quality;
    cfg.sourceImagePath = sourceImagePath;

    generateImage(convId, prompt, cfg);
}

void ImageService::setMessageService(MessageService* svc) {
    m_msgService = svc;
}

void ImageService::setSettingsService(SettingsService* svc) {
    m_settingsService = svc;
}

void ImageService::setImageProviderRegistry(ImageProviderRegistry* registry) {
    m_providerRegistry = registry;
}

void ImageService::setAuditService(AuditService* svc) {
    m_auditService = svc;
}

void ImageService::setTaskObserver(TaskObserver* observer) {
    m_taskObserver = observer;
}

void ImageService::setTaskGateService(TaskGateService* gate) {
    m_taskGate = gate;
}

/*
 * @brief Sends an image to the active LLM for vision analysis.
 *
 * Reads the image file (max 10 MiB), encodes as base64, and builds a
 * multimodal LlmRequest appropriate for the active provider format.
 * The response is streamed back via ModelRouter signals; the caller should
 * connect to ModelRouter::chunkReceived / requestFinished directly.
 *
 * @param convId     Conversation UUID.
 * @param imagePath  Absolute path to image file.
 * @param question   Question about the image.
 */
void ImageService::analyzeImage(const QString& convId,
                                const QString& imagePath,
                                const QString& question) {
    Q_UNUSED(convId)

    // Read and encode the image
    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit error(convId, QStringLiteral("Cannot read image file: ") + imagePath);
        return;
    }
    const QByteArray rawBytes = f.read(10 * 1024 * 1024);  // cap at 10 MiB
    f.close();

    const QString mime = m_fileService.mimeType(imagePath);
    const QString encoded = QString::fromLatin1(rawBytes.toBase64());
    const QString dataUri = QStringLiteral("data:%1;base64,%2").arg(mime, encoded);

    // Build a multimodal message matching OpenAI vision format.
    // Most modern providers accept this or a close variant.
    QJsonArray contentArr;
    {
        QJsonObject textPart;
        textPart[QStringLiteral("type")] = QStringLiteral("text");
        textPart[QStringLiteral("text")] = question;
        contentArr.append(textPart);

        QJsonObject imgPart;
        imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
        QJsonObject urlObj;
        urlObj[QStringLiteral("url")] = dataUri;
        imgPart[QStringLiteral("image_url")] = urlObj;
        contentArr.append(imgPart);
    }

    LlmMessage userMsg;
    userMsg.role = QStringLiteral("user");
    userMsg.content = QJsonDocument(contentArr).toJson(QJsonDocument::Compact);

    LlmRequest req;
    req.messages = {userMsg};
    req.systemPrompt = QStringLiteral("You are a helpful vision assistant. Describe the image and "
                                      "answer the question accurately.");

    // Route via ModelRouter — streaming response goes to whoever is connected
    m_router.route(req);
}

/*
 * @brief Returns all image paths for the given conversation.
 * @param convId Conversation UUID.
 * @return List of absolute paths.
 */
QStringList ImageService::conversationImages(const QString& convId) const {
    return m_conversationImages.value(convId);
}


/**
 * @brief Worker progress callback.
 *
 * @param ctx     Job context echoed from the worker's inbound dispatch.
 * @param percent 0-100 generation progress.
 */
void ImageService::onWorkerProgress(const JobContext& ctx, int percent) {
    // Public QML-facing signal still takes (convId, percent) for source
    // compatibility — the convId comes from ctx, not from a service
    // field. A degenerate empty ctx is logged once and skipped (L3
    // makes this structurally impossible but the guard makes future
    // bugs visible).
    if (!ctx.isValid()) {
        qCWarning(verzetaUi) << "ImageService::onWorkerProgress invoked with invalid ctx"
                             << "(jobId or conversationId empty) — dropping progress emit";
        return;
    }
    emit generationProgress(ctx.conversationId, percent);
}

/**
 * @brief Called when the worker signals imageReady.
 *
 * @param ctx       Job context echoed from the worker's inbound dispatch.
 * @param localPath Path to the saved image file.
 *
 * Every value comes from ctx, which was stamped at enqueue and is
 * unique per job. No mutable service field participates in
 * attribution, so overlapping generateImage() calls cannot clobber
 * each other's outcome.
 */
void ImageService::onImageReady(const JobContext& ctx, const QString& localPath) {
    if (!ctx.isValid()) {
        qCWarning(verzetaUi) << "ImageService::onImageReady invoked with invalid ctx"
                             << "(jobId or conversationId empty) — dropping completion";
        return;
    }
    const QString convId = ctx.conversationId;
    const QString prompt = ctx.prompt;

    m_conversationImages[convId].append(localPath);

    // File the image into the PROJECT WORKSPACE (the same root
    // write_file artifacts land in) so it is visible to the files /
    // artifact surfaces — previously it only existed as an internal
    // attachment, chat-visible but invisible everywhere else. A slug
    // of the prompt + the job id keeps names meaningful and unique;
    // failure never blocks the chat persist below.
    QString workspaceRelPath;
    QString workspaceAbsPath;
    if (QFileInfo::exists(localPath)) {
        // Resolve the workspace from THIS image's conversation, not the
        // FileService's mutable active context — a UI-triggered refine has
        // no active project set, which is how images landed in the base
        // projects dir instead of the conversation's project folder.
        const QString workspaceRoot = m_workspaceDirResolver ? m_workspaceDirResolver(convId)
                                                             : m_fileService.activeProjectDir();
        const QString imagesDir = workspaceRoot + QStringLiteral("/images");
        if (QDir().mkpath(imagesDir)) {
            QString slug = prompt.toLower();
            static const QRegularExpression kNonSlugRx(QStringLiteral("[^a-z0-9]+"));
            slug.replace(kNonSlugRx, QStringLiteral("-"));
            slug = slug.left(40).trimmed();
            while (slug.startsWith(QLatin1Char('-')))
                slug.remove(0, 1);
            while (slug.endsWith(QLatin1Char('-')))
                slug.chop(1);
            if (slug.isEmpty())
                slug = QStringLiteral("image");
            const QString name =
                slug + QStringLiteral("-") + ctx.jobId.left(8) + QStringLiteral(".png");
            const QString dest = imagesDir + QStringLiteral("/") + name;
            if (QFile::copy(localPath, dest)) {
                workspaceRelPath = QStringLiteral("images/") + name;
                workspaceAbsPath = dest;
                qCInfo(verzetaUi) << "ImageService: filed generated image into the"
                                  << "workspace:" << dest;
            } else {
                qCWarning(verzetaUi)
                    << "ImageService: workspace copy failed for" << localPath << "→" << dest;
            }
        }
    }

    // File artifact on the active plan (timeline/artifact surfaces).
    // The tool CALL was recorded when generate_image dispatched; this
    // records the asynchronous OUTPUT once it exists.
    if (!workspaceAbsPath.isEmpty() && m_taskObserver && m_taskGate) {
        const QString planId = m_taskGate->activePlanId();
        if (!planId.isEmpty()) {
            m_taskObserver->recordFile(
                planId, workspaceAbsPath, ctx.requestingAlias, QStringLiteral("generate_image"));
        }
    }

    if (m_msgService && !convId.isEmpty()) {
        const QFileInfo fi(localPath);

        Message msg;
        msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        msg.conversationId = convId;
        msg.role = QStringLiteral("assistant");
        msg.content = prompt.isEmpty() ? QStringLiteral("Generated image.")
                                       : QStringLiteral("Generated image: %1").arg(prompt);
        if (!workspaceRelPath.isEmpty()) {
            msg.content += QStringLiteral("\n\nSaved to project files: %1").arg(workspaceRelPath);
        }
        msg.createdAt = QDateTime::currentDateTime();
        msg.finishReason = QStringLiteral("stop");
        // Tag the metadata so downstream consumers (artifact model,
        // task observer) can distinguish this from a normal LLM reply.
        msg.metadata.insert(QStringLiteral("produced_by"), QStringLiteral("image_service"));
        // Workspace location of the filed copy — ArtifactsModel's
        // image case reads these. Without them a generated image was
        // structurally invisible to the Artifacts panel: the tool
        // result is async ("queued", no path field) and this
        // completion message is a plain assistant row, so neither of
        // the panel's original sources ever matched.
        if (!workspaceAbsPath.isEmpty()) {
            msg.metadata.insert(QStringLiteral("workspace_path"), workspaceAbsPath);
            msg.metadata.insert(QStringLiteral("workspace_rel"), workspaceRelPath);
        }
        msg.metadata.insert(QStringLiteral("job_id"), ctx.jobId);
        // Attribute the completion to the REQUESTING member so the
        // bubble and group history show whose image this is (empty for
        // user/QML-driven generations — unchanged rendering there).
        msg.agentId = ctx.requestingAgentId;
        msg.memberAlias = ctx.requestingAlias;

        const QString msgId = m_msgService->addMessage(msg);
        if (msgId.isEmpty()) {
            qCWarning(verzetaUi) << "ImageService: failed to persist auto-generated message"
                                 << "for image" << localPath << "(job" << ctx.jobId << ")";
        } else {
            Attachment att;
            att.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            att.messageId = msgId;
            att.type = QStringLiteral("image");
            att.filename = fi.fileName();
            att.mimeType = QStringLiteral("image/png");
            att.dataPath = localPath;  // absolute path; QML prefixes file://
            att.createdAt = QDateTime::currentDateTime();

            if (m_msgService->addAttachment(att).isEmpty()) {
                qCWarning(verzetaUi)
                    << "ImageService: persisted message but attachment write failed"
                    << "for image" << localPath << "msg" << msgId;
            }
        }
    }

    emit imageGenerated(convId, localPath);

    // Agent-requested generations get a FOLLOW-UP: the chat layer
    // resumes the requesting agent so the completion is acted on
    // instead of landing as a silent history row after the cascade
    // ended (the per-image stall). Prefer the workspace copy (what the
    // chat message references); fall back to the raw output file.
    if (!ctx.requestingAgentId.isEmpty()) {
        emit imageReadyForFollowUp(convId,
                                   ctx.requestingAlias,
                                   ctx.requestingAgentId,
                                   workspaceAbsPath.isEmpty() ? localPath : workspaceAbsPath);
    }

    if (m_auditService) {
        m_auditService->record(ActivityEvent::forImageGenerated(QString(),
                                                                convId,
                                                                ctx.jobId,
                                                                ctx.requestingAlias,
                                                                ctx.requestingAgentId,
                                                                prompt,
                                                                localPath));
    }
}

/**
 * @brief Called when the worker signals errorOccurred.
 *
 * @param ctx      Job context echoed from the worker's inbound dispatch.
 * @param errorMsg Error description.
 */
void ImageService::onWorkerError(const JobContext& ctx, const QString& errorMsg) {
    if (!ctx.isValid()) {
        qCWarning(verzetaUi) << "ImageService::onWorkerError invoked with invalid ctx"
                             << "— dropping error emit:" << errorMsg;
        return;
    }
    // Async worker failure (queued connection) — lands after the tool
    // pair is persisted, so role=assistant is safe + visible.
    emitGenerationError(ctx.conversationId,
                        ctx.jobId,
                        errorMsg,
                        /*synchronous=*/false);
}

void ImageService::emitGenerationError(const QString& convId,
                                       const QString& jobId,
                                       const QString& message,
                                       bool synchronous) {
    // Persist the failure as a visible row in the owning conversation so
    // it is never silent (Rule 11). Without it the user waits for an image
    // that never arrives and neither they nor the agent learns why.
    if (m_msgService && !convId.isEmpty()) {
        const QString content = QStringLiteral("⚠ Image generation failed: %1").arg(message);
        QJsonObject meta;
        meta.insert(QStringLiteral("produced_by"), QStringLiteral("image_service"));
        meta.insert(QStringLiteral("image_error"), true);
        meta.insert(QStringLiteral("job_id"), jobId);

        QString persistedId;
        if (synchronous) {
            // Mid-tool-invoke: the generate_image tool calls generateImage
            // synchronously, so this row would land BETWEEN the
            // assistant[tool_calls] row and its role=tool result. It MUST
            // be a role=system intervening row or the request-builder
            // walk-and-pair drops the tool result as an orphan and the
            // agent loops calling the tool every turn.
            persistedId = m_msgService->addInterveningSystemMessage(convId, content, meta);
        } else {
            // Async worker failure: arrives after the tool pair is already
            // persisted, so role=assistant is safe (same categorisation as
            // onImageReady) and renders inline for the user.
            Message msg;
            msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            msg.conversationId = convId;
            msg.role = QStringLiteral("assistant");
            msg.content = content;
            msg.createdAt = QDateTime::currentDateTime();
            msg.finishReason = QStringLiteral("error");
            msg.metadata = meta;
            persistedId = m_msgService->addMessage(msg);
        }
        if (persistedId.isEmpty()) {
            qCWarning(verzetaUi) << "ImageService: failed to persist image-error message for"
                                 << "conv" << convId << "(job" << jobId << "):" << message;
        }
    }

    emit error(convId, message);
}

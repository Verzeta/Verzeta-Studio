// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file app-controller.cpp
 * @brief Implementation of the application bootstrap controller. Initializes
 *        all services in dependency order and wires C++ types to the QML
 *        engine as singletons and context properties.
 * @layer API (application bootstrap)
 * @dependencies All services, DbManager, Logger, Qt6::Quick
 */


#include "app-controller.h"

#include "api/anthropic-provider.h"
#include "api/deepseek-provider.h"
#include "api/gemini-provider.h"
#include "api/llamacpp-provider.h"
#include "api/llamacpp-remote-provider.h"
#include "api/ollama-provider.h"
#include "api/openai-provider.h"
#include "api/openrouter-provider.h"
#include "models/conversation.h"
#include "models/heartbeat-reports-model.h"
#include "models/llm-config.h"
#include "models/skills-model.h"
#include "remote/wire-host-bridge.h"
#include "services/acn-service.h"
#include "services/agent-memory-service.h"
#include "services/agent-service.h"
#include "services/audio-service.h"
#include "services/audit-service.h"
#include "services/background-process-service.h"
#include "services/chat/acn-source.h"
#include "services/chat/aim-source.h"
#include "services/chat/cascade-controller.h"
#include "services/chat/memory-retriever.h"
#include "services/chat/provider-scheduler.h"
#include "services/chat/rag-source.h"
#include "services/chat/request-builder.h"
#include "services/clawhub-client.h"
#include "services/conversation-summarizer.h"
#include "services/custom-server-registry.h"
#include "services/export-service.h"
#include "services/file-service.h"
#include "services/folder-mount-client-wire.h"
#include "services/folder-mount-registry.h"
#include "services/heartbeat-config-service.h"
#include "services/heartbeat-subagent-service.h"
#include "services/image-provider-registry.h"
#include "services/image-service.h"
#include "services/rag-service.h"
#include "services/search-service.h"
#include "services/search/web-search-provider-registry.h"
#include "services/search/web-search-service.h"
#include "services/skill-service.h"
#include "services/subagent-run-service.h"
#include "services/tool-service.h"
#include "services/voice-call-service.h"
#include "services/voice-model-downloader.h"
#include "tools/tool-registration-context.h"
#include "tools/tool-registration.h"
#include "utils/logger.h"
#include "utils/markdown-utils.h"
#include "utils/notification-manager.h"
#include "utils/path-utils.h"
#include "utils/process-sandbox.h"

#include <QTimer>
#include <QtQml/qqmlengine.h>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QQmlContext>
#include <QStandardPaths>
// utils/syntax-highlighter.h removed — replaced by upstream
// org.kde.syntaxhighlighting QML module (see CodeBlock.qml /
// CanvasEditor.qml). KF6::SyntaxHighlighting is still linked for
// runtime QML module resolution.
#include "services/inference-sidecar-host.h"
#include "workers/embedding-worker.h"

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs AppController. Does not perform initialization.
 * @param parent Optional Qt parent.
 */
AppController::AppController(QObject* parent) : QObject(parent), m_db(DbManager::instance()) {}

/**
 * @brief Destroys AppController and closes the database connection.
 * @sideeffects Destroys services in LIFO order (ChatController first,
 *              then providers, then DB services). Stops embedding thread.
 *              Closes DB last.
 */
AppController::~AppController() {
    // Proper worker-thread shutdown sequence:
    //   1. Ask the worker to stop any in-flight computation (atomic flag
    //      + abort the pending HTTP reply, unblocking its QEventLoop).
    //   2. Schedule the worker's deletion on its own thread via deleteLater.
    //      release() the unique_ptr so we don't double-delete.
    //   3. Tell the thread's event loop to stop (quit()).
    //   4. Wait for the thread to finish, bounded by a 3s timeout.
    if (m_embeddingWorker) {
        m_embeddingWorker->requestStop();
        EmbeddingWorker* raw = m_embeddingWorker.release();
        raw->deleteLater();
    }
    if (m_queryEmbeddingWorker) {
        m_queryEmbeddingWorker->requestStop();
        EmbeddingWorker* raw = m_queryEmbeddingWorker.release();
        raw->deleteLater();
    }
    if (m_embeddingThread && m_embeddingThread->isRunning()) {
        m_embeddingThread->quit();
        if (!m_embeddingThread->wait(3000)) {
            qCWarning(verzetaUi) << "AppController: embedding thread did not exit in 3s — "
                                    "forcing terminate";
            m_embeddingThread->terminate();
            m_embeddingThread->wait();
        }
    }
    if (m_queryEmbeddingThread && m_queryEmbeddingThread->isRunning()) {
        m_queryEmbeddingThread->quit();
        if (!m_queryEmbeddingThread->wait(3000)) {
            qCWarning(verzetaUi) << "AppController: query-embedding thread did not exit in 3s — "
                                    "forcing terminate";
            m_queryEmbeddingThread->terminate();
            m_queryEmbeddingThread->wait();
        }
    }

    m_db.close();
    qCInfo(verzetaUi) << "AppController destroyed, database closed";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void AppController::applyEmbeddingBackend(EmbeddingWorker* worker) {
    if (!worker || !m_settingsService) {
        return;
    }
    // Local (in-process llama.cpp) only when selected AND the build has
    // llama.cpp AND the chosen GGUF exists — mirrors the RAGP local/remote
    // decision. Otherwise fall back to the remote OpenAI-compatible endpoint.
    if (m_settingsService->embeddingLocalEnabled() && m_settingsService->hasLocalLlama()) {
        const QString fn = m_settingsService->embeddingLocalModelFilename();
        const QString path = m_settingsService->embeddingModelsDir() + QLatin1Char('/') + fn;
        if (!fn.isEmpty() && QFileInfo::exists(path)) {
            worker->setBackend(EmbeddingWorker::EmbeddingBackend::LlamaCpp);
            worker->setModelPath(path);
            return;
        }
        qCWarning(verzetaUi) << "AppController: local embeddings selected but model unavailable"
                             << "(" << path << ") — using the remote endpoint instead.";
    }
    worker->setBackend(EmbeddingWorker::EmbeddingBackend::OpenAI);
    worker->setApiKey(m_settingsService->apiKey(QStringLiteral("openai")));
    worker->setEndpointBaseUrl(m_settingsService->embeddingEndpointBaseUrl());
    worker->setModel(m_settingsService->embeddingModel());
}

/*
 * @brief Stores the parsed command-line options for later use by
 *        `initialize()`'s wire-daemon autostart hook.
 * @param opts Resolved options struct. Stored by value.
 */
void AppController::setCommandLineOptions(const Verzeta::Utils::CommandLineOptions& opts) {
    m_cliOptions = opts;
}

/**
 * @brief Initializes logging, database, and all services in dependency order.
 * @return true if all initialization steps succeeded.
 * @sideeffects Opens DB, creates services, registers all LLM providers,
 *              applies saved settings to providers.  When command-line
 *              options were supplied with `headlessRemote = true`, fires
 *              `WireHostBridge::startRemoteServer(...)` at the end of the
 *              setup graph.
 */
bool AppController::initialize() {
    // 1. Set up logging first so all subsequent steps are visible in logs
    const QString logDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
    Logger::initialize(logDir);

    qCInfo(verzetaUi) << "Verzeta Studio initializing...";

    // 2. Open the database
    const QString dbPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                           QStringLiteral("/verzeta-studio.db");

    if (!m_db.open(dbPath)) {
        qCCritical(verzetaUi) << "Failed to open database at" << dbPath;
        return false;
    }

    // 3. Run schema migrations
    if (!m_db.runMigrations()) {
        qCCritical(verzetaUi) << "Database migration failed";
        return false;
    }

    {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList vecCandidates = {
            appDir + QStringLiteral("/vec0.so"),
            appDir + QStringLiteral("/vec0.dll"),
            appDir + QStringLiteral("/vec0.dylib"),
        };
        QString vecPath;
        for (const QString& candidate : vecCandidates) {
            if (QFileInfo::exists(candidate)) {
                vecPath = candidate;
                break;
            }
        }
        m_db.loadVectorExtension(vecPath);  // false/no-op when path is empty
    }

    m_convService = std::make_unique<ConversationService>(m_db, this);
    m_msgService = std::make_unique<MessageService>(m_db, this);

    m_auditService = std::make_unique<AuditService>(m_db, this);
    m_auditService->setConversationService(m_convService.get());

    // Workspace mount registry — metadata-only substrate for client-
    // owned virtual workspace mounts. Constructed AFTER AuditService so
    // we can wire the lifecycle audit hooks immediately. Persists rows
    // in the `folder_mounts` table (schema v17); bytes of workspace
    // files are NEVER stored here.
    m_folderMountRegistry = std::make_unique<FolderMountRegistry>(m_db, this);
    m_folderMountRegistry->setAuditService(m_auditService.get());

    m_helpService = std::make_unique<HelpService>(this);

    m_settingsService = std::make_unique<SettingsService>(m_db, this);

    // Voice-daemon detection + lifecycle. Persistence rides SettingsService
    // through the public voice accessors, injected as callbacks so the
    // service has no SettingsService link dependency (unit tests stay
    // link-light). QPointer guards the captures: the callbacks are inert if
    // settings is ever gone. The constructor runs the start-of-app detect
    // (and the autostart spawn when set); every later detect is
    // user-triggered (no filesystem polling).
    {
        QPointer<SettingsService> settings = m_settingsService.get();
        Verzeta::Voice::VoiceCallService::Persistence persist;
        persist.readExplicitPath = [settings]() {
            return settings ? settings->voiceBinaryPath() : QString();
        };
        persist.writeExplicitPath = [settings](const QString& value) {
            if (settings)
                settings->setVoiceBinaryPath(value);
        };
        persist.readAutostart = [settings]() {
            return settings ? settings->voiceAutostart() : false;
        };
        persist.writeAutostart = [settings](bool on) {
            if (settings)
                settings->setVoiceAutostart(on);
        };
        persist.readDefaultVoice = [settings]() {
            return settings ? settings->voiceDefaultVoice() : QString();
        };
        persist.writeDefaultVoice = [settings](const QString& voiceId) {
            if (settings)
                settings->setVoiceDefaultVoice(voiceId);
        };
        persist.readVoiceAssignments = [settings]() {
            return settings ? settings->voiceAssignments() : QVariantMap();
        };
        persist.writeVoiceAssignments = [settings](const QVariantMap& v) {
            if (settings)
                settings->setVoiceAssignments(v);
        };
        persist.readPttHotkey = [settings]() {
            return settings ? settings->voicePttHotkey() : QString();
        };
        persist.writePttHotkey = [settings](const QString& sequence) {
            if (settings)
                settings->setVoicePttHotkey(sequence);
        };
        m_voiceCallService =
            std::make_unique<Verzeta::Voice::VoiceCallService>(std::move(persist), this);

        // Model installer for the add-on. Its target directory follows
        // the daemon's greet (empty until then), so a download can never
        // guess at a path the daemon does not actually read.
        m_voiceModelDownloader = std::make_unique<Verzeta::Voice::VoiceModelDownloader>(this);
        connect(
            m_voiceCallService.get(),
            &Verzeta::Voice::VoiceCallService::serviceStateChanged,
            m_voiceModelDownloader.get(),
            [this]() { m_voiceModelDownloader->setTargetDir(m_voiceCallService->modelsDir()); });
    }

    // Each provider gets its own HttpClient so requests don't conflict
    m_httpOllama = std::make_unique<HttpClient>(this);
    m_httpOpenai = std::make_unique<HttpClient>(this);
    m_httpAnthropic = std::make_unique<HttpClient>(this);
    m_httpGemini = std::make_unique<HttpClient>(this);

    m_httpOllamaBg = std::make_unique<HttpClient>(this);
    m_httpOpenaiBg = std::make_unique<HttpClient>(this);
    m_httpAnthropicBg = std::make_unique<HttpClient>(this);
    m_httpGeminiBg = std::make_unique<HttpClient>(this);

    m_httpOpenRouter = std::make_unique<HttpClient>(this);
    m_httpOpenRouterBg = std::make_unique<HttpClient>(this);
    m_httpDeepSeek = std::make_unique<HttpClient>(this);
    m_httpDeepSeekBg = std::make_unique<HttpClient>(this);
    m_httpLlamaCppRemote = std::make_unique<HttpClient>(this);
    m_httpLlamaCppRemoteBg = std::make_unique<HttpClient>(this);

    m_router = std::make_unique<ModelRouter>(this);

    m_summarizer = std::make_unique<ConversationSummarizer>(
        m_db, *m_msgService, *m_convService, *m_router, *m_settingsService, this);

    // L9 invalidation wiring — roster + folder context shifts make a
    // summary's attributions stale. Rename / touch / pin do NOT
    // invalidate (conversationFolderChanged exists precisely so we
    // don't have to listen to the over-broad conversationUpdated).
    connect(m_convService.get(),
            &ConversationService::conversationFolderChanged,
            m_summarizer.get(),
            [this](const QString& convId) {
                m_summarizer->invalidate(convId, QStringLiteral("folder_changed"));
            });


    // Ollama (local) — always available; URL from settings
    {
        // Foreground
        auto p = std::make_unique<OllamaProvider>(*m_httpOllama, this);
        p->setBaseUrl(m_settingsService->ollamaBaseUrl());
        connect(m_settingsService.get(),
                &SettingsService::ollamaBaseUrlChanged,
                p.get(),
                [this, raw = p.get()]() { raw->setBaseUrl(m_settingsService->ollamaBaseUrl()); });
        m_router->registerProvider(std::move(p));

        // Background sibling
        auto pBg = std::make_unique<OllamaProvider>(*m_httpOllamaBg, this);
        pBg->setBaseUrl(m_settingsService->ollamaBaseUrl());
        connect(m_settingsService.get(),
                &SettingsService::ollamaBaseUrlChanged,
                pBg.get(),
                [this, raw = pBg.get()]() { raw->setBaseUrl(m_settingsService->ollamaBaseUrl()); });
        m_router->registerBackgroundProvider(std::move(pBg));
    }

    // OpenAI — API key from settings
    {
        auto p = std::make_unique<OpenAIProvider>(*m_httpOpenai, this);
        p->setApiKey(m_settingsService->apiKey(QStringLiteral("openai")));
        m_router->registerProvider(std::move(p));

        auto pBg = std::make_unique<OpenAIProvider>(*m_httpOpenaiBg, this);
        pBg->setApiKey(m_settingsService->apiKey(QStringLiteral("openai")));
        m_router->registerBackgroundProvider(std::move(pBg));
    }

    // Anthropic — API key from settings
    {
        auto p = std::make_unique<AnthropicProvider>(*m_httpAnthropic, this);
        p->setApiKey(m_settingsService->apiKey(QStringLiteral("anthropic")));
        m_router->registerProvider(std::move(p));

        auto pBg = std::make_unique<AnthropicProvider>(*m_httpAnthropicBg, this);
        pBg->setApiKey(m_settingsService->apiKey(QStringLiteral("anthropic")));
        m_router->registerBackgroundProvider(std::move(pBg));
    }

    // Gemini — API key from settings
    {
        auto p = std::make_unique<GeminiProvider>(*m_httpGemini, this);
        p->setApiKey(m_settingsService->apiKey(QStringLiteral("gemini")));
        m_router->registerProvider(std::move(p));

        auto pBg = std::make_unique<GeminiProvider>(*m_httpGeminiBg, this);
        pBg->setApiKey(m_settingsService->apiKey(QStringLiteral("gemini")));
        m_router->registerBackgroundProvider(std::move(pBg));
    }

    {
        // The sidecar host must exist before any provider/bridge that
        // rides it (cheap: no process spawns until the first request).
        if (!m_inferenceSidecarHost) {
            m_inferenceSidecarHost = std::make_unique<Verzeta::Infer::InferenceSidecarHost>();
        }
        // Live acceleration relay: every successful model load reports
        // what actually runs (ggml device table) — the Settings pages
        // bind to ragpAccelerationLive instead of a compile-time guess.
        connect(m_inferenceSidecarHost.get(),
                &Verzeta::Infer::InferenceSidecarHost::slotAccelerationChanged,
                this,
                [this](const QString& slot, const QString& accel) {
                    if (slot == QLatin1String("embed"))
                        return;
                    if (m_ragpAccelerationLive == accel)
                        return;
                    m_ragpAccelerationLive = accel;
                    emit ragpAccelerationLiveChanged();
                });
        auto p = std::make_unique<LlamaCppProvider>(this);
        p->setSidecarHost(m_inferenceSidecarHost.get());
        p->setModelDir(m_settingsService->llamaCppModelDir());
        m_router->registerProvider(std::move(p));
    }

    // Recommended-model downloader (First Run Wizard + provider
    // settings pages). Downloads run ONLY on explicit user action;
    // targets are the same folders the Browse pickers symlink into.
    m_modelDownloadService = std::make_unique<ModelDownloadService>(this);
    m_modelDownloadService->setTargetDirProvider([this](const QString& kind) {
        return kind == QLatin1String("embed") ? m_settingsService->embeddingModelsDir()
                                              : m_settingsService->ragpModelsDir();
    });


    // OpenRouter — API key from settings, optional base URL override.
    {
        auto p = std::make_unique<OpenRouterProvider>(*m_httpOpenRouter, this);
        p->setApiKey(m_settingsService->apiKey(QStringLiteral("openrouter")));
        const QString baseOverride =
            m_settingsService->providerBaseUrl(QStringLiteral("openrouter"));
        if (!baseOverride.isEmpty())
            p->setBaseUrl(baseOverride);
        m_router->registerProvider(std::move(p));

        auto pBg = std::make_unique<OpenRouterProvider>(*m_httpOpenRouterBg, this);
        pBg->setApiKey(m_settingsService->apiKey(QStringLiteral("openrouter")));
        if (!baseOverride.isEmpty())
            pBg->setBaseUrl(baseOverride);
        m_router->registerBackgroundProvider(std::move(pBg));
    }

    // DeepSeek — API key from settings, optional base URL override.
    {
        auto p = std::make_unique<DeepSeekProvider>(*m_httpDeepSeek, this);
        p->setApiKey(m_settingsService->apiKey(QStringLiteral("deepseek")));
        const QString baseOverride = m_settingsService->providerBaseUrl(QStringLiteral("deepseek"));
        if (!baseOverride.isEmpty())
            p->setBaseUrl(baseOverride);
        m_router->registerProvider(std::move(p));

        auto pBg = std::make_unique<DeepSeekProvider>(*m_httpDeepSeekBg, this);
        pBg->setApiKey(m_settingsService->apiKey(QStringLiteral("deepseek")));
        if (!baseOverride.isEmpty())
            pBg->setBaseUrl(baseOverride);
        m_router->registerBackgroundProvider(std::move(pBg));
    }

    // llama.cpp Remote — auth-less by default; base URL configurable
    // (default http://localhost:8080/v1). Tool calling default OFF; user
    // toggle via SettingsService. UNLIKE the in-process LlamaCppProvider
    // this builds unconditionally (no VERZETA_HAS_LLAMACPP guard) and
    // is registered on BOTH foreground AND background slots: the remote
    // variant doesn't load llama.cpp into this process so the H0
    // double-load / B-5 abort risks that gated in-process llama.cpp
    // from background don't apply.
    {
        auto p = std::make_unique<LlamaCppRemoteProvider>(*m_httpLlamaCppRemote, this);
        // API key is optional for llama.cpp server (only used when the
        // user puts the server behind a reverse-proxy that requires
        // bearer auth). We still pass whatever is stored.
        p->setApiKey(m_settingsService->apiKey(QStringLiteral("llamacpp_remote")));
        const QString baseOverride =
            m_settingsService->providerBaseUrl(QStringLiteral("llamacpp_remote"));
        if (!baseOverride.isEmpty())
            p->setBaseUrl(baseOverride);
        p->setSupportsToolCalling(m_settingsService->llamaCppRemoteSupportsTools());
        m_router->registerProvider(std::move(p));

        auto pBg = std::make_unique<LlamaCppRemoteProvider>(*m_httpLlamaCppRemoteBg, this);
        pBg->setApiKey(m_settingsService->apiKey(QStringLiteral("llamacpp_remote")));
        if (!baseOverride.isEmpty())
            pBg->setBaseUrl(baseOverride);
        pBg->setSupportsToolCalling(m_settingsService->llamaCppRemoteSupportsTools());
        m_router->registerBackgroundProvider(std::move(pBg));
    }

    connect(m_settingsService.get(),
            &SettingsService::providerBaseUrlChanged,
            this,
            [this](const QString& providerId, const QString& url) {
                if (auto* fg = m_router->providerForId(providerId)) {
                    if (auto* op = qobject_cast<OpenAIProvider*>(fg)) {
                        op->setBaseUrl(url);
                    }
                }
                if (auto* bg = m_router->bgProviderForId(providerId)) {
                    if (auto* op = qobject_cast<OpenAIProvider*>(bg)) {
                        op->setBaseUrl(url);
                    }
                }
            });
    connect(m_settingsService.get(),
            &SettingsService::llamaCppRemoteSupportsToolsChanged,
            this,
            [this](bool enabled) {
                if (auto* fg = m_router->providerForId(QStringLiteral("llamacpp_remote"))) {
                    if (auto* lr = qobject_cast<LlamaCppRemoteProvider*>(fg)) {
                        lr->setSupportsToolCalling(enabled);
                    }
                }
                if (auto* bg = m_router->bgProviderForId(QStringLiteral("llamacpp_remote"))) {
                    if (auto* lr = qobject_cast<LlamaCppRemoteProvider*>(bg)) {
                        lr->setSupportsToolCalling(enabled);
                    }
                }
            });

    // -----------------------------------------------------------------------
    // Set default active provider
    // -----------------------------------------------------------------------
    const QString defaultProvider = m_settingsService->defaultProvider();
    if (!defaultProvider.isEmpty() && m_router->providerForId(defaultProvider)) {
        // Use saved provider + a sensible default model
        m_router->setActiveProvider(defaultProvider, {});
    } else {
        // Fall back to Ollama with an empty model name until user picks one
        m_router->setActiveProvider(QStringLiteral("ollama"), {});
    }

    // Fetch model lists at startup.
    // Only call refreshModels() on providers that don't require API keys
    // or that already have keys configured. This avoids auth errors.
    if (auto* ollama = m_router->providerForId(QStringLiteral("ollama"))) {
        ollama->refreshModels();
    }
    if (auto* llamacpp = m_router->providerForId(QStringLiteral("llamacpp"))) {
        llamacpp->refreshModels();
    }
    // Cloud providers refresh when their API key is set
    if (m_settingsService->hasApiKey(QStringLiteral("openai"))) {
        if (auto* p = m_router->providerForId(QStringLiteral("openai")))
            p->refreshModels();
    }
    if (m_settingsService->hasApiKey(QStringLiteral("anthropic"))) {
        if (auto* p = m_router->providerForId(QStringLiteral("anthropic")))
            p->refreshModels();
    }
    if (m_settingsService->hasApiKey(QStringLiteral("gemini"))) {
        if (auto* p = m_router->providerForId(QStringLiteral("gemini")))
            p->refreshModels();
    }
    if (m_settingsService->hasApiKey(QStringLiteral("openrouter"))) {
        if (auto* p = m_router->providerForId(QStringLiteral("openrouter")))
            p->refreshModels();
    }
    if (m_settingsService->hasApiKey(QStringLiteral("deepseek"))) {
        if (auto* p = m_router->providerForId(QStringLiteral("deepseek")))
            p->refreshModels();
    }
    if (auto* p = m_router->providerForId(QStringLiteral("llamacpp_remote"))) {
        p->refreshModels();
    }

    // Custom OpenAI-API-compatible server registry — hydrates the
    // persisted catalogue, constructs an OpenAICompatProvider +
    // dedicated HttpClient per row, registers each provider with
    // ModelRouter, and triggers refreshModels() on every hydrated
    // row so the model dropdown is populated before the user opens
    // it.  Declared in the member section AFTER m_router so reverse-
    // declaration destruction unregisters owned providers from
    // ModelRouter before the per-instance HttpClients die.
    m_customServerRegistry =
        std::make_unique<CustomServerRegistry>(*m_router, *m_settingsService, this);

    // ImageProviderRegistry — persisted catalogue of image-generation
    // providers + active-selection controller.  Depends only on
    // SettingsService; hydrates from the image_providers blob (or seeds
    // from the legacy image-gen settings on first run).
    m_imageProviderRegistry = std::make_unique<ImageProviderRegistry>(*m_settingsService, this);

    m_exportService = std::make_unique<ExportService>(*m_convService, *m_msgService, this);
    // ConversationListModel auto-subscribes to ConversationService signals
    // in its own constructor — it handles every change as a row-level
    // mutation. No refresh() wiring needed here.
    m_convListModel = std::make_unique<ConversationListModel>(*m_convService, this);

    // app-level per-provider dispatch scheduler. Created
    // BEFORE the ChatControllers so it can be attached to the local
    // instance below and to every wire-session instance via the
    // SessionRouter setup callback. One instance for the whole process —
    // the single point of per-provider serialization across all runs and
    // all sessions.
    m_providerScheduler = std::make_unique<Chat::ProviderScheduler>(this);

    m_chatController = std::make_unique<ChatController>(
        *m_router, *m_convService, *m_msgService, *m_exportService, this);
    // Share the one scheduler with the local ChatController (and, via the
    // setup callback further below, with every wire-session instance).
    m_chatController->setProviderScheduler(m_providerScheduler.get());
    m_chatController->setInferenceSidecarHost(m_inferenceSidecarHost.get());

    // Voice calls ride the ordinary chat paths in both directions, so a
    // call adds no new way for a message to enter or leave a conversation.
    // Both connections are inert unless a call is live in that exact
    // conversation, which cannot happen without the Verzeta-Voice add-on.
    {
        QPointer<Verzeta::Voice::VoiceCallService> voice = m_voiceCallService.get();
        QPointer<ChatController> chat = m_chatController.get();
        MessageService* messages = m_msgService.get();

        // A finalized assistant reply becomes speech.
        connect(m_chatController.get(),
                &ChatController::streamFinalized,
                this,
                [voice, chat, messages](const QString& msgId, const QString&, bool ok) {
                    if (!ok || !voice || !chat || msgId.isEmpty())
                        return;
                    if (!voice->callActive())
                        return;
                    const Message m = messages->getMessage(msgId);
                    // Named reasons: a silent call is otherwise impossible
                    // to tell apart from a message that was never fetched.
                    if (m.role != QLatin1String("assistant")) {
                        qCWarning(verzetaUi)
                            << "not speaking" << msgId << ": role is"
                            << (m.role.isEmpty() ? QStringLiteral("<not found>") : m.role);
                        return;
                    }
                    if (m.conversationId != voice->callConversationId()) {
                        qCWarning(verzetaUi)
                            << "not speaking" << msgId << ": message is in another conversation";
                        return;
                    }
                    voice->speakMessage(msgId, m.memberAlias, m.content);
                });

        // A finished utterance becomes a user message on the normal send
        // path (the same one typing uses).
        connect(m_voiceCallService.get(),
                &Verzeta::Voice::VoiceCallService::utteranceReady,
                this,
                [chat](const QString& convId, const QString& text) {
                    if (!chat || text.isEmpty())
                        return;
                    if (chat->activeConversationId() != convId)
                        return;
                    chat->sendMessage(text);
                });

        // Pace a group cascade to speech: the next member does not start
        // until the reply that just landed has finished being spoken, so
        // a spoken interruption is heard in the order it happened. The
        // gate is installed only while a call is live; outside a call the
        // cascade holds a null pointer and behaves exactly as before.
        if (Chat::CascadeController* cascade = m_chatController->cascadeInternal()) {
            QPointer<Chat::CascadeController> cascadeGuard(cascade);
            connect(m_voiceCallService.get(),
                    &Verzeta::Voice::VoiceCallService::callStateChanged,
                    this,
                    [voice, cascadeGuard]() {
                        if (!voice || !cascadeGuard)
                            return;
                        cascadeGuard->setVoiceTurnGate(voice->callActive() ? voice.data()
                                                                           : nullptr);
                    });
            connect(m_voiceCallService.get(),
                    &Verzeta::Voice::VoiceCallService::speechFinished,
                    this,
                    [cascadeGuard](const QString& msgId, bool) {
                        if (cascadeGuard)
                            cascadeGuard->releaseVoiceHold(msgId);
                    });
        }
    }

    m_sessionRouter = std::make_unique<Verzeta::Session::SessionRouter>(
        *m_convService, *m_msgService, *m_router, *m_exportService, this);
    m_sessionRouter->registerLocalSession(m_chatController.get());

    m_convController = std::make_unique<ConversationController>(*m_convService, *m_router, this);

    m_agentSettings = std::make_unique<AgentSettingsController>(*m_convService, *m_router, this);
    // Attach SettingsService so availableProviders() / modelsForProvider()
    // filter out providers the user hasn't configured (API key missing,
    // server URL unset, model path empty). Without this every registered
    // provider shows up in the QML dropdowns with its hardcoded default
    // model list.
    m_agentSettings->setSettingsService(m_settingsService.get());

    // Re-fire availableProvidersChanged whenever the custom-server
    // catalogue mutates so QML provider dropdowns pick up a renamed,
    // added, or removed custom server immediately.  The add and remove
    // paths already route through ModelRouter::providersChanged which
    // AgentSettings hooks; this connection covers the update path
    // (display name edit) where ModelRouter's registered set is
    // unchanged but the visible label moved.
    connect(m_customServerRegistry.get(),
            &CustomServerRegistry::serversChanged,
            m_agentSettings.get(),
            &AgentSettingsController::availableProvidersChanged);

    m_exportController = std::make_unique<ExportController>(*m_convService, *m_exportService, this);
    connect(m_chatController.get(),
            &ChatController::activeConversationChanged,
            m_agentSettings.get(),
            [this]() {
                m_agentSettings->setActiveConversationId(m_chatController->activeConversationId());
            });
    // Prime the cached id in case a conversation is already loaded
    // at app start.
    m_agentSettings->setActiveConversationId(m_chatController->activeConversationId());

    m_sessionRouter->registerLocalAgentSettings(m_agentSettings.get());

    // Cache-mirror signal subscriptions on ChatController. The
    // canonical values live on AgentSettings; ChatController stores
    // synchronous mirrors so sendMessage doesn't have to chase a
    // pointer / cross-controller read on every turn.
    connect(m_agentSettings.get(),
            &AgentSettingsController::agentPatternChanged,
            m_chatController.get(),
            &ChatController::onExternalAgentPatternChanged);
    connect(m_agentSettings.get(),
            &AgentSettingsController::requireConfirmationChanged,
            m_chatController.get(),
            &ChatController::onExternalRequireConfirmationChanged);
    connect(m_agentSettings.get(),
            &AgentSettingsController::toolsEnabledChanged,
            m_chatController.get(),
            [this]() {
                m_chatController->onExternalToolsEnabledChanged(m_agentSettings->toolsEnabled());
            });

    m_fileService = std::make_unique<FileService>(this);
    m_terminalController = std::make_unique<ProcessSandbox>(this);

    m_terminalController->setAllowList(m_settingsService->shellAllowList());
    connect(m_settingsService.get(), &SettingsService::shellAllowListChanged, this, [this]() {
        if (m_terminalController)
            m_terminalController->setAllowList(m_settingsService->shellAllowList());
    });

    m_backgroundProcessService =
        std::make_unique<BackgroundProcessService>(*m_terminalController, this);
    connect(m_convController.get(),
            &ConversationController::conversationDeleted,
            m_backgroundProcessService.get(),
            &BackgroundProcessService::reapConversation);

    m_fileService->setMountRegistry(m_folderMountRegistry.get());

    m_webSearchService = std::make_unique<Search::WebSearchService>(this);
    m_toolService = std::make_unique<ToolService>(this);
    m_toolService->setWebSearchService(m_webSearchService.get());
    m_toolService->registerBuiltInTools(
        *m_terminalController, *m_fileService, m_backgroundProcessService.get());

    m_webSearchRegistry = std::make_unique<Search::WebSearchProviderRegistry>(
        *m_webSearchService, *m_settingsService, this);

    m_subagentService = std::make_unique<SubagentRunService>(
        m_db, *m_toolService, *m_router, *m_settingsService, this);

    // Decision #2 (AMENDED after live verification) — result delivery:
    // a finished run posts its report into the parent conversation as
    // a first-class system message (visible to every member + present
    // in subsequent LLM context), AND the requester agent is then
    // dispatched to react to it. The original V1 rule ("no
    // auto-redispatch; the parent reacts on its next turn") proved
    // wrong in live use: when the cascade has already ended — the
    // requester typically says "I'll wait for the report" and stops —
    // there IS no next turn, so the report landed into silence until
    // the user typed something. dispatchSubagentReaction skips safely
    // when a generation is in flight (the persisted report is then in
    // context for the running cascade anyway).
    connect(m_subagentService.get(),
            &SubagentRunService::runFinished,
            this,
            [this](const QString& runId,
                   const QString& convId,
                   const QString& alias,
                   const QString& status,
                   const QString& resultText) {
                Message msg;
                msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                msg.conversationId = convId;
                msg.role = QStringLiteral("system");
                msg.createdAt = QDateTime::currentDateTimeUtc();
                if (status == QStringLiteral("done")) {
                    msg.content = QStringLiteral("🤖 Sub-agent report for @%1 (run %2):\n\n%3")
                                      .arg(alias, runId.left(8), resultText);
                } else {
                    const auto r = m_subagentService->run(runId);
                    msg.content = QStringLiteral("🤖 Sub-agent run %1 for @%2 ended: %3%4")
                                      .arg(runId.left(8),
                                           alias,
                                           status,
                                           (r.has_value() && !r->failReason.isEmpty())
                                               ? QStringLiteral(" (%1)").arg(r->failReason)
                                               : QString());
                }
                if (m_msgService->addMessage(msg).isEmpty()) {
                    qCWarning(verzetaUi)
                        << "Sub-agent result message insert failed for" << runId.left(8);
                    return;  // conversation gone — nothing to react to
                }
                // React to success AND failure alike: a failed run
                // needs the requester to decide the fallback, not the
                // user.
                m_chatController->dispatchSubagentReaction(convId, alias);
            });

    // 9c. Agent Registry — persistent named-agent definitions
    m_agentRegistry = std::make_unique<AgentRegistry>(m_db, this);
    m_agentRegistry->initialize();

    // 9d. Membership Service — project_members + conversation_members CRUD
    m_membershipService = std::make_unique<MembershipService>(m_db, this);

    connect(m_membershipService.get(),
            &MembershipService::conversationMembersChanged,
            m_summarizer.get(),
            [this](const QString& convId) {
                m_summarizer->invalidate(convId, QStringLiteral("member_changed"));
            });
    m_membershipService->setAuditService(m_auditService.get());

    m_pollService = std::make_unique<PollService>(m_db, this);
    m_pollService->setAuditService(m_auditService.get());

    m_projectTemplateService = std::make_unique<ProjectTemplateService>(
        *m_convService,
        *m_membershipService,
        *m_agentRegistry,
        m_db,
        QString(),  // userTemplateDir — empty resolves to <AppData>/user-templates
        this);

    m_embeddingThread = std::make_unique<QThread>(this);
    if (!m_inferenceSidecarHost) {
        m_inferenceSidecarHost = std::make_unique<Verzeta::Infer::InferenceSidecarHost>();
    }

    m_embeddingWorker = std::make_unique<EmbeddingWorker>();
    m_embeddingWorker->setSidecarHost(m_inferenceSidecarHost.get());
    applyEmbeddingBackend(m_embeddingWorker.get());
    m_embeddingWorker->moveToThread(m_embeddingThread.get());

    // G4 — a second, query-priority embedding worker on its own thread. Index
    // embeds go to the bulk worker above; live retrieval embeds go here, so a
    // recall never queues behind a backlog of indexing jobs. Same config.
    m_queryEmbeddingThread = std::make_unique<QThread>(this);
    m_queryEmbeddingWorker = std::make_unique<EmbeddingWorker>();
    m_queryEmbeddingWorker->setSidecarHost(m_inferenceSidecarHost.get());
    applyEmbeddingBackend(m_queryEmbeddingWorker.get());
    m_queryEmbeddingWorker->moveToThread(m_queryEmbeddingThread.get());

    // NOTE: Do NOT connect finished→deleteLater here. The unique_ptr owns the
    // EmbeddingWorker lifetime. AppController::~AppController() stops the thread
    // before any unique_ptr destructors run, so the delete is safe from main thread.
    m_embeddingThread->start();
    m_queryEmbeddingThread->start();

    // Keep embedding + providers in sync with settings changes.
    //
    // Critical: when the user updates a provider URL or API key from the
    // Settings page, we must refresh that provider's model list so the
    // model selector bar populates immediately — without this, users see
    // an empty model list and think the app is broken. Previously a
    // restart was needed. Each provider's setBaseUrl / setApiKey are
    // called first so refreshModels() uses the new config.
    connect(m_settingsService.get(),
            &SettingsService::settingsChanged,
            this,
            [this](const QString& settingsKey) {
                // ---- Provider API key changed ----
                //
                // SettingsService::setApiKey emits
                // settingsChanged("apikey.<providerId>"). The previous
                // code here matched the literal "api_key_openai" /
                // "api_key_anthropic" / "api_key_gemini" — strings that
                // are NEVER emitted — so adding or changing ANY cloud
                // provider's key mid-session did nothing (no setApiKey
                // on the provider, no refreshModels) and a full restart
                // was required. It also never covered OpenRouter /
                // DeepSeek at all. This generic branch fixes both: it
                // keys off the real "apikey." prefix and works for every
                // provider, current and future.
                //
                // setApiKey is not on the ILLMProvider interface — it
                // lives on the concrete subclasses — so this casts the
                // same way the providerBaseUrlChanged handler above
                // does. Both the foreground and background slot
                // instances get the new key.
                if (settingsKey.startsWith(QStringLiteral("apikey."))) {
                    const QString providerId = settingsKey.mid(QStringLiteral("apikey.").size());
                    const QString key = m_settingsService->apiKey(providerId);

                    // Both embedding workers reuse the OpenAI key (remote
                    // backend). Re-apply so the active backend picks it up.
                    if (providerId == QStringLiteral("openai")) {
                        applyEmbeddingBackend(m_embeddingWorker.get());
                        applyEmbeddingBackend(m_queryEmbeddingWorker.get());
                    }

                    const auto applyKey = [&key](ILLMProvider* p) {
                        if (!p)
                            return;
                        // OpenAIProvider covers OpenAI itself plus its
                        // subclasses: OpenRouter, DeepSeek, llama.cpp
                        // Remote — all OpenAI-compatible.
                        if (auto* op = qobject_cast<OpenAIProvider*>(p)) {
                            op->setApiKey(key);
                        } else if (auto* ap = qobject_cast<AnthropicProvider*>(p)) {
                            ap->setApiKey(key);
                        } else if (auto* gp = qobject_cast<GeminiProvider*>(p)) {
                            gp->setApiKey(key);
                        }
                    };
                    applyKey(m_router->providerForId(providerId));
                    applyKey(m_router->bgProviderForId(providerId));

                    // Refresh the foreground model list now the key is
                    // set, so the model selector populates without a
                    // restart. Skip when the key was cleared — a keyless
                    // GET /models just 401s.
                    if (!key.isEmpty()) {
                        if (auto* fg = m_router->providerForId(providerId)) {
                            fg->refreshModels();
                        }
                    }
                }

                // Ollama base URL — reconfigure the provider and refresh
                if (settingsKey == QStringLiteral("ollama_base_url")) {
                    if (auto* p = m_router->providerForId(QStringLiteral("ollama"))) {
                        if (auto* ollama = dynamic_cast<OllamaProvider*>(p)) {
                            ollama->setBaseUrl(m_settingsService->ollamaBaseUrl());
                            p->refreshModels();
                        }
                    }
                }

                // llama.cpp model directory — rescan
                if (settingsKey == QStringLiteral("llamacpp_model_path")) {
                    if (auto* p = m_router->providerForId(QStringLiteral("llamacpp"))) {
                        p->refreshModels();
                    }
                }

                if (settingsKey == QStringLiteral("embedding_endpoint_base_url") ||
                    settingsKey == QStringLiteral("embedding_model") ||
                    settingsKey == QStringLiteral("embedding_local_enabled") ||
                    settingsKey == QStringLiteral("embedding_local_model_filename")) {
                    applyEmbeddingBackend(m_embeddingWorker.get());
                    applyEmbeddingBackend(m_queryEmbeddingWorker.get());
                }
            });

    m_ragService =
        std::make_unique<RagService>(m_db, *m_embeddingWorker, *m_queryEmbeddingWorker, this);

    m_agentMemoryService = std::make_unique<AgentMemoryService>(m_db, *m_embeddingWorker, this);
    m_acnService = std::make_unique<AcnService>(m_db, *m_embeddingWorker, this);

    m_ragSource = std::make_unique<Chat::RagSource>(*m_ragService, *m_convService);
    m_aimSource = std::make_unique<Chat::AimSource>(*m_agentMemoryService, *m_convService);
    m_acnSource = std::make_unique<Chat::AcnSource>(*m_acnService, *m_convService);
    m_memoryRetriever = std::make_unique<Chat::MemoryRetriever>(*m_queryEmbeddingWorker, this);
    m_memoryRetriever->addSource(m_ragSource.get());
    m_memoryRetriever->addSource(m_aimSource.get());
    m_memoryRetriever->addSource(m_acnSource.get());

    connect(m_summarizer.get(),
            &ConversationSummarizer::summaryReady,
            this,
            [this](const QString& convId, const QString& /*reason*/) {
                if (!m_acnService || !m_convService) {
                    return;
                }
                const QString ancestor = m_convService->projectOrgAncestorId(convId);
                if (ancestor.isEmpty() || !m_convService->folderAcnEnabled(ancestor)) {
                    return;  // not an ACN-enabled project/org
                }
                const auto conv = m_convService->getConversation(convId);
                if (!conv || !LlmConfig::fromJson(conv->llmConfig).acnEnabled) {
                    return;  // this conversation opted out
                }
                const auto summary = m_summarizer->summaryFor(convId);
                if (!summary.has_value() || summary->summaryText.trimmed().isEmpty()) {
                    return;
                }
                m_acnService->record(summary->summaryText,
                                     QStringLiteral("project:%1").arg(ancestor),
                                     QStringLiteral("summary"),
                                     convId,
                                     QStringLiteral("%1 messages").arg(summary->coveredCount));
            });

    connect(m_convService.get(),
            &ConversationService::folderDeleted,
            this,
            [this](const QString& folderId) {
                if (m_acnService) {
                    m_acnService->purgeScope(QStringLiteral("project:%1").arg(folderId));
                }
            });

    connect(
        m_agentRegistry.get(), &AgentRegistry::agentDeleted, this, [this](const QString& agentId) {
            if (m_agentMemoryService) {
                m_agentMemoryService->purgeScope(QStringLiteral("agent:%1").arg(agentId));
            }
        });
    connect(m_convService.get(),
            &ConversationService::conversationDeleted,
            this,
            [this](const QString& convId) {
                if (m_agentMemoryService) {
                    m_agentMemoryService->purgeScope(QStringLiteral("conversation:%1").arg(convId));
                }
                if (m_ragService) {
                    m_ragService->clearConversationEmbeddings(convId);
                }
            });

    connect(m_msgService.get(),
            &MessageService::messageAdded,
            this,
            [this](const QString& convId, const QString& msgId) {
                if (!m_ragService) {
                    return;
                }
                // RAG indexing is a per-conversation opt-in
                // (llm_config.rag_enabled): only index messages from
                // conversations that turned RAG on. The embedding provider
                // is configured globally; if it is unreachable the async
                // embed simply fails out, so this is the only index gate.
                const auto conv = m_convService->getConversation(convId);
                if (!conv || !LlmConfig::fromJson(conv->llmConfig).ragEnabled) {
                    return;
                }
                const Message m = m_msgService->getMessage(msgId);
                // Index both sides of the conversation. messageAdded fires once
                // per message: for user messages at send, and for streamed
                // assistant messages at finalize (finalizeStreamingMessage →
                // addMessage), so the assistant content here is complete — no
                // partial/duplicate indexing.
                if ((m.role == QStringLiteral("user") || m.role == QStringLiteral("assistant")) &&
                    !m.content.trimmed().isEmpty()) {
                    // Scope the chunk to its conversation so retrieval can
                    // restrict to {conversation} ∪ {project} ∪ {global} and
                    // never bleed across unrelated conversations.
                    m_ragService->indexMessage(
                        msgId, m.content, QStringLiteral("conversation:%1").arg(convId));
                }
            });

    m_chatController->setToolService(m_toolService.get());
    m_chatController->setSummarizer(m_summarizer.get());
    m_chatController->setRagService(m_ragService.get());
    m_chatController->setMemoryRetriever(m_memoryRetriever.get());
    m_chatController->setFileService(m_fileService.get());
    m_chatController->setAgentRegistry(m_agentRegistry.get());
    m_chatController->setMembershipService(m_membershipService.get());

    m_convController->setFileService(m_fileService.get());
    m_convController->setMembershipService(m_membershipService.get());

    connect(m_convController.get(),
            &ConversationController::conversationAboutToBeDeleted,
            m_chatController.get(),
            &ChatController::onExternalConversationAboutToBeDeleted);
    connect(m_convController.get(),
            &ConversationController::conversationDeleted,
            m_chatController.get(),
            &ChatController::onExternalConversationDeleted);


    if (m_settingsService) {
        m_chatController->configureRagpBackend(m_settingsService.get());

        // Reconfigure on runtime changes. The signal aggregates
        // ragpLocalEnabledChanged + ragpDefaultModelFilenameChanged
        // so one handler covers both toggles. We DEFER the actual
        // reconfigure via QTimer::singleShot(0, ...) to keep the
        // Settings write path (a QML property setter) snappy and
        // to avoid a UI freeze if the old backend's destructor
        // takes its worst-case 5s wait — the setter returns
        // immediately; the swap happens on the next tick.
        connect(
            m_settingsService.get(), &SettingsService::ragpBackendConfigChanged, this, [this]() {
                QTimer::singleShot(0, this, [this]() {
                    if (!m_chatController || !m_settingsService)
                        return;
                    m_chatController->configureRagpBackend(m_settingsService.get());
                });
            });

        // Mirror ChatController's backend-changed signal into our
        // Q_PROPERTY NOTIFY so QML bindings update automatically.
        connect(m_chatController.get(),
                &ChatController::ragpBackendChanged,
                this,
                [this](const QString& /*name*/) { emit ragpBackendLiveChanged(); });
    }

    m_agentService = std::make_unique<AgentService>(*m_router, *m_toolService, *m_ragService, this);
    m_chatController->setAgentService(m_agentService.get());

    m_imageService = std::make_unique<ImageService>(*m_router, *m_fileService, this);
    // File every generated/refined image into ITS conversation's project
    // workspace, resolved deterministically from the folder chain — not the
    // FileService's mutable active context, which is unset for a
    // UI-triggered refine and left images in the base projects dir.
    m_imageService->setWorkspaceDirResolver([this](const QString& convId) -> QString {
        if (convId.isEmpty())
            return m_fileService->activeProjectDir();
        const QList<Folder> chain = m_convService->folderChainForConversation(convId);
        for (const Folder& f : chain)
            if (f.isProject())
                return m_fileService->projectWorkspaceDir(f.id, f.name);
        return m_fileService->conversationWorkspaceDir(convId);
    });
    m_imageService->setMessageService(m_msgService.get());
    m_imageService->setSettingsService(m_settingsService.get());
    // Resolve the active image provider for the QML generate path and
    // the generate_image tool from the persisted registry catalogue.
    m_imageService->setImageProviderRegistry(m_imageProviderRegistry.get());
    m_imageService->setAuditService(m_auditService.get());

    m_pollService->setMessageService(m_msgService.get());
    m_audioService = std::make_unique<AudioService>(*m_fileService, this);

    m_searchService = std::make_unique<SearchService>(m_db, *m_ragService, this);
    // SearchService is consumed directly by the memory-tool classes
    // registered via Tools::registerAllBuiltInTools() further below —
    // no ChatController intermediary.
    // Initialise NotificationManager singleton (creates tray icon)
    NotificationManager::instance();

    m_planService = std::make_unique<PlanService>(m_db, this);
    m_taskRunner = std::make_unique<TaskRunner>(*m_planService, this);
    m_taskObserver = std::make_unique<TaskObserver>(*m_planService, this);

    m_taskGateService = std::make_unique<TaskGateService>(*m_msgService, this);
    m_taskGateService->setPlanService(m_planService.get());
    m_taskGateService->setTaskRunner(m_taskRunner.get());
    m_taskGateService->setTaskObserver(m_taskObserver.get());

    // Image completions file into the workspace and record a File
    // artifact on the active plan — both hooks live on ImageService
    // (created earlier) and need TaskObserver + TaskGateService,
    // which only exist from this point.
    m_imageService->setTaskObserver(m_taskObserver.get());
    // Async image completions resume the requesting agent so the
    // conversation continues instead of stalling on the finished image
    // (ConversationRun::resumeAfterImageGenerated owns the guards).
    connect(m_imageService.get(),
            &ImageService::imageReadyForFollowUp,
            m_chatController.get(),
            &ChatController::onImageReadyForFollowUp);
    m_imageService->setTaskGateService(m_taskGateService.get());

    m_taskController = std::make_unique<TaskController>(
        *m_convService, *m_msgService, *m_planService, *m_taskRunner, this);
    m_taskController->setAgentRegistry(m_agentRegistry.get());
    m_taskController->setMembershipService(m_membershipService.get());
    m_taskController->setFileService(m_fileService.get());

    m_chatController->setTaskGateService(m_taskGateService.get());

    m_slashCommandService = std::make_unique<SlashCommandService>(this);
    m_slashCommandService->setConversationService(m_convService.get());
    m_slashCommandService->setMessageService(m_msgService.get());
    m_slashCommandService->setFileService(m_fileService.get());
    m_slashCommandService->setToolService(m_toolService.get());
    m_slashCommandService->setMessageListModel(m_chatController->messages());
    m_slashCommandService->setSummarizer(m_summarizer.get());
    {
        ChatController* ccPtr = m_chatController.get();
        m_slashCommandService->setActiveConversationIdGetter(
            [ccPtr]() -> QString { return ccPtr->activeConversationId(); });
    }
    m_chatController->setSlashCommandService(m_slashCommandService.get());

    m_canvasService = std::make_unique<CanvasService>(m_db, this);
    m_canvasService->setFileService(m_fileService.get());
    m_canvasService->setConversationService(m_convService.get());
    {
        ChatController* ccPtr = m_chatController.get();
        m_canvasService->setActiveConversationIdGetter(
            [ccPtr]() -> QString { return ccPtr ? ccPtr->activeConversationId() : QString(); });
    }
    m_chatController->setCanvasService(m_canvasService.get());

    m_canvasConsoleModel = std::make_unique<CanvasConsoleModel>(this);
    m_canvasRunner = std::make_unique<CanvasRunner>(this);
    m_canvasRunner->setCanvasService(m_canvasService.get());
    m_canvasRunner->setFileService(m_fileService.get());
    m_canvasRunner->setConsoleModel(m_canvasConsoleModel.get());

    m_canvasAiActions = std::make_unique<CanvasAiActions>(this);
    m_canvasAiActions->setCanvasService(m_canvasService.get());
    m_canvasAiActions->setChatController(m_chatController.get());

    {
        auto refreshIfActive = [this](const QString& convId) {
            if (!m_chatController)
                return;
            if (convId != m_chatController->activeConversationId())
                return;
            const QVariantMap m = m_canvasService->activeCanvasFor(convId);
            if (m.isEmpty()) {
                m_chatController->onActiveCanvasMetadataRefresh(QString(), QString(), 0, 0, 0);
            } else {
                m_chatController->onActiveCanvasMetadataRefresh(
                    m.value(QStringLiteral("filename")).toString(),
                    m.value(QStringLiteral("language")).toString(),
                    m.value(QStringLiteral("revision")).toInt(),
                    m.value(QStringLiteral("lineCount")).toInt(),
                    static_cast<qint64>(m.value(QStringLiteral("byteSize")).toLongLong()));
            }
        };
        connect(
            m_canvasService.get(),
            &CanvasService::canvasOpened,
            this,
            [refreshIfActive](const QString& convId, const QString&) { refreshIfActive(convId); },
            Qt::QueuedConnection);
        connect(
            m_canvasService.get(),
            &CanvasService::canvasUpdated,
            this,
            [refreshIfActive](const QString& convId, const QString&, int) {
                refreshIfActive(convId);
            },
            Qt::QueuedConnection);
        connect(
            m_canvasService.get(),
            &CanvasService::canvasClosed,
            this,
            [refreshIfActive](const QString& convId, const QString&) { refreshIfActive(convId); },
            Qt::QueuedConnection);
        // Also refresh on conversation switch — different conversations
        // have different active canvases. Queued so the refresh runs
        // AFTER the conversation switch has fully propagated through
        // every other subscriber (model router, AgentSettings cache,
        // MessageListModel, etc.).
        ChatController* cc = m_chatController.get();
        connect(
            cc,
            &ChatController::activeConversationChanged,
            this,
            [this, cc, refreshIfActive]() { refreshIfActive(cc->activeConversationId()); },
            Qt::QueuedConnection);
    }

    m_taskController->registerTaskToolStubs(*m_toolService);

    m_toolService->loadCustomTools(*m_terminalController);

    m_mcpService = std::make_unique<McpService>(m_toolService.get(), this);
    m_mcpService->loadAndConnect();

    connect(m_taskController.get(),
            &TaskController::taskStarted,
            m_chatController.get(),
            &ChatController::onExternalTaskStarted);
    connect(m_taskController.get(),
            &TaskController::planStopped,
            m_chatController.get(),
            &ChatController::onExternalPlanStopped);
    connect(m_taskController.get(),
            &TaskController::allPlansStoppedInConversation,
            m_chatController.get(),
            &ChatController::onExternalAllPlansStoppedInConversation);

    // Push-based plans model. Auto-subscribes to PlanService signals —
    // no refresh() calls, no polling. The PlansOverlay QML binds to this
    // via a context property and sees row-level updates as PlanService
    // mutates plan/step/artifact rows.
    m_plansModel = std::make_unique<PlansModel>(*m_planService, this);
    m_toolCallLogModel = std::make_unique<ToolCallLogModel>(*m_msgService, this);
    m_artifactsModel = std::make_unique<ArtifactsModel>(*m_msgService, this);

    // Flat sidebar model: derives its row stream from the tree model
    // and membership service. Entirely push-based — no refresh() method.
    m_sidebarModel =
        std::make_unique<SidebarFlatModel>(*m_convListModel, *m_membershipService, this);

    // Keep all conversation-scoped models focused on whichever conversation
    // the user is currently looking at. The signal fires whenever
    // ChatController switches the active conversation, so no refresh is
    // ever needed — each model reloads its own rows on setActiveConversation.
    connect(m_chatController.get(), &ChatController::activeConversationChanged, this, [this]() {
        const QString id = m_chatController->activeConversationId();
        m_plansModel->setActiveConversation(id);
        m_toolCallLogModel->setActiveConversation(id);
        // Feed the shared workspace dir first so a project chat (group
        // OR 1:1) lists the same files, then reload once.
        m_artifactsModel->setWorkspaceDir(m_chatController->activeArtifactsPath());
        m_artifactsModel->setActiveConversation(id);
        m_sidebarModel->setActiveConversationId(id);
        if (m_heartbeatReportsModel) {
            m_heartbeatReportsModel->setActiveConversation(id);
        }
    });
    // Prime the initial selection in case a conversation is already
    // loaded at app start.
    {
        const QString id = m_chatController->activeConversationId();
        m_plansModel->setActiveConversation(id);
        m_toolCallLogModel->setActiveConversation(id);
        m_artifactsModel->setWorkspaceDir(m_chatController->activeArtifactsPath());
        m_artifactsModel->setActiveConversation(id);
        m_sidebarModel->setActiveConversationId(id);
        if (m_heartbeatReportsModel) {
            m_heartbeatReportsModel->setActiveConversation(id);
        }
    }

    m_chatController->setPlanService(m_planService.get());
    m_chatController->setTaskRunner(m_taskRunner.get());
    m_chatController->setTaskObserver(m_taskObserver.get());
    // CRITICAL: tool handlers run on a QtConcurrent background thread (see
    // ChatController::onRequestFinished tool dispatch). When start_task fires
    // there, TaskRunner synchronously invokes this callback from the same
    // background thread. enqueueTaskDispatch then touches m_msgModel, emits
    // Qt signals, and ultimately calls QNetworkAccessManager — all of which
    // are NOT thread-safe and segfault when called off the main thread.
    // Marshal the call onto m_chatController's thread via QueuedConnection.
    ChatController* cc = m_chatController.get();
    m_taskRunner->setEnqueueCallback([cc](const TaskDispatchRequest& req) {
        QMetaObject::invokeMethod(
            cc, [cc, req]() { cc->enqueueTaskDispatch(req); }, Qt::QueuedConnection);
    });

    connect(m_taskRunner.get(),
            &TaskRunner::taskEventRequested,
            m_taskController.get(),
            &TaskController::postTaskEventMessage);

    connect(m_taskController.get(),
            &TaskController::taskArtifactReady,
            m_chatController.get(),
            &ChatController::onTaskArtifactReady,
            Qt::QueuedConnection);
    {
        Q_ASSERT(m_taskGateService);
        ChatController* ccPtr = m_chatController.get();
        m_taskController->installTaskToolHandlers(
            *m_toolService,
            *m_taskGateService,
            [ccPtr]() -> Chat::CascadeController* { return ccPtr->cascadeInternal(); },
            [ccPtr]() -> QString { return ccPtr->activeConversationId(); });
    }

    m_skillService = std::make_unique<SkillService>(*m_convService, this);
    m_skillService->initialize();
    m_skillsModel = std::make_unique<SkillsModel>(*m_skillService, this);

    // Wire ChatController to use SkillService for preferred-list
    // resolution at request-build time. ChatController is the only
    // foreground caller; HeartbeatSubagentService takes its own
    // separate ctor reference (S2 wiring further below in this file).
    m_chatController->setSkillService(m_skillService.get());
    m_chatController->setHeartbeatConfigService(m_heartbeatConfigService.get());
    m_chatController->setPollService(m_pollService.get());
    m_chatController->setAuditService(m_auditService.get());

    m_sessionRouter->setChatControllerSetupFn([this](ChatController* cc) {
        // Same setter recipe as local ChatController above. If you
        // add a new setter to the local instance, add it here too.
        cc->setToolService(m_toolService.get());
        cc->setRagService(m_ragService.get());
        cc->setFileService(m_fileService.get());
        cc->setAgentRegistry(m_agentRegistry.get());
        cc->setMembershipService(m_membershipService.get());
        cc->configureRagpBackend(m_settingsService.get());
        cc->setAgentService(m_agentService.get());
        cc->setTaskGateService(m_taskGateService.get());
        cc->setSlashCommandService(m_slashCommandService.get());
        cc->setCanvasService(m_canvasService.get());
        cc->setPlanService(m_planService.get());
        cc->setTaskRunner(m_taskRunner.get());
        cc->setTaskObserver(m_taskObserver.get());
        cc->setSkillService(m_skillService.get());
        cc->setHeartbeatConfigService(m_heartbeatConfigService.get());
        cc->setPollService(m_pollService.get());
        cc->setSummarizer(m_summarizer.get());
        cc->setAuditService(m_auditService.get());
        // every wire-side ChatController shares the
        // ONE app-level ProviderScheduler so wire-session turns
        // serialize per-provider against local + other wire turns
        // (and run in parallel across providers).
        cc->setProviderScheduler(m_providerScheduler.get());
        cc->setInferenceSidecarHost(m_inferenceSidecarHost.get());
    });

    // Per-client AgentSettings setup callback. Mirrors the local
    // AgentSettings configuration so wire clients see the same
    // provider-filtering behaviour: availableProviders / models-
    // ForProvider hide entries the user hasn't configured.
    m_sessionRouter->setAgentSettingsSetupFn(
        [this](AgentSettingsController* as) { as->setSettingsService(m_settingsService.get()); });

    m_clawHubClient = std::make_unique<ClawHubClient>(this);
    connect(m_clawHubClient.get(),
            &ClawHubClient::downloadCompleted,
            this,
            [this](const QString& slug, const QString& /*version*/, const QString& localZip) {
                const QString sourceUrl = QStringLiteral("https://clawhub.ai/skills/") + slug;
                // Pass the slug as expectedId so a flat-ZIP layout
                // (no top-level wrapper dir) doesn't leak the staging
                // directory's `.staging-<uuid>` name through to the
                // parser's id-fallback chain.
                const QString err = m_skillService->installFromZip(localZip, sourceUrl, slug);
                // Always remove the staging zip; install copied bytes
                // into skills/installed/<id>/ (or quarantined).
                QFile::remove(localZip);
                if (!err.isEmpty()) {
                    emit m_clawHubClient->downloadFailed(slug, err);
                }
            });

    // collaborators. Memory tools need SearchService + MessageService
    // + ConversationService; task handlers need PlanService + TaskRunner
    // (wired just above). Routed through the unified helper so future
    // extractions can replace each family's delegation with direct
    // ITool registration without touching this call site.
    {
        Tools::RegistrationContext toolCtx;
        toolCtx.sandbox = m_terminalController.get();
        toolCtx.fileService = m_fileService.get();
        toolCtx.searchService = m_searchService.get();
        toolCtx.msgService = m_msgService.get();
        toolCtx.convService = m_convService.get();
        toolCtx.chatController = m_chatController.get();
        toolCtx.canvasService = m_canvasService.get();
        toolCtx.skillService = m_skillService.get();
        toolCtx.agentRegistry = m_agentRegistry.get();
        toolCtx.membershipService = m_membershipService.get();
        toolCtx.imageService = m_imageService.get();
        toolCtx.settingsService = m_settingsService.get();
        toolCtx.imageProviderRegistry = m_imageProviderRegistry.get();
        toolCtx.pollService = m_pollService.get();
        toolCtx.subagentService = m_subagentService.get();
        toolCtx.agentMemoryService = m_agentMemoryService.get();
        Tools::registerAllBuiltInTools(*m_toolService, toolCtx);
    }

    m_taskRunner->setIsConversationGeneratingFn([this](const QString& convId) -> bool {
        return m_chatController->isConversationGenerating(convId);
    });

    // 2. Conversation deletion → drop plans belonging to that conv,
    //    clean up in-memory state (progress / heartbeat maps).
    connect(m_convService.get(),
            &ConversationService::conversationDeleted,
            m_taskRunner.get(),
            &TaskRunner::onConversationDeleted);

    // 2b. Conversation deletion → abort any streaming slots still
    //     buffering content for that conversation. Without this, a
    //     conversation deleted mid-stream would leave an orphan slot
    //     that never finalises, and if finalize is somehow triggered
    //     it would drop the row because addMessage now guards on
    //     conversation existence. Aborting cleanly emits the right
    //     signal so MessageListModel drops the placeholder row.
    connect(m_convService.get(),
            &ConversationService::conversationDeleted,
            m_msgService.get(),
            &MessageService::onConversationDeleted);

    // 2c. Conversation deletion → PlansModel cascade cleanup.
    //     Note: the DB's ON DELETE CASCADE FK on agent_plans.conversation_id
    //     already removes plan rows atomically with the conversation
    //     delete. Because SQLite cascades don't fire PlanService signals,
    //     PlansModel would otherwise hold stale rows pointing to a dead
    //     conversation id. This direct hook removes them from the
    //     in-memory model list when conversationDeleted fires.
    connect(m_convService.get(),
            &ConversationService::conversationDeleted,
            m_plansModel.get(),
            &PlansModel::onSourceConversationDeleted);

    // 3. Open plans SURVIVE app restarts. The task system is a
    //    group-owned TRACKER: a plan is an anchor + status with no
    //    in-process dispatch state, so a restart loses nothing and
    //    there is nothing to "fail". TaskGateService re-anchors the
    //    conversation's open plan on activation (auto-resume). The
    //    old executor-era restart fail-safe marked every open plan
    //    failed here, which made complete_task error with "no active
    //    task" for the rest of the conversation's life and silently
    //    dropped plan-artifact recording after every app launch.

    // 4. Start the 60-second heartbeat timer.
    m_taskRunner->startHeartbeat();

    m_heartbeatConfigService = std::make_unique<HeartbeatConfigService>(m_db, this);

    // App-layer cleanup hooks (polymorphic FK substitute — see
    // heartbeat-config-service.h).
    connect(m_convService.get(),
            &ConversationService::conversationDeleted,
            m_heartbeatConfigService.get(),
            &HeartbeatConfigService::onConversationDeleted);
    connect(m_convService.get(),
            &ConversationService::folderDeleted,
            m_heartbeatConfigService.get(),
            &HeartbeatConfigService::onFolderDeleted);

    // Dedicated RequestBuilder for the heartbeat service. ChatController
    // owns its own RequestBuilder for foreground turns; we keep a
    // separate instance here so heartbeat prompt-building never collides
    // with the chat turn machinery. For H1 the service inlines its own
    // build path; this instance is reserved for the H3 surface-review
    // call site.
    m_heartbeatRequestBuilder =
        std::make_unique<Chat::RequestBuilder>(*m_convService, *m_msgService, *m_router, this);

    m_heartbeatSubagentService = std::make_unique<HeartbeatSubagentService>(
        *m_heartbeatConfigService,
        *m_agentRegistry,
        *m_convService,
        *m_msgService,
        *m_router,
        *m_toolService,
        *m_skillService,
        *m_heartbeatRequestBuilder,
        // resolver (not a startup-pinned pointer) so the
        // Tier-3 cascade host follows the in-flight run. The stream-
        // finalize drain now hangs off the ChatController aggregated
        // signal (passed below), not a pinned StreamingManager.
        [ccPtr = m_chatController.get()]() -> Chat::CascadeController* {
            return ccPtr->cascadeInternal();
        },
        m_chatController.get(),
        this);

    m_heartbeatSubagentService->initialize();

    {
        Tools::RegistrationContext hbToolCtx;
        hbToolCtx.convService = m_convService.get();
        hbToolCtx.chatController = m_chatController.get();
        hbToolCtx.heartbeatConfigService = m_heartbeatConfigService.get();
        hbToolCtx.heartbeatSubagentService = m_heartbeatSubagentService.get();
        hbToolCtx.agentRegistry = m_agentRegistry.get();
        hbToolCtx.cascadeController = m_chatController->cascadeInternal();
        Tools::registerHeartbeatSelfConfigTools(*m_toolService, hbToolCtx);
    }

    m_heartbeatReportsModel = std::make_unique<HeartbeatReportsModel>(
        *m_heartbeatConfigService, *m_heartbeatSubagentService, m_db, *m_agentRegistry, this);
    connect(m_convService.get(),
            &ConversationService::conversationDeleted,
            m_heartbeatReportsModel.get(),
            &HeartbeatReportsModel::onSourceConversationDeleted);
    connect(m_convService.get(),
            &ConversationService::folderDeleted,
            m_heartbeatReportsModel.get(),
            &HeartbeatReportsModel::onSourceFolderDeleted);
    // Prime the model — the activeConversationChanged fan-out earlier in
    // initialize() ran before this model was constructed, so we replay
    // the current active-conv id here.
    {
        const QString primeId = m_chatController->activeConversationId();
        if (!primeId.isEmpty()) {
            m_heartbeatReportsModel->setActiveConversation(primeId);
        }
    }

    // -------------------------------------------------------------------
    // Verzeta Remote — host-side bridge (separate-process architecture).
    //
    // This is the ONLY new piece of code linked into verzeta-studio for
    // the remote subsystem. The bridge runs on its own QThread, listens
    // on a QLocalServer for the verzeta-remote binary to connect, and
    // forwards existing host signals via Qt::QueuedConnection / receives
    // commands and dispatches them via QMetaObject::invokeMethod on
    // EXISTING host Q_INVOKABLE methods. ZERO new methods or signals on
    // host classes. ZERO BlockingQueuedConnection from main → worker.
    //
    // The remote WebSocket server itself is a SEPARATE BINARY
    // (verzeta-remote) — if it crashes, this host is unaffected.
    // -------------------------------------------------------------------
    {
        Verzeta::Remote::WireHostBridge::Services svc;
        svc.msg = QPointer<MessageService>(m_msgService.get());
        svc.convSvc = QPointer<ConversationService>(m_convService.get());
        svc.convCtrl = QPointer<ConversationController>(m_convController.get());
        svc.chatCtrl = QPointer<ChatController>(m_chatController.get());
        svc.agentSettings = QPointer<AgentSettingsController>(m_agentSettings.get());
        svc.agentRegistry = QPointer<AgentRegistry>(m_agentRegistry.get());
        svc.agentService = QPointer<AgentService>(m_agentService.get());
        svc.toolService = QPointer<ToolService>(m_toolService.get());
        svc.mcpService = QPointer<McpService>(m_mcpService.get());
        svc.skillService = QPointer<SkillService>(m_skillService.get());
        svc.membershipService = QPointer<MembershipService>(m_membershipService.get());
        svc.pollService = QPointer<PollService>(m_pollService.get());
        svc.auditService = QPointer<AuditService>(m_auditService.get());
        svc.projectTemplateService =
            QPointer<ProjectTemplateService>(m_projectTemplateService.get());
        // Workspace mount registry — 5 wire ops dispatched here for
        // mount registration / unregistration / tree refresh / tier
        // change / for-folder lookup.
        svc.folderMountRegistry = QPointer<FolderMountRegistry>(m_folderMountRegistry.get());
        svc.heartbeatConfig = QPointer<HeartbeatConfigService>(m_heartbeatConfigService.get());
        svc.heartbeatSubagent =
            QPointer<HeartbeatSubagentService>(m_heartbeatSubagentService.get());
        svc.taskController = QPointer<TaskController>(m_taskController.get());
        svc.ragService = QPointer<RagService>(m_ragService.get());
        svc.planService = QPointer<PlanService>(m_planService.get());
        svc.fileService = QPointer<FileService>(m_fileService.get());
        svc.imageService = QPointer<ImageService>(m_imageService.get());
        svc.audioService = QPointer<AudioService>(m_audioService.get());
        svc.canvasService = QPointer<CanvasService>(m_canvasService.get());
        svc.canvasRunner = QPointer<CanvasRunner>(m_canvasRunner.get());
        svc.canvasAiActions = QPointer<CanvasAiActions>(m_canvasAiActions.get());
        svc.canvasConsole = QPointer<CanvasConsoleModel>(m_canvasConsoleModel.get());
        svc.settings = QPointer<SettingsService>(m_settingsService.get());
        svc.webSearchRegistry =
            QPointer<Search::WebSearchProviderRegistry>(m_webSearchRegistry.get());
        svc.sessionRouter = m_sessionRouter.get();
        m_wireHostBridge = std::make_unique<Verzeta::Remote::WireHostBridge>(std::move(svc), this);
    }

    m_folderMountClient = std::make_unique<FolderMountClientWire>(
        QPointer<Verzeta::Remote::WireHostBridge>(m_wireHostBridge.get()),
        m_folderMountRegistry.get());
    m_fileService->setMountClient(m_folderMountClient.get());
    m_fileService->setMountCommandClient(m_folderMountClient.get());

    // -------------------------------------------------------------------
    // Headless autostart hook. When the binary was invoked with
    // --headless-remote, fire the wire daemon now (after every engine
    // service is constructed and wired). Uses the resolved CLI / QSettings
    // values handed in via setCommandLineOptions() prior to initialize().
    //
    // Same call shape as the QML Remote Access dialog uses — no new
    // bridge surface, no per-mode service composition.
    // -------------------------------------------------------------------
    if (m_cliOptions.headlessRemote && m_wireHostBridge) {
        const bool started = m_wireHostBridge->startRemoteServer(m_cliOptions.remoteBind,
                                                                 m_cliOptions.remotePort,
                                                                 m_cliOptions.remoteTls,
                                                                 m_cliOptions.remoteCert,
                                                                 m_cliOptions.remoteKey);
        if (started) {
            qCInfo(verzetaUi).noquote()
                << QStringLiteral("Wire daemon autostarted: bind=%1 port=%2 tls=%3")
                       .arg(m_cliOptions.remoteBind)
                       .arg(m_cliOptions.remotePort)
                       .arg(m_cliOptions.remoteTls ? QStringLiteral("on") : QStringLiteral("off"));
        } else {
            qCCritical(verzetaUi) << "Wire daemon autostart FAILED — verzeta-remote could not "
                                     "be launched. Check binary path, TLS cert/key paths, "
                                     "and port availability.";
            // Non-fatal: the engine stays up so the operator can SSH in,
            // diagnose, restart. Exiting here would mask the actual cause
            // behind a generic init-failed message.
        }
    }

    qCInfo(verzetaUi) << "AppController initialized successfully";
    return true;
}


QString AppController::ragpBackendLive() const {
    if (!m_chatController)
        return QString();
    return m_chatController->ragpBackendName();
}

QString AppController::ragpAccelerationLive() const {
    return m_ragpAccelerationLive;
}

/*
 * @brief Registers C++ services as QML singletons and context properties.
 *        Must be called after initialize() and before engine.load().
 * @param engine The QML application engine.
 * @sideeffects Registers ChatController, MarkdownConverter, and SettingsService
 *              as QML singletons in the "org.verzeta.studio" URI.
 */
void AppController::registerTypes(QQmlApplicationEngine& engine) {
    // Path/URL conversion. Stateless; parented to the engine so it outlives
    // every QML binding that reaches it and dies with the engine. QML must
    // never hand-build "file://" + path — see path-utils.h for why that
    // silently produces a UNC path on Windows.
    qmlRegisterSingletonInstance<Verzeta::PathUtils>(
        "org.verzeta.studio", 1, 0, "PathUtils", new Verzeta::PathUtils(&engine));

    // Voice-daemon detection state for Settings (and, in later stages, the
    // call UI). Registered directly, no forwarder.
    qmlRegisterSingletonInstance<Verzeta::Voice::VoiceCallService>(
        "org.verzeta.studio", 1, 0, "VoiceCallService", m_voiceCallService.get());

    // Voice model installer (catalog + progress) for the Voice Calls
    // dialog. Registered directly, no forwarder.
    qmlRegisterSingletonInstance<Verzeta::Voice::VoiceModelDownloader>(
        "org.verzeta.studio", 1, 0, "VoiceModelDownloader", m_voiceModelDownloader.get());

    qmlRegisterSingletonInstance<ChatController>(
        "org.verzeta.studio", 1, 0, "ChatController", m_chatController.get());

    qmlRegisterSingletonInstance<ConversationController>(
        "org.verzeta.studio", 1, 0, "Conversations", m_convController.get());

    qmlRegisterSingletonInstance<TaskController>(
        "org.verzeta.studio", 1, 0, "Tasks", m_taskController.get());

    qmlRegisterSingletonInstance<AgentSettingsController>(
        "org.verzeta.studio", 1, 0, "AgentSettings", m_agentSettings.get());

    qmlRegisterSingletonInstance<ExportController>(
        "org.verzeta.studio", 1, 0, "Export", m_exportController.get());

    qmlRegisterSingletonInstance<CanvasService>(
        "org.verzeta.studio", 1, 0, "Canvas", m_canvasService.get());

    // Recommended-model downloader — First Run Wizard + the RAGP /
    // Embeddings provider sub-pages bind to this for the explicit
    // "Download recommended model" action.
    qmlRegisterSingletonInstance<ModelDownloadService>(
        "org.verzeta.studio", 1, 0, "ModelDownloads", m_modelDownloadService.get());

    qmlRegisterSingletonInstance<CanvasRunner>(
        "org.verzeta.studio", 1, 0, "CanvasRunner", m_canvasRunner.get());
    qmlRegisterSingletonInstance<CanvasAiActions>(
        "org.verzeta.studio", 1, 0, "CanvasAiActions", m_canvasAiActions.get());

    qmlRegisterSingletonInstance<MarkdownConverter>(
        "org.verzeta.studio", 1, 0, "MarkdownConverter", &MarkdownConverter::instance());

    qmlRegisterSingletonInstance<SettingsService>(
        "org.verzeta.studio", 1, 0, "SettingsService", m_settingsService.get());

    // Custom OpenAI-API-compatible server registry — QML setup-sheet
    // surface for adding / editing / removing user-configured custom
    // servers and reading the current catalogue.  Q_INVOKABLE
    // surface: list / byslug / add / update / remove / refreshModels.
    // Signal: serversChanged (re-fires QML bindings on every
    // mutation).
    qmlRegisterSingletonInstance<CustomServerRegistry>(
        "org.verzeta.studio", 1, 0, "CustomServers", m_customServerRegistry.get());

    // Image-generation provider registry — QML setup surface for adding /
    // editing / removing user-configured image providers, reading the
    // current catalogue, and selecting the active provider.  Q_INVOKABLE
    // surface: list / byId / upsert / remove / slugForBaseUrl.  Signals:
    // providersChanged + activeProviderChanged.
    qmlRegisterSingletonInstance<ImageProviderRegistry>(
        "org.verzeta.studio", 1, 0, "ImageProviders", m_imageProviderRegistry.get());

    qmlRegisterSingletonInstance<Search::WebSearchProviderRegistry>(
        "org.verzeta.studio", 1, 0, "WebSearchProviders", m_webSearchRegistry.get());

    qmlRegisterSingletonInstance<AuditService>(
        "org.verzeta.studio", 1, 0, "Activity", m_auditService.get());

    qmlRegisterSingletonInstance<HelpService>(
        "org.verzeta.studio", 1, 0, "Help", m_helpService.get());

    // Project Rooms — `ProjectTemplates` singleton. The eventual
    // Project Rooms overlay reads `ProjectTemplates.templates()` /
    // `ProjectTemplates.templateById(id)` and invokes
    // `ProjectTemplates.createProjectFromTemplate(id, customisations)`.
    qmlRegisterSingletonInstance<ProjectTemplateService>(
        "org.verzeta.studio", 1, 0, "ProjectTemplates", m_projectTemplateService.get());

    qmlRegisterSingletonInstance<HeartbeatSubagentService>(
        "org.verzeta.studio", 1, 0, "HeartbeatSubagent", m_heartbeatSubagentService.get());

    qmlRegisterSingletonInstance<HeartbeatConfigService>(
        "org.verzeta.studio", 1, 0, "HeartbeatConfig", m_heartbeatConfigService.get());

    qmlRegisterSingletonInstance<SkillService>(
        "org.verzeta.studio", 1, 0, "Skills", m_skillService.get());

    qmlRegisterSingletonInstance<ClawHubClient>(
        "org.verzeta.studio", 1, 0, "ClawHub", m_clawHubClient.get());

    if (!m_buildInfo) {
        m_buildInfo = std::make_unique<BuildInfo>(this);
    }

    QQmlContext* ctx = engine.rootContext();
    ctx->setContextProperty(QStringLiteral("BuildInfo"), m_buildInfo.get());
    ctx->setContextProperty(QStringLiteral("CanvasConsoleModel"), m_canvasConsoleModel.get());
    ctx->setContextProperty(QStringLiteral("ConversationService"), m_convService.get());
    ctx->setContextProperty(QStringLiteral("MessageService"), m_msgService.get());
    ctx->setContextProperty(QStringLiteral("ConversationListModel"), m_convListModel.get());
    ctx->setContextProperty(QStringLiteral("SidebarModel"), m_sidebarModel.get());
    ctx->setContextProperty(QStringLiteral("PlansModel"), m_plansModel.get());
    ctx->setContextProperty(QStringLiteral("ToolCallLogModel"), m_toolCallLogModel.get());
    ctx->setContextProperty(QStringLiteral("ArtifactsModel"), m_artifactsModel.get());
    ctx->setContextProperty(QStringLiteral("HeartbeatReportsModel"), m_heartbeatReportsModel.get());
    ctx->setContextProperty(QStringLiteral("SkillsModel"), m_skillsModel.get());
    ctx->setContextProperty(QStringLiteral("FileService"), m_fileService.get());
    ctx->setContextProperty(QStringLiteral("TerminalController"), m_terminalController.get());

    ctx->setContextProperty(QStringLiteral("AppController"), this);

    // SyntaxHighlighterWrapper registration removed — CodeBlock.qml
    // and CanvasEditor.qml now `import org.kde.syntaxhighlighting`
    // (the upstream QML module that ships with KF6 SyntaxHighlighting).
    // The upstream `SyntaxHighlighter` QML type is registered by the
    // module itself; no app-side qmlRegisterType is needed.

    qmlRegisterType<CanvasLineNumberGutter>("org.verzeta.studio", 1, 0, "CanvasLineNumberGutter");

    ctx->setContextProperty(QStringLiteral("ToolService"), m_toolService.get());
    ctx->setContextProperty(QStringLiteral("McpService"), m_mcpService.get());
    ctx->setContextProperty(QStringLiteral("RagService"), m_ragService.get());
    ctx->setContextProperty(QStringLiteral("AgentRegistry"), m_agentRegistry.get());
    ctx->setContextProperty(QStringLiteral("MembershipService"), m_membershipService.get());
    ctx->setContextProperty(QStringLiteral("PollService"), m_pollService.get());

    // SlashCommandService exposed to QML as `SlashCommands` — the chat
    // composer's autocomplete picker reads availableCommands() from it
    // as the single source of truth for the `/`-command list.
    ctx->setContextProperty(QStringLiteral("SlashCommands"), m_slashCommandService.get());

    ctx->setContextProperty(QStringLiteral("AgentService"), m_agentService.get());

    ctx->setContextProperty(QStringLiteral("ImageService"), m_imageService.get());
    ctx->setContextProperty(QStringLiteral("AudioService"), m_audioService.get());

    ctx->setContextProperty(QStringLiteral("SearchService"), m_searchService.get());

    // Verzeta Remote — admin surface for the QML "Remote Access" dialog.
    // Start / stop the verzeta-remote daemon, generate pairing codes,
    // list paired Android clients, revoke them. The bridge object is
    // already constructed; only the QML registration is added here.
    ctx->setContextProperty(QStringLiteral("RemoteAccess"), m_wireHostBridge.get());

    qCInfo(verzetaUi) << "QML types registered";
}

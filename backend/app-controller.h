// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file app-controller.h
 * @brief Application-level bootstrap controller. Initializes all services,
 *        opens the database, sets up logging, and registers C++ types with
 *        the QML engine. Acts as the composition root for dependency
 *        injection.
 * @layer API (application bootstrap)
 * @dependencies All services, DbManager, Qt6::Quick, KF6::Kirigami2
 */


#pragma once

#include <QThread>

#include <memory>
#include <QObject>
#include <QQmlApplicationEngine>

// Forward-decl Verzeta::Remote::WireHostBridge so this header doesn't
// pull in QtNetwork via wire-host-bridge.h. The unique_ptr holds an
// incomplete type fine for member declaration; the full include lands
// in app-controller.cpp.
namespace Verzeta::Remote {
class WireHostBridge;
}
namespace Verzeta::Infer {
class InferenceSidecarHost;
}
namespace Verzeta::Voice {
class VoiceCallService;
class VoiceModelDownloader;
}  // namespace Verzeta::Voice

#include "models/artifacts-model.h"
#include "models/canvas-console-model.h"
#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/plans-model.h"
#include "models/sidebar-flat-model.h"
#include "models/tool-call-log-model.h"
#include "services/agent-memory-service.h"
#include "services/agent-registry.h"
#include "services/agent-service.h"
#include "services/agent-settings-controller.h"
#include "services/audio-service.h"
#include "services/canvas-ai-actions.h"
#include "services/canvas-runner.h"
#include "services/canvas-service.h"
#include "services/chat-controller.h"
#include "services/conversation-controller.h"
#include "services/conversation-service.h"
#include "services/export-controller.h"
#include "services/export-service.h"
#include "services/file-service.h"
#include "services/help-service.h"
#include "services/image-service.h"
#include "services/mcp-service.h"
#include "services/membership-service.h"
#include "services/message-service.h"
#include "services/model-download-service.h"
#include "services/model-router.h"
#include "services/plan-service.h"
#include "services/poll-service.h"
#include "services/project-template-service.h"
#include "services/rag-service.h"
#include "services/search-service.h"
#include "services/session/session-router.h"
#include "services/settings-service.h"
#include "services/slash-command-service.h"
#include "services/task-controller.h"
#include "services/task-gate-service.h"
#include "services/task-observer.h"
#include "services/task-runner.h"
#include "services/tool-service.h"
#include "utils/build-info.h"
#include "utils/canvas-line-number-gutter.h"
#include "utils/command-line-options.h"
#include "utils/http-client.h"
#include "utils/notification-manager.h"
#include "utils/process-sandbox.h"
// utils/syntax-highlighter.h removed — replaced by upstream
// org.kde.syntaxhighlighting QML module.
#include "workers/embedding-worker.h"

namespace Chat {
class RequestBuilder;
}
namespace Chat {
class ProviderScheduler;
}
namespace Chat {
class MemoryRetriever;
}
namespace Chat {
class RagSource;
}
namespace Chat {
class AimSource;
}
namespace Chat {
class AcnSource;
}
class AcnService;

// Forward declaration — full header included in app-controller.cpp where the
// unique_ptr destructor instantiates.
class BackgroundProcessService;

// Forward declaration — CustomServerRegistry full header is included
// in app-controller.cpp where the unique_ptr destructor instantiates.
class CustomServerRegistry;

// Forward declaration — ImageProviderRegistry full header is included
// in app-controller.cpp where the unique_ptr destructor instantiates.
class ImageProviderRegistry;

namespace Search {
class WebSearchService;
class WebSearchProviderRegistry;
}  // namespace Search

/**
 * @brief Composition root that owns all services and wires up the application.
 *
 * AppController is created in main() before the QML engine. It:
 *  1. Calls Logger::initialize() with the log directory.
 *  2. Opens the SQLite database via DbManager::open().
 *  3. Runs schema migrations via DbManager::runMigrations().
 *  4. Creates all service instances in dependency order.
 *  5. Registers providers with ModelRouter.
 *  6. Registers C++ singleton types with the QML engine.
 *
 * Ownership: AppController owns all services via std::unique_ptr. Services are
 * destroyed in LIFO order when AppController is destroyed, before the QApplication
 * event loop exits.
 */
class AppController : public QObject {
    Q_OBJECT

    /**
     * @brief LIVE RAGP backend name, e.g. `local:llama.cpp:phi-4.gguf`
     *        or `remote:ollama`. Mirrors ChatController::ragpBackendName()
     *        via the ragpBackendChanged signal; QML binds to this for
     *        the Settings UI "Current backend" chip.
     *
     * This is distinct from the CONFIGURED intent (stored in
     * SettingsService::ragpLocalEnabled + ragpDefaultModelFilename).
     * After an auto-fallback (local load failed at runtime), the
     * live value shows "remote:ollama" even though the configured
     * intent is still "local", giving the user a truthful view of
     * what's actually running.
     */
    Q_PROPERTY(QString ragpBackendLive READ ragpBackendLive NOTIFY ragpBackendLiveChanged)

    /**
     * @brief LIVE acceleration of the sidecar's instruct slot
     *        ("gpu (<device>)" or "cpu"), reported by the sidecar itself
     *        from the ggml device table on every successful model load.
     *        Never a compile-time guess. Empty until the first local
     *        load this session (the sidecar is lazy, so before any
     *        local classify/chat there is nothing truthful to report).
     */
    Q_PROPERTY(
        QString ragpAccelerationLive READ ragpAccelerationLive NOTIFY ragpAccelerationLiveChanged)

  public:
    /**
     * @brief Constructs AppController. Does not perform initialization.
     * @param parent Optional Qt parent.
     */
    explicit AppController(QObject* parent = nullptr);

    /**
     * @brief Destroys the AppController and all owned services.
     * @sideeffects Closes the database connection via DbManager::close().
     */
    ~AppController() override;

    /**
     * @brief Hands the parsed command-line options to the controller so
     *        `initialize()` can apply the autostart-remote hook at the
     *        right point in the setup graph.
     * @param opts Resolved options struct from
     *        `Verzeta::Utils::parseCommandLine`.  Stored by value.
     *
     * Must be called BEFORE `initialize()`.  Default-constructed options
     * (no setter call) produce normal desktop behaviour.
     */
    void setCommandLineOptions(const Verzeta::Utils::CommandLineOptions& opts);

    /**
     * @brief Initializes the database, logging, and all services.
     * @return true if all initialization steps succeeded.
     *         Returns false if the database cannot be opened or migrations fail.
     * @sideeffects Opens database file at AppDataLocation/verzeta-studio.db.
     *              Creates application data directories if absent.
     *              Installs Qt message handler via Logger::initialize().
     *              Registers all LLM providers with ModelRouter.
     *              Sets the default active provider (Ollama) if configured.
     *              When `setCommandLineOptions` was called with
     *              `headlessRemote = true`, fires
     *              `WireHostBridge::startRemoteServer(...)` at the end of
     *              the setup graph using the resolved bind / port / TLS
     *              fields on the options struct.
     */
    bool initialize();

    /**
     * @brief Registers all C++ types and context properties with the QML engine.
     *        Must be called after initialize() and before engine.load().
     * @param engine The QML application engine to register types with.
     * @sideeffects Registers the following QML singletons in "org.verzeta.studio":
     *              - ChatController    (C++ instance)
     *              - MarkdownConverter (C++ instance)
     *              - SettingsService   (C++ instance)
     */
    void registerTypes(QQmlApplicationEngine& engine);

    /**
     * @brief Reader for the ragpBackendLive Q_PROPERTY. Forwards to
     *        ChatController::ragpBackendName(); main-thread only.
     * @returns The live backend name string, or empty if the chat
     *          controller is not constructed yet (only possible
     *          before initialize() runs).
     */
    QString ragpBackendLive() const;

    /**
     * @brief Reader for the ragpAccelerationLive Q_PROPERTY.
     * @returns Last acceleration reported by the sidecar's instruct
     *          slot, or empty before the first local load.
     */
    QString ragpAccelerationLive() const;

  signals:
    /**
     * @brief Notifier for the ragpBackendLive Q_PROPERTY. Fires when
     *        ChatController emits ragpBackendChanged, i.e. after
     *        every configureRagpBackend call and after every
     *        auto-fallback swap.
     */
    void ragpBackendLiveChanged();

    /**
     * @brief Notifier for the ragpAccelerationLive Q_PROPERTY. Fires
     *        once per change, driven by the sidecar host's
     *        slotAccelerationChanged reports.
     */
    void ragpAccelerationLiveChanged();

  private:
    // Core infrastructure
    DbManager& m_db;

    /** Parsed command-line options; populated by setCommandLineOptions
     *  before initialize() runs.  Default-constructed value matches
     *  normal desktop behaviour (no headless, no autostart). */
    Verzeta::Utils::CommandLineOptions m_cliOptions;

    /** Last sidecar-reported acceleration for the instruct slot (main
     *  thread; backs the ragpAccelerationLive Q_PROPERTY). */
    QString m_ragpAccelerationLive;

    /** User-initiated recommended-model downloader (the ModelDownloads
     *  QML singleton). Declared after SettingsService (whose model
     *  dirs it resolves through the injected provider). */
    std::unique_ptr<ModelDownloadService> m_modelDownloadService;

    std::unique_ptr<Verzeta::Infer::InferenceSidecarHost> m_inferenceSidecarHost;

    std::unique_ptr<ConversationService> m_convService;
    std::unique_ptr<MessageService> m_msgService;

    // Voice calls: detection of the external verzeta-voice daemon. No
    // threads and no child process at this stage; daemon lifecycle and
    // call state are later stages and never touch the chat pipeline.
    std::unique_ptr<Verzeta::Voice::VoiceCallService> m_voiceCallService;
    /** Checksum-verified voice/STT model installer for the add-on. */
    std::unique_ptr<Verzeta::Voice::VoiceModelDownloader> m_voiceModelDownloader;

    std::unique_ptr<class AuditService> m_auditService;

    // Workspace mount registry — metadata-only substrate for client-
    // owned virtual workspace mounts. Persists rows in the
    // `folder_mounts` table (schema v17). The host bridge dispatches
    // the 5 `workspace.mount.*` wire ops here. Constructed right
    // after AuditService so lifecycle audit hooks attach immediately.
    std::unique_ptr<class FolderMountRegistry> m_folderMountRegistry;

    std::unique_ptr<class HelpService> m_helpService;

    std::unique_ptr<class ProjectTemplateService> m_projectTemplateService;

    std::unique_ptr<HttpClient> m_httpOllama;
    std::unique_ptr<HttpClient> m_httpOpenai;
    std::unique_ptr<HttpClient> m_httpAnthropic;
    std::unique_ptr<HttpClient> m_httpGemini;

    std::unique_ptr<HttpClient> m_httpOllamaBg;
    std::unique_ptr<HttpClient> m_httpOpenaiBg;
    std::unique_ptr<HttpClient> m_httpAnthropicBg;
    std::unique_ptr<HttpClient> m_httpGeminiBg;

    std::unique_ptr<HttpClient> m_httpOpenRouter;
    std::unique_ptr<HttpClient> m_httpOpenRouterBg;
    std::unique_ptr<HttpClient> m_httpDeepSeek;
    std::unique_ptr<HttpClient> m_httpDeepSeekBg;
    std::unique_ptr<HttpClient> m_httpLlamaCppRemote;
    std::unique_ptr<HttpClient> m_httpLlamaCppRemoteBg;

    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<SettingsService> m_settingsService;

    std::unique_ptr<Chat::ProviderScheduler> m_providerScheduler;

    std::unique_ptr<class ConversationSummarizer> m_summarizer;

    /** Sub-agent run service. Declared after every
     *  dependency (db, tool service ref is held internally, router,
     *  settings) so reverse-declaration destruction tears it down
     *  first (rule 12); its dtor aborts in-flight provider requests. */
    std::unique_ptr<class SubagentRunService> m_subagentService;

    // CustomServerRegistry — catalogue + lifecycle controller for
    // user-configured custom OpenAI-API-compatible servers (vLLM,
    // LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI,
    // SGLang, Oobabooga, ...).  Declared AFTER m_router and
    // m_settingsService so reverse-declaration destruction order
    // unregisters every registry-owned provider from ModelRouter
    // BEFORE the per-instance HttpClients (owned in the registry)
    // are torn down — without this ordering, a dying
    // OpenAICompatProvider would dereference its dangling
    // HttpClient&.
    std::unique_ptr<CustomServerRegistry> m_customServerRegistry;

    // ImageProviderRegistry — persisted, fully-customisable catalogue of
    // image-generation providers (OpenAI Images / DALL·E,
    // OpenAI-compatible image servers, Automatic1111 / SD WebUI, local SD
    // CLIs, chat-style image models) plus the active-provider selection.
    // Depends only on SettingsService; owns no provider instances.
    std::unique_ptr<ImageProviderRegistry> m_imageProviderRegistry;

    std::unique_ptr<ExportService> m_exportService;
    std::unique_ptr<ConversationListModel> m_convListModel;

    std::unique_ptr<TaskGateService> m_taskGateService;

    std::unique_ptr<SlashCommandService> m_slashCommandService;

    std::unique_ptr<CanvasService> m_canvasService;

    std::unique_ptr<CanvasRunner> m_canvasRunner;
    std::unique_ptr<CanvasConsoleModel> m_canvasConsoleModel;
    std::unique_ptr<CanvasAiActions> m_canvasAiActions;

    std::unique_ptr<ChatController> m_chatController;

    std::unique_ptr<Verzeta::Session::SessionRouter> m_sessionRouter;

    std::unique_ptr<ConversationController> m_convController;

    std::unique_ptr<TaskController> m_taskController;

    std::unique_ptr<AgentSettingsController> m_agentSettings;

    std::unique_ptr<ExportController> m_exportController;

    std::unique_ptr<FileService> m_fileService;
    std::unique_ptr<ProcessSandbox> m_terminalController;
    std::unique_ptr<BackgroundProcessService> m_backgroundProcessService;

    std::unique_ptr<Search::WebSearchService> m_webSearchService;
    std::unique_ptr<Search::WebSearchProviderRegistry> m_webSearchRegistry;
    std::unique_ptr<ToolService> m_toolService;
    std::unique_ptr<McpService> m_mcpService;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    std::unique_ptr<MembershipService> m_membershipService;
    std::unique_ptr<PollService> m_pollService;
    std::unique_ptr<EmbeddingWorker>
        m_embeddingWorker;  ///< Bulk-index worker; lives on m_embeddingThread
    // G4 — a dedicated query-priority embedding worker on its own thread so a
    // live recall never queues behind a backlog of background indexing jobs.
    std::unique_ptr<EmbeddingWorker> m_queryEmbeddingWorker;  ///< Lives on m_queryEmbeddingThread
    std::unique_ptr<RagService> m_ragService;
    std::unique_ptr<AgentMemoryService> m_agentMemoryService;  ///< Per-agent memory service
    std::unique_ptr<AcnService> m_acnService;                  ///< Team memory service (ACN)
    // Unified pre-turn recall coordinator + its sources. Declared
    // AFTER the services they reference so reverse-order destruction tears the
    // coordinator + sources down before the memory services.
    std::unique_ptr<Chat::RagSource> m_ragSource;
    std::unique_ptr<Chat::AimSource> m_aimSource;
    std::unique_ptr<Chat::AcnSource> m_acnSource;
    std::unique_ptr<Chat::MemoryRetriever> m_memoryRetriever;
    std::unique_ptr<QThread> m_embeddingThread;       ///< Bulk-index thread
    std::unique_ptr<QThread> m_queryEmbeddingThread;  ///< Query-priority thread

    /**
     * @brief Applies the configured embedding backend (local llama.cpp vs
     *        remote endpoint) to an embedding worker, mirroring the RAGP
     *        local/remote decision: local is used only when selected, the build
     *        has llama.cpp (hasLocalLlama), and the chosen GGUF exists;
     *        otherwise the remote OpenAI-compatible endpoint. Called for both
     *        workers at startup and whenever an embedding setting changes.
     * @param worker The embedding worker to (re)configure (no-op if null).
     */
    void applyEmbeddingBackend(EmbeddingWorker* worker);

    std::unique_ptr<AgentService> m_agentService;

    std::unique_ptr<ImageService> m_imageService;
    std::unique_ptr<AudioService> m_audioService;

    std::unique_ptr<SearchService> m_searchService;

    std::unique_ptr<PlanService> m_planService;
    std::unique_ptr<TaskRunner> m_taskRunner;
    std::unique_ptr<TaskObserver> m_taskObserver;

    // Push-based plans model fed by PlanService signals. Registered as
    // a QML context property so PlansOverlay can bind to it directly
    // without the old polling Timer + Q_INVOKABLE refresh pattern.
    std::unique_ptr<PlansModel> m_plansModel;

    // Push-based tool activity log fed by MessageService signals.
    // Rows are role="tool" messages in the active conversation.
    std::unique_ptr<ToolCallLogModel> m_toolCallLogModel;

    // Push-based file artifacts list fed by MessageService signals.
    // Rows are file-producing tool results and submit_result rows.
    std::unique_ptr<ArtifactsModel> m_artifactsModel;

    // Flat sidebar model derived from ConversationListModel + MembershipService.
    // Owns folder expansion state and row ordering. QML binds directly.
    std::unique_ptr<SidebarFlatModel> m_sidebarModel;

    std::unique_ptr<BuildInfo> m_buildInfo;

    std::unique_ptr<class HeartbeatConfigService> m_heartbeatConfigService;
    std::unique_ptr<Chat::RequestBuilder> m_heartbeatRequestBuilder;
    std::unique_ptr<class HeartbeatSubagentService> m_heartbeatSubagentService;

    std::unique_ptr<class HeartbeatReportsModel> m_heartbeatReportsModel;

    std::unique_ptr<class SkillService> m_skillService;
    std::unique_ptr<class SkillsModel> m_skillsModel;

    std::unique_ptr<class ClawHubClient> m_clawHubClient;

    // Verzeta Remote — host-side bridge. The ONLY new piece of code
    // linked into verzeta-studio for the remote subsystem. Lives on
    // its own QThread, listens on a QLocalServer for the verzeta-remote
    // binary to connect, forwards existing host signals via Qt::QueuedConnection,
    // dispatches inbound commands via QMetaObject::invokeMethod with
    // Qt::QueuedConnection on EXISTING host Q_INVOKABLE methods. ZERO
    // new methods or signals are added to host classes. ZERO
    // BlockingQueuedConnection from main → worker. Forward-declared so
    // this header doesn't pull in QtNetwork.
    std::unique_ptr<Verzeta::Remote::WireHostBridge> m_wireHostBridge;

    std::unique_ptr<class FolderMountClientWire> m_folderMountClient;
};

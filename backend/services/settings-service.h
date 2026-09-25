// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file settings-service.h
 * @brief Manages application-wide and per-provider settings.
 *        Persists settings to the SQLite settings table via DbManager.
 *        Provides Q_PROPERTY bindings for direct QML access.
 * @layer Service
 * @dependencies CryptoUtils (Utility), DbManager (Data Access), Qt6::Core, Qt6::Network
 */


#pragma once

#include "../models/db-manager.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVariantMap>

/**
 * @brief Application-wide settings service.
 *
 * All settings are persisted to the `settings` table in the SQLite database.
 * API keys are stored via CryptoUtils (OS keychain or obfuscated fallback).
 *
 * QML usage:
 *   SettingsService.defaultProvider          // read/write
 *   SettingsService.ollamaBaseUrl            // read/write
 *   SettingsService.setApiKey("openai", key) // method call
 *   SettingsService.apiKey("openai")         // method call
 *
 * Change notification:
 *   All Q_PROPERTY changes emit their corresponding NOTIFY signal AND
 *   the generic settingsChanged(key) signal for bulk listeners.
 */
class SettingsService : public QObject {
    Q_OBJECT

    // Provider settings
    Q_PROPERTY(QString defaultProvider READ defaultProvider WRITE setDefaultProvider NOTIFY
                   defaultProviderChanged)

    Q_PROPERTY(
        QString ollamaBaseUrl READ ollamaBaseUrl WRITE setOllamaBaseUrl NOTIFY ollamaBaseUrlChanged)

    Q_PROPERTY(QString llamaCppModelPath READ llamaCppModelPath WRITE setLlamaCppModelPath NOTIFY
                   llamaCppModelPathChanged)

    Q_PROPERTY(QString llamaCppModelDir READ llamaCppModelDir WRITE setLlamaCppModelDir NOTIFY
                   llamaCppModelDirChanged)

    // Appearance settings
    Q_PROPERTY(QString appTheme READ appTheme WRITE setAppTheme NOTIFY appThemeChanged)

    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged)

    Q_PROPERTY(QString codeFontFamily READ codeFontFamily WRITE setCodeFontFamily NOTIFY
                   codeFontFamilyChanged)

    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)

    /** Per-turn tool-iteration cap (runaway-loop backstop). Default 25;
     *  0 = UNLIMITED (no cap). Adjustable from the settings UI. */
    Q_PROPERTY(int toolIterationCap READ toolIterationCap WRITE setToolIterationCap NOTIFY
                   toolIterationCapChanged)

    /** User-owned shell command allow-list: the programs run_shell may invoke.
     *  Fully editable: add any program or remove a compiled default; unset
     *  falls back to ProcessSandbox::defaultAllowList(). Edited from the
     *  Execution & Permissions surface; the user is responsible for what they
     *  allow. The destructive-pattern scanner still gates every command. */
    Q_PROPERTY(QStringList shellAllowList READ shellAllowList WRITE setShellAllowList NOTIFY
                   shellAllowListChanged)

    Q_PROPERTY(QString databasePath READ databasePath CONSTANT)

    Q_PROPERTY(bool firstRunComplete READ firstRunComplete WRITE setFirstRunComplete NOTIFY
                   firstRunCompleteChanged)

    Q_PROPERTY(bool hasLocalLlama READ hasLocalLlama CONSTANT)

    /** Explicit path override for the external verzeta-voice daemon
     *  (voice calls). Empty means "search the standard locations". */
    Q_PROPERTY(QString voiceBinaryPath READ voiceBinaryPath WRITE setVoiceBinaryPath NOTIFY
                   voiceBinaryPathChanged)

    /** Start the voice service automatically with the application. */
    Q_PROPERTY(bool voiceAutostart READ voiceAutostart WRITE setVoiceAutostart NOTIFY
                   voiceAutostartChanged)


    Q_PROPERTY(bool ragpLocalEnabled READ ragpLocalEnabled WRITE setRagpLocalEnabled NOTIFY
                   ragpLocalEnabledChanged)

    Q_PROPERTY(QString ragpDefaultModelFilename READ ragpDefaultModelFilename WRITE
                   setRagpDefaultModelFilename NOTIFY ragpDefaultModelFilenameChanged)

    Q_PROPERTY(QString ragpModelsDir READ ragpModelsDir CONSTANT)

    Q_PROPERTY(QString ragpAccelerationBackend READ ragpAccelerationBackend CONSTANT)

    Q_PROPERTY(bool embeddingLocalEnabled READ embeddingLocalEnabled WRITE setEmbeddingLocalEnabled
                   NOTIFY embeddingLocalEnabledChanged)
    Q_PROPERTY(QString embeddingLocalModelFilename READ embeddingLocalModelFilename WRITE
                   setEmbeddingLocalModelFilename NOTIFY embeddingLocalModelFilenameChanged)
    Q_PROPERTY(QString embeddingModelsDir READ embeddingModelsDir CONSTANT)

  public:
    /**
     * @brief Constructs the SettingsService.
     * @param db Reference to the open DbManager instance.
     * @param parent Optional Qt parent.
     */
    explicit SettingsService(DbManager& db, QObject* parent = nullptr);


    /**
     * @brief Returns the absolute path of the SQLite database file.
     * @return Absolute path string, for display in the settings UI.
     */
    QString databasePath() const;

    /**
     * @brief Reads the first-run wizard completion flag.
     * @returns True after the user has finished (or skipped) the wizard.
     *
     * Persisted as the `first_run_complete` row in the settings
     * key-value table.
     */
    bool firstRunComplete() const;

    /**
     * @brief Persists the first-run wizard completion flag.
     * @param complete  True once the wizard has been finished or skipped.
     */
    void setFirstRunComplete(bool complete);

    /**
     * @brief Reports whether this build was linked against the vendored
     *        llama.cpp.
     * @returns True when the verzeta-inference sidecar binary is present
     *          beside the application (local inference availability is a
     *          runtime fact now; the app itself links no llama code).
     *
     * The first-run wizard's RAGP step uses this to grey out the
     * "Local llama.cpp" radio on builds compiled without llama.cpp.
     */
    bool hasLocalLlama() const;

    /**
     * @brief Asynchronously tests connectivity to an Ollama server.
     *
     * Issues a GET to {url}/api/tags with a 5 second timeout. On success
     * (HTTP 200), emits ollamaConnectionResult(true, "Connected"). On
     * failure (network error or non-200 response), emits
     * ollamaConnectionResult(false, errorMessage). An empty URL fails
     * immediately with "URL is empty". TLS certificate errors are
     * ignored, so local servers with self-signed certificates can be
     * reached. Each call issues its own request, so overlapping calls are
     * handled independently.
     *
     * @param url Base URL of the Ollama server (e.g. "http://localhost:11434").
     * @sideeffects Issues an async HTTP GET request.
     */
    Q_INVOKABLE void testOllamaConnection(const QString& url);

    // -----------------------------------------------------------------------
    // API key management (delegates to CryptoUtils)
    // -----------------------------------------------------------------------

    /**
     * @brief Retrieves a stored API key for the given provider.
     * @param provider Provider identifier (e.g., "openai", "anthropic", "gemini").
     * @return API key string, or empty string if not set.
     * @sideeffects Reads from OS keychain or obfuscated QSettings.
     */
    Q_INVOKABLE QString apiKey(const QString& provider) const;

    /**
     * @brief Stores an API key for the given provider.
     * @param provider Provider identifier.
     * @param key API key value. Empty string clears the stored key.
     * @sideeffects Writes to OS keychain or obfuscated QSettings.
     */
    Q_INVOKABLE void setApiKey(const QString& provider, const QString& key);

    /**
     * @brief Returns whether an API key has been configured for the provider.
     * @param provider Provider identifier.
     * @return true if a non-empty key is stored.
     */
    Q_INVOKABLE bool hasApiKey(const QString& provider) const;

    /**
     * @brief Generic per-provider base URL accessor.
     * @param provider  Provider id (e.g. "openrouter", "deepseek",
     *                  "llamacpp_remote").
     * @returns Stored override URL, or empty when the provider should
     *          fall back to its built-in default.
     *
     * Stored under settings key `provider_base_url_\<id\>`.
     */
    Q_INVOKABLE QString providerBaseUrl(const QString& provider) const;

    /**
     * @brief Stores or clears the per-provider base URL override.
     * @param provider  Provider id.
     * @param url       Override URL, or empty to clear (provider falls
     *                  back to its built-in default).
     */
    Q_INVOKABLE void setProviderBaseUrl(const QString& provider, const QString& url);

    /**
     * @brief Reports whether LlamaCppRemoteProvider should advertise
     *        tool-calling support.
     * @returns Stored toggle value. Default false, because many user-shipped
     *          llama.cpp server builds do not enable the `tools` slot.
     */
    Q_INVOKABLE bool llamaCppRemoteSupportsTools() const;

    /**
     * @brief Returns the serialised custom-OpenAI-compat-server
     *        catalogue maintained by `CustomServerRegistry`.
     * @returns JSON array string; empty when no custom servers are
     *          configured.
     *
     * The CustomServerRegistry hydrates from this on construction
     * and persists to it on every add / update / remove.  Stored
     * under settings key `custom_servers`.  API keys are NEVER
     * embedded in this blob; they live in the keychain under
     * `apikey_custom.\<slug\>` via the existing `apiKey` / `setApiKey`
     * surface.
     */
    QString customServersBlob() const;

    /**
     * @brief Persists the custom-server catalogue blob.
     * @param json JSON array serialised by `CustomServerRegistry`.
     *             Pass empty string to clear.
     */
    void setCustomServersBlob(const QString& json);

    /**
     * @brief Returns the serialised image-generation-provider catalogue
     *        maintained by `ImageProviderRegistry`.
     * @returns JSON array string; empty when no image providers are
     *          configured (the registry seeds itself from the legacy
     *          image-gen settings on first run while this is empty).
     *
     * The ImageProviderRegistry hydrates from this on construction and
     * persists to it on every upsert / remove.  Stored under settings
     * key `image_providers`.  API keys are NEVER embedded in this blob;
     * they live in the keychain under `apikey_image.\<id\>` via the
     * existing `apiKey` / `setApiKey` surface.
     */
    QString imageProvidersBlob() const;

    /**
     * @brief Persists the image-generation-provider catalogue blob.
     * @param json JSON array serialised by `ImageProviderRegistry`.
     *             Pass empty string to clear.
     */
    void setImageProvidersBlob(const QString& json);

    /**
     * @brief Persists the LlamaCppRemoteProvider tool-calling toggle.
     * @param enabled  True to advertise tool calling.
     */
    Q_INVOKABLE void setLlamaCppRemoteSupportsTools(bool enabled);

    // -----------------------------------------------------------------------
    // Provider settings
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the default provider id used for new conversations.
     * @returns Provider id string (e.g. "ollama").
     */
    QString defaultProvider() const;

    /**
     * @brief Persists the default provider id for new conversations.
     * @param id  Provider id.
     */
    void setDefaultProvider(const QString& id);

    /**
     * @brief Returns the Ollama server base URL.
     * @returns Persisted URL, or the empty default.
     */
    QString ollamaBaseUrl() const;

    /**
     * @brief Persists the Ollama server base URL.
     * @param url  Server URL (e.g. "http://localhost:11434").
     */
    void setOllamaBaseUrl(const QString& url);

    /**
     * @brief Returns the absolute path of the active llama.cpp GGUF model.
     * @returns Stored path, or empty when none is selected.
     */
    QString llamaCppModelPath() const;

    /**
     * @brief Persists the active llama.cpp GGUF model path.
     * @param path  Absolute path to the .gguf file.
     */
    void setLlamaCppModelPath(const QString& path);

    /**
     * @brief Returns the directory the llama.cpp model picker scans.
     * @returns Absolute directory path.
     */
    QString llamaCppModelDir() const;

    /**
     * @brief Persists the llama.cpp model directory.
     * @param dir  Absolute directory path.
     */
    void setLlamaCppModelDir(const QString& dir);


    /**
     * @brief Provider override for dynamic-compaction
     *        summarization. Empty (default) = use the active
     *        foreground provider.
     * @returns Provider id override, or empty for the default.
     */
    Q_INVOKABLE QString summarizationProvider() const;

    /**
     * @brief Persists the summarization provider override.
     * @param id Provider id; empty restores the foreground default.
     */
    Q_INVOKABLE void setSummarizationProvider(const QString& id);

    /**
     * @brief model override for summarization. Empty
     *        (default) = use the active foreground model.
     * @returns Model name override, or empty for the default.
     */
    Q_INVOKABLE QString summarizationModel() const;

    /**
     * @brief Persists the summarization model override.
     * @param model Model name; empty restores the foreground default.
     */
    Q_INVOKABLE void setSummarizationModel(const QString& model);

    /**
     * @brief Base URL of the OpenAI-compatible embeddings endpoint used by
     *        the RAG/embedding pipeline. Empty (default) = OpenAI
     *        (`https://api.openai.com/v1`). Set e.g. `http://localhost:11434/v1`
     *        for a local Ollama / LM Studio / llama.cpp server.
     * @returns The configured base URL, or empty for the OpenAI default.
     */
    Q_INVOKABLE QString embeddingEndpointBaseUrl() const;

    /**
     * @brief Persists the embeddings endpoint base URL.
     * @param baseUrl Endpoint base URL; empty restores the OpenAI default.
     */
    Q_INVOKABLE void setEmbeddingEndpointBaseUrl(const QString& baseUrl);

    /**
     * @brief Embedding model name sent to the embeddings endpoint. Empty
     *        (default) = `text-embedding-3-small`.
     * @returns The configured model name, or empty for the default.
     */
    Q_INVOKABLE QString embeddingModel() const;

    /**
     * @brief Persists the embedding model name.
     * @param model Model name; empty restores the default.
     */
    Q_INVOKABLE void setEmbeddingModel(const QString& model);

    /**
     * @brief Whether the local (in-process llama.cpp) embedding backend is
     *        selected. Gated by hasLocalLlama(); has no effect on builds
     *        compiled without llama.cpp (they always use the remote backend).
     * @returns true when local embeddings are selected.
     */
    bool embeddingLocalEnabled() const;

    /**
     * @brief Selects the local vs remote embedding backend.
     * @param enabled true = local llama.cpp, false = remote endpoint.
     */
    void setEmbeddingLocalEnabled(bool enabled);

    /**
     * @brief Basename of the selected local GGUF embedding model under
     *        embeddingModelsDir().
     * @returns The model basename, or empty when none is chosen.
     */
    QString embeddingLocalModelFilename() const;

    /**
     * @brief Persists the selected local embedding model basename.
     * @param filename Basename (no path separators) under embeddingModelsDir().
     */
    void setEmbeddingLocalModelFilename(const QString& filename);

    /**
     * @brief Directory holding local GGUF embedding models
     *        (AppLocalDataLocation + "/embedding-models").
     * @returns Absolute directory path (auto-created on first scan).
     */
    QString embeddingModelsDir() const;

    /**
     * @brief Lists the *.gguf files available in embeddingModelsDir().
     * @returns Basenames of available local embedding models.
     *
     * Emits embeddingAvailableModelsChanged() when the returned set differs
     * from the previous scan. Creates the directory on first access.
     */
    Q_INVOKABLE QStringList embeddingAvailableModels();

    /**
     * @brief Adds a GGUF embedding model by symlinking it into
     *        embeddingModelsDir(), using the same browse→symlink flow the
     *        RAGP model picker uses (shared implementation), so the
     *        user never has to hand-copy files into the folder.
     *        Idempotent when the file is already listed; refuses to
     *        overwrite a different file with the same basename.
     *
     *        Q_INVOKABLE so QML's FileDialog.onAccepted handler can
     *        call it directly with the picked absolute path. The
     *        picked basename is NOT auto-selected; the caller sets
     *        embeddingLocalModelFilename explicitly.
     * @param sourceAbsolutePath Absolute path to an existing .gguf
     *                           file anywhere on the filesystem.
     * @return Empty string on success; human-readable error message
     *         on any validation or filesystem failure. Never throws.
     */
    Q_INVOKABLE QString embeddingAddModelFromPath(const QString& sourceAbsolutePath);

    /**
     * @brief Removes a listed embedding model by deleting its symlink
     *        in embeddingModelsDir(). Only removes SYMLINKS: a real
     *        file the user dropped in via the shell is never deleted
     *        by this UI action. Clears embeddingLocalModelFilename
     *        when it named the removed entry.
     * @param filename Basename as it appears in
     *                 embeddingAvailableModels() (not a path).
     * @return Empty string on success; human-readable error message
     *         otherwise. Never throws.
     */
    Q_INVOKABLE QString embeddingRemoveModel(const QString& filename);

    /**
     * @brief Returns the active image-generation backend id.
     * @returns One of "openai_dalle", "local_cli", "remote_openai_compat",
     *          "remote_a1111", or empty when image generation is disabled.
     */
    Q_INVOKABLE QString imageGenActiveBackend() const;

    /**
     * @brief Persists the active image-generation backend id.
     * @param backend  Backend id (see imageGenActiveBackend()).
     */
    Q_INVOKABLE void setImageGenActiveBackend(const QString& backend);


    /**
     * @brief Returns the active web-search backend id.
     * @returns Provider id (e.g. "ddg-html", "tavily"); defaults to
     *          "ddg-html" when unset.
     */
    Q_INVOKABLE QString webSearchActiveProvider() const;

    /**
     * @brief Persists the active web-search backend id.
     * @param providerId Provider id to make active.
     */
    Q_INVOKABLE void setWebSearchActiveProvider(const QString& providerId);

    /**
     * @brief Returns the configured base-URL override for a provider.
     * @param providerId Provider id (e.g. "searxng").
     * @returns The override URL, or empty when the provider's built-in
     *          default endpoint should be used.
     */
    Q_INVOKABLE QString webSearchBaseUrl(const QString& providerId) const;

    /**
     * @brief Persists a base-URL override for a provider (e.g. self-hosted
     *        SearXNG instance).
     * @param providerId Provider id.
     * @param url Base URL, or empty to clear the override.
     */
    Q_INVOKABLE void setWebSearchBaseUrl(const QString& providerId, const QString& url);

    /**
     * @brief Returns the Custom (JSON) web-search provider config as a JSON
     *        string (url/method/auth/query + response field mapping).
     * @returns JSON object string, or empty when unconfigured.
     */
    Q_INVOKABLE QString webSearchCustomConfig() const;

    /**
     * @brief Persists the Custom (JSON) provider config.
     * @param json JSON object string (see webSearchCustomConfig()).
     */
    Q_INVOKABLE void setWebSearchCustomConfig(const QString& json);

    /**
     * @brief Returns the local SD CLI executable path.
     * @returns Absolute path, or empty when none is configured.
     *
     * Only consulted when active backend is "local_cli".
     */
    Q_INVOKABLE QString imageGenLocalSdPath() const;

    /**
     * @brief Persists the local SD CLI executable path.
     * @param path  Absolute path to the SD CLI executable.
     */
    Q_INVOKABLE void setImageGenLocalSdPath(const QString& path);

    /**
     * @brief Returns the OpenAI-compat image server base URL.
     * @returns Stored URL (the worker posts to `\<url\>/images/generations`),
     *          or empty when none is configured.
     */
    Q_INVOKABLE QString imageGenOpenAICompatUrl() const;

    /**
     * @brief Persists the OpenAI-compat image server base URL.
     * @param url  Server base URL.
     */
    Q_INVOKABLE void setImageGenOpenAICompatUrl(const QString& url);

    /**
     * @brief Returns the OpenAI-compat image server bearer token.
     * @returns Stored token via CryptoUtils, or empty when the
     *          server is unauthenticated.
     */
    Q_INVOKABLE QString imageGenOpenAICompatKey() const;

    /**
     * @brief Persists the OpenAI-compat image server bearer token.
     * @param key  Bearer token, or empty to clear.
     */
    Q_INVOKABLE void setImageGenOpenAICompatKey(const QString& key);

    /**
     * @brief Returns the Automatic1111 / SD WebUI server base URL.
     * @returns Stored URL (the worker posts to
     *          `\<url\>/sdapi/v1/txt2img`), or empty when none is
     *          configured.
     */
    Q_INVOKABLE QString imageGenA1111Url() const;

    /**
     * @brief Persists the Automatic1111 / SD WebUI server base URL.
     * @param url  Server base URL.
     */
    Q_INVOKABLE void setImageGenA1111Url(const QString& url);

    /**
     * @brief Returns the A1111 bearer token used when fronted by an
     *        auth-requiring reverse proxy.
     * @returns Stored token via CryptoUtils, or empty when the
     *          server is unauthenticated.
     */
    Q_INVOKABLE QString imageGenA1111Key() const;

    /**
     * @brief Persists the A1111 bearer token.
     * @param key  Bearer token, or empty to clear.
     */
    Q_INVOKABLE void setImageGenA1111Key(const QString& key);

    /**
     * @brief Reports whether the active image backend is fully configured.
     * @returns True iff the active backend is set AND its required
     *          config field (URL / path / key) is non-empty.
     *
     * The QML "Generate Image" button and the `generate_image` tool
     * both gate on this.
     */
    Q_INVOKABLE bool isImageGenConfigured() const;

    // -----------------------------------------------------------------------
    // Appearance settings
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the selected app theme name.
     * @returns Theme id (e.g. "auto", "light", "dark").
     */
    QString appTheme() const;

    /**
     * @brief Persists the selected app theme.
     * @param theme  Theme id.
     */
    void setAppTheme(const QString& theme);

    /**
     * @brief Returns the explicit verzeta-voice daemon path override.
     * @returns Absolute binary path, or empty when the standard search
     *          locations should be used.
     */
    QString voiceBinaryPath() const;

    /**
     * @brief Persists the explicit verzeta-voice daemon path override.
     * @param path Absolute binary path; empty clears the override.
     */
    void setVoiceBinaryPath(const QString& path);

    /**
     * @brief Returns whether the voice service starts with the app.
     * @returns The persisted autostart flag; default off.
     */
    bool voiceAutostart() const;

    /**
     * @brief Persists the voice-service autostart flag.
     * @param on True to spawn the daemon at application start.
     */
    void setVoiceAutostart(bool on);

    /**
     * @brief Returns the voice used by agents with no voice of their own.
     * @returns Daemon voice id, or empty to let the daemon pick.
     */
    QString voiceDefaultVoice() const;

    /**
     * @brief Persists the default speaking voice.
     * @param voiceId Daemon voice id; empty lets the daemon pick.
     */
    void setVoiceDefaultVoice(const QString& voiceId);

    /**
     * @brief Returns the push-to-talk hotkey for voice calls.
     * @returns A portable key-sequence string (e.g. "F9"); empty when
     *          unbound (the on-screen button is then the only control).
     */
    QString voicePttHotkey() const;

    /**
     * @brief Persists the push-to-talk hotkey.
     * @param sequence Portable key-sequence string; empty unbinds.
     */
    void setVoicePttHotkey(const QString& sequence);

    /**
     * @brief Returns the saved per-agent voice assignments.
     * @returns Member alias to daemon voice id; empty when none saved.
     */
    QVariantMap voiceAssignments() const;

    /**
     * @brief Persists the per-agent voice assignments.
     * @param assignments Member alias to daemon voice id. Stored as JSON
     *                    in a single setting, so no schema change is
     *                    needed for a voice-only concern.
     */
    void setVoiceAssignments(const QVariantMap& assignments);

    /**
     * @brief Returns the proportional UI font family.
     * @returns Font family name.
     */
    QString fontFamily() const;

    /**
     * @brief Persists the proportional UI font family.
     * @param family  Font family name.
     */
    void setFontFamily(const QString& family);

    /**
     * @brief Returns the monospace code-font family.
     * @returns Font family name.
     */
    QString codeFontFamily() const;

    /**
     * @brief Persists the monospace code-font family.
     * @param family  Font family name.
     */
    void setCodeFontFamily(const QString& family);

    /**
     * @brief Returns the base UI font size in points.
     * @returns Point size.
     */
    int fontSize() const;

    /**
     * @brief Persists the base UI font size in points.
     * @param size  Point size.
     */
    void setFontSize(int size);

    /**
     * @brief The per-turn tool-iteration budget (runaway-loop backstop).
     * @returns Maximum chained tool iterations per turn; 0 = unlimited.
     *          Defaults to 25.
     */
    int toolIterationCap() const;

    /**
     * @brief The effective shell command allow-list (programs run_shell runs).
     * @returns The persisted user-edited list, or
     *          ProcessSandbox::defaultAllowList() when the user has never edited
     *          it. An explicitly-emptied list is honored (blocks everything).
     */
    QStringList shellAllowList() const;

    /**
     * @brief Persists the effective shell allow-list (deduplicated, trimmed).
     * @param programs The full effective list of permitted program names.
     */
    void setShellAllowList(const QStringList& programs);

    /**
     * @brief Restores the compiled default allow-list (clears the user override
     *        so shellAllowList() returns ProcessSandbox::defaultAllowList()).
     */
    Q_INVOKABLE void resetShellAllowListToDefault();

    /**
     * @brief Persists the per-turn tool-iteration budget.
     * @param cap Maximum chained tool iterations; 0 = unlimited. Negatives
     *            are treated as the default.
     */
    void setToolIterationCap(int cap);

    // -----------------------------------------------------------------------
    // RAGP local backend
    // -----------------------------------------------------------------------

    /**
     * @brief Reports whether the local llama.cpp RAGP backend is enabled.
     * @returns True when the user has opted into local Tier-3 classification.
     */
    bool ragpLocalEnabled() const;

    /**
     * @brief Persists the local-RAGP enable flag.
     * @param enabled  True to enable local classification.
     */
    void setRagpLocalEnabled(bool enabled);

    /**
     * @brief Returns the default RAGP model filename.
     * @returns Basename of the GGUF inside ragpModelsDir(), or empty
     *          when none is selected.
     */
    QString ragpDefaultModelFilename() const;

    /**
     * @brief Persists the default RAGP model filename.
     * @param filename  Basename of the GGUF inside ragpModelsDir().
     */
    void setRagpDefaultModelFilename(const QString& filename);

    /**
     * @brief Returns the absolute path to the RAGP models scan directory.
     * @returns `QStandardPaths::AppLocalDataLocation + "/ragp-models/"`.
     *
     * Directory is auto-created by ragpAvailableModels() on first access.
     */
    QString ragpModelsDir() const;

    /**
     * @brief Returns the compiled-in llama.cpp acceleration backend name.
     * @returns Backend name (e.g. "cuda", "vulkan", "cpu"), or
     *          "unavailable" when VERZETA_HAS_LLAMA is not defined.
     */
    QString ragpAccelerationBackend() const;

    /**
     * @brief Scans the RAGP models directory for `.gguf` files.
     * @returns Sorted (case-insensitive) basenames; the directory is
     *          created when missing.
     *
     * Emits ragpAvailableModelsChanged() when the returned list differs
     * from the previous scan's result.
     */
    Q_INVOKABLE QStringList ragpAvailableModels();

    /**
     * @brief Resolves a RAGP model filename to its absolute path.
     * @param filename  Basename from ragpAvailableModels().
     * @returns Absolute path under ragpModelsDir(), or empty when
     *          `filename` is empty.
     */
    Q_INVOKABLE QString ragpModelPath(const QString& filename) const;

    /**
     * @brief Add an existing GGUF file to the scanned-models list
     *        by symlinking it from ragpModelsDir() to its source
     *        location. The source file is NOT copied or moved;
     *        only a symlink is created. This lets users select a
     *        model stored anywhere on disk (common: models pre-
     *        downloaded by sibling tools into
     *        ~/.local/share/\<tool\>/models/) without duplicating
     *        multi-gigabyte files.
     *
     *        Validation (all failures return a non-empty error
     *        string and leave state unchanged):
     *          - sourceAbsolutePath must be non-empty and absolute
     *          - file must exist, be a regular file, readable
     *          - extension must be ".gguf" (case-insensitive)
     *          - first 4 bytes must be the GGUF magic ("GGUF")
     *          - target name in ragpModelsDir() must not already
     *            exist pointing to a DIFFERENT target (idempotent
     *            if it points to the same path)
     *
     *        Behavioural notes:
     *          - If the source is already INSIDE ragpModelsDir(),
     *            no symlink is created; the method just confirms
     *            its presence and returns success.
     *          - On success, emits ragpAvailableModelsChanged so
     *            the UI dropdown refreshes. The target basename
     *            is NOT automatically set as
     *            ragpDefaultModelFilename; the caller does that
     *            explicitly if they want.
     *
     *        Q_INVOKABLE so QML's FileDialog.onAccepted handler
     *        can call it directly with the picked absolute path.
     *
     * @param sourceAbsolutePath Absolute path to an existing .gguf
     *                           file anywhere on the filesystem.
     * @return Empty string on success; human-readable error
     *         message on any validation or filesystem failure.
     *         Never throws.
     */
    Q_INVOKABLE QString ragpAddModelFromPath(const QString& sourceAbsolutePath);

    /**
     * @brief Remove a scanned model from the list by removing its
     *        symlink in ragpModelsDir(). Only removes SYMLINKS and
     *        never touches real files. This is a safety property:
     *        a user who dropped a real .gguf into the models
     *        directory via the shell and then clicks "Remove from
     *        list" in the UI should NOT lose their file; they
     *        have to delete it manually from the shell.
     *
     *        Q_INVOKABLE so QML's "Remove" button can call it
     *        directly with the selected filename.
     *
     * @param filename Basename as it appears in
     *                 ragpAvailableModels() (not an absolute path).
     * @return Empty string on success; human-readable error
     *         message if the file is missing, is not a symlink,
     *         or cannot be removed. Never throws.
     */
    Q_INVOKABLE QString ragpRemoveModel(const QString& filename);


    /**
     * @brief Reports whether the settings table has a row for the
     *        given key.
     * @param key  Settings key.
     * @returns True iff the user has explicitly stored a value; false
     *          when the getter falls back to its hardcoded default.
     *
     * Used by isProviderConfigured() to decide whether Ollama (which
     * has a default URL) was actually set up by the user.
     */
    Q_INVOKABLE bool hasSetting(const QString& key) const;

    /**
     * @brief Reports whether the user has fully configured a provider.
     * @param providerId  Provider id (e.g. "ollama", "openai",
     *                    "anthropic", "gemini", "openrouter",
     *                    "deepseek", "llamacpp_local",
     *                    "llamacpp_remote").
     * @returns True iff the provider's required configuration is set.
     *
     * Drives the AgentSettings provider dropdown so providers without
     * an API key / URL / model path don't surface in chat / settings
     * UIs with their hardcoded default model lists.
     *
     * Per-provider rules:
     *   - openai / anthropic / gemini / openrouter / deepseek →
     *     `hasApiKey(id)`.
     *   - ollama          → `hasSetting("ollama_base_url")`.
     *   - llamacpp_local  → `!llamaCppModelPath().isEmpty()`.
     *   - llamacpp_remote → `!providerBaseUrl(id).isEmpty()`.
     *   - Any other id    → false.
     */
    Q_INVOKABLE bool isProviderConfigured(const QString& providerId) const;

  signals:
    /** @brief Emitted whenever the default provider id changes. */
    void defaultProviderChanged();

    /** @brief Emitted whenever the Ollama base URL changes. */
    void ollamaBaseUrlChanged();

    /**
     * @brief Emitted when the stored base URL for a provider mutates.
     * @param provider  Provider id whose URL changed.
     * @param url       New URL (empty when the override was cleared).
     *
     * AppController wires the registered provider's `setBaseUrl` to
     * this signal so changes propagate without a restart.
     */
    void providerBaseUrlChanged(const QString& provider, const QString& url);

    /**
     * @brief Emitted when the llama.cpp Remote tool-calling toggle
     *        is flipped.
     * @param enabled  New toggle value.
     *
     * AppController wires the provider's setSupportsToolCalling to
     * this signal.
     */
    void llamaCppRemoteSupportsToolsChanged(bool enabled);

    /**
     * @brief Emitted whenever image-gen config changes (active
     *        backend, local CLI path, etc.) so QML bindings of
     *        isImageGenConfigured() re-evaluate without polling.
     */
    void imageGenConfigChanged();

    /** @brief Emitted whenever the active llama.cpp model path changes. */
    void llamaCppModelPathChanged();

    /** @brief Emitted whenever the llama.cpp model directory changes. */
    void llamaCppModelDirChanged();

    /** @brief Emitted whenever the app theme selection changes. */
    void appThemeChanged();

    /** @brief Emitted whenever the UI font family changes. */
    void fontFamilyChanged();
    /** @brief Voice daemon explicit-path override changed. */
    void voiceBinaryPathChanged();
    /** @brief Voice-service autostart flag changed. */
    void voiceAutostartChanged();

    /** @brief Emitted whenever the monospace code-font family changes. */
    void codeFontFamilyChanged();

    /** @brief Emitted whenever the base UI font size changes. */
    void fontSizeChanged();

    /** @brief Emitted whenever the tool-iteration budget changes. */
    void toolIterationCapChanged();

    /** @brief Emitted whenever the shell command allow-list changes. */
    void shellAllowListChanged();

    /** @brief Emitted whenever firstRunComplete() flips. */
    void firstRunCompleteChanged();

    // RAGP local backend

    /** @brief Emitted whenever the local-RAGP enable flag changes. */
    void ragpLocalEnabledChanged();

    /** @brief Emitted whenever the default RAGP model filename changes. */
    void ragpDefaultModelFilenameChanged();
    /**
     * @brief Emitted when ragpAvailableModels() observes a changed
     *        set of .gguf files in ragpModelsDir() compared to the
     *        prior scan. QML can connect this to re-populate the
     *        model-selector dropdown.
     */
    void ragpAvailableModelsChanged();
    /**
     * @brief Consolidated signal for AppController. Fires when EITHER
     *        ragpLocalEnabled or ragpDefaultModelFilename changes. In
     *        both cases the AppController needs to re-evaluate which
     *        backend Ragp::Service should hold. Saves AppController
     *        from listening to two separate signals and debouncing.
     */
    void ragpBackendConfigChanged();

    // Embeddings local backend (mirrors the RAGP signals above).
    /** @brief Emitted whenever the local-embeddings enable flag changes. */
    void embeddingLocalEnabledChanged();
    /** @brief Emitted whenever the selected local embedding model changes. */
    void embeddingLocalModelFilenameChanged();
    /**
     * @brief Emitted when embeddingAvailableModels() observes a changed set of
     *        .gguf files in embeddingModelsDir() versus the prior scan. QML
     *        connects this to re-populate the model selector.
     */
    void embeddingAvailableModelsChanged();

    /**
     * @brief Emitted whenever any setting changes.
     * @param key The settings key that changed (e.g., "default_provider").
     */
    void settingsChanged(const QString& key);

    /**
     * @brief Emitted after testOllamaConnection() completes.
     * @param success true if the server responded with HTTP 200.
     * @param message "Connected" on success, or a human-readable error on failure.
     */
    void ollamaConnectionResult(bool success, const QString& message);

  private:
    DbManager& m_db;
    QNetworkAccessManager m_networkMgr;  ///< For Ollama connectivity test

    mutable QStringList m_cachedAvailableModels;

    mutable QStringList m_cachedEmbeddingModels;

    /**
     * @brief Reads a setting from the database.
     * @param key Settings key.
     * @param defaultValue Value to return if key is not found.
     * @return Stored string value, or defaultValue.
     * @complexity O(1), single indexed key lookup.
     */
    QString getSetting(const QString& key, const QString& defaultValue = {}) const;

    /**
     * @brief Writes or updates a setting in the database.
     * @param key Settings key.
     * @param value Settings value.
     * @sideeffects Inserts or replaces row in settings table. Emits settingsChanged(key).
     * @complexity O(1), single parameterized UPSERT.
     */
    void setSetting(const QString& key, const QString& value);
};

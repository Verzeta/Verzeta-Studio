// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file settings-service.cpp
 * @brief Implementation of the application-wide settings service.
 *
 *        Persists all settings to SQLite; delegates API key storage
 *        to CryptoUtils. Exposes `databasePath` and async helpers
 *        such as `testOllamaConnection()`.
 * @layer Service
 * @dependencies CryptoUtils, DbManager, Qt6::Core, Qt6::Sql, Qt6::Network.
 */


#include "settings-service.h"

#include <optional>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>

// Hard-link creation + file identity on Windows. QFile::link there is a
// COM shell shortcut, which is the wrong primitive for a models folder.
// See addGgufToModelsDir. NOMINMAX / WIN32_LEAN_AND_MEAN keep windows.h
// from defining min/max macros that break Qt headers.
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "../utils/crypto-utils.h"
#include "../utils/logger.h"
#include "../utils/process-sandbox.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the SettingsService.
 * @param db Reference to the open DbManager instance.
 * @param parent Optional Qt parent.
 */
SettingsService::SettingsService(DbManager& db, QObject* parent) : QObject(parent), m_db(db) {}

// ---------------------------------------------------------------------------
// API key management
// ---------------------------------------------------------------------------

/*
 * @brief Retrieves a stored API key for the given provider.
 * @param provider Provider identifier (e.g., "openai").
 * @return API key string, or empty string if not set.
 */
QString SettingsService::apiKey(const QString& provider) const {
    return CryptoUtils::retrieveApiKey(provider);
}

/*
 * @brief Stores an API key for the given provider.
 * @param provider Provider identifier.
 * @param key API key value.
 * @sideeffects Writes to OS keychain or obfuscated QSettings.
 */
void SettingsService::setApiKey(const QString& provider, const QString& key) {
    if (key.isEmpty()) {
        CryptoUtils::deleteApiKey(provider);
    } else {
        CryptoUtils::storeApiKey(provider, key);
    }
    emit settingsChanged(QStringLiteral("apikey.") + provider);
    if (provider == QStringLiteral("openai")) {
        emit imageGenConfigChanged();
    }
}

/**
 * @brief Returns whether an API key has been configured for the provider.
 * @param provider Provider identifier.
 * @return true if a non-empty key is stored.
 */
bool SettingsService::hasApiKey(const QString& provider) const {
    return !CryptoUtils::retrieveApiKey(provider).isEmpty();
}

// ---------------------------------------------------------------------------
// Provider settings
// ---------------------------------------------------------------------------

QString SettingsService::defaultProvider() const {
    return getSetting(QStringLiteral("default_provider"), QStringLiteral("ollama"));
}

void SettingsService::setDefaultProvider(const QString& id) {
    setSetting(QStringLiteral("default_provider"), id);
    emit defaultProviderChanged();
}

QString SettingsService::ollamaBaseUrl() const {
    return getSetting(QStringLiteral("ollama_base_url"), QStringLiteral("http://localhost:11434"));
}

void SettingsService::setOllamaBaseUrl(const QString& url) {
    setSetting(QStringLiteral("ollama_base_url"), url);
    emit ollamaBaseUrlChanged();
}


QString SettingsService::providerBaseUrl(const QString& provider) const {
    if (provider.isEmpty())
        return QString();
    return getSetting(QStringLiteral("provider_base_url_") + provider);
}

void SettingsService::setProviderBaseUrl(const QString& provider, const QString& url) {
    if (provider.isEmpty())
        return;
    setSetting(QStringLiteral("provider_base_url_") + provider, url);
    emit providerBaseUrlChanged(provider, url);
}

QString SettingsService::imageGenLocalSdPath() const {
    return getSetting(QStringLiteral("image_gen_local_sd_path"));
}

void SettingsService::setImageGenLocalSdPath(const QString& path) {
    setSetting(QStringLiteral("image_gen_local_sd_path"), path);
    emit imageGenConfigChanged();
}

QString SettingsService::summarizationProvider() const {
    return getSetting(QStringLiteral("summarization_provider"));
}

void SettingsService::setSummarizationProvider(const QString& id) {
    setSetting(QStringLiteral("summarization_provider"), id);
}

QString SettingsService::summarizationModel() const {
    return getSetting(QStringLiteral("summarization_model"));
}

void SettingsService::setSummarizationModel(const QString& model) {
    setSetting(QStringLiteral("summarization_model"), model);
}

QString SettingsService::embeddingEndpointBaseUrl() const {
    return getSetting(QStringLiteral("embedding_endpoint_base_url"));
}

void SettingsService::setEmbeddingEndpointBaseUrl(const QString& baseUrl) {
    setSetting(QStringLiteral("embedding_endpoint_base_url"), baseUrl);
}

QString SettingsService::embeddingModel() const {
    return getSetting(QStringLiteral("embedding_model"));
}

void SettingsService::setEmbeddingModel(const QString& model) {
    setSetting(QStringLiteral("embedding_model"), model);
}

// ---------------------------------------------------------------------------
// Embeddings local backend (mirrors the RAGP local-backend pattern)
// ---------------------------------------------------------------------------

bool SettingsService::embeddingLocalEnabled() const {
    // Default OFF — remote is the out-of-the-box path; the user opts into local
    // after placing a GGUF in embeddingModelsDir() (and only on llama builds).
    return getSetting(QStringLiteral("embedding_local_enabled"), QStringLiteral("0")) ==
           QStringLiteral("1");
}

void SettingsService::setEmbeddingLocalEnabled(bool enabled) {
    if (embeddingLocalEnabled() == enabled)
        return;
    setSetting(QStringLiteral("embedding_local_enabled"),
               enabled ? QStringLiteral("1") : QStringLiteral("0"));
    emit embeddingLocalEnabledChanged();
}

QString SettingsService::embeddingLocalModelFilename() const {
    return getSetting(QStringLiteral("embedding_local_model_filename"));
}

void SettingsService::setEmbeddingLocalModelFilename(const QString& filename) {
    // Defensive: basenames only — never a path that could escape the dir.
    if (filename.contains(QLatin1Char('/')) || filename.contains(QLatin1Char('\\'))) {
        qCWarning(verzetaUi) << "SettingsService::setEmbeddingLocalModelFilename: rejecting"
                             << filename << "— basenames only, no path separators.";
        return;
    }
    if (embeddingLocalModelFilename() == filename)
        return;

    if (filename.isEmpty()) {
        // Empty = "none selected". settings.value is NOT NULL and an empty
        // QString binds to SQL NULL, so clear by deleting the row (a missing
        // row reads back as empty — exactly the no-selection state).
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("DELETE FROM settings WHERE key = ?"));
        q.addBindValue(QStringLiteral("embedding_local_model_filename"));
        if (!q.exec()) {
            qCWarning(verzetaDb) << "SettingsService::setEmbeddingLocalModelFilename: "
                                    "failed to clear row:"
                                 << q.lastError().text();
            return;
        }
        emit settingsChanged(QStringLiteral("embedding_local_model_filename"));
    } else {
        setSetting(QStringLiteral("embedding_local_model_filename"), filename);
    }
    emit embeddingLocalModelFilenameChanged();
}

QString SettingsService::embeddingModelsDir() const {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        qCWarning(verzetaUi) << "SettingsService::embeddingModelsDir: AppLocalDataLocation "
                                "unavailable; falling back to '.embedding-models' in cwd.";
        return QDir::current().absoluteFilePath(QStringLiteral(".embedding-models"));
    }
    return root + QStringLiteral("/embedding-models");
}

QStringList SettingsService::embeddingAvailableModels() {
    const QString dirPath = embeddingModelsDir();
    QDir dir(dirPath);
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            qCWarning(verzetaUi) << "SettingsService::embeddingAvailableModels: failed to "
                                    "create models directory at"
                                 << dirPath;
            return {};
        }
        qCInfo(verzetaUi) << "SettingsService: created embedding models directory at" << dirPath;
    }
    const QStringList filters = {QStringLiteral("*.gguf")};
    QStringList result =
        dir.entryList(filters, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    if (result != m_cachedEmbeddingModels) {
        m_cachedEmbeddingModels = result;
        emit embeddingAvailableModelsChanged();
    }
    return result;
}

QString SettingsService::imageGenActiveBackend() const {
    return getSetting(QStringLiteral("image_gen_active_backend"));
}

void SettingsService::setImageGenActiveBackend(const QString& backend) {
    setSetting(QStringLiteral("image_gen_active_backend"), backend);
    emit imageGenConfigChanged();
}


QString SettingsService::webSearchActiveProvider() const {
    return getSetting(QStringLiteral("web_search_active_provider"), QStringLiteral("ddg-html"));
}

void SettingsService::setWebSearchActiveProvider(const QString& providerId) {
    setSetting(QStringLiteral("web_search_active_provider"), providerId);
}

QString SettingsService::webSearchBaseUrl(const QString& providerId) const {
    return getSetting(QStringLiteral("web_search_base_url_") + providerId);
}

void SettingsService::setWebSearchBaseUrl(const QString& providerId, const QString& url) {
    setSetting(QStringLiteral("web_search_base_url_") + providerId, url);
}

QString SettingsService::webSearchCustomConfig() const {
    return getSetting(QStringLiteral("web_search_custom_config"));
}

void SettingsService::setWebSearchCustomConfig(const QString& json) {
    setSetting(QStringLiteral("web_search_custom_config"), json);
}

QString SettingsService::imageGenOpenAICompatUrl() const {
    return getSetting(QStringLiteral("image_gen_openai_compat_url"));
}

void SettingsService::setImageGenOpenAICompatUrl(const QString& url) {
    setSetting(QStringLiteral("image_gen_openai_compat_url"), url);
    emit imageGenConfigChanged();
}

QString SettingsService::imageGenOpenAICompatKey() const {
    return CryptoUtils::retrieveApiKey(QStringLiteral("image_gen_openai_compat"));
}

void SettingsService::setImageGenOpenAICompatKey(const QString& key) {
    if (key.isEmpty()) {
        CryptoUtils::deleteApiKey(QStringLiteral("image_gen_openai_compat"));
    } else {
        CryptoUtils::storeApiKey(QStringLiteral("image_gen_openai_compat"), key);
    }
    emit imageGenConfigChanged();
}

QString SettingsService::imageGenA1111Url() const {
    return getSetting(QStringLiteral("image_gen_a1111_url"));
}

void SettingsService::setImageGenA1111Url(const QString& url) {
    setSetting(QStringLiteral("image_gen_a1111_url"), url);
    emit imageGenConfigChanged();
}

QString SettingsService::imageGenA1111Key() const {
    return CryptoUtils::retrieveApiKey(QStringLiteral("image_gen_a1111"));
}

void SettingsService::setImageGenA1111Key(const QString& key) {
    if (key.isEmpty()) {
        CryptoUtils::deleteApiKey(QStringLiteral("image_gen_a1111"));
    } else {
        CryptoUtils::storeApiKey(QStringLiteral("image_gen_a1111"), key);
    }
    emit imageGenConfigChanged();
}

bool SettingsService::isImageGenConfigured() const {
    const QString backend = imageGenActiveBackend();
    if (backend == QStringLiteral("openai_dalle")) {
        return hasApiKey(QStringLiteral("openai"));
    }
    if (backend == QStringLiteral("local_cli")) {
        return !imageGenLocalSdPath().isEmpty();
    }
    if (backend == QStringLiteral("remote_openai_compat")) {
        // Key is optional (many self-hosted deployments are
        // unauthenticated); URL is required.
        return !imageGenOpenAICompatUrl().isEmpty();
    }
    if (backend == QStringLiteral("remote_a1111")) {
        return !imageGenA1111Url().isEmpty();
    }
    return false;
}

bool SettingsService::llamaCppRemoteSupportsTools() const {
    return getSetting(QStringLiteral("llamacpp_remote_supports_tools")) == QStringLiteral("1");
}

void SettingsService::setLlamaCppRemoteSupportsTools(bool enabled) {
    setSetting(QStringLiteral("llamacpp_remote_supports_tools"),
               enabled ? QStringLiteral("1") : QStringLiteral("0"));
    emit llamaCppRemoteSupportsToolsChanged(enabled);
}

QString SettingsService::customServersBlob() const {
    return getSetting(QStringLiteral("custom_servers"));
}

void SettingsService::setCustomServersBlob(const QString& json) {
    setSetting(QStringLiteral("custom_servers"), json);
}

QString SettingsService::imageProvidersBlob() const {
    return getSetting(QStringLiteral("image_providers"));
}

void SettingsService::setImageProvidersBlob(const QString& json) {
    setSetting(QStringLiteral("image_providers"), json);
}

QString SettingsService::llamaCppModelPath() const {
    return getSetting(QStringLiteral("llamacpp_model_path"));
}

void SettingsService::setLlamaCppModelPath(const QString& path) {
    setSetting(QStringLiteral("llamacpp_model_path"), path);
    emit llamaCppModelPathChanged();
}

QString SettingsService::llamaCppModelDir() const {
    return getSetting(QStringLiteral("llamacpp_model_dir"));
}

void SettingsService::setLlamaCppModelDir(const QString& dir) {
    setSetting(QStringLiteral("llamacpp_model_dir"), dir);
    emit llamaCppModelDirChanged();
}

// ---------------------------------------------------------------------------
// Appearance settings
// ---------------------------------------------------------------------------

QString SettingsService::appTheme() const {
    return getSetting(QStringLiteral("app_theme"), QStringLiteral("system"));
}

void SettingsService::setAppTheme(const QString& theme) {
    setSetting(QStringLiteral("app_theme"), theme);
    emit appThemeChanged();
}

bool SettingsService::firstRunComplete() const {
    return getSetting(QStringLiteral("first_run_complete"), QStringLiteral("0")) ==
           QStringLiteral("1");
}

void SettingsService::setFirstRunComplete(bool complete) {
    setSetting(QStringLiteral("first_run_complete"),
               complete ? QStringLiteral("1") : QStringLiteral("0"));
    emit firstRunCompleteChanged();
}

bool SettingsService::hasLocalLlama() const {
    // Local inference = the verzeta-inference sidecar (the app links
    // no llama code). Runtime presence check, not a compile flag.
    QString name = QStringLiteral("verzeta-inference");
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return QFileInfo::exists(QCoreApplication::applicationDirPath() + QLatin1Char('/') + name);
}

QString SettingsService::fontFamily() const {
    return getSetting(QStringLiteral("font_family"));
}

void SettingsService::setFontFamily(const QString& family) {
    setSetting(QStringLiteral("font_family"), family);
    emit fontFamilyChanged();
}

QString SettingsService::voiceBinaryPath() const {
    return getSetting(QStringLiteral("voice_binary_path"));
}

void SettingsService::setVoiceBinaryPath(const QString& path) {
    setSetting(QStringLiteral("voice_binary_path"), path);
    emit voiceBinaryPathChanged();
}

bool SettingsService::voiceAutostart() const {
    return getSetting(QStringLiteral("voice_autostart")) == QLatin1String("1");
}

void SettingsService::setVoiceAutostart(bool on) {
    if (on == voiceAutostart())
        return;  // No-op; don't fire signals.
    setSetting(QStringLiteral("voice_autostart"), on ? QStringLiteral("1") : QStringLiteral("0"));
    emit voiceAutostartChanged();
}
QString SettingsService::voiceDefaultVoice() const {
    return getSetting(QStringLiteral("voice_default_voice"));
}
void SettingsService::setVoiceDefaultVoice(const QString& voiceId) {
    if (voiceId == voiceDefaultVoice())
        return;
    setSetting(QStringLiteral("voice_default_voice"), voiceId);
}
QString SettingsService::voicePttHotkey() const {
    return getSetting(QStringLiteral("voice_ptt_hotkey"));
}
void SettingsService::setVoicePttHotkey(const QString& sequence) {
    if (sequence == voicePttHotkey())
        return;
    setSetting(QStringLiteral("voice_ptt_hotkey"), sequence);
}
QVariantMap SettingsService::voiceAssignments() const {
    // One JSON setting rather than a table: voice choice is an add-on
    // preference, not conversation data, so it needs no schema change and
    // no migration.
    const QString raw = getSetting(QStringLiteral("voice_assignments"));
    if (raw.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap{};
}
void SettingsService::setVoiceAssignments(const QVariantMap& assignments) {
    const QString encoded = QString::fromUtf8(
        QJsonDocument(QJsonObject::fromVariantMap(assignments)).toJson(QJsonDocument::Compact));
    if (encoded == getSetting(QStringLiteral("voice_assignments")))
        return;
    setSetting(QStringLiteral("voice_assignments"), encoded);
}

QString SettingsService::codeFontFamily() const {
    return getSetting(QStringLiteral("code_font_family"), QStringLiteral("Monospace"));
}

void SettingsService::setCodeFontFamily(const QString& family) {
    setSetting(QStringLiteral("code_font_family"), family);
    emit codeFontFamilyChanged();
}

int SettingsService::fontSize() const {
    return getSetting(QStringLiteral("font_size"), QStringLiteral("14")).toInt();
}

int SettingsService::toolIterationCap() const {
    // Default 25; 0 = unlimited. A negative (corrupt) value falls back to 25.
    const int v = getSetting(QStringLiteral("tool_iteration_cap"), QStringLiteral("25")).toInt();
    return v < 0 ? 25 : v;
}

void SettingsService::setToolIterationCap(int cap) {
    setSetting(QStringLiteral("tool_iteration_cap"), QString::number(cap < 0 ? 25 : cap));
    emit toolIterationCapChanged();
}

QStringList SettingsService::shellAllowList() const {
    // Unset/corrupt → the compiled default (single source of truth). A stored
    // (even empty) JSON array is an explicit user choice and is honored as-is.
    const QString raw = getSetting(QStringLiteral("shell_allow_list"), QString());
    if (raw.isEmpty())
        return ProcessSandbox::defaultAllowList();
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isArray())
        return ProcessSandbox::defaultAllowList();
    QStringList out;
    const QJsonArray arr = doc.array();
    for (const QJsonValue& v : arr) {
        const QString s = v.toString().trimmed();
        if (!s.isEmpty() && !out.contains(s))
            out.append(s);
    }
    return out;
}

void SettingsService::setShellAllowList(const QStringList& programs) {
    // Deduplicate + trim, preserving order, and persist as a JSON array.
    QJsonArray arr;
    QStringList seen;
    for (const QString& p : programs) {
        const QString s = p.trimmed();
        if (!s.isEmpty() && !seen.contains(s)) {
            seen.append(s);
            arr.append(s);
        }
    }
    setSetting(QStringLiteral("shell_allow_list"),
               QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    emit shellAllowListChanged();
}

void SettingsService::resetShellAllowListToDefault() {
    // Delete the override row so shellAllowList() falls back to the compiled
    // default (getSetting returns its default when no row exists). We DELETE
    // rather than store an empty string because an empty stored array is a
    // legitimate, distinct "block everything" choice.
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM settings WHERE key = ?"));
    q.addBindValue(QStringLiteral("shell_allow_list"));
    if (!q.exec()) {
        qCWarning(verzetaDb) << "SettingsService: failed to reset shell allow-list:"
                             << q.lastError().text();
    }
    emit shellAllowListChanged();
}

void SettingsService::setFontSize(int size) {
    setSetting(QStringLiteral("font_size"), QString::number(size));
    emit fontSizeChanged();
}


bool SettingsService::ragpLocalEnabled() const {
    return getSetting(QStringLiteral("ragp_local_enabled"), QStringLiteral("0")) ==
           QStringLiteral("1");
}

void SettingsService::setRagpLocalEnabled(bool enabled) {
    const bool current = ragpLocalEnabled();
    if (current == enabled)
        return;  // No-op; don't fire signals.
    setSetting(QStringLiteral("ragp_local_enabled"),
               enabled ? QStringLiteral("1") : QStringLiteral("0"));
    emit ragpLocalEnabledChanged();
    emit ragpBackendConfigChanged();
}

QString SettingsService::ragpDefaultModelFilename() const {
    return getSetting(QStringLiteral("ragp_default_model_filename"));
}

void SettingsService::setRagpDefaultModelFilename(const QString& filename) {
    // Defensive: reject path separators so a stale/malicious value
    // can't escape ragpModelsDir(). We only accept a basename.
    if (filename.contains(QLatin1Char('/')) || filename.contains(QLatin1Char('\\'))) {
        qCWarning(verzetaUi) << "SettingsService::setRagpDefaultModelFilename: rejecting"
                             << filename << "— basenames only, no path separators.";
        return;
    }
    const QString current = ragpDefaultModelFilename();
    if (current == filename)
        return;

    if (filename.isEmpty()) {
        // Clear the setting by DELETING the row. The `settings.value`
        // column is NOT NULL, and Qt's SQL driver converts an empty
        // QString bind to SQL NULL — so we can't just write "" via
        // setSetting. A missing row naturally yields the empty
        // default on read, which is exactly the "no default model
        // selected" state we want.
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("DELETE FROM settings WHERE key = ?"));
        q.addBindValue(QStringLiteral("ragp_default_model_filename"));
        if (!q.exec()) {
            qCWarning(verzetaDb) << "SettingsService::setRagpDefaultModelFilename: "
                                    "failed to clear row:"
                                 << q.lastError().text();
            return;
        }
        emit settingsChanged(QStringLiteral("ragp_default_model_filename"));
    } else {
        setSetting(QStringLiteral("ragp_default_model_filename"), filename);
    }

    emit ragpDefaultModelFilenameChanged();
    emit ragpBackendConfigChanged();
}

QString SettingsService::ragpModelsDir() const {
    // AppLocalDataLocation matches what QStandardPaths chose for the
    // rest of the app (database, logs, per-project generated files).
    // Keeping the RAGP models under the same root makes backup and
    // cleanup predictable for the user.
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        // AppLocalDataLocation is always populated on desktop Linux /
        // macOS / Windows, so empty means the platform is misconfigured.
        // Fall back to the current working directory as a last resort so
        // the app still functions; user sees the path in Settings UI.
        qCWarning(verzetaUi) << "SettingsService::ragpModelsDir: AppLocalDataLocation "
                                "unavailable; falling back to '.ragp-models' in cwd.";
        return QDir::current().absoluteFilePath(QStringLiteral(".ragp-models"));
    }
    return root + QStringLiteral("/ragp-models");
}

QString SettingsService::ragpAccelerationBackend() const {
    // Static capability only: local inference runs in the sidecar
    // process. The ACTUAL acceleration (GPU device vs CPU) is decided
    // per model load at runtime and reported live through
    // AppController::ragpAccelerationLive — the UI prefers that value
    // and uses this one only as the before-first-load fallback. The
    // old hardcoded "cpu (sidecar)" predates the GPU-first loader and
    // misreported GPU machines.
    return hasLocalLlama() ? QStringLiteral("sidecar") : QStringLiteral("unavailable");
}

QStringList SettingsService::ragpAvailableModels() {
    const QString dirPath = ragpModelsDir();
    QDir dir(dirPath);

    if (!dir.exists()) {
        // Create on first access so the user has an obvious place to
        // drop models into. mkpath is idempotent and only logs if it
        // actually has to do work; failure here is non-fatal (we'll
        // just return an empty list and the UI shows "no models").
        if (!dir.mkpath(QStringLiteral("."))) {
            qCWarning(verzetaUi) << "SettingsService::ragpAvailableModels: failed to "
                                    "create models directory at"
                                 << dirPath;
            return {};
        }
        qCInfo(verzetaUi) << "SettingsService: created RAGP models directory at" << dirPath;
    }

    // Case-insensitive .gguf match so both model.gguf and MODEL.GGUF
    // show up. Hidden files (dotfiles) excluded by default.
    const QStringList filters = {QStringLiteral("*.gguf")};
    QStringList result =
        dir.entryList(filters, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);

    // Fire changed-signal only when the set genuinely differs from
    // what we last returned. QML can keep a binding without spurious
    // re-renders on every settings-page open.
    if (result != m_cachedAvailableModels) {
        m_cachedAvailableModels = result;
        // Emit through a const_cast: the scan is logically const from
        // the caller's perspective (it reads the filesystem) but the
        // cache field is mutable so we can update it. The Q_OBJECT
        // signal infrastructure requires a non-const `this`.
        const_cast<SettingsService*>(this)->emit ragpAvailableModelsChanged();
    }
    return result;
}

QString SettingsService::ragpModelPath(const QString& filename) const {
    if (filename.isEmpty())
        return {};
    // Basename-only contract: strip any path separators defensively
    // even though setRagpDefaultModelFilename rejects them. A caller
    // might pass a filename from elsewhere.
    const QFileInfo fi(filename);
    return ragpModelsDir() + QLatin1Char('/') + fi.fileName();
}


namespace {

// Cheap 4-byte magic sniff. GGUF files start with ASCII "GGUF".
// Same check LocalLlamaBackend::Worker::hasGgufMagic performs
// before the engine initialises; duplicated here (rather than exposed
// from the worker) because SettingsService sits below the RAGP
// layer in the dependency graph and cannot include from it.
bool fileHasGgufMagic(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    char magic[4] = {0};
    const qint64 n = f.read(magic, 4);
    f.close();
    if (n != 4)
        return false;
    return magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F';
}

}  // namespace

namespace {

#ifdef Q_OS_WIN
/**
 * @brief Identity of an existing file on Windows: which volume it lives on,
 *        its index within that volume, and how many directory entries point
 *        at its data.
 *
 * A hard link is not distinguishable from a regular file by name or by
 * QFileInfo, so identity has to come from the filesystem itself. Two paths
 * naming the same data share a (volume, index) pair, and `links > 1` means at
 * least one other directory entry still references the data.
 */
struct WinFileId {
    quint32 volume = 0;
    quint64 index = 0;
    quint32 links = 0;
};

/** @returns The file's identity, or nullopt when it cannot be opened. */
std::optional<WinFileId> winFileId(const QString& path) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    // Zero access rights: we only want metadata, so this succeeds even while
    // another process holds the file open. FILE_FLAG_BACKUP_SEMANTICS lets the
    // same call work for directories.
    const HANDLE h = CreateFileW(native.c_str(),
                                 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::nullopt;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok)
        return std::nullopt;

    WinFileId id;
    id.volume = info.dwVolumeSerialNumber;
    id.index = (static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    id.links = info.nNumberOfLinks;
    return id;
}

/** @returns True iff both paths exist and name the same file data. */
bool winSameFile(const QString& a, const QString& b) {
    const auto ia = winFileId(a);
    const auto ib = winFileId(b);
    return ia && ib && ia->volume == ib->volume && ia->index == ib->index;
}

/**
 * @brief Creates a hard link at @p linkPath pointing at @p targetPath.
 * @returns False when the two paths are on different volumes, when the
 *          filesystem is not NTFS, or when @p linkPath already exists.
 *
 * Deliberately NOT QFile::link. On Windows Qt implements that with COM's
 * IShellLink (see QFileSystemEngine::createLink), which writes a shell
 * shortcut. A shortcut named "model.gguf" is listed by the models scan and
 * then fails to load, because its bytes are a .lnk, not a GGUF. A hard link
 * needs no elevation, costs no extra disk, and reads exactly like the
 * original file.
 */
bool winCreateHardLink(const QString& linkPath, const QString& targetPath) {
    const std::wstring link = QDir::toNativeSeparators(linkPath).toStdWString();
    const std::wstring target = QDir::toNativeSeparators(targetPath).toStdWString();
    return CreateHardLinkW(link.c_str(), target.c_str(), nullptr) != 0;
}
#endif  // Q_OS_WIN

/**
 * @brief Shared browse→link core for every GGUF models directory
 *        (RAGP and embeddings use the identical flow; one
 *        implementation so the two pickers can never drift).
 *        Validates the source (.gguf suffix + magic), then either
 *        confirms an in-directory file (idempotent), refuses a
 *        basename collision, or links the file into the directory.
 *
 *        The link is a symlink on Unix and a hard link on Windows, falling
 *        back to a copy there when the source sits on another volume. Both
 *        forms leave the user's original file exactly where it was.
 * @param sourceAbsolutePath Absolute path to an existing .gguf file.
 * @param modelsDir          Destination models directory (created on
 *                           demand).
 * @param logTag             Caller name for the success log line.
 * @return Empty string on success (including idempotent re-adds);
 *         human-readable error message otherwise.
 */
QString addGgufToModelsDir(const QString& sourceAbsolutePath,
                           const QString& modelsDir,
                           const char* logTag) {
    using ST = SettingsService;
    if (sourceAbsolutePath.isEmpty()) {
        return ST::tr("No file selected.");
    }

    const QFileInfo src(sourceAbsolutePath);
    if (!src.isAbsolute()) {
        return ST::tr("Path must be absolute: %1").arg(sourceAbsolutePath);
    }
    if (!src.exists()) {
        return ST::tr("File does not exist: %1").arg(sourceAbsolutePath);
    }
    if (!src.isFile()) {
        return ST::tr("Not a regular file: %1").arg(sourceAbsolutePath);
    }
    if (!src.isReadable()) {
        return ST::tr("File is not readable: %1").arg(sourceAbsolutePath);
    }
    if (src.suffix().compare(QStringLiteral("gguf"), Qt::CaseInsensitive) != 0) {
        return ST::tr("Not a .gguf file: %1").arg(sourceAbsolutePath);
    }
    if (!fileHasGgufMagic(sourceAbsolutePath)) {
        return ST::tr("File is not a valid GGUF (missing 'GGUF' magic "
                      "bytes): %1")
            .arg(sourceAbsolutePath);
    }

    QDir dir(modelsDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return ST::tr("Could not create models directory: %1").arg(modelsDir);
    }

    // Resolve both paths to their canonical form so the
    // "already inside models dir" check is robust against symlink
    // chains, trailing slashes, relative traversal etc.
    const QString canonicalSource = src.canonicalFilePath();
    const QString canonicalDir = QFileInfo(modelsDir).canonicalFilePath();
    const QString basename = src.fileName();
    const QString targetInDir = dir.absoluteFilePath(basename);

    // Case 1: the source is already inside the models dir. No
    // action needed — the scan will pick it up. Idempotent success.
    if (canonicalSource.startsWith(canonicalDir + QLatin1Char('/'))) {
        return QString{};
    }

    // Case 2: a path with the target basename already exists in
    // the models dir.
    QFileInfo existing(targetInDir);
    if (existing.exists() || existing.isSymLink()) {
        // If it's a symlink pointing to the SAME source, this is
        // idempotent success (user re-picked the same file).
        if (existing.isSymLink() && existing.symLinkTarget() == canonicalSource) {
            return QString{};
        }
#ifdef Q_OS_WIN
        // A hard link carries no marker: by name and by QFileInfo it looks
        // like an ordinary file. Ask the filesystem whether the entry we
        // already have and the file the user just picked are the same data,
        // so re-picking the same model is idempotent here too.
        if (winSameFile(targetInDir, canonicalSource)) {
            return QString{};
        }
#endif
        // Otherwise refuse — we do not overwrite. User has to
        // remove the existing entry first to avoid silent data
        // replacement.
        return ST::tr("A model named '%1' is already in the list. "
                      "Remove it first, or pick a file with a "
                      "different name.")
            .arg(basename);
    }

    // Case 3: link the source into the models directory.
#ifdef Q_OS_WIN
    // NEVER QFile::link here. Qt implements it on Windows with COM's
    // IShellLink, which writes a shell shortcut and does not append ".lnk"
    // (Qt's own docs: "To create a valid link on Windows, linkName must have
    // a .lnk file extension"). The result would be a shortcut *named*
    // model.gguf: the scan lists it, the loader reads a .lnk header instead
    // of GGUF magic, and adding a model appears to work but never does.
    //
    // A hard link is the right primitive: no elevation, no extra disk, and
    // the entry reads byte-for-byte as the original. It cannot span volumes,
    // so fall back to a copy when the user's file lives on another drive.
    if (!winCreateHardLink(targetInDir, canonicalSource)) {
        if (!QFile::copy(canonicalSource, targetInDir)) {
            return ST::tr("Could not add the model: %1 could not be linked "
                          "or copied into %2.")
                .arg(canonicalSource, modelsDir);
        }
        qCDebug(verzetaDb) << logTag << ": copied (cross-volume)" << canonicalSource << "→"
                           << targetInDir;
        return QString{};
    }
    qCDebug(verzetaDb) << logTag << ": hard-linked" << canonicalSource << "→" << targetInDir;
    return QString{};
#else
    if (!QFile::link(canonicalSource, targetInDir)) {
        return ST::tr("Could not link %2 into the models folder as %1.")
            .arg(targetInDir, canonicalSource);
    }

    qCDebug(verzetaDb) << logTag << ": symlinked" << canonicalSource << "→" << targetInDir;
    return QString{};
#endif
}

/**
 * @brief Shared remove-from-list core: removes ONLY symlinks from a
 *        models directory. A real file the user dropped in via the
 *        shell is never deleted by a UI action.
 * @param filename  Plain basename as shown in the list.
 * @param modelsDir The models directory to operate in.
 * @param logTag    Caller name for the success log line.
 * @return Empty string on success; human-readable error otherwise.
 */
QString
removeGgufFromModelsDir(const QString& filename, const QString& modelsDir, const char* logTag) {
    using ST = SettingsService;
    if (filename.isEmpty()) {
        return ST::tr("No model name provided.");
    }
    // Defensive: the caller should pass a basename but guard
    // against path injection same way the *ModelPath resolvers do.
    const QFileInfo fi(filename);
    if (fi.fileName() != filename) {
        return ST::tr("Model name must be a plain filename, not a "
                      "path: %1")
            .arg(filename);
    }

    const QDir dir(modelsDir);
    const QString target = dir.absoluteFilePath(filename);
    QFileInfo entry(target);

    if (!entry.exists() && !entry.isSymLink()) {
        return ST::tr("Model is not in the list: %1").arg(filename);
    }

    // Safety: only remove LINKS, never the last copy of the user's data.
    //
    // On Unix that means a symlink. On Windows the entry we create is a hard
    // link, which reports isSymLink() == false and is otherwise
    // indistinguishable from an ordinary file — so ask the filesystem how many
    // directory entries still reference the data. More than one means the
    // user's original survives this removal; exactly one means this IS their
    // only copy (a file they dropped in by hand, or the cross-volume copy
    // fallback), and we refuse for the same reason Unix refuses a real file.
#ifdef Q_OS_WIN
    const auto id = winFileId(target);
    const bool isRemovableLink = entry.isSymLink() || (id && id->links > 1);
#else
    const bool isRemovableLink = entry.isSymLink();
#endif
    if (!isRemovableLink) {
        return ST::tr("Cannot remove %1: it is a real model file, not a link "
                      "that Verzeta Studio created. Delete it from the models folder yourself.")
            .arg(target);
    }

    if (!QFile::remove(target)) {
        return ST::tr("Could not remove the model link: %1").arg(target);
    }

    qCDebug(verzetaDb) << logTag << ": removed symlink" << target;
    return QString{};
}

}  // namespace

QString SettingsService::ragpAddModelFromPath(const QString& sourceAbsolutePath) {
    const QString err = addGgufToModelsDir(
        sourceAbsolutePath, ragpModelsDir(), "SettingsService::ragpAddModelFromPath");
    if (!err.isEmpty()) {
        return err;
    }
    // Refresh the cached scan list + notify observers (also covers
    // the idempotent cases so the dropdown reflects a just-dropped
    // file).
    ragpAvailableModels();
    emit ragpAvailableModelsChanged();
    return QString{};
}

QString SettingsService::ragpRemoveModel(const QString& filename) {
    const QString err =
        removeGgufFromModelsDir(filename, ragpModelsDir(), "SettingsService::ragpRemoveModel");
    if (!err.isEmpty()) {
        return err;
    }

    // If the removed entry was the user's current default, clear
    // the default so the next configureRagpBackend falls back to
    // remote rather than trying to resolve a now-stale filename.
    if (ragpDefaultModelFilename() == filename) {
        setRagpDefaultModelFilename(QString{});
    }

    ragpAvailableModels();
    emit ragpAvailableModelsChanged();
    return QString{};
}

QString SettingsService::embeddingAddModelFromPath(const QString& sourceAbsolutePath) {
    const QString err = addGgufToModelsDir(
        sourceAbsolutePath, embeddingModelsDir(), "SettingsService::embeddingAddModelFromPath");
    if (!err.isEmpty()) {
        return err;
    }
    // embeddingAvailableModels() re-scans and emits
    // embeddingAvailableModelsChanged() itself when the set differs.
    embeddingAvailableModels();
    return QString{};
}

QString SettingsService::embeddingRemoveModel(const QString& filename) {
    const QString err = removeGgufFromModelsDir(
        filename, embeddingModelsDir(), "SettingsService::embeddingRemoveModel");
    if (!err.isEmpty()) {
        return err;
    }

    // Clear a now-stale selection so the embed worker degrades to the
    // configured remote/FTS chain instead of resolving a dead name.
    if (embeddingLocalModelFilename() == filename) {
        setEmbeddingLocalModelFilename(QString{});
    }

    embeddingAvailableModels();
    return QString{};
}


/**
 * @brief Returns the absolute path of the SQLite database file.
 * @return Absolute path string derived from QStandardPaths::AppDataLocation.
 */
QString SettingsService::databasePath() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/verzeta-studio.db");
}

/*
 * @brief Asynchronously tests connectivity to an Ollama server at the given URL.
 *
 * Issues a GET to {url}/api/tags. The response triggers ollamaConnectionResult()
 * on completion (success or failure). Safe to call multiple times; each call
 * creates a new reply and handles it independently.
 *
 * @param url Base URL of the Ollama server (e.g. "http://localhost:11434").
 * @sideeffects Issues an HTTP GET request via QNetworkAccessManager.
 */
void SettingsService::testOllamaConnection(const QString& url) {
    if (url.trimmed().isEmpty()) {
        emit ollamaConnectionResult(false, QStringLiteral("URL is empty"));
        return;
    }

    QString testUrl = url.trimmed();
    if (!testUrl.endsWith(QStringLiteral("/"))) {
        testUrl += QStringLiteral("/");
    }
    testUrl += QStringLiteral("api/tags");

    QNetworkRequest req{QUrl(testUrl)};
    req.setTransferTimeout(5000);  // 5 second timeout

    QNetworkReply* reply = m_networkMgr.get(req);
    reply->ignoreSslErrors();  // Accept self-signed certs for local providers
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit ollamaConnectionResult(false, reply->errorString());
            return;
        }

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (httpStatus == 200) {
            emit ollamaConnectionResult(true, QStringLiteral("Connected"));
        } else {
            emit ollamaConnectionResult(false, QStringLiteral("HTTP %1").arg(httpStatus));
        }
    });
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Reads a setting value from the SQLite settings table.
 * @param key Settings key.
 * @param defaultValue Default value if key is not found.
 * @return String value or defaultValue.
 */
QString SettingsService::getSetting(const QString& key, const QString& defaultValue) const {
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(key);

    if (!q.exec() || !q.next()) {
        return defaultValue;
    }
    return q.value(0).toString();
}

/**
 * @brief Returns true iff a row for @p key exists in the settings table.
 *        Distinguishes user-set values from hardcoded-default fallbacks.
 */
bool SettingsService::hasSetting(const QString& key) const {
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT 1 FROM settings WHERE key = ? LIMIT 1"));
    q.addBindValue(key);
    return q.exec() && q.next();
}

/**
 * @brief Returns true iff the user has fully configured @p providerId.
 *        Definitions live in the header doc-comment.
 */
bool SettingsService::isProviderConfigured(const QString& providerId) const {
    if (providerId == QStringLiteral("openai") || providerId == QStringLiteral("anthropic") ||
        providerId == QStringLiteral("gemini") || providerId == QStringLiteral("openrouter") ||
        providerId == QStringLiteral("deepseek")) {
        return hasApiKey(providerId);
    }
    if (providerId == QStringLiteral("ollama")) {
        // ollamaBaseUrl() has a hardcoded fallback default — we want
        // "user explicitly saved this row" semantics so an out-of-the-
        // box install doesn't surface Ollama before the user runs
        // the first-run wizard or visits Text Providers.
        return hasSetting(QStringLiteral("ollama_base_url"));
    }
    if (providerId == QStringLiteral("llamacpp_local")) {
        return !llamaCppModelPath().isEmpty();
    }
    if (providerId == QStringLiteral("llamacpp_remote")) {
        return !providerBaseUrl(providerId).isEmpty();
    }
    // Custom OpenAI-API-compatible servers — every "custom.<slug>"
    // id registered with ModelRouter came through CustomServerRegistry,
    // which only registers fully-configured rows (validates the URL,
    // applies every flag, optionally persists the key).  We trust the
    // registration itself; the AgentSettings dropdown lists every
    // configured custom server unconditionally.
    if (providerId.startsWith(QStringLiteral("custom."))) {
        return true;
    }
    return false;
}

/**
 * @brief Writes or updates a setting in the SQLite settings table.
 * @param key Settings key.
 * @param value Settings value.
 * @sideeffects UPSERT into settings table. Emits settingsChanged(key).
 */
void SettingsService::setSetting(const QString& key, const QString& value) {
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO settings (key, value, updated_at) "
                             "VALUES (?, ?, strftime('%s','now') * 1000) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value, "
                             "updated_at = excluded.updated_at"));
    q.addBindValue(key);
    q.addBindValue(value);

    if (!q.exec()) {
        qCWarning(verzetaDb) << "SettingsService: failed to write key" << key << ":"
                             << q.lastError().text();
        return;
    }

    emit settingsChanged(key);
}

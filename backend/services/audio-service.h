// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file audio-service.h
 * @brief Orchestrates text-to-speech audio generation, storage,
 *        and per-conversation audio catalog management.
 * @layer Service
 * @dependencies AudioGenWorker (Worker), FileService (Service)
 */


#pragma once

#include "../models/job-context.h"

#include <QThread>

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

class FileService;
class AudioGenWorker;

/**
 * @brief Configuration for audio generation requests.
 */
struct AudioGenConfig {
    QString backend = QStringLiteral("openai");  ///< "openai" | "local_tts"
    QString voice = QStringLiteral("alloy");     ///< OpenAI voice ID
    QString model = QStringLiteral("tts-1");     ///< "tts-1" | "tts-1-hd"
    QString ttsPath;  ///< Absolute path to local TTS binary (local_tts only)
    QString outputFormat = QStringLiteral("mp3");  ///< "mp3" | "wav"
    QString apiKey;                                ///< OpenAI API key (passed from SettingsService)
};

/**
 * @brief Service for text-to-speech generation and audio catalog management.
 *
 * Owns one `AudioGenWorker` on a private `QThread`. The worker is started in the
 * constructor and stopped in the destructor. Callers invoke `generateAudio()` from
 * the main thread; the result arrives via `audioGenerated()` signal.
 *
 * ## QML Integration
 *
 * Exposed as `AudioService` context property. QML can call `generateAudio()` and
 * `conversationAudio()`.
 */
class AudioService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs AudioService and starts the worker thread.
     * @param fileService FileService (currently unused; reserved for future path ops).
     * @param parent      Optional Qt parent.
     */
    explicit AudioService(FileService& fileService, QObject* parent = nullptr);

    /**
     * @brief Stops the worker thread and destroys the service.
     */
    ~AudioService() override;

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    /**
     * @brief Generates speech audio from the given text.
     * @param convId Conversation UUID (used to organise the audio catalog).
     * @param text   Text to synthesize (UTF-8).
     * @param config Audio generation configuration.
     * @sideeffects Dispatches generation to background thread.
     *              Emits generationStarted(convId), then audioGenerated() or error().
     *
     * Each call builds its own immutable JobContext, which the worker echoes
     * back on every result. Results are attributed from that context, so
     * overlapping calls always resolve to the conversation that started them.
     */
    Q_INVOKABLE void
    generateAudio(const QString& convId, const QString& text, const AudioGenConfig& config);

    /**
     * @brief Returns all audio file paths for a conversation.
     * @param convId Conversation UUID.
     * @return List of absolute audio file paths, in generation order.
     */
    Q_INVOKABLE QStringList conversationAudio(const QString& convId) const;

  signals:
    /** @brief Emitted when audio generation begins. */
    void generationStarted(const QString& convId);

    /** @brief Emitted when the audio file is ready. */
    void audioGenerated(const QString& convId, const QString& audioPath);

    /**
     * @brief Emitted on generation error.
     * @param convId  Conversation UUID whose generation failed.
     * @param message Human-readable error description.
     */
    void error(const QString& convId, const QString& message);

    // Internal cross-thread invocation signals — every signal threads
    // a JobContext that the worker echoes back unchanged on result
    // emit. The service slot reads attribution from the echoed
    // context, never from a mutable m_active* field. Same L3 contract
    // as ImageService.
    /**
     * @brief Worker invocation: request a remote OpenAI TTS render.
     * @param ctx    Job context echoed back unchanged.
     * @param text   Text to synthesize.
     * @param voice  OpenAI voice id.
     * @param model  OpenAI model id.
     * @param apiKey User-provided OpenAI API key.
     */
    void requestOpenAI(const JobContext& ctx,
                       const QString& text,
                       const QString& voice,
                       const QString& model,
                       const QString& apiKey);

    /**
     * @brief Worker invocation: request a local TTS render.
     * @param ctx          Job context echoed back unchanged.
     * @param text         Text to synthesize.
     * @param ttsPath      Local TTS binary path.
     * @param outputFormat Output container format.
     */
    void requestLocalTTS(const JobContext& ctx,
                         const QString& text,
                         const QString& ttsPath,
                         const QString& outputFormat);

  private slots:
    /**
     * @brief Handle a worker file-ready signal.
     * @param ctx       Echoed job context with the originating convId.
     * @param localPath Absolute path to the produced audio file.
     */
    void onAudioFileReady(const JobContext& ctx, const QString& localPath);

    /**
     * @brief Handle a worker error.
     * @param ctx      Echoed job context with the originating convId.
     * @param errorMsg Human-readable error description.
     */
    void onWorkerError(const JobContext& ctx, const QString& errorMsg);

  private:
    FileService& m_fileService;
    AudioGenWorker* m_worker = nullptr;
    QThread* m_workerThread = nullptr;


    // Per-conversation audio catalogs: convId → list of absolute paths.
    // Written from onAudioFileReady's ctx.conversationId, NOT from a
    // mutable active-conv field.
    QMap<QString, QStringList> m_conversationAudio;
};

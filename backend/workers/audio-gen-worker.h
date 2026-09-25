// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file audio-gen-worker.h
 * @brief Background worker for text-to-speech audio generation
 *        via OpenAI TTS API or a local TTS engine (piper, espeak-ng).
 * @layer Worker
 * @dependencies Qt6::Network (QNetworkAccessManager), Qt6::Core (QProcess),
 *               JobContext
 */


#pragma once

#include "../models/job-context.h"

#include <QByteArray>
#include <QObject>
#include <QString>

/**
 * @brief Generates TTS audio via OpenAI TTS API or a local TTS engine.
 *
 * Designed to run on a dedicated QThread. All public slots should be invoked
 * via Qt::QueuedConnection from the owning service.
 */
class AudioGenWorker : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the worker.
     * @param parent Optional Qt parent.
     */
    explicit AudioGenWorker(QObject* parent = nullptr);

    /**
     * @brief Destroys the worker.
     */
    ~AudioGenWorker() override = default;

  public slots:
    /**
     * @brief Generates speech audio via OpenAI TTS API.
     *
     * POST https://api.openai.com/v1/audio/speech with model/voice/format.
     * The binary response is written to disk as an MP3 file and then
     * emitted via audioFileReady().
     *
     * @param ctx     Per-job attribution context.  Echoed unchanged on
     *                every result signal so the owning service can
     *                route the response back to the originating
     *                conversation / message without sharing mutable
     *                state between concurrent jobs.
     * @param text    Text to synthesize (max 4096 characters per OpenAI limit).
     * @param voice   Voice ID: "alloy", "echo", "fable", "onyx", "nova", "shimmer".
     * @param model   TTS model: "tts-1" or "tts-1-hd".
     * @param apiKey  OpenAI API key (Bearer token).
     * @sideeffects HTTP POST to api.openai.com. Saves MP3 to AppDataLocation/attachments/.
     *              Emits audioFileReady() on success, errorOccurred() on failure.
     */
    void generateViaOpenAI(const JobContext& ctx,
                           const QString& text,
                           const QString& voice,
                           const QString& model,
                           const QString& apiKey);

    /**
     * @brief Generates speech audio via a local TTS engine (piper, espeak-ng).
     *
     * Pipes text to the TTS executable:
     *   echo "{text}" | {ttsPath} --output_file {outputPath}
     *
     * @param ctx           Per-job attribution context.  Echoed
     *                      unchanged on every result signal.
     * @param text          Text to synthesize.
     * @param ttsPath       Absolute path to the TTS executable.
     * @param outputFormat  "wav" or "mp3" (determines file extension).
     * @sideeffects Spawns QProcess. Emits audioFileReady(path) on success.
     */
    void generateViaLocalTTS(const JobContext& ctx,
                             const QString& text,
                             const QString& ttsPath,
                             const QString& outputFormat = QStringLiteral("wav"));

  signals:
    /**
     * @brief Streaming audio data chunk (unused by file-based flow; reserved
     *        for future streaming playback integration).
     * @param ctx       Job context echoed unchanged from the inbound slot.
     * @param audioData Raw audio bytes.
     */
    void audioChunkReady(const JobContext& ctx, const QByteArray& audioData);

    /**
     * @brief Emitted when the complete audio file is ready on disk.
     * @param ctx       Job context echoed unchanged from the inbound slot.
     * @param localPath Absolute path to the saved audio file.
     */
    void audioFileReady(const JobContext& ctx, const QString& localPath);

    /**
     * @brief Emitted when an error occurs.
     * @param ctx   Job context echoed unchanged from the inbound slot.
     * @param error Human-readable error message.
     */
    void errorOccurred(const JobContext& ctx, const QString& error);
};

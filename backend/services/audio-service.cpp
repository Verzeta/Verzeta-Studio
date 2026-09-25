// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file audio-service.cpp
 * @brief Implementation of TTS orchestration and audio catalog management.
 * @layer Service
 * @dependencies AudioGenWorker, FileService, Qt6::Core, JobContext
 */


#include "audio-service.h"

#include "../services/file-service.h"
#include "../utils/logger.h"
#include "../workers/audio-gen-worker.h"

#include <QThread>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs AudioService and starts the background worker thread.
 * @param fileService FileService reference (reserved for future I/O helpers).
 * @param parent      Optional Qt parent.
 */
AudioService::AudioService(FileService& fileService, QObject* parent)
    : QObject(parent)
    , m_fileService(fileService)
    , m_worker(new AudioGenWorker)
    , m_workerThread(new QThread(this)) {
    m_worker->moveToThread(m_workerThread);

    connect(m_worker,
            &AudioGenWorker::audioFileReady,
            this,
            &AudioService::onAudioFileReady,
            Qt::QueuedConnection);

    connect(m_worker,
            &AudioGenWorker::errorOccurred,
            this,
            &AudioService::onWorkerError,
            Qt::QueuedConnection);

    // Cross-thread invocation signals — first arg is JobContext.
    connect(this,
            &AudioService::requestOpenAI,
            m_worker,
            &AudioGenWorker::generateViaOpenAI,
            Qt::QueuedConnection);

    connect(this,
            &AudioService::requestLocalTTS,
            m_worker,
            &AudioGenWorker::generateViaLocalTTS,
            Qt::QueuedConnection);

    m_workerThread->start();
    qCInfo(verzetaUi) << "AudioService initialised";
}

/**
 * @brief Stops worker thread and frees worker object.
 */
AudioService::~AudioService() {
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
    }
    delete m_worker;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/*
 * @brief Starts audio generation for the given conversation and text.
 * @param convId Conversation UUID.
 * @param text   Text to synthesize.
 * @param config AudioGenConfig with backend, voice, model, API key.
 *
 * Builds a JobContext (immutable, per-job)
 * and threads it through the dispatch signal to the worker. The worker
 * echoes the same JobContext back on every result emit. The completion
 * slot reads attribution from ctx.conversationId, never from
 * m_activeConvId (which no longer exists). Two overlapping calls each
 * carry their own context and resolve to the correct conversation
 * regardless of timing.
 */
void AudioService::generateAudio(const QString& convId,
                                 const QString& text,
                                 const AudioGenConfig& config) {
    // Stamp the per-job attribution context at enqueue. The prompt
    // field carries `text` so the audio catalog / future audit log
    // can correlate completions back to the request.
    const JobContext ctx = JobContext::makeNew(convId, text);
    emit generationStarted(convId);

    if (config.backend == QStringLiteral("local_tts")) {
        if (config.ttsPath.isEmpty()) {
            emit error(convId, QStringLiteral("Local TTS path not configured"));
            return;
        }
        emit requestLocalTTS(ctx, text, config.ttsPath, config.outputFormat);
    } else {
        // Default: OpenAI TTS
        if (config.apiKey.isEmpty()) {
            emit error(convId, QStringLiteral("OpenAI API key not configured for TTS"));
            return;
        }
        emit requestOpenAI(ctx, text, config.voice, config.model, config.apiKey);
    }
}

/*
 * @brief Returns all audio file paths for the given conversation.
 * @param convId Conversation UUID.
 * @return List of absolute file paths in generation order.
 */
QStringList AudioService::conversationAudio(const QString& convId) const {
    return m_conversationAudio.value(convId);
}


/**
 * @brief Called when worker signals audioFileReady.
 * @param ctx       Job context echoed unchanged from the inbound dispatch.
 * @param localPath Absolute path to the saved audio file.
 */
void AudioService::onAudioFileReady(const JobContext& ctx, const QString& localPath) {
    if (!ctx.isValid()) {
        qCWarning(verzetaUi) << "AudioService::onAudioFileReady invoked with invalid ctx"
                             << "— dropping audio completion at" << localPath;
        return;
    }
    m_conversationAudio[ctx.conversationId].append(localPath);
    emit audioGenerated(ctx.conversationId, localPath);
}

/**
 * @brief Called when worker signals errorOccurred.
 * @param ctx      Job context echoed unchanged from the inbound dispatch.
 * @param errorMsg Error description.
 */
void AudioService::onWorkerError(const JobContext& ctx, const QString& errorMsg) {
    if (!ctx.isValid()) {
        qCWarning(verzetaUi) << "AudioService::onWorkerError invoked with invalid ctx"
                             << "— dropping error emit:" << errorMsg;
        return;
    }
    emit error(ctx.conversationId, errorMsg);
}

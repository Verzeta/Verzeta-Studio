// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file audio-gen-worker.cpp
 * @brief Implementation of TTS audio generation via OpenAI API and local engine.
 * @layer Worker
 * @dependencies QNetworkAccessManager, QProcess, Qt6::Core
 */


#include "audio-gen-worker.h"

#include <QDir>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/**
 * @brief Constructs AudioGenWorker.
 * @param parent Optional Qt parent.
 */
AudioGenWorker::AudioGenWorker(QObject* parent) : QObject(parent) {}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

/**
 * @brief Generates speech audio via OpenAI TTS API.
 *
 * POST /v1/audio/speech → receive binary MP3 → save to disk → emit audioFileReady.
 *
 * @param text   Text to synthesize.
 * @param voice  Voice identifier.
 * @param model  TTS model name.
 * @param apiKey OpenAI API key.
 */
void AudioGenWorker::generateViaOpenAI(const JobContext& ctx,
                                       const QString& text,
                                       const QString& voice,
                                       const QString& model,
                                       const QString& apiKey) {
    QNetworkAccessManager nam;

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.isEmpty() ? QStringLiteral("tts-1") : model;
    payload[QStringLiteral("input")] = text;
    payload[QStringLiteral("voice")] = voice.isEmpty() ? QStringLiteral("alloy") : voice;
    payload[QStringLiteral("response_format")] = QStringLiteral("mp3");

    QNetworkRequest req(QUrl(QStringLiteral("https://api.openai.com/v1/audio/speech")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());

    QEventLoop loop;
    QNetworkReply* reply = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));

    QByteArray accumulated;
    connect(reply, &QNetworkReply::readyRead, this, [reply, &accumulated, ctx, this]() {
        const QByteArray chunk = reply->readAll();
        accumulated += chunk;
        emit audioChunkReady(ctx, chunk);
    });

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = reply->errorString();
        reply->deleteLater();
        emit errorOccurred(ctx, msg);
        return;
    }
    reply->deleteLater();

    if (accumulated.isEmpty()) {
        emit errorOccurred(ctx, QStringLiteral("OpenAI TTS: received empty audio response"));
        return;
    }

    // Save to AppDataLocation/attachments/{uuid}.mp3
    const QString attachDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/attachments");
    QDir().mkpath(attachDir);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString savePath = attachDir + QStringLiteral("/") + uuid + QStringLiteral(".mp3");

    QFile f(savePath);
    if (!f.open(QIODevice::WriteOnly)) {
        emit errorOccurred(ctx, QStringLiteral("Cannot write audio file: ") + savePath);
        return;
    }
    f.write(accumulated);
    f.close();

    emit audioFileReady(ctx, savePath);
}

/**
 * @brief Generates speech audio via a local TTS engine (piper, espeak-ng).
 *
 * Uses a shell pipe: echo "text" | {ttsPath} --output_file {outputPath}
 *
 * @param text          Text to synthesize.
 * @param ttsPath       Absolute path to TTS executable.
 * @param outputFormat  "wav" or "mp3" (file extension only; format is engine-determined).
 */
void AudioGenWorker::generateViaLocalTTS(const JobContext& ctx,
                                         const QString& text,
                                         const QString& ttsPath,
                                         const QString& outputFormat) {
    const QString attachDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/attachments");
    QDir().mkpath(attachDir);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString ext =
        (outputFormat == QStringLiteral("mp3")) ? QStringLiteral(".mp3") : QStringLiteral(".wav");
    const QString outputPath = attachDir + QStringLiteral("/") + uuid + ext;

    // Build: echo "text" | ttsPath --output_file outputPath
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);

    // Write text to proc stdin, use --output_file argument
    const QStringList args{QStringLiteral("--output_file"), outputPath};
    proc.start(ttsPath, args);

    if (!proc.waitForStarted(5000)) {
        emit errorOccurred(ctx, QStringLiteral("Local TTS: failed to start: ") + ttsPath);
        return;
    }

    // Write text to stdin and close it
    proc.write(text.toUtf8());
    proc.closeWriteChannel();

    proc.waitForFinished(-1);

    if (proc.exitCode() != 0) {
        const QString errOut = QString::fromUtf8(proc.readAll());
        emit errorOccurred(ctx,
                           QStringLiteral("Local TTS exited with code %1: %2")
                               .arg(proc.exitCode())
                               .arg(errOut.trimmed()));
        return;
    }

    if (!QFile::exists(outputPath)) {
        emit errorOccurred(ctx, QStringLiteral("Local TTS: output file not found: ") + outputPath);
        return;
    }

    emit audioFileReady(ctx, outputPath);
}

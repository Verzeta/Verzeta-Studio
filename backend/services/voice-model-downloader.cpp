// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-model-downloader.cpp
 * @brief Sequential, checksum-verified installs from the pinned voice
 *        model catalog. Streams to a .part file, hashes as bytes
 *        arrive, and renames into place only after size and SHA-256
 *        both match, so a broken or tampered download can never be
 *        picked up by the daemon.
 * @layer Service
 * @dependencies voice-model-catalog, Qt6::Network, Qt6::Core.
 */

#include "voice-model-downloader.h"

#include <QDir>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

Q_LOGGING_CATEGORY(verzetaVoiceDl, "verzeta.voice.download", QtInfoMsg)

namespace Verzeta::Voice {

VoiceModelDownloader::VoiceModelDownloader(QObject* parent)
    : QObject(parent), m_nam(std::make_unique<QNetworkAccessManager>()) {
    // The catalog URLs resolve through the LFS storage redirect.
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

VoiceModelDownloader::~VoiceModelDownloader() {
    releaseTransfer();
}

void VoiceModelDownloader::setTargetDir(const QString& dir) {
    if (dir == m_targetDir)
        return;
    m_targetDir = dir;
    emit targetDirChanged();
}

QString VoiceModelDownloader::targetPath(const QString& fileName) const {
    return QDir(m_targetDir).filePath(fileName);
}

QVariantList VoiceModelDownloader::catalog() const {
    QVariantList rows;
    const QList<VoiceModelEntry> entries = voiceModelCatalog();
    for (const VoiceModelEntry& e : entries) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), e.id);
        row.insert(QStringLiteral("kind"), e.kind);
        row.insert(QStringLiteral("displayName"), e.displayName);
        row.insert(QStringLiteral("license"), e.license);
        row.insert(QStringLiteral("sizeBytes"), static_cast<double>(e.totalBytes()));
        row.insert(QStringLiteral("installed"), isInstalled(e.id));
        rows.append(row);
    }
    return rows;
}

bool VoiceModelDownloader::isInstalled(const QString& id) const {
    if (m_targetDir.isEmpty())
        return false;
    const QList<VoiceModelEntry> entries = voiceModelCatalog();
    for (const VoiceModelEntry& e : entries) {
        if (e.id != id)
            continue;
        for (const VoiceModelFile& f : e.files) {
            const QFileInfo fi(targetPath(f.fileName));
            // Size equality is the cheap integrity check for files
            // already on disk; the full hash ran at install time.
            if (!fi.exists() || fi.size() != f.sizeBytes)
                return false;
        }
        return true;
    }
    return false;
}

void VoiceModelDownloader::download(const QString& id) {
    if (!m_activeId.isEmpty()) {
        emit finished(id, false, tr("Another download is already running."));
        return;
    }
    if (m_targetDir.isEmpty()) {
        emit finished(id,
                      false,
                      tr("Start the voice service first, so the models "
                         "folder is known."));
        return;
    }
    const QList<VoiceModelEntry> entries = voiceModelCatalog();
    for (const VoiceModelEntry& e : entries) {
        if (e.id != id)
            continue;
        if (!QDir().mkpath(m_targetDir)) {
            emit finished(id, false, tr("Cannot create %1.").arg(m_targetDir));
            return;
        }
        m_activeId = id;
        m_entry = e;
        m_fileIndex = 0;
        m_completedBytes = 0;
        m_receivedBytes = 0;
        m_totalBytes = e.totalBytes();
        emit progressChanged();
        startNextFile();
        return;
    }
    emit finished(id, false, tr("Unknown model id."));
}

void VoiceModelDownloader::startNextFile() {
    if (m_fileIndex >= m_entry.files.size()) {
        // Every file verified and renamed into place.
        const QString id = m_activeId;
        m_activeId.clear();
        m_totalBytes = 0;
        m_receivedBytes = 0;
        emit progressChanged();
        qCInfo(verzetaVoiceDl) << "installed" << id;
        emit finished(id, true, QString());
        return;
    }
    const VoiceModelFile& file = m_entry.files.at(m_fileIndex);
    m_partFile.setFileName(targetPath(file.fileName) + QLatin1String(".part"));
    if (!m_partFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot write to %1.").arg(m_targetDir));
        return;
    }
    m_hash.reset();

    QNetworkRequest request{QUrl(file.url)};
    m_reply = m_nam->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        const QByteArray chunk = m_reply->readAll();
        m_hash.addData(chunk);
        if (m_partFile.write(chunk) != chunk.size()) {
            fail(tr("Write failed (disk full?)."));
            return;
        }
        m_receivedBytes = m_completedBytes + m_partFile.size();
        emit progressChanged();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // Cancel arrives here as OperationCanceledError; fail()
            // already ran from cancel(), so only real errors report.
            if (reply->error() != QNetworkReply::OperationCanceledError) {
                fail(reply->errorString());
            }
            return;
        }
        const VoiceModelFile& file = m_entry.files.at(m_fileIndex);
        m_partFile.close();
        const QString actualSha = QString::fromLatin1(m_hash.result().toHex());
        if (m_partFile.size() != file.sizeBytes || actualSha != file.sha256) {
            fail(tr("%1 failed verification and was discarded.").arg(file.fileName));
            return;
        }
        // Verified: move into place. Replace any older copy so a
        // re-download repairs a damaged install.
        const QString finalPath = targetPath(file.fileName);
        QFile::remove(finalPath);
        if (!m_partFile.rename(finalPath)) {
            fail(tr("Cannot move %1 into place.").arg(file.fileName));
            return;
        }
        m_completedBytes += file.sizeBytes;
        m_receivedBytes = m_completedBytes;
        ++m_fileIndex;
        emit progressChanged();
        startNextFile();
    });
}

void VoiceModelDownloader::cancel() {
    if (m_activeId.isEmpty())
        return;
    fail(tr("Cancelled."));
}

void VoiceModelDownloader::fail(const QString& error) {
    const QString id = m_activeId;
    releaseTransfer();
    m_activeId.clear();
    m_totalBytes = 0;
    m_receivedBytes = 0;
    emit progressChanged();
    qCWarning(verzetaVoiceDl) << id << "failed:" << error;
    emit finished(id, false, error);
}

void VoiceModelDownloader::releaseTransfer() {
    if (m_reply) {
        // Detach first: abort() emits finished() synchronously and the
        // handler must not run its error path a second time.
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (m_partFile.isOpen())
        m_partFile.close();
    if (!m_partFile.fileName().isEmpty()) {
        QFile::remove(m_partFile.fileName());  // Never leave a .part.
        m_partFile.setFileName(QString());
    }
}

bool VoiceModelDownloader::verifyFileSha256(const QString& filePath, const QString& sha256Hex) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f))
        return false;
    return QString::fromLatin1(hash.result().toHex()) == sha256Hex.toLower();
}

}  // namespace Verzeta::Voice

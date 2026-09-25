// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file model-download-service.cpp
 * @brief User-initiated recommended-model downloads with pinned URL +
 *        SHA256 stream verification. Write-to-.part + verify + rename
 *        so a truncated or corrupted transfer can never masquerade as
 *        a usable model file.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Network.
 */

#include "model-download-service.h"

#include "../utils/logger.h"

#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {

// ---------------------------------------------------------------------------
// Pinned recommendations. SHA256 + byte sizes were read from the
// hosting service's file metadata for exactly these artifacts; the
// verifier rejects anything else. Updating a recommendation = update
// pin + hash + size TOGETHER.
//
// RAGP: an instruct model in the Phi-4-mini class — small models below
// this class repeatedly failed structured classify output in live use,
// so the recommendation is the smallest model that held up.
// Embeddings: nomic-embed-text-v1.5 (Q8_0) — compact, strong retrieval
// quality, the project's standing recommendation.
// ---------------------------------------------------------------------------
const QString kRagpUrl =
    QStringLiteral("https://huggingface.co/unsloth/Phi-4-mini-instruct-GGUF/resolve/"
                   "main/Phi-4-mini-instruct-Q4_K_M.gguf");
const QString kRagpSha =
    QStringLiteral("88c00229914083cd112853aab84ed51b87bdf6b9ce42f532d8c85c7c63b1730a");
constexpr qint64 kRagpBytes = 2491874272LL;
const QString kRagpFile = QStringLiteral("Phi-4-mini-instruct-Q4_K_M.gguf");

const QString kEmbedUrl =
    QStringLiteral("https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/"
                   "main/nomic-embed-text-v1.5.Q8_0.gguf");
const QString kEmbedSha =
    QStringLiteral("3e24342164b3d94991ba9692fdc0dd08e3fd7362e0aacc396a9a5c54a544c3b7");
constexpr qint64 kEmbedBytes = 146146432LL;
const QString kEmbedFile = QStringLiteral("nomic-embed-text-v1.5.Q8_0.gguf");

}  // namespace

ModelDownloadService::ModelDownloadService(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

ModelDownloadService::~ModelDownloadService() {
    // Abort quietly; the .part cleanup below keeps the models dir free
    // of stale partials after an exit mid-download.
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_partFile.isOpen()) {
        const QString part = m_partFile.fileName();
        m_partFile.close();
        QFile::remove(part);
    }
}

void ModelDownloadService::setTargetDirProvider(std::function<QString(const QString&)> provider) {
    m_targetDirProvider = std::move(provider);
}

ModelDownloadService::Pin ModelDownloadService::pinFor(const QString& kind) const {
    if (kind == QLatin1String("ragp")) {
        if (!m_ragpOverride.url.isEmpty())
            return m_ragpOverride;
        return Pin{kRagpUrl, kRagpSha, kRagpBytes, kRagpFile};
    }
    if (kind == QLatin1String("embed")) {
        if (!m_embedOverride.url.isEmpty())
            return m_embedOverride;
        return Pin{kEmbedUrl, kEmbedSha, kEmbedBytes, kEmbedFile};
    }
    return {};
}

QString ModelDownloadService::targetDirFor(const QString& kind) const {
    return m_targetDirProvider ? m_targetDirProvider(kind) : QString();
}

QString ModelDownloadService::recommendedFilename(const QString& kind) const {
    return pinFor(kind).filename;
}

qint64 ModelDownloadService::recommendedBytes(const QString& kind) const {
    return pinFor(kind).bytes;
}

bool ModelDownloadService::recommendedPresent(const QString& kind) const {
    const Pin pin = pinFor(kind);
    const QString dir = targetDirFor(kind);
    if (pin.filename.isEmpty() || dir.isEmpty())
        return false;
    return QFile::exists(dir + QLatin1Char('/') + pin.filename);
}

bool ModelDownloadService::start(const QString& kind) {
    if (m_reply)
        return false;  // one at a time
    const Pin pin = pinFor(kind);
    if (pin.url.isEmpty())
        return false;
    const QString dirPath = targetDirFor(kind);
    if (dirPath.isEmpty())
        return false;
    QDir dir(dirPath);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        m_lastError = QStringLiteral("cannot create models folder: %1").arg(dirPath);
        emit stateChanged();
        return false;
    }
    if (QFile::exists(dirPath + QLatin1Char('/') + pin.filename)) {
        return false;  // already present — UI shows the present state
    }

    const QString partPath = dirPath + QLatin1Char('/') + pin.filename + QStringLiteral(".part");
    m_partFile.setFileName(partPath);
    if (!m_partFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = QStringLiteral("cannot write to models folder: %1").arg(partPath);
        emit stateChanged();
        return false;
    }

    m_activeKind = kind;
    m_activePin = pin;
    m_received = 0;
    m_lastError.clear();
    m_hash.reset();

    QNetworkRequest req{QUrl(pin.url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);

    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        const QByteArray chunk = m_reply->readAll();
        if (chunk.isEmpty())
            return;
        m_hash.addData(chunk);
        m_partFile.write(chunk);
        m_received += chunk.size();
        emit progressChanged();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();

        const QString kind = m_activeKind;
        const Pin pin = m_activePin;
        const QString partPath = m_partFile.fileName();
        m_partFile.close();

        if (reply->error() == QNetworkReply::OperationCanceledError) {
            QFile::remove(partPath);
            m_activeKind.clear();
            emit stateChanged();
            return;  // user cancel — not an error
        }
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(partPath);
            finishFailure(reply->errorString());
            return;
        }
        if (m_received != pin.bytes) {
            QFile::remove(partPath);
            finishFailure(QStringLiteral("The download was discarded because of a size mismatch "
                                         "(%1 bytes instead of %2).")
                              .arg(m_received)
                              .arg(pin.bytes));
            return;
        }
        const QString gotSha = QString::fromLatin1(m_hash.result().toHex());
        if (gotSha != pin.sha256) {
            QFile::remove(partPath);
            finishFailure(QStringLiteral("The download does not match the expected model file "
                                         "(checksum mismatch) and was discarded."));
            return;
        }

        const QString finalPath = partPath.left(partPath.size() - 5);  // strip ".part"
        if (!QFile::rename(partPath, finalPath)) {
            QFile::remove(partPath);
            finishFailure(QStringLiteral("The file was verified but could not be moved into "
                                         "the models folder: %1")
                              .arg(finalPath));
            return;
        }

        qCInfo(verzetaLlm).noquote() << "ModelDownloadService: downloaded + verified"
                                     << pin.filename << "(" << pin.bytes << "bytes ) for" << kind;
        m_activeKind.clear();
        emit stateChanged();
        emit downloadFinished(kind, pin.filename);
    });

    emit stateChanged();
    emit progressChanged();
    return true;
}

void ModelDownloadService::cancel() {
    if (m_reply)
        m_reply->abort();  // finished handler cleans up
}

void ModelDownloadService::finishFailure(const QString& error) {
    qCWarning(verzetaLlm).noquote()
        << "ModelDownloadService: download failed for" << m_activeKind << "—" << error;
    const QString kind = m_activeKind;
    m_activeKind.clear();
    m_lastError = error;
    emit stateChanged();
    emit downloadFailed(kind, error);
}

bool ModelDownloadService::active() const {
    return m_reply != nullptr;
}
QString ModelDownloadService::activeKind() const {
    return m_activeKind;
}

double ModelDownloadService::progress() const {
    if (!m_reply || m_activePin.bytes <= 0)
        return 0.0;
    return static_cast<double>(m_received) / static_cast<double>(m_activePin.bytes);
}

qint64 ModelDownloadService::receivedBytes() const {
    return m_received;
}

qint64 ModelDownloadService::totalBytes() const {
    return m_reply ? m_activePin.bytes : 0;
}

QString ModelDownloadService::lastError() const {
    return m_lastError;
}

void ModelDownloadService::setPinOverrideForTest(const QString& kind,
                                                 const QString& url,
                                                 const QString& sha256,
                                                 qint64 bytes,
                                                 const QString& filename) {
    const Pin pin{url, sha256, bytes, filename};
    if (kind == QLatin1String("ragp"))
        m_ragpOverride = pin;
    if (kind == QLatin1String("embed"))
        m_embedOverride = pin;
}

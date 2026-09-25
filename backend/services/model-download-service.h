// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file model-download-service.h
 * @brief Explicit, user-initiated download of the RECOMMENDED
 *        local GGUF models (one RAGP instruct model, one embedding
 *        model) with pinned URL + SHA256 verification. Nothing ever
 *        downloads automatically and no model is bundled in any
 *        artifact (the build ships engines, never weights). This
 *        service exists so the First Run Wizard and the provider
 *        settings pages can offer a one-click "Download recommended
 *        model" that lands the file in the same models folder the
 *        Browse pickers use.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Network (QNetworkAccessManager).
 */

#pragma once

#include <functional>
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * @brief One-at-a-time downloader for the recommended local models.
 *
 * Registered as the `ModelDownloads` QML singleton. Kinds:
 *   - "ragp":  instruct GGUF for routing/classify (and local chat).
 *   - "embed": embedding GGUF for RAG retrieval.
 *
 * Each kind carries a compile-time pin (URL + SHA256 + byte size)
 * verified while streaming: the file is written to `\<name\>.part`, the
 * hash is fed incrementally, and only a byte-exact, hash-exact
 * download is renamed into place. Any mismatch deletes the partial
 * file and reports an error, so a truncated or tampered download can
 * never be mistaken for a model.
 *
 * Threading: main-thread only (Qt network I/O is async; no blocking).
 */
class ModelDownloadService : public QObject {
    Q_OBJECT

    /** @brief True while a download is running (one at a time). */
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    /** @brief Kind of the running download ("ragp"/"embed"), else empty. */
    Q_PROPERTY(QString activeKind READ activeKind NOTIFY stateChanged)
    /** @brief 0.0-1.0 progress of the running download. */
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    /** @brief Bytes received so far for the running download. */
    Q_PROPERTY(qint64 receivedBytes READ receivedBytes NOTIFY progressChanged)
    /** @brief Expected total bytes of the running download's pin. */
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY progressChanged)
    /** @brief Last failure reason (cleared when a download starts). */
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)

  public:
    /**
     * @brief Constructs the service (no network activity yet).
     * @param parent Qt parent (AppController).
     */
    explicit ModelDownloadService(QObject* parent = nullptr);
    ~ModelDownloadService() override;

    /**
     * @brief Installs the resolver mapping a download kind to its
     *        destination directory (the same folders the Browse
     *        pickers symlink into: SettingsService::ragpModelsDir /
     *        embeddingModelsDir). Wired by AppController; injectable
     *        so tests can point at a temp dir.
     * @param provider Callable: kind ("ragp"/"embed") → absolute dir.
     */
    void setTargetDirProvider(std::function<QString(const QString&)> provider);

    /**
     * @brief Display name of a kind's recommended model file.
     * @param kind "ragp" or "embed".
     * @returns The pinned filename (e.g. the .gguf basename), or empty
     *          for an unknown kind.
     */
    Q_INVOKABLE QString recommendedFilename(const QString& kind) const;

    /**
     * @brief Pinned payload size of a kind's recommended model, for
     *        showing "2.3 GB" next to the download button.
     * @param kind "ragp" or "embed".
     * @returns Size in bytes, or 0 for an unknown kind.
     */
    Q_INVOKABLE qint64 recommendedBytes(const QString& kind) const;

    /**
     * @brief Whether the recommended model for @p kind already exists
     *        in its target directory (download button flips to a
     *        "present" state).
     * @param kind "ragp" or "embed".
     * @returns True when the pinned filename is already on disk.
     */
    Q_INVOKABLE bool recommendedPresent(const QString& kind) const;

    /**
     * @brief Starts downloading the recommended model for @p kind.
     *        EXPLICIT USER ACTION ONLY; nothing in the app calls this
     *        without a button press. One download at a time.
     * @param kind "ragp" or "embed".
     * @returns True when the download started; false when one is
     *          already running, the kind is unknown, the target dir
     *          cannot be created, or the file is already present.
     */
    Q_INVOKABLE bool start(const QString& kind);

    /**
     * @brief Cancels the running download and deletes its partial
     *        file. Safe to call when idle.
     */
    Q_INVOKABLE void cancel();

    /**
     * @brief Reader for the active Q_PROPERTY.
     * @returns True while a download is running.
     */
    bool active() const;

    /**
     * @brief Reader for the activeKind Q_PROPERTY.
     * @returns "ragp"/"embed" while downloading, else empty.
     */
    QString activeKind() const;

    /**
     * @brief Reader for the progress Q_PROPERTY.
     * @returns 0.0-1.0 fraction of the pinned byte count received.
     */
    double progress() const;

    /**
     * @brief Reader for the receivedBytes Q_PROPERTY.
     * @returns Bytes received so far for the running download.
     */
    qint64 receivedBytes() const;

    /**
     * @brief Reader for the totalBytes Q_PROPERTY.
     * @returns The running download's pinned byte count, or 0 idle.
     */
    qint64 totalBytes() const;

    /**
     * @brief Reader for the lastError Q_PROPERTY.
     * @returns Last failure reason, or empty.
     */
    QString lastError() const;

    /**
     * @brief TEST SEAM: overrides one kind's pin so unit tests can
     *        exercise the full stream-verify-rename pipeline against a
     *        local HTTP fixture instead of the real 100MB+ payloads.
     * @param kind     "ragp" or "embed".
     * @param url      Replacement download URL.
     * @param sha256   Replacement content hash (lowercase hex).
     * @param bytes    Replacement expected byte count.
     * @param filename Replacement destination filename.
     */
    void setPinOverrideForTest(const QString& kind,
                               const QString& url,
                               const QString& sha256,
                               qint64 bytes,
                               const QString& filename);

  signals:
    /** @brief Active/idle/error state changed. */
    void stateChanged();

    /** @brief Byte progress of the running download changed. */
    void progressChanged();

    /**
     * @brief A download completed and verified; the file is in place.
     * @param kind     "ragp" or "embed".
     * @param filename The .gguf basename now present in the models dir
     *                 (callers typically select it as the default).
     */
    void downloadFinished(const QString& kind, const QString& filename);

    /**
     * @brief A download failed (network, size, or hash mismatch). The
     *        partial file has been deleted.
     * @param kind  "ragp" or "embed".
     * @param error Human-readable reason.
     */
    void downloadFailed(const QString& kind, const QString& error);

  private:
    /**
     * @brief One pinned recommendation: where to fetch it, what its
     *        bytes must hash to, how large it must be, and the
     *        filename it lands under.
     */
    struct Pin {
        QString url;
        QString sha256;  ///< lowercase hex
        qint64 bytes = 0;
        QString filename;
    };

    Pin pinFor(const QString& kind) const;
    QString targetDirFor(const QString& kind) const;
    void finishFailure(const QString& error);

    std::function<QString(const QString&)> m_targetDirProvider;
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_reply = nullptr;
    QFile m_partFile;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_activeKind;
    Pin m_activePin;
    qint64 m_received = 0;
    QString m_lastError;

    // Test-only pin overrides (empty in production).
    Pin m_ragpOverride;
    Pin m_embedOverride;
};

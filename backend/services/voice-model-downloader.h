// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-model-downloader.h
 * @brief Downloads voice-call models (Piper voices, whisper tiers) from
 *        the pinned catalog into the Verzeta-Voice daemon's models
 *        directory, verifying size and SHA-256 before anything is
 *        installed. One download at a time; progress is streamed for
 *        the percent + MB/MB rows in the Voice Calls dialog.
 * @layer Service
 * @dependencies voice-model-catalog.h, Qt6::Network (QNAM), Qt6::Core.
 * \@thread Main thread only; all network work is asynchronous signals.
 */

#pragma once

#include "../voice/voice-model-catalog.h"

#include <memory>
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QString>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;

namespace Verzeta::Voice {

/**
 * @brief Catalog-driven, checksum-verified model installer for the
 *        voice add-on, exposed to QML as a direct singleton.
 *
 * Nothing is fetched at runtime except the files themselves: the
 * catalog, sizes and hashes are compiled in. A file downloads to
 * "\<name\>.part", is hashed incrementally as it arrives, and is renamed
 * into place only when both the size and the SHA-256 match; a failed or
 * cancelled download leaves no partial file behind. Multi-file entries
 * (a voice's model + config) install all files or none.
 */
class VoiceModelDownloader : public QObject {
    Q_OBJECT

    /** Directory downloads install into (the daemon's models dir, from
     *  its greet). Empty disables downloading. */
    Q_PROPERTY(QString targetDir READ targetDir WRITE setTargetDir NOTIFY targetDirChanged)
    /** Catalog id currently downloading; empty when idle. */
    Q_PROPERTY(QString activeId READ activeId NOTIFY progressChanged)
    /** Bytes received so far for the active entry (all its files). */
    Q_PROPERTY(double receivedBytes READ receivedBytes NOTIFY progressChanged)
    /** Total bytes of the active entry; 0 when idle. */
    Q_PROPERTY(double totalBytes READ totalBytes NOTIFY progressChanged)

  public:
    /**
     * @brief Creates the downloader (no network activity yet).
     * @param parent Owner in the Qt sense.
     */
    explicit VoiceModelDownloader(QObject* parent = nullptr);
    /** @brief Aborts any active transfer and removes its partial file. */
    ~VoiceModelDownloader() override;

    /**
     * @brief The install directory.
     * @returns The daemon's models directory, or empty when unknown.
     */
    QString targetDir() const { return m_targetDir; }

    /**
     * @brief Points installs at the daemon's models directory.
     * @param dir Absolute path from the daemon's greet; empty disables.
     */
    void setTargetDir(const QString& dir);

    /**
     * @brief The entry currently downloading.
     * @returns Its catalog id, or empty when idle.
     */
    QString activeId() const { return m_activeId; }

    /**
     * @brief Progress numerator across the active entry's files.
     * @returns Bytes received including completed files.
     */
    double receivedBytes() const { return static_cast<double>(m_receivedBytes); }

    /**
     * @brief Progress denominator for the active entry.
     * @returns The entry's total bytes, or 0 when idle.
     */
    double totalBytes() const { return static_cast<double>(m_totalBytes); }

    /**
     * @brief The catalog as QML rows, with per-entry installed state
     *        resolved against the current target directory.
     * @returns Maps of {id, kind, displayName, license, sizeBytes,
     *          installed}.
     */
    Q_INVOKABLE QVariantList catalog() const;

    /**
     * @brief Whether every file of an entry exists in the target
     *        directory at its exact pinned size.
     * @param id A catalog id.
     * @returns True when the entry is fully installed.
     */
    Q_INVOKABLE bool isInstalled(const QString& id) const;

    /**
     * @brief Starts downloading one entry. Refused (finished with an
     *        error) when the id is unknown, the target directory is
     *        unset, or another download is active.
     * @param id The catalog id to install.
     */
    Q_INVOKABLE void download(const QString& id);

    /** @brief Aborts the active download and removes its partial file. */
    Q_INVOKABLE void cancel();

    /**
     * @brief Verifies a file's content hash (streaming, constant
     *        memory). Pure enough for direct unit testing.
     * @param filePath  The file to hash.
     * @param sha256Hex Expected lower-case hex SHA-256.
     * @returns True when the file exists and its hash matches.
     */
    static bool verifyFileSha256(const QString& filePath, const QString& sha256Hex);

  signals:
    /** @brief targetDir changed (installed states may differ too). */
    void targetDirChanged();
    /** @brief activeId / receivedBytes / totalBytes changed. */
    void progressChanged();
    /**
     * @brief A download ended, one way or the other.
     * @param id      The catalog id.
     * @param success True when every file verified and installed.
     * @param error   Human-readable reason when success is false.
     */
    void finished(const QString& id, bool success, const QString& error);

  private:
    /** @brief Begins the transfer of m_entry.files[m_fileIndex]. */
    void startNextFile();
    /** @brief Finishes the active entry with an error; cleans up. */
    void fail(const QString& error);
    /** @brief Drops reply/file state (no signals). */
    void releaseTransfer();
    /** @brief Absolute path of a file inside the target directory. */
    QString targetPath(const QString& fileName) const;

    QString m_targetDir;
    std::unique_ptr<QNetworkAccessManager> m_nam;

    // ----- Active entry state (reset when idle) -----
    QString m_activeId;
    VoiceModelEntry m_entry;
    int m_fileIndex = 0;
    qint64 m_receivedBytes = 0;   ///< Completed files + current file.
    qint64 m_completedBytes = 0;  ///< Completed files only.
    qint64 m_totalBytes = 0;
    QNetworkReply* m_reply = nullptr;
    QFile m_partFile;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
};

}  // namespace Verzeta::Voice

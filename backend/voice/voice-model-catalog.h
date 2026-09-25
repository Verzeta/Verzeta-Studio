// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-model-catalog.h
 * @brief The pinned catalog of downloadable voice-call models: Piper
 *        voices and whisper speech-recognition tiers, each with its
 *        exact size and SHA-256 so a download is verified before it is
 *        installed. Every voice was licence-audited individually;
 *        non-commercial datasets (CC BY-NC) are excluded by policy.
 * @layer Utility (Voice)
 * @dependencies Qt6::Core only.
 */

#pragma once

#include <QList>
#include <QString>

namespace Verzeta::Voice {

/**
 * @brief One file of a catalog entry (a voice is two files: the model
 *        and its config; a speech model is one).
 */
struct VoiceModelFile {
    /** File name as installed into the daemon's models directory. */
    QString fileName;
    /** Direct HTTPS download URL. */
    QString url;
    /** Exact size in bytes; a mismatch fails verification. */
    qint64 sizeBytes = 0;
    /** Lower-case hex SHA-256 of the file's content. */
    QString sha256;
};

/**
 * @brief One downloadable model: a Piper voice or a whisper STT tier.
 */
struct VoiceModelEntry {
    /** Stable id: the voice id ("en_US-joe-medium") or the whisper
     *  file base ("ggml-base.en-q5_1"). */
    QString id;
    /** "voice" or "stt". */
    QString kind;
    /** Human-readable name for the download row. */
    QString displayName;
    /** The voice DATASET's licence, shown beside the row (the Piper
     *  models themselves are distributed freely; the dataset licence is
     *  the binding one). */
    QString license;
    /** Every file the entry installs; all must verify. */
    QList<VoiceModelFile> files;

    /**
     * @brief Sum of all file sizes.
     * @returns Total bytes this entry downloads.
     */
    qint64 totalBytes() const;
};

/**
 * @brief The full pinned catalog.
 *
 * Checksums come from the Hugging Face LFS metadata (model files) or
 * were computed from the fetched content (the small voice configs) at
 * pin time; they are constants, never fetched at runtime.
 *
 * @returns All entries, voices first, then speech models.
 */
QList<VoiceModelEntry> voiceModelCatalog();

}  // namespace Verzeta::Voice

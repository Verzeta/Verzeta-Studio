// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-archive-extractor.h
 * @brief Safe ZIP extraction for ClawHub-installed skills.  Uses
 *        KArchive's KZip and enforces:
 *
 *          - reject path-traversal entries (`../`, absolute paths,
 *            normalized escapes outside staging)
 *          - reject any entry that is a symlink
 *          - reject if file count > 256
 *          - reject if any single file > 1 MiB
 *          - reject if total uncompressed > 8 MiB (decompression bomb)
 *
 *        On any rejection, returns false; caller is responsible for
 *        moving the staging dir into skills/quarantine/.
 *
 * @layer Utility
 * @dependencies Qt6::Core, KF6::Archive (KZip).
 */


#pragma once

#include <QString>

namespace SkillArchive {

/**
 * @brief Outcome of a single extraction attempt.
 *
 * Always populated by extract(). `success` is true iff every entry
 * in the archive passed the safety caps and was written to disk.
 * On failure, `error` carries a user-displayable diagnostic and
 * the partially-extracted staging directory should be discarded
 * (treated as poisoned by the caller).
 */
struct ExtractResult {
    bool success = false;          ///< true iff extraction completed cleanly.
    QString error;                 ///< Diagnostic on failure; empty on success.
    int fileCount = 0;             ///< Number of regular files written.
    qint64 uncompressedBytes = 0;  ///< Sum of uncompressed sizes written.
};

/**
 * @brief Extract a ZIP archive into `stagingDir` with the archive size caps.
 *
 * @param zipPath     Absolute path to the downloaded .zip.
 * @param stagingDir  Absolute path to a clean (pre-mkpath'd) staging
 *                    directory. Caller owns its lifetime.
 * @return ExtractResult with `success=true` if every entry passed all
 *         caps and was written; `success=false` with `error` populated
 *         otherwise. Partially-extracted state may exist in stagingDir;
 *         the caller should treat the dir as poisoned and remove or
 *         quarantine it.
 */
ExtractResult extract(const QString& zipPath, const QString& stagingDir);

}  // namespace SkillArchive

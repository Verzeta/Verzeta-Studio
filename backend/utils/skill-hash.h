// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-hash.h
 * @brief Deterministic SHA-256 over a skill folder.  Same input bytes
 *        → same hex digest.  Reordered files on disk → same digest
 *        (sort step).  Any byte change in any file → diff digest.
 *        Symlinks are rejected at the parser layer; this hash
 *        function assumes the input is already a clean folder.
 * @layer Utility
 * @dependencies Qt6::Core (QFile, QDir, QCryptographicHash).  No
 *               service deps.
 */

#pragma once

#include <QString>

namespace SkillHash {

/**
 * @brief Compute SHA-256 over every regular file in `folderPath`,
 *        sorted lex-ascending by relative POSIX path.
 *
 *        Per-file feed sequence: relpath UTF-8 + '\0' + 8-byte LE size
 *        + '\0' + file contents (chunked) + '\0'. Final hex digest is
 *        lowercase 64 chars.
 *
 * @param folderPath  Absolute path to the skill folder. Must exist.
 * @param outError    If non-null and an error occurs (unreadable file,
 *                    folder not found), populated with a human-readable
 *                    reason and the function returns empty string.
 * @return 64-char lowercase hex SHA-256, or empty string on error.
 * @complexity O(N + sum(file_sizes)) where N is the file count.
 * @sideeffects None (pure read).
 */
QString computeFolderHash(const QString& folderPath, QString* outError = nullptr);

}  // namespace SkillHash

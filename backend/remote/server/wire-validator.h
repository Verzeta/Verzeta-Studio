// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-validator.h
 * @brief Input validation helpers for the verzeta-remote-side wire ops.
 *
 *        Pure functions; no Qt object dependencies. Validators return
 *        true on accept and false on reject. Callers map a false
 *        return to a wire `invalid_params` error envelope.
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core only.
 */

#pragma once

#include <QChar>
#include <QString>

namespace Verzeta::Remote::Validate {

/** @brief Hard upper bound on any single wire-string field. */
inline constexpr int kMaxStringLen = 4096;

/**
 * @brief Accepts non-empty strings up to the per-string size cap.
 * @param s  Candidate string.
 * @returns True iff non-empty and `s.size() <= kMaxStringLen`.
 */
inline bool nonEmptyBounded(const QString& s) {
    return !s.isEmpty() && s.size() <= kMaxStringLen;
}

/**
 * @brief Accepts any string up to the per-string size cap (empty allowed).
 * @param s  Candidate string.
 * @returns True iff `s.size() <= kMaxStringLen`.
 */
inline bool optionalBounded(const QString& s) {
    return s.size() <= kMaxStringLen;
}

/**
 * @brief Accepts 32- or 36-character hex UUIDs.
 * @param s  Candidate string.
 * @returns True iff the string matches the canonical UUID shape.
 */
inline bool isUuidShape(const QString& s) {
    if (s.isEmpty())
        return false;
    if (s.size() != 32 && s.size() != 36)
        return false;
    for (QChar c : s) {
        if (c == QChar('-'))
            continue;
        const ushort u = c.unicode();
        const bool hex = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
        if (!hex)
            return false;
    }
    return true;
}

/**
 * @brief Accepts a folder-type discriminator string.
 * @param t  Candidate string.
 * @returns True iff `t` is one of "regular", "project", "organization".
 */
inline bool isValidFolderType(const QString& t) {
    return t == QStringLiteral("regular") || t == QStringLiteral("project") ||
           t == QStringLiteral("organization");
}

/**
 * @brief Refuses path-traversal-style filenames.
 * @param name  Candidate basename.
 * @returns True when `name` is non-empty, contains no slashes /
 *          backslashes, does not contain "..", and does not start
 *          with a dot.
 */
inline bool isSafeFileName(const QString& name) {
    if (name.isEmpty())
        return false;
    if (name.contains(QChar('/')))
        return false;
    if (name.contains(QChar('\\')))
        return false;
    if (name.contains(QStringLiteral("..")))
        return false;
    if (name.startsWith(QChar('.')))
        return false;
    return true;
}

}  // namespace Verzeta::Remote::Validate

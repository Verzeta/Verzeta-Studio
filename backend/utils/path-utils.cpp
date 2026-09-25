// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file path-utils.cpp
 * @brief Implementation of the QML path/URL conversion singleton.
 * @layer Utility (presentation)
 * @dependencies Qt6::Core (QUrl).
 */

#include "path-utils.h"

namespace Verzeta {

QString PathUtils::toLocalFile(const QUrl& url) const {
    if (url.isEmpty())
        return {};
    // A remote URL has no local path. Returning its `path()` here is exactly
    // the bug this class exists to prevent, so refuse instead.
    if (!url.isLocalFile())
        return {};
    return url.toLocalFile();
}

QUrl PathUtils::fromLocalFile(const QString& path) const {
    if (path.isEmpty())
        return {};
    // Tolerate an already-URL string. Callers pass paths that came from the DB
    // (bare) and from earlier conversions (URLs); re-wrapping a URL would
    // percent-encode its colon and produce a relative path.
    const QUrl asUrl(path);
    if (asUrl.isValid() && !asUrl.scheme().isEmpty()) {
        // "C:/x" parses with scheme "c" on every platform, so a single-letter
        // scheme is a Windows drive letter, never a real URL scheme.
        if (asUrl.scheme().size() > 1)
            return asUrl;
    }
    return QUrl::fromLocalFile(path);
}

}  // namespace Verzeta

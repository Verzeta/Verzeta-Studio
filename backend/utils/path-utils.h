// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file path-utils.h
 * @brief QML singleton that converts between local filesystem paths and
 *        `file:` URLs using Qt's own parser, so no QML file has to do string
 *        surgery on a URL.
 * @layer Utility (presentation)
 * @dependencies Qt6::Core (QUrl), Qt6::Qml (singleton registration).
 */

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

namespace Verzeta {

/**
 * @brief Path/URL conversion for QML.
 *
 * QML code that hand-builds `"file://" + path`, or strips a prefix with
 * `url.toString().replace("file://", "")`, is correct only on platforms whose
 * absolute paths already begin with a separator. On Windows both forms are
 * wrong, and wrong in a way that never raises an error:
 *
 *   - `"file://" + "C:/x"` parses as `file://c/x`. The drive letter becomes the
 *     URL's *host*, the colon is discarded, and the result still reports
 *     `isValid() == true` and `isLocalFile() == true` while pointing at the UNC
 *     path `\\c\x`. `Qt.openUrlExternally` then returns false and does nothing.
 *   - `QUrl("file:///C:/x").toString().replace("file://", "")` yields `/C:/x`,
 *     a path with a leading separator that no Windows API will open.
 *
 * `QUrl::fromLocalFile` and `QUrl::toLocalFile` already handle drive letters,
 * UNC shares and percent-encoding on every platform. This singleton is the one
 * place QML reaches them. It is stateless and holds no resources.
 */
class PathUtils : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the singleton.
     * @param parent Optional Qt parent.
     */
    explicit PathUtils(QObject* parent = nullptr) : QObject(parent) {}

    /**
     * @brief Converts a `file:` URL to a native absolute path.
     * @param url A URL, typically a `FileDialog`'s `selectedFile`/`selectedFolder`.
     * @returns The local path, or an empty QString when @p url is empty or is
     *          not a local file (a remote scheme, for instance). Callers must
     *          treat empty as "nothing was picked" rather than as a valid path.
     */
    Q_INVOKABLE QString toLocalFile(const QUrl& url) const;

    /**
     * @brief Converts a native absolute path to a `file:` URL.
     * @param path An absolute local path. A path that is already a URL string
     *             (it has a scheme) is returned parsed rather than re-wrapped,
     *             so callers can pass either without double-encoding.
     * @returns A URL suitable for `Qt.openUrlExternally`, an `Image.source`, or
     *          a `FileDialog.currentFile`. Empty in, empty out.
     */
    Q_INVOKABLE QUrl fromLocalFile(const QString& path) const;
};

}  // namespace Verzeta

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ragp-backend-selector.cpp
 * @brief Pure decision logic for RAGP backend selection. See
 *        ragp-backend-selector.h for the full contract.
 * @layer Service
 * @dependencies Qt6::Core (QFileInfo).
 */

#include "ragp-backend-selector.h"

#include <QFileInfo>

namespace Ragp {

QString chooseRagpBackendSpec(bool localEnabled, const QString& resolvedModelPath) {
    // The local backend is a sidecar BRIDGE and exists in every build;
    // a missing sidecar degrades at runtime through loadFailed → the
    // remote auto-fallback. Selection is therefore pure policy.
    if (!localEnabled) {
        return QStringLiteral("remote");
    }
    if (resolvedModelPath.isEmpty()) {
        return QStringLiteral("remote");
    }
    const QFileInfo info(resolvedModelPath);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        return QStringLiteral("remote");
    }
    return QStringLiteral("local:") + resolvedModelPath;
}

}  // namespace Ragp

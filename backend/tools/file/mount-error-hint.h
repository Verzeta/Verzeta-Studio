// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file mount-error-hint.h
 * @brief Shared mapping from machine-readable mount
 *        error kinds to agent-facing recovery hints, embedded by the
 *        file tools (read_file / write_file / list_files) in their
 *        structured error JSON.
 * @layer Tool
 * @dependencies Qt6::Core.
 */

#pragma once

#include <QString>

/**
 * @brief Maps a FileService::lastErrorKind() value to a one-line
 *        recovery hint for the agent.
 * @param kind Machine-readable error kind.
 * @returns Human-readable hint; generic fallback for unknown kinds.
 */
inline QString mountErrorHint(const QString& kind) {
    if (kind == QStringLiteral("bridge_offline") || kind == QStringLiteral("mount_offline") ||
        kind == QStringLiteral("session_superseded") || kind == QStringLiteral("session_closed") ||
        kind == QStringLiteral("session_destroyed") || kind.startsWith(QStringLiteral("timeout"))) {
        return QStringLiteral("The workspace client that owns this file is unreachable "
                              "right now. Retry later, or write a NEW filename to use "
                              "host-local storage.");
    }
    if (kind == QStringLiteral("caller_client_not_mounted")) {
        return QStringLiteral("Your workspace mount is not active on this folder. "
                              "Re-register the workspace from the editor extension.");
    }
    if (kind == QStringLiteral("mount_namespace_unknown")) {
        return QStringLiteral("No active client matches that /mount/<id>/ prefix. Call "
                              "list_files to see the current namespaces.");
    }
    if (kind == QStringLiteral("unsafe_path") || kind == QStringLiteral("blocked_path") ||
        kind == QStringLiteral("permission_denied")) {
        return QStringLiteral("This path is blocked by workspace security rules. Choose "
                              "a different path inside the workspace.");
    }
    if (kind == QStringLiteral("stale_fingerprint")) {
        return QStringLiteral("The file changed since you last read it. Read it again "
                              "and re-apply your edit.");
    }
    return QStringLiteral("Unexpected mount error. Call list_files to inspect the "
                          "current state, then retry.");
}

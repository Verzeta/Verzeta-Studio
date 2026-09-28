// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file write-guard.h
 * @brief Optional write restriction for shell commands, built on the Linux
 *        Landlock security module. A restricted command (and everything it
 *        starts) can read and run anything, but can only create, change or
 *        delete files inside a given set of folders.
 *
 *        Landlock needs no privileges, no user namespaces and no extra
 *        packages, so it also works where bubblewrap is blocked (for example
 *        Ubuntu's AppArmor user-namespace restriction). On other platforms,
 *        or kernels without Landlock, available() is false and nothing is
 *        restricted.
 * @layer Utility
 * @dependencies Qt6::Core, Linux Landlock (kernel 5.13+)
 */

#pragma once

#include <functional>
#include <QString>
#include <QStringList>

namespace Verzeta::WriteGuard {

/**
 * @brief The Landlock ABI version the running kernel offers.
 * @returns 1 or higher when Landlock can be used; 0 when it cannot (not
 *          Linux, kernel without Landlock, or Landlock disabled).
 */
int abiVersion();

/**
 * @brief Whether the write restriction can be applied on this system.
 * @returns abiVersion() > 0.
 */
bool available();

/**
 * @brief Builds the function that restricts a child process.
 *
 * All paths are resolved and copied in the calling (parent) process; the
 * returned function only performs async-signal-safe system calls, so it is
 * safe to run between fork() and exec(), for example via
 * QProcess::setChildProcessModifier(). Folders that do not exist are
 * skipped. If the restriction cannot be applied the child writes a message
 * to stderr and exits with status 126 instead of running unrestricted.
 *
 * @param writableRoots Folders the process may write inside (recursively).
 * @returns The child-side setup function; a no-op when available() is false.
 */
std::function<void()> childSetup(const QStringList& writableRoots);

}  // namespace Verzeta::WriteGuard

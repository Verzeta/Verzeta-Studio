// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file command-line-options.h
 * @brief Parses verzeta-studio's command-line arguments into a typed
 *        struct. The only mode flag is `--headless-remote`; remaining
 *        flags override individual settings of the wire-host daemon
 *        with precedence CLI > persisted QSettings > built-in default.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#pragma once

#include <QString>
#include <QStringList>

class QCommandLineParser;
class QSettings;

namespace Verzeta::Utils {

/**
 * @brief Resolved command-line + persisted-setting values, ready to
 *        hand to AppController + WireHostBridge.
 *
 * Fields are post-precedence: each field already reflects whatever
 * the CLI passed, falling through to persisted QSettings, falling
 * through to the built-in default. Consumers do not consult QSettings
 * again; this struct is the single source of truth for the run.
 */
struct CommandLineOptions {
    /** When true: skip QML window load AND auto-start the wire
     *  daemon at AppController::initialize() completion. Default
     *  false (normal desktop mode). */
    bool headlessRemote = false;

    /** Bind address for the wire daemon. */
    QString remoteBind;

    /** TCP port for the wire daemon. */
    int remotePort = 0;

    /** True iff the wire daemon should be started with TLS. */
    bool remoteTls = false;

    /** Server certificate path (PEM). Empty when TLS is off. */
    QString remoteCert;

    /** Server private-key path (PEM). Empty when TLS is off. */
    QString remoteKey;

    /** True iff parsing produced an error. Consumers should print
     *  `errorMessage` and exit with non-zero. */
    bool hasError = false;

    /** Human-readable error message when `hasError` is true. */
    QString errorMessage;
};

/**
 * @brief Parses argv into a CommandLineOptions struct, layering
 *        persisted QSettings values under CLI flags. Validates flag
 *        collisions (e.g. `--remote-tls` without `--remote-cert` /
 *        `--remote-key`).
 * @param arguments The argv list (typically QCoreApplication::arguments()).
 * @param settings A QSettings instance scoped to the production
 *        organisation / application name. Read-only access.
 * @returns CommandLineOptions with resolved values. On parse failure
 *          `hasError = true` and `errorMessage` is populated; the
 *          caller should print + exit non-zero.
 */
CommandLineOptions parseCommandLine(const QStringList& arguments, QSettings& settings);

}  // namespace Verzeta::Utils

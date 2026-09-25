// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file command-line-options.cpp
 * @brief Implementation of the CommandLineOptions parser. Layers CLI
 *        flags over persisted QSettings over built-in defaults.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#include "command-line-options.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSettings>

namespace Verzeta::Utils {

namespace {

constexpr int kDefaultPort = 9180;

QString defaultBindAddr() {
    return QStringLiteral("0.0.0.0");
}

QString settingsBindKey() {
    return QStringLiteral("remote/bindAddr");
}
QString settingsPortKey() {
    return QStringLiteral("remote/port");
}
QString settingsTlsKey() {
    return QStringLiteral("remote/tlsEnabled");
}
QString settingsCertKey() {
    return QStringLiteral("remote/certPath");
}
QString settingsKeyKey() {
    return QStringLiteral("remote/keyPath");
}

}  // namespace

CommandLineOptions parseCommandLine(const QStringList& arguments, QSettings& settings) {
    CommandLineOptions opts;

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Verzeta Studio, an AI chat host. Run without flags "
                       "for the normal desktop app, or pass --headless-remote "
                       "to run without a window for paired remote clients."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption optHeadlessRemote(
        QStringLiteral("headless-remote"),
        QStringLiteral("Run without a window and start the remote "
                       "server for paired clients (Android, VS Code)."));

    const QCommandLineOption optRemoteBind(
        QStringLiteral("remote-bind"),
        QStringLiteral("Bind address for the remote server (overrides the "
                       "persisted Remote Access setting). Requires "
                       "--headless-remote."),
        QStringLiteral("addr"));

    const QCommandLineOption optRemotePort(
        QStringLiteral("remote-port"),
        QStringLiteral("TCP port for the remote server (overrides the "
                       "persisted Remote Access setting). Requires "
                       "--headless-remote."),
        QStringLiteral("port"));

    const QCommandLineOption optRemoteTls(
        QStringLiteral("remote-tls"),
        QStringLiteral("Force TLS on for the remote server (overrides the "
                       "saved headless TLS setting). Requires "
                       "--remote-cert and --remote-key. Requires "
                       "--headless-remote."));

    const QCommandLineOption optNoRemoteTls(
        QStringLiteral("no-remote-tls"),
        QStringLiteral("Force TLS off for the remote server (overrides the "
                       "saved headless TLS setting). Requires "
                       "--headless-remote."));

    const QCommandLineOption optRemoteCert(
        QStringLiteral("remote-cert"),
        QStringLiteral("Server certificate PEM path. Pairs with --remote-tls."),
        QStringLiteral("path"));

    const QCommandLineOption optRemoteKey(
        QStringLiteral("remote-key"),
        QStringLiteral("Server private-key PEM path. Pairs with --remote-tls."),
        QStringLiteral("path"));

    parser.addOptions({optHeadlessRemote,
                       optRemoteBind,
                       optRemotePort,
                       optRemoteTls,
                       optNoRemoteTls,
                       optRemoteCert,
                       optRemoteKey});

    if (!parser.parse(arguments)) {
        opts.hasError = true;
        opts.errorMessage = parser.errorText();
        return opts;
    }

    // --help / --version are handled by QCommandLineParser exiting the
    // process directly when we call `process()`; we used `parse()` so
    // we can return errors as data instead. Honour the help / version
    // requests explicitly here so callers don't have to re-handle them.
    if (parser.isSet(QStringLiteral("help"))) {
        parser.showHelp(0);  // exits
    }
    if (parser.isSet(QStringLiteral("version"))) {
        parser.showVersion();  // exits
    }

    // --- Mode flag ------------------------------------------------------
    opts.headlessRemote = parser.isSet(optHeadlessRemote);

    // --- Bind address ---------------------------------------------------
    if (parser.isSet(optRemoteBind)) {
        opts.remoteBind = parser.value(optRemoteBind);
    } else if (settings.contains(settingsBindKey())) {
        opts.remoteBind = settings.value(settingsBindKey()).toString();
    }
    if (opts.remoteBind.isEmpty()) {
        opts.remoteBind = defaultBindAddr();
    }

    // --- Port -----------------------------------------------------------
    if (parser.isSet(optRemotePort)) {
        bool ok = false;
        const int v = parser.value(optRemotePort).toInt(&ok);
        if (!ok || v < 1 || v > 65535) {
            opts.hasError = true;
            opts.errorMessage = QStringLiteral("--remote-port must be an integer in [1, 65535]; "
                                               "got: %1")
                                    .arg(parser.value(optRemotePort));
            return opts;
        }
        opts.remotePort = v;
    } else if (settings.contains(settingsPortKey())) {
        opts.remotePort = settings.value(settingsPortKey()).toInt();
        if (opts.remotePort < 1 || opts.remotePort > 65535) {
            opts.remotePort = kDefaultPort;  // sanitise corrupted persisted value
        }
    }
    if (opts.remotePort == 0) {
        opts.remotePort = kDefaultPort;
    }

    // --- TLS ------------------------------------------------------------
    const bool tlsSet = parser.isSet(optRemoteTls);
    const bool tlsUnset = parser.isSet(optNoRemoteTls);
    if (tlsSet && tlsUnset) {
        opts.hasError = true;
        opts.errorMessage =
            QStringLiteral("Cannot pass --remote-tls and --no-remote-tls together.");
        return opts;
    }
    if (tlsSet) {
        opts.remoteTls = true;
    } else if (tlsUnset) {
        opts.remoteTls = false;
    } else if (settings.contains(settingsTlsKey())) {
        opts.remoteTls = settings.value(settingsTlsKey()).toBool();
    }

    // --- Cert / Key -----------------------------------------------------
    if (parser.isSet(optRemoteCert)) {
        opts.remoteCert = parser.value(optRemoteCert);
    } else if (settings.contains(settingsCertKey())) {
        opts.remoteCert = settings.value(settingsCertKey()).toString();
    }
    if (parser.isSet(optRemoteKey)) {
        opts.remoteKey = parser.value(optRemoteKey);
    } else if (settings.contains(settingsKeyKey())) {
        opts.remoteKey = settings.value(settingsKeyKey()).toString();
    }

    // --- Collision validation ------------------------------------------
    // Remote-* flags only make sense with --headless-remote (desktop
    // mode uses the persisted QSettings via the QML toggle).
    if (!opts.headlessRemote) {
        QStringList stray;
        if (parser.isSet(optRemoteBind))
            stray << QStringLiteral("--remote-bind");
        if (parser.isSet(optRemotePort))
            stray << QStringLiteral("--remote-port");
        if (parser.isSet(optRemoteTls))
            stray << QStringLiteral("--remote-tls");
        if (parser.isSet(optNoRemoteTls))
            stray << QStringLiteral("--no-remote-tls");
        if (parser.isSet(optRemoteCert))
            stray << QStringLiteral("--remote-cert");
        if (parser.isSet(optRemoteKey))
            stray << QStringLiteral("--remote-key");
        if (!stray.isEmpty()) {
            opts.hasError = true;
            opts.errorMessage = QStringLiteral("Flags %1 require --headless-remote. "
                                               "In desktop mode, set these in the "
                                               "Remote Access dialog instead.")
                                    .arg(stray.join(QStringLiteral(", ")));
            return opts;
        }
    }

    // TLS demands cert + key.
    if (opts.remoteTls) {
        if (opts.remoteCert.isEmpty() || opts.remoteKey.isEmpty()) {
            opts.hasError = true;
            opts.errorMessage =
                QStringLiteral("--remote-tls requires --remote-cert and --remote-key "
                               "(or persisted Remote Access TLS settings from a prior "
                               "desktop configuration).");
            return opts;
        }
    }

    return opts;
}

}  // namespace Verzeta::Utils

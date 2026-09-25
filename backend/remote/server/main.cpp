// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file main.cpp
 * @brief Entry point for the verzeta-remote standalone binary.
 *
 *        This is a SEPARATE PROCESS from verzeta-studio. It owns the
 *        Android-facing QWebSocketServer + its own SQLite database for
 *        paired client tokens. Communicates with verzeta-studio over a
 *        QLocalSocket. If this binary crashes, verzeta-studio is
 *        unaffected. If verzeta-studio crashes, this binary will
 *        reconnect when the host comes back up.
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::WebSockets, Qt6::Network, Qt6::Sql.
 *
 *        Command-line:
 *          --bind <addr>     bind address (default 127.0.0.1)
 *          --port <n>        listen port  (default 9180)
 *          --pair-code       generate a 6-digit pairing code, print
 *                            it to stdout, and exit. Used by host UI
 *                            to drive the "Pair Device" button without
 *                            needing the WS server up.
 *          --list-clients    print paired clients as JSON, exit.
 *          --revoke \<id\>     revoke client by id, exit.
 */

#include "wire-auth.h"
#include "wire-db-reader.h"
#include "wire-host-client.h"
#include "wire-server.h"

#include <QTextStream>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(wireMain, "verzeta.remote.main", QtInfoMsg)

using namespace Verzeta::Remote;

/**
 * @brief Converts a paired client to the JSON printed by --list-clients.
 * @param c Client row.
 * @returns Object with id, name, created_at and last_seen_at (ms since
 *          epoch, or null), and revoked.
 */
static QJsonObject clientToJson(const PairedClient& c) {
    return QJsonObject{
        {QStringLiteral("id"), c.id},
        {QStringLiteral("name"), c.name},
        {QStringLiteral("created_at"),
         c.createdAt.isValid() ? QJsonValue(static_cast<qint64>(c.createdAt.toMSecsSinceEpoch()))
                               : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("last_seen_at"),
         c.lastSeenAt.isValid() ? QJsonValue(static_cast<qint64>(c.lastSeenAt.toMSecsSinceEpoch()))
                                : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("revoked"), !c.isActive()},
    };
}

/**
 * @brief Entry point of the verzeta-remote daemon.
 * @param argc Argument count.
 * @param argv Arguments; run with --help for the options.
 * @returns 0 on success; 1 when the auth database cannot open, TLS is
 *          requested without --cert and --key, or listening fails; 2
 *          when --revoke names an unknown client.
 */
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    // AppDataLocation must match the host's, because both processes open the
    // same database. The host sets organizationName "Verzeta" and
    // applicationName "verzeta-studio" (see backend/main.cpp); this daemon
    // adopts the identical pair rather than its own, so the two agree on one
    // directory. Setting a different pair here does not fail loudly: the
    // daemon simply resolves a different path and opens an empty database.
    app.setOrganizationName(QStringLiteral("Verzeta"));
    app.setApplicationName(QStringLiteral("verzeta-studio"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Verzeta Remote: the WebSocket server that connects "
                       "paired clients to Verzeta Studio. Runs as a separate process."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("bind"),
                      QStringLiteral("Bind address"),
                      QStringLiteral("addr"),
                      QStringLiteral("127.0.0.1")});
    parser.addOption({QStringLiteral("port"),
                      QStringLiteral("Listen port"),
                      QStringLiteral("n"),
                      QStringLiteral("9180")});
    parser.addOption(
        {QStringLiteral("pair-code"), QStringLiteral("Print a new pairing code and exit.")});
    parser.addOption({QStringLiteral("list-clients"),
                      QStringLiteral("Print the paired clients as JSON and exit.")});
    parser.addOption({QStringLiteral("revoke"),
                      QStringLiteral("Revoke the paired client with this ID and exit."),
                      QStringLiteral("id")});
    parser.addOption(
        {QStringLiteral("tls"), QStringLiteral("Enable TLS (wss://). Requires --cert and --key.")});
    parser.addOption({QStringLiteral("cert"),
                      QStringLiteral("PEM-encoded server certificate file"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("key"),
                      QStringLiteral("PEM-encoded private key file"),
                      QStringLiteral("path")});
    parser.process(app);

    WireAuth auth;
    if (!auth.open()) {
        qCCritical(wireMain) << "main: failed to open wire-state.db";
        return 1;
    }

    QTextStream out(stdout);

    if (parser.isSet(QStringLiteral("pair-code"))) {
        out << auth.generatePairingCode() << '\n';
        out.flush();
        return 0;
    }
    if (parser.isSet(QStringLiteral("list-clients"))) {
        QJsonArray arr;
        for (const auto& c : auth.listClients(true)) {
            arr.append(clientToJson(c));
        }
        out << QString::fromUtf8(QJsonDocument(arr).toJson()) << '\n';
        out.flush();
        return 0;
    }
    if (parser.isSet(QStringLiteral("revoke"))) {
        const QString id = parser.value(QStringLiteral("revoke"));
        const bool ok = auth.revokeClient(id);
        out << (ok ? "ok\n" : "not-found\n");
        out.flush();
        return ok ? 0 : 2;
    }

    const QString bind = parser.value(QStringLiteral("bind"));
    const quint16 port = static_cast<quint16>(parser.value(QStringLiteral("port")).toInt());

    WireHostClient hostClient;
    hostClient.start();

    // Read-only WAL connection to the host's main database for state
    // hydration of ops the host doesn't expose as Q_INVOKABLE.
    // Failure to open is non-fatal — wire still runs, but SQL-backed
    // ops return "host db unavailable" until the host comes up.
    WireDbReader dbReader;
    if (!dbReader.open()) {
        qCWarning(wireMain) << "main: WireDbReader::open() failed; "
                               "SQL-backed ops will return host_offline";
    }

    WireServer server(&auth, &hostClient, &dbReader);

    hostClient.setWireServer(&server);

    const bool useTls = parser.isSet(QStringLiteral("tls")) ||
                        parser.isSet(QStringLiteral("cert")) || parser.isSet(QStringLiteral("key"));
    bool listening;
    if (useTls) {
        const QString certPath = parser.value(QStringLiteral("cert"));
        const QString keyPath = parser.value(QStringLiteral("key"));
        if (certPath.isEmpty() || keyPath.isEmpty()) {
            qCCritical(wireMain) << "TLS requested but --cert / --key paths missing";
            return 1;
        }
        listening = server.listenTls(bind, port, certPath, keyPath);
    } else {
        listening = server.listen(bind, port);
    }
    if (!listening)
        return 1;

    qCInfo(wireMain) << "verzeta-remote: ready. Listening on" << (useTls ? "wss://" : "ws://")
                     << bind << ":" << port << "/ — bridge socket:"
                     << "verzeta-host-bridge";
    return app.exec();
}

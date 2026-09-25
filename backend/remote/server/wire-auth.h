// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-auth.h
 * @brief Paired-client storage, 6-digit pairing code generator, and
 *        bearer token issue / verify.
 *
 *        Lives in the verzeta-remote PROCESS and owns its OWN SQLite
 *        database (`<AppData>/wire-state.db`) so the host database is
 *        never touched.
 *
 *        Pairing flow: the user clicks Pair Device in the host UI; the
 *        daemon generates a code which the user enters on Android.
 *        Tokens are 256-bit CSPRNG; only their SHA-256 hash is
 *        persisted.
 *
 *        Threading: this object is created on the remote process's
 *        main thread (the only thread that runs the
 *        QWebSocketServer event loop). All methods are
 *        main-thread-only; no cross-thread access.
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::Sql.
 */

#pragma once

#include <optional>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

namespace Verzeta::Remote {

/**
 * @brief One paired-client row from the wire `clients` table.
 */
struct PairedClient {
    QString id;            ///< Client UUID.
    QString name;          ///< Name the client gave when pairing.
    QDateTime createdAt;   ///< When the client paired.
    QDateTime lastSeenAt;  ///< Last successful token check; invalid if never.
    QDateTime revokedAt;   ///< When access was revoked; invalid while active.

    /**
     * @brief Reports whether the client is still active.
     * @returns True iff `revokedAt` is unset.
     */
    bool isActive() const noexcept { return !revokedAt.isValid(); }
};

/**
 * @brief Owns the wire daemon's auth state: paired clients, pairing
 *        codes, bearer tokens. Lives in its own SQLite database
 *        (`wire-state.db`) so the host database is never touched.
 */
class WireAuth : public QObject {
    Q_OBJECT
  public:
    /**
     * @brief Constructs an unopened WireAuth. Call open() before use.
     * @param parent  Optional Qt parent.
     */
    explicit WireAuth(QObject* parent = nullptr);
    ~WireAuth() override;

    /**
     * @brief Opens and migrates the wire-state.db at
     *        `<AppData>/verzeta-remote/wire-state.db`.
     * @returns True on success; false on open / migrate failure.
     */
    bool open();

    /**
     * @brief Generates a 6-digit pairing code valid for 5 minutes,
     *        single-use.
     * @returns The freshly issued code.
     */
    QString generatePairingCode();

    /**
     * @brief Atomically validates and consumes a pairing code.
     * @param code  Code the client just typed.
     * @returns True iff the code is active and unconsumed.
     */
    bool consumePairingCode(const QString& code);

    /**
     * @brief Bearer token + client id returned by issueToken().
     */
    struct IssuedToken {
        QString token;
        QString clientId;
    };
    /** @var Verzeta::Remote::WireAuth::IssuedToken::token
     *  Bearer token; only its SHA-256 is stored. */
    /** @var Verzeta::Remote::WireAuth::IssuedToken::clientId
     *  UUID of the new client row. */

    /**
     * @brief Issues a new bearer token and persists the client row.
     * @param clientName  Human-readable name supplied by the client.
     * @returns IssuedToken `{token, clientId}` on success; empty
     *          optional on failure.
     */
    std::optional<IssuedToken> issueToken(const QString& clientName);

    /**
     * @brief Looks up a client by bearer token.
     * @param token  Bearer token presented by the client.
     * @returns The matching PairedClient on hit (and bumps
     *          `last_seen_at`); empty optional on miss / revoked.
     */
    std::optional<PairedClient> verifyToken(const QString& token);

    /**
     * @brief Lists paired clients.
     * @param includeRevoked  When true, also returns revoked rows.
     * @returns Client rows ordered by creation time.
     */
    QList<PairedClient> listClients(bool includeRevoked = false);

    /**
     * @brief Marks a client revoked.
     * @param clientId  Client UUID.
     * @returns True on success; false when the client does not exist.
     */
    bool revokeClient(const QString& clientId);

  private:
    /**
     * @brief Hashes a bearer token to its persisted SHA-256 form.
     * @param token  Plain bearer token.
     * @returns Hex SHA-256 string.
     */
    static QString hashToken(const QString& token);

    /**
     * @brief Mints a new client UUID.
     * @returns UUID v4 string with braces stripped.
     */
    static QString newClientId();

    /**
     * @brief Drops expired pairing codes from the table.
     */
    void purgeExpiredCodes();

    QString m_connectionName;
    QSqlDatabase m_db;
};

}  // namespace Verzeta::Remote

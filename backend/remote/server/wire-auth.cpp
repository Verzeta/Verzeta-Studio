// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file wire-auth.cpp
 * @brief Implementation of the wire daemon's auth store: paired
 *        clients, pairing codes, bearer tokens. Owns its own SQLite
 *        database (`wire-state.db`).
 * @layer Service (presentation; remote-access daemon).
 * @dependencies Qt6::Core, Qt6::Sql.
 */

#include "wire-auth.h"

#include <QTimeZone>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

Q_LOGGING_CATEGORY(wireAuth, "verzeta.remote.auth", QtInfoMsg)

namespace Verzeta::Remote {

namespace {
constexpr int kPairingCodeTtlSeconds = 5 * 60;
}

WireAuth::WireAuth(QObject* parent) : QObject(parent) {
    m_connectionName =
        QStringLiteral("verzeta-remote-auth-%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
}

WireAuth::~WireAuth() {
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool WireAuth::open() {
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir.isEmpty()) {
        dataDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) +
                  QStringLiteral("/.verzeta-remote");
    }
    // Place under a verzeta-remote-specific subdir to keep the wire DB
    // separate from the host DB even though they share AppDataLocation.
    const QString remoteDir = dataDir + QStringLiteral("/verzeta-remote");
    QDir().mkpath(remoteDir);
    const QString dbPath = remoteDir + QStringLiteral("/wire-state.db");

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        qCCritical(wireAuth) << "WireAuth::open: cannot open" << dbPath << ":"
                             << m_db.lastError().text();
        return false;
    }

    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    if (!q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS wire_clients (
            id           TEXT PRIMARY KEY,
            name         TEXT NOT NULL,
            token_hash   TEXT NOT NULL UNIQUE,
            created_at   INTEGER NOT NULL,
            last_seen_at INTEGER,
            revoked_at   INTEGER
        )
    )"))) {
        qCWarning(wireAuth) << "create wire_clients failed:" << q.lastError().text();
        return false;
    }

    // Pairing codes table — short-lived, single-use credentials. Persisted
    // (rather than in-memory) so the `--pair-code` CLI invocation and the
    // running WS server process share the same set, since they are
    // separate OS processes that hit the same `wire-state.db`.
    if (!q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS wire_pairing_codes (
            code         TEXT PRIMARY KEY,
            expires_at   INTEGER NOT NULL,
            created_at   INTEGER NOT NULL
        )
    )"))) {
        qCWarning(wireAuth) << "create wire_pairing_codes failed:" << q.lastError().text();
        return false;
    }
    return true;
}

QString WireAuth::hashToken(const QString& token) {
    const QByteArray digest = QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex());
}

QString WireAuth::newClientId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void WireAuth::purgeExpiredCodes() {
    const qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM wire_pairing_codes WHERE expires_at <= ?"));
    q.addBindValue(nowMs);
    q.exec();
}

QString WireAuth::generatePairingCode() {
    purgeExpiredCodes();
    auto& rng = *QRandomGenerator::system();
    // Six unique digits — re-roll on collision (extremely rare).
    QString code;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const quint32 raw = rng.bounded(900000u);
        code = QString::number(100000u + raw);
        QSqlQuery exists(m_db);
        exists.prepare(QStringLiteral("SELECT 1 FROM wire_pairing_codes WHERE code=? LIMIT 1"));
        exists.addBindValue(code);
        if (exists.exec() && !exists.next())
            break;
    }
    const qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO wire_pairing_codes "
                             "(code, expires_at, created_at) VALUES (?,?,?)"));
    q.addBindValue(code);
    q.addBindValue(nowMs + qint64(kPairingCodeTtlSeconds) * 1000);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        qCWarning(wireAuth) << "generatePairingCode INSERT failed:" << q.lastError().text();
    }
    return code;
}

bool WireAuth::consumePairingCode(const QString& code) {
    purgeExpiredCodes();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM wire_pairing_codes WHERE code=?"));
    q.addBindValue(code);
    if (!q.exec())
        return false;
    return q.numRowsAffected() > 0;
}

std::optional<WireAuth::IssuedToken> WireAuth::issueToken(const QString& clientName) {
    QByteArray raw(32, Qt::Uninitialized);
    auto& rng = *QRandomGenerator::system();
    rng.fillRange(reinterpret_cast<quint32*>(raw.data()), raw.size() / sizeof(quint32));
    const QString token = QString::fromLatin1(
        raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    const QString clientId = newClientId();
    const QString tokenHash = hashToken(token);
    const qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO wire_clients (id,name,token_hash,created_at) "
                             "VALUES (?,?,?,?)"));
    q.addBindValue(clientId);
    q.addBindValue(clientName);
    q.addBindValue(tokenHash);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        qCWarning(wireAuth) << "issueToken INSERT failed:" << q.lastError().text();
        return std::nullopt;
    }
    return IssuedToken{token, clientId};
}

std::optional<PairedClient> WireAuth::verifyToken(const QString& token) {
    const QString tokenHash = hashToken(token);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id,name,created_at,last_seen_at,revoked_at "
                             "FROM wire_clients WHERE token_hash=? LIMIT 1"));
    q.addBindValue(tokenHash);
    if (!q.exec() || !q.next())
        return std::nullopt;

    PairedClient pc;
    pc.id = q.value(0).toString();
    pc.name = q.value(1).toString();
    pc.createdAt = QDateTime::fromMSecsSinceEpoch(q.value(2).toLongLong(), QTimeZone::UTC);
    if (!q.value(3).isNull())
        pc.lastSeenAt = QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong(), QTimeZone::UTC);
    if (!q.value(4).isNull())
        pc.revokedAt = QDateTime::fromMSecsSinceEpoch(q.value(4).toLongLong(), QTimeZone::UTC);

    if (!pc.isActive())
        return std::nullopt;

    const qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QSqlQuery up(m_db);
    up.prepare(QStringLiteral("UPDATE wire_clients SET last_seen_at=? WHERE id=?"));
    up.addBindValue(nowMs);
    up.addBindValue(pc.id);
    up.exec();
    pc.lastSeenAt = QDateTime::fromMSecsSinceEpoch(nowMs, QTimeZone::UTC);
    return pc;
}

QList<PairedClient> WireAuth::listClients(bool includeRevoked) {
    QList<PairedClient> out;
    const QString sql = includeRevoked
                            ? QStringLiteral("SELECT id,name,created_at,last_seen_at,revoked_at "
                                             "FROM wire_clients ORDER BY created_at DESC")
                            : QStringLiteral("SELECT id,name,created_at,last_seen_at,revoked_at "
                                             "FROM wire_clients WHERE revoked_at IS NULL "
                                             "ORDER BY created_at DESC");
    QSqlQuery q(m_db);
    if (!q.exec(sql))
        return out;
    while (q.next()) {
        PairedClient pc;
        pc.id = q.value(0).toString();
        pc.name = q.value(1).toString();
        pc.createdAt = QDateTime::fromMSecsSinceEpoch(q.value(2).toLongLong(), QTimeZone::UTC);
        if (!q.value(3).isNull())
            pc.lastSeenAt = QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong(), QTimeZone::UTC);
        if (!q.value(4).isNull())
            pc.revokedAt = QDateTime::fromMSecsSinceEpoch(q.value(4).toLongLong(), QTimeZone::UTC);
        out.append(pc);
    }
    return out;
}

bool WireAuth::revokeClient(const QString& clientId) {
    const qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE wire_clients SET revoked_at=? "
                             "WHERE id=? AND revoked_at IS NULL"));
    q.addBindValue(nowMs);
    q.addBindValue(clientId);
    if (!q.exec())
        return false;
    return q.numRowsAffected() > 0;
}

}  // namespace Verzeta::Remote

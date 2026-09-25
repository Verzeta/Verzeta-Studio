// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file crypto-utils.cpp
 * @brief API key storage: the OS credential store through QtKeychain when
 *        built with VERZETA_HAS_QTKEYCHAIN, otherwise an XOR-obfuscated
 *        QSettings fallback.
 * @layer Utility
 * @dependencies Qt6::Core; QtKeychain when built with VERZETA_HAS_QTKEYCHAIN.
 *
 * Security notes:
 * - API keys are never logged. Functions only emit success/failure messages.
 * - The fallback XORs each key with SHA-256 of the machine id and a random
 *   salt, and the salt is persisted in the same settings store. This is
 *   obfuscation, not encryption: it prevents a casual read of the settings
 *   store, but anyone who can read that store on the same machine can
 *   recover the key.
 * - Zeroing memory: Qt does not guarantee zeroing QString internal buffers;
 *   keys are held only in short-lived local variables here.
 */

#include "crypto-utils.h"

#include "logger.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QSettings>
#include <QSysInfo>
#include <QUuid>

// Compile-time switch: prefer QtKeychain when available
#ifdef VERZETA_HAS_QTKEYCHAIN
#include <qt6keychain/keychain.h>

#include <QEventLoop>
#endif

namespace CryptoUtils {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief Returns the QSettings group key for a stored (fallback) API key.
 * @param service Provider service identifier.
 * @return Settings key string like "apikeys/verzeta.openai".
 */
QString settingsKey(const QString& service) {
    return QStringLiteral("apikeys/") + service;
}

/**
 * @brief Derives the machine-unique key used to obfuscate fallback-stored
 *        API keys. Key = SHA-256(machine_id + "|" + salt), where the salt is
 *        a random UUID persisted in settings on first use.
 * @return 32-byte SHA-256 digest.
 * @complexity O(1)
 */
QByteArray fallbackEncryptionKey() {
    QSettings settings;
    const QString saltKey = QStringLiteral("security/key_salt");

    QString salt = settings.value(saltKey).toString();
    if (salt.isEmpty()) {
        salt = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(saltKey, salt);
    }

    // Combine machine-unique identifier with persisted salt
    const QString material = QSysInfo::machineUniqueId() + QStringLiteral("|") + salt;

    return QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha256);
}

/**
 * @brief Obfuscates a byte string by XOR with the derived key. The
 *        operation is its own inverse, so the same call deobfuscates.
 *        NOTE: This is NOT production-strength encryption. It prevents casual
 *        plaintext reads but is not cryptographically secure. A real AES-CBC
 *        implementation should be used with a proper crypto library (libsodium).
 * @param data Data to obfuscate.
 * @param key Key bytes.
 * @return The XORed bytes; callers Base64-encode them for storage.
 */
QByteArray xorObfuscate(const QByteArray& data, const QByteArray& key) {
    QByteArray result = data;
    for (int i = 0; i < result.size(); ++i) {
        result[i] = static_cast<char>(static_cast<unsigned char>(result[i]) ^
                                      static_cast<unsigned char>(key[i % key.size()]));
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * @brief Returns whether QtKeychain is available.
 * @return true if compiled with VERZETA_HAS_QTKEYCHAIN.
 */
bool isKeychainAvailable() {
#ifdef VERZETA_HAS_QTKEYCHAIN
    return true;
#else
    return false;
#endif
}

/*
 * @brief Stores an API key (see crypto-utils.h for how and how safely).
 * @param service Provider identifier (e.g., "verzeta.openai").
 * @param key The API key. Never logged.
 * @return true on success.
 * @sideeffects Writes to OS keychain or XOR-obfuscated QSettings.
 */
bool storeApiKey(const QString& service, const QString& key) {
#ifdef VERZETA_HAS_QTKEYCHAIN
    QKeychain::WritePasswordJob job(QStringLiteral("Verzeta Studio"));
    job.setAutoDelete(false);
    job.setKey(service);
    job.setTextData(key);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError) {
        qCWarning(verzetaUi) << "Keychain write failed for service" << service << "— falling back";
        // Fall through to settings fallback
    } else {
        qCInfo(verzetaUi) << "API key stored in OS keychain for" << service;
        return true;
    }
#endif

    // Fallback: XOR obfuscation stored in QSettings
    const QByteArray obfuscated = xorObfuscate(key.toUtf8(), fallbackEncryptionKey());
    QSettings settings;
    settings.setValue(settingsKey(service), QString::fromLatin1(obfuscated.toBase64()));
    qCInfo(verzetaUi) << "API key stored in settings (fallback) for" << service;
    return true;
}

/*
 * @brief Retrieves an API key from the OS keychain or settings fallback.
 * @param service Provider identifier.
 * @return The API key string, or empty on failure.
 */
QString retrieveApiKey(const QString& service) {
#ifdef VERZETA_HAS_QTKEYCHAIN
    QKeychain::ReadPasswordJob job(QStringLiteral("Verzeta Studio"));
    job.setAutoDelete(false);
    job.setKey(service);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() == QKeychain::NoError) {
        return job.textData();
    }
    qCWarning(verzetaUi) << "Keychain read failed for service" << service
                         << "— trying settings fallback";
#endif

    // Fallback: read from QSettings and deobfuscate
    QSettings settings;
    const QString stored = settings.value(settingsKey(service)).toString();
    if (stored.isEmpty()) {
        return {};
    }

    const QByteArray decoded = QByteArray::fromBase64(stored.toLatin1());
    const QByteArray deobfuscated = xorObfuscate(decoded, fallbackEncryptionKey());
    return QString::fromUtf8(deobfuscated);
}

/*
 * @brief Deletes an API key from the keychain and settings.
 * @param service Provider identifier.
 * @return true if deletion succeeded.
 */
bool deleteApiKey(const QString& service) {
    bool success = true;

#ifdef VERZETA_HAS_QTKEYCHAIN
    QKeychain::DeletePasswordJob job(QStringLiteral("Verzeta Studio"));
    job.setAutoDelete(false);
    job.setKey(service);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
        qCWarning(verzetaUi) << "Keychain delete failed for" << service;
        success = false;
    }
#endif

    // Also clear from settings fallback
    QSettings settings;
    settings.remove(settingsKey(service));

    qCInfo(verzetaUi) << "API key deleted for service" << service;
    return success;
}

}  // namespace CryptoUtils

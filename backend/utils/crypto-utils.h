// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file crypto-utils.h
 * @brief API key storage and retrieval.
 * @layer Utility
 * @dependencies Qt6::Core; QtKeychain when built with VERZETA_HAS_QTKEYCHAIN.
 *
 * Storage:
 * - When built with VERZETA_HAS_QTKEYCHAIN, keys are kept in the OS
 *   credential store through QtKeychain (libsecret on Linux, Keychain on
 *   macOS, Credential Manager on Windows).
 * - Otherwise keys are XOR-obfuscated and written to the application's
 *   QSettings store. No build configuration in this repository defines
 *   VERZETA_HAS_QTKEYCHAIN, so this is the path that runs. It is
 *   obfuscation, not encryption: it stops a key being read at a glance, but
 *   anyone who can read that settings store on the same machine can recover
 *   the key. See crypto-utils.cpp for the derivation.
 * - Keys are never logged or printed.
 */

#pragma once

#include <QString>

/**
 * @brief Namespace for cryptographic and credential management utilities.
 */
namespace CryptoUtils {

/**
 * @brief Stores an API key: in the OS credential store when built with
 *        VERZETA_HAS_QTKEYCHAIN, otherwise XOR-obfuscated in QSettings.
 *        See the file documentation for what the fallback does and does not
 *        protect against.
 * @param service Provider identifier used as keychain service name
 *                (e.g., "verzeta.openai", "verzeta.anthropic").
 *                Must not contain spaces or special characters.
 * @param key The API key string to store. Never logged or exposed in errors.
 * @return true if the key was stored successfully.
 * @sideeffects Writes to the OS credential store or the QSettings store.
 *              Never logs the key value, only success or failure.
 */
bool storeApiKey(const QString& service, const QString& key);

/**
 * @brief Retrieves an API key from the OS credential store or the
 *        XOR-obfuscated QSettings fallback.
 * @param service Provider identifier (same value used when storing).
 * @return The API key string, or empty string if not found or on error.
 * @sideeffects Reads from the OS credential store or the QSettings store.
 */
QString retrieveApiKey(const QString& service);

/**
 * @brief Deletes an API key from the OS credential store, when built with
 *        VERZETA_HAS_QTKEYCHAIN, and always from the QSettings fallback.
 * @param service Provider identifier.
 * @return true if the key was successfully deleted.
 * @sideeffects Removes the entry from the credential store and settings.
 */
bool deleteApiKey(const QString& service);

/**
 * @brief Returns whether QtKeychain is compiled in.
 *        If false, the XOR-obfuscated QSettings fallback is used.
 * @return true when built with VERZETA_HAS_QTKEYCHAIN.
 */
bool isKeychainAvailable();

}  // namespace CryptoUtils

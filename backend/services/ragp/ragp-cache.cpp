// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-cache.cpp
 * @brief In-memory cache implementation for RAGP classifications.
 * @layer Service
 * @dependencies Qt6::Core
 */

#include "ragp-cache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QMutexLocker>

namespace Ragp {

Cache::Cache(int maxEntries, qint64 ttlMs)
    : m_maxEntries(maxEntries > 0 ? maxEntries : 256), m_ttlMs(ttlMs > 0 ? ttlMs : 5 * 60 * 1000) {}

QByteArray Cache::makeKey(const QString& content, const QStringList& rosterAliases) {
    // Sort roster for order-independence (same roster, any input order,
    // yields the same key).
    QStringList sortedRoster = rosterAliases;
    std::sort(sortedRoster.begin(), sortedRoster.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(content.toUtf8());
    hasher.addData(QByteArrayLiteral("\x1f"));  // record separator
    for (const QString& alias : sortedRoster) {
        hasher.addData(alias.toLower().toUtf8());
        hasher.addData(QByteArrayLiteral("\x1e"));  // unit separator
    }
    return hasher.result();
}

bool Cache::lookup(const QString& content,
                   const QStringList& rosterAliases,
                   Classification& result) {
    const QByteArray key = makeKey(content, rosterAliases);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    QMutexLocker locker(&m_mutex);

    const auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return false;
    }

    // Check TTL expiry.
    if (nowMs - it->lastAccessMs > m_ttlMs) {
        m_entries.erase(it);
        return false;
    }

    // Hit — refresh access timestamp and return value.
    it->lastAccessMs = nowMs;
    result = it->value;
    result.fromCache = true;
    return true;
}

void Cache::store(const QString& content,
                  const QStringList& rosterAliases,
                  const Classification& result) {
    const QByteArray key = makeKey(content, rosterAliases);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    QMutexLocker locker(&m_mutex);

    // Drop any expired entries first.
    evictExpiredLocked(nowMs);

    // If at capacity and this is a new key, evict oldest.
    if (!m_entries.contains(key) && m_entries.size() >= m_maxEntries) {
        evictOldestLocked();
    }

    Entry entry;
    entry.value = result;
    entry.lastAccessMs = nowMs;
    m_entries.insert(key, entry);
}

void Cache::clear() {
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
}

int Cache::size() const {
    QMutexLocker locker(&m_mutex);
    return m_entries.size();
}

void Cache::evictExpiredLocked(qint64 nowMs) {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (nowMs - it->lastAccessMs > m_ttlMs) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

void Cache::evictOldestLocked() {
    if (m_entries.isEmpty())
        return;

    // Linear scan for oldest. At 256 entries this is ~microseconds.
    auto oldest = m_entries.begin();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->lastAccessMs < oldest->lastAccessMs) {
            oldest = it;
        }
    }
    m_entries.erase(oldest);
}

}  // namespace Ragp

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-cache.h
 * @brief In-memory cache for RAGP classifications (Tier 2). Avoids
 *        re-classifying identical agent responses within a session.
 * @layer Service
 * @dependencies Qt6::Core
 *
 * Keyed by content + roster composition. Bounded capacity (default
 * 256 entries) with oldest-access eviction. Entries expire after a
 * configurable TTL (default 5 minutes) to prevent stale
 * classifications from surviving roster changes silently.
 *
 * Thread safety: access is serialized internally with a QMutex so
 * the cache can be safely shared between the main thread and any
 * background classification worker.
 */

#pragma once

#include "ragp-types.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>

namespace Ragp {

/**
 * @brief Thread-safe bounded in-memory cache for RAGP
 *        classifications, keyed by content + roster composition.
 */
class Cache {
  public:
    /**
     * @brief Construct a cache with the given capacity and TTL.
     * @param maxEntries Maximum entries before oldest-access eviction.
     * @param ttlMs      Time-to-live in milliseconds per entry.
     */
    explicit Cache(int maxEntries = 256, qint64 ttlMs = 5 * 60 * 1000);

    /**
     * @brief Look up a classification for the given content + roster.
     *        Updates the entry's last-access timestamp on hit.
     * @param content       Agent response text.
     * @param rosterAliases All agent aliases in the conversation.
     * @param result        Output, populated on hit.
     * @returns true on hit (result populated); false on miss or expiry.
     */
    bool lookup(const QString& content, const QStringList& rosterAliases, Classification& result);

    /**
     * @brief Store a classification keyed by content + roster.
     *        Evicts oldest-access entry if at capacity.
     * @param content       Agent response text.
     * @param rosterAliases All agent aliases in the conversation.
     * @param result        Classification to cache.
     */
    void
    store(const QString& content, const QStringList& rosterAliases, const Classification& result);

    /**
     * @brief Clear all entries.
     */
    void clear();

    /**
     * @brief Current number of entries (for metrics / tests).
     * @returns Entry count.
     */
    int size() const;

  private:
    /** @brief One cache row: classification + last-access stamp. */
    struct Entry {
        Classification value;
        qint64 lastAccessMs = 0;
    };

    /**
     * @brief Build a deterministic cache key from content + roster.
     *        Roster is sorted for order-independence.
     */
    static QByteArray makeKey(const QString& content, const QStringList& rosterAliases);

    void evictExpiredLocked(qint64 nowMs);
    void evictOldestLocked();

    mutable QMutex m_mutex;
    int m_maxEntries;
    qint64 m_ttlMs;
    QHash<QByteArray, Entry> m_entries;
};

}  // namespace Ragp

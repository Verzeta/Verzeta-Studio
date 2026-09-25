// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file contention-ledger.cpp
 * @brief Implementation of ContentionLedger, a mutex-guarded in-flight
 *        table over a monotonic clock, emitting one structured log line
 *        per completed inference call.
 * @layer Utility
 * @dependencies Qt6::Core (QMutex, QHash, QElapsedTimer, QLoggingCategory).
 */

#include "contention-ledger.h"

#include <QElapsedTimer>
#include <QHash>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

// Default level QtInfoMsg: the ledger's per-request report is emitted at
// Debug, so it is off by default. Enable it for troubleshooting with
// QT_LOGGING_RULES="verzeta.ledger.debug=true".
Q_LOGGING_CATEGORY(verzetaLedger, "verzeta.ledger", QtInfoMsg)

namespace {

/** @brief One in-flight call entry (timestamps in ms on the shared
 *         monotonic clock; -1 = phase never reached). */
struct Entry {
    ContentionLedger::CallClass cls{};
    QString providerKey;
    QString model;
    qint64 beganMs = 0;
    qint64 dispatchedMs = -1;
    qint64 firstTokMs = -1;
    QString concurrentAtStart;  ///< "class:provider" list, comma-joined
};

/** @brief Shared mutable state behind one mutex. Function-local statics
 *         so initialization is thread-safe and ordered on first use. */
struct State {
    QMutex mutex;
    QHash<quint64, Entry> inflight;
    quint64 nextId = 1;
    QElapsedTimer clock;
};

State& state() {
    static State s;
    if (!s.clock.isValid())
        s.clock.start();
    return s;
}

/** @brief Short lowercase tag for a call class (stable log grammar). */
const char* clsTag(ContentionLedger::CallClass c) {
    switch (c) {
        case ContentionLedger::CallClass::Turn:
            return "turn";
        case ContentionLedger::CallClass::Ragp:
            return "ragp";
        case ContentionLedger::CallClass::Confirm:
            return "confirm";
        case ContentionLedger::CallClass::Summary:
            return "summary";
        case ContentionLedger::CallClass::Embed:
            return "embed";
        case ContentionLedger::CallClass::Warm:
            return "warm";
    }
    return "?";
}

}  // namespace

quint64 ContentionLedger::begin(CallClass cls, const QString& providerKey, const QString& model) {
    State& s = state();
    QMutexLocker lock(&s.mutex);
    // Snapshot every OTHER in-flight call — the overlap evidence this
    // ledger exists to produce.
    QStringList concurrent;
    concurrent.reserve(s.inflight.size());
    for (auto it = s.inflight.cbegin(); it != s.inflight.cend(); ++it) {
        concurrent << QStringLiteral("%1:%2").arg(QLatin1String(clsTag(it->cls)), it->providerKey);
    }
    const quint64 id = s.nextId++;
    Entry e;
    e.cls = cls;
    e.providerKey = providerKey;
    e.model = model;
    e.beganMs = s.clock.elapsed();
    e.concurrentAtStart = concurrent.join(QLatin1Char(','));
    s.inflight.insert(id, e);
    return id;
}

void ContentionLedger::markDispatched(quint64 id) {
    State& s = state();
    QMutexLocker lock(&s.mutex);
    auto it = s.inflight.find(id);
    if (it == s.inflight.end())
        return;
    if (it->dispatchedMs < 0)
        it->dispatchedMs = s.clock.elapsed();
}

void ContentionLedger::markFirstToken(quint64 id) {
    State& s = state();
    QMutexLocker lock(&s.mutex);
    auto it = s.inflight.find(id);
    if (it == s.inflight.end())
        return;
    if (it->firstTokMs < 0)
        it->firstTokMs = s.clock.elapsed();
}

void ContentionLedger::end(quint64 id, bool ok, const QString& note) {
    Entry e;
    qint64 endMs = 0;
    {
        State& s = state();
        QMutexLocker lock(&s.mutex);
        auto it = s.inflight.find(id);
        if (it == s.inflight.end())
            return;  // idempotent
        e = *it;
        endMs = s.clock.elapsed();
        s.inflight.erase(it);
    }
    // Log OUTSIDE the lock — the message handler may do I/O.
    const qint64 queueMs = (e.dispatchedMs >= 0) ? (e.dispatchedMs - e.beganMs) : -1;
    const qint64 ttftMs =
        (e.firstTokMs >= 0 && e.dispatchedMs >= 0) ? (e.firstTokMs - e.dispatchedMs) : -1;
    const qint64 totalMs = endMs - e.beganMs;
    qCDebug(verzetaLedger).noquote()
        << QStringLiteral("LEDGER class=%1 provider=%2 model=%3 queue_ms=%4 "
                          "ttft_ms=%5 total_ms=%6 ok=%7 concurrent=[%8]%9")
               .arg(QLatin1String(clsTag(e.cls)),
                    e.providerKey.isEmpty() ? QStringLiteral("?") : e.providerKey,
                    e.model.isEmpty() ? QStringLiteral("?") : e.model)
               .arg(queueMs)
               .arg(ttftMs)
               .arg(totalMs)
               .arg(ok ? QStringLiteral("1") : QStringLiteral("0"))
               .arg(e.concurrentAtStart,
                    note.isEmpty() ? QString() : QStringLiteral(" note=%1").arg(note));
}

int ContentionLedger::openCount() {
    State& s = state();
    QMutexLocker lock(&s.mutex);
    return static_cast<int>(s.inflight.size());
}

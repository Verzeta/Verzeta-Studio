// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file stub-ragp-backend.cpp
 * @brief Stub RAGP backend implementation. Returns UNKNOWN for every
 *        request, used until a real backend is wired in.
 * @layer Service
 * @dependencies Qt6::Concurrent (QFuture).
 */

#include "stub-ragp-backend.h"

#include <QFuture>

namespace Ragp {

QFuture<Classification> StubBackend::classifyAsync(const Request& req) {
    Q_UNUSED(req);
    Classification result;
    result.source = QStringLiteral("stub");
    result.confidence = 0.0;
    result.fromCache = false;
    // No targets populated — the service's UNKNOWN-fallback policy
    // applies. Return an immediately-ready future so the QFutureWatcher
    // fires on the next event-loop iteration.
    return QtFuture::makeReadyValueFuture(result);
}

}  // namespace Ragp

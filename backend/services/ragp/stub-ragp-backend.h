// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file stub-ragp-backend.h
 * @brief Stub RAGP backend that always returns UNKNOWN. Used as the
 *        Tier 3 placeholder until a real backend (remote or local
 *        llama.cpp) is wired.
 * @layer Service
 * @dependencies Qt6::Core
 *
 * Having this stub in place lets the full RAGP pipeline (service +
 * cache + rule classifier) boot and start routing through tiered
 * logic immediately. When Tier 1 rules return UNKNOWN, the service
 * consults the stub which also returns UNKNOWN; the service then
 * applies the UNKNOWN-fallback policy (no cascade).
 */

#pragma once

#include "iragp-backend.h"

namespace Ragp {

/**
 * @brief No-op RAGP Tier-3 backend; every classify resolves to
 *        UNKNOWN.
 */
class StubBackend : public IRagpBackend {
  public:
    StubBackend() = default;
    ~StubBackend() override = default;

    /**
     * @brief IRagpBackend: return an immediately-ready future with
     *        UNKNOWN.
     * @param req Classification request (ignored).
     * @returns Future resolved with an UNKNOWN-filled Classification.
     */
    QFuture<Classification> classifyAsync(const Request& req) override;

    /**
     * @brief IRagpBackend: always reports available.
     * @returns Always true.
     */
    bool isAvailable() const override { return true; }

    /**
     * @brief IRagpBackend: canonical name "stub".
     * @returns The literal string "stub".
     */
    QString backendName() const override { return QStringLiteral("stub"); }
};

}  // namespace Ragp

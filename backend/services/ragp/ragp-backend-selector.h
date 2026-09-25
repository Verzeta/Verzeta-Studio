// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ragp-backend-selector.h
 * @brief Pure decision function that chooses which RAGP backend to
 *        use based on user settings + model-file presence.
 * @layer Service
 * @dependencies Qt6::Core.
 *
 * The function is intentionally separated from ChatController so it
 * can be unit-tested in isolation without constructing a full
 * SettingsService / ChatController graph. It takes primitive inputs
 * (a bool + a QString) and returns a primitive output (a QString
 * spec), so tests can cover every branch with direct arguments.
 *
 * Contract: the returned spec is one of
 *   - "remote": caller constructs Ragp::RemoteBackend
 *   - "local:<absolutePath>": caller constructs
 *     Ragp::LocalLlamaBackend with that path
 *
 * The function NEVER returns anything else and NEVER throws. When
 * VERZETA_HAS_LLAMA is not defined, it always returns "remote"
 * regardless of inputs (local backend class doesn't exist in that
 * build configuration).
 */

#pragma once

#include <QString>

namespace Ragp {

/**
 * @brief Decide which RAGP backend to install, given the user's
 *        intent and the resolved local-model path.
 *
 * @param localEnabled       Value of SettingsService::ragpLocalEnabled().
 *                           False → always "remote".
 * @param resolvedModelPath  Absolute path produced by
 *                           SettingsService::ragpModelPath(filename),
 *                           or an empty QString when the filename
 *                           setting is empty. An empty path always
 *                           yields "remote". A non-empty path is
 *                           checked via QFileInfo: only a file that
 *                           exists AND is readable counts; anything
 *                           else yields "remote" (common case: user
 *                           selected a model that was later
 *                           removed).
 *
 * @return "remote" or "local:<absolutePath>". Never empty, never
 *         throws.
 */
QString chooseRagpBackendSpec(bool localEnabled, const QString& resolvedModelPath);

}  // namespace Ragp

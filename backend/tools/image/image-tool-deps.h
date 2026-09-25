// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file image-tool-deps.h
 * @brief Non-owning service references shared by the image-generation
 *        tool family.
 *
 *        Mirrors the SkillToolDeps / MembershipToolDeps shape so the
 *        tool can be stood up in tests without a full ChatController.
 * @layer Service (Tool subsystem)
 * @dependencies ImageService, SettingsService.
 */


#pragma once

#include <functional>
#include <QString>

class ImageService;
class SettingsService;
class ImageProviderRegistry;

namespace Tools {

/**
 * @brief Bundle of non-owning service references that the image tool
 *        family needs at invocation time.
 */
struct ImageToolDeps {
    ImageService* image = nullptr;        ///< Runs the generation.
    SettingsService* settings = nullptr;  ///< Required by isValid(); not read by the tool.
    /// Source of the active image provider. The tool builds its
    /// ImageGenConfig from `registry->activeConfig()`.
    ImageProviderRegistry* registry = nullptr;
    /// Returns the UI-active conversation id; used when the call carries
    /// no `__caller_conv_id`.
    std::function<QString()> activeConvIdGetter;

    /**
     * @brief Reports whether the bundle carries every required reference.
     * @returns True iff `image`, `settings`, `registry`, and
     *          `activeConvIdGetter` are all present.
     */
    bool isValid() const {
        return image && settings && registry && static_cast<bool>(activeConvIdGetter);
    }
};

}  // namespace Tools

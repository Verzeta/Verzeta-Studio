// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file app-attribution.h
 * @brief The app identity sent to model services that attribute requests to
 *        the calling app. OpenRouter identifies the app by HTTP-Referer
 *        and names it from X-OpenRouter-Title (X-Title is the older,
 *        still accepted name). Every request to it (chat, routing,
 *        images) sends the same set so the app is never listed as unknown
 *        or under another name.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#pragma once

#include <QMap>
#include <QString>

namespace Verzeta {

/**
 * @brief Headers that identify Verzeta Studio to OpenRouter.
 * @returns HTTP-Referer set to the project website, and X-OpenRouter-Title
 *          and X-Title set to the app name.
 */
inline QMap<QString, QString> openRouterAttributionHeaders() {
    return {
        {QStringLiteral("HTTP-Referer"), QStringLiteral("https://verzeta.com")},
        {QStringLiteral("X-OpenRouter-Title"), QStringLiteral("Verzeta Studio")},
        {QStringLiteral("X-Title"), QStringLiteral("Verzeta Studio")},
    };
}

}  // namespace Verzeta

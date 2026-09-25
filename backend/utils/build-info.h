// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file build-info.h
 * @brief Compile-time identity for the About dialog.
 *
 *        CMake injects three compile definitions at the top-level
 *        configure step:
 *
 *          VERZETA_VERSION_STRING:  from project(VerzetaStudio VERSION ...)
 *          VERZETA_BUILD_COMMIT:    `git rev-parse --short HEAD` at
 *                                    configure time, or "unknown" when
 *                                    not built from a git checkout.
 *          VERZETA_BUILD_TYPE:      CMake build type (Debug / Release / …)
 *
 *        Other identity bits (Qt + KF6 versions, llama.cpp build flag)
 *        come from existing Qt / project headers.
 *
 *        Registered as a `BuildInfo` QML context property by
 *        AppController; the AboutDialog binds straight to its CONSTANT
 *        properties.
 *
 * @layer Utility
 * @dependencies Qt6::Core only.
 */


#pragma once

#include <QObject>
#include <QString>

/**
 * @brief Compile-time identity surface for the About dialog.
 *
 * Exposes every public-facing project-identity string as a CONSTANT
 * Q_PROPERTY so the QML layer can bind directly without going
 * through a service lookup.  All values are immutable for the
 * lifetime of the process, computed once from compile definitions,
 * Qt / KF6 runtime headers, and the project's licence + URL constants.
 *
 * Lives as a single QML context property; no ownership semantics
 * beyond standard QObject parent.
 */
class BuildInfo : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString version       READ version       CONSTANT)  ///< Project version (e.g. "0.1.0").
    Q_PROPERTY(QString commit        READ commit        CONSTANT)  ///< Short build commit hash, or "unknown".
    Q_PROPERTY(QString buildType     READ buildType     CONSTANT)  ///< CMake build type (Debug / Release / RelWithDebInfo / MinSizeRel).
    Q_PROPERTY(QString qtVersion     READ qtVersion     CONSTANT)  ///< Qt runtime version string.
    Q_PROPERTY(QString kf6Version    READ kf6Version    CONSTANT)  ///< KDE Frameworks 6 runtime version string.
    Q_PROPERTY(bool    hasLlamaCpp   READ hasLlamaCpp   CONSTANT)  ///< True if built with VERZETA_ENABLE_LLAMACPP=ON.
    Q_PROPERTY(QString homepage      READ homepage      CONSTANT)  ///< Project homepage URL.
    Q_PROPERTY(QString bugTracker    READ bugTracker    CONSTANT)  ///< Issue / bug tracker URL.
    Q_PROPERTY(QString sourceRepo    READ sourceRepo    CONSTANT)  ///< Public source repository URL.
    Q_PROPERTY(QString licenseId     READ licenseId     CONSTANT)  ///< SPDX licence identifier (e.g. "LGPL-3.0-or-later").
    Q_PROPERTY(QString author        READ author        CONSTANT)  ///< Project author / copyright holder display name.
    Q_PROPERTY(QString authorEmail   READ authorEmail   CONSTANT)  ///< Contact email for the author.

public:
    /**
     * @brief Constructs the BuildInfo identity surface.
     * @param parent  Optional Qt parent.
     */
    explicit BuildInfo(QObject* parent = nullptr);

    /** @brief Project semantic version string injected by CMake at configure time.
     *  @returns The contents of the VERZETA_VERSION_STRING compile definition
     *           (matches `project(VerzetaStudio VERSION ...)` in the top CMakeLists). */
    QString version() const;

    /** @brief Short git commit hash captured at CMake configure time.
     *  @returns A short git hash string, or the literal "unknown"
     *           when not built from a git checkout. */
    QString commit() const;

    /** @brief CMake build type the binary was compiled under.
     *  @returns One of "Debug", "Release", "RelWithDebInfo", "MinSizeRel". */
    QString buildType() const;

    /** @brief Qt runtime library version the binary is linked against.
     *  @returns Qt's `QT_VERSION_STR` (e.g. "6.6.2"). */
    QString qtVersion() const;

    /** @brief KDE Frameworks 6 runtime version the binary is linked against.
     *  @returns The runtime KF6 version string from `KCoreAddons::versionString()`. */
    QString kf6Version() const;

    /** @brief Whether the binary includes the bundled llama.cpp inference path.
     *  @returns True iff VERZETA_ENABLE_LLAMACPP=ON at CMake configure time. */
    bool    hasLlamaCpp() const;

    /** @brief Project homepage URL displayed in the About dialog.
     *  @returns A user-displayable HTTPS URL. */
    QString homepage() const;

    /** @brief Issue / bug tracker URL displayed in the About dialog.
     *  @returns A user-displayable HTTPS URL. */
    QString bugTracker() const;

    /** @brief Public source repository URL displayed in the About dialog.
     *  @returns A user-displayable HTTPS URL pointing to the canonical source mirror. */
    QString sourceRepo() const;

    /** @brief SPDX licence identifier under which the binary is distributed.
     *  @returns An SPDX licence string (e.g. "LGPL-3.0-or-later"). */
    QString licenseId() const;

    /** @brief Project author / copyright holder display name.
     *  @returns Display name as it should appear in the About dialog. */
    QString author() const;

    /** @brief Contact email for the project author.
     *  @returns A mailto-compatible email address. */
    QString authorEmail() const;
};

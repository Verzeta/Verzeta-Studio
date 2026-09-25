// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file build-info.cpp
 * @brief Implementation of BuildInfo.  See header for design.
 *
 * @layer Utility
 * @dependencies Qt6::Core (QString, QtGlobal).  Compile-time
 *               definitions VERZETA_VERSION_STRING / VERZETA_BUILD_COMMIT
 *               / VERZETA_BUILD_TYPE supplied by CMake configure.
 */

#include <QCoreApplication>
#include <QFileInfo>
#include "build-info.h"

#include <QString>
#include <QtGlobal>

// Fall-backs in case CMake didn't inject the compile definitions
// (shouldn't happen in normal builds, but keeps test fixtures linkable).
#ifndef VERZETA_VERSION_STRING
/// Release version; normally set by CMake.
#  define VERZETA_VERSION_STRING "0.0.0"
#endif
#ifndef VERZETA_BUILD_COMMIT
/// Source commit of the build; normally set by CMake.
#  define VERZETA_BUILD_COMMIT "unknown"
#endif
#ifndef VERZETA_BUILD_TYPE
/// CMake build type; normally set by CMake.
#  define VERZETA_BUILD_TYPE "unknown"
#endif

/// Helper for VERZETA_STRINGIFY; turns its argument into a string literal.
#define VERZETA_STRINGIFY_INNER(x) #x
/// Turns a macro's expanded value into a string literal. Currently unused.
#define VERZETA_STRINGIFY(x) VERZETA_STRINGIFY_INNER(x)

BuildInfo::BuildInfo(QObject* parent) : QObject(parent) {}

QString BuildInfo::version() const {
    return QStringLiteral(VERZETA_VERSION_STRING);
}
QString BuildInfo::commit() const {
    return QStringLiteral(VERZETA_BUILD_COMMIT);
}
QString BuildInfo::buildType() const {
    return QStringLiteral(VERZETA_BUILD_TYPE);
}
QString BuildInfo::qtVersion() const {
    return QStringLiteral(QT_VERSION_STR);
}
QString BuildInfo::kf6Version() const {
    // KF6 doesn't ship a single canonical version macro accessible
    // without pulling in extra modules. The build manifest pins
    // KF6 ≥ 6.0 in CMakeLists.txt; reporting that as the floor is
    // honest enough for the About dialog. A future enhancement could
    // include find_package(KF6 ... CONFIG) and inject the resolved
    // version via compile definitions.
    return QStringLiteral(">= 6.0");
}
bool BuildInfo::hasLlamaCpp() const {
    // Local llama.cpp inference lives in the verzeta-inference sidecar
    // (the app links no llama code) — availability is the binary's
    // presence beside the application, a runtime fact.
    QString name = QStringLiteral("verzeta-inference");
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return QFileInfo::exists(QCoreApplication::applicationDirPath()
                              + QLatin1Char('/') + name);
}
QString BuildInfo::homepage() const {
    return QStringLiteral("https://verzeta.com");
}
QString BuildInfo::bugTracker() const {
    return QStringLiteral("https://github.com/Verzeta/verzeta/issues");
}
QString BuildInfo::sourceRepo() const {
    return QStringLiteral("https://github.com/Verzeta/Verzeta-Studio");
}
QString BuildInfo::licenseId() const {
    // The project is triple-licensed and every file declares its own SPDX
    // identifier. Reporting "LGPL-3.0-or-later" alone understated the
    // obligation on the GPL-classified components, which are the ones a
    // reader is most likely to want to build on. LICENSING.md is the
    // authority; this string must not claim to be narrower than it is.
    return QStringLiteral("GPL-3.0-or-later / LGPL-3.0-or-later / Commercial");
}
QString BuildInfo::author() const {
    return QStringLiteral("Aditya Mehra");
}
QString BuildInfo::authorEmail() const {
    return QStringLiteral("aix.m@outlook.com");
}

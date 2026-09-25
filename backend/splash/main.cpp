// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file main.cpp
 * @brief Entry point for `verzeta-splash`, the standalone startup-splash
 *        process.
 * @layer API (application bootstrap, separate splash binary)
 * @dependencies Qt6::Gui, Qt6::Qml, Qt6::Quick.
 *
 * verzeta-studio spawns this binary at the very start of its own startup
 * and terminates it once its main window has painted its first frame.
 * Because the splash lives in its OWN process (own GUI thread, own event
 * loop), it animates smoothly while verzeta-studio's main thread is busy
 * with backend initialisation and the (blocking) QML engine load.
 *
 * It is intentionally tiny: QtQuick only, no Kirigami, no app backend,
 * so the splash binary itself starts fast. Importing Kirigami here would
 * make the splash as slow to appear as the window it is meant to cover.
 *
 * Lifetime: verzeta-studio calls QProcess::terminate() (SIGTERM on Unix)
 * when its window is up; SIGTERM's default action ends this process. A
 * 30-second self-timeout (and, on Linux, PR_SET_PDEATHSIG) guarantees
 * the splash can never outlive a crashed host.
 */

#include <QTimer>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>

#if defined(Q_OS_LINUX)
#include <csignal>
#include <sys/prctl.h>
#endif

/**
 * @brief verzeta-splash entry point.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on clean exit, 1 if the splash QML failed to load.
 */
int main(int argc, char* argv[]) {
#if defined(Q_OS_LINUX)
    // Safety net: if verzeta-studio (our parent) dies before it can
    // terminate us, the kernel delivers SIGTERM so the splash can never
    // be orphaned on screen.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif

    // Honour fractional display scaling (mirrors verzeta-studio) so the
    // splash is not rendered tiny on HiDPI screens.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QGuiApplication app(argc, argv);
    // Same organization as the host, so the splash's Qt-managed QML cache
    // lands under ~/.cache/Verzeta/ with everything else rather than at
    // the top of the user's cache directory.
    app.setOrganizationName(QStringLiteral("Verzeta"));
    app.setApplicationName(QStringLiteral("verzeta-splash"));

    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/splash/Splash.qml")));
    if (engine.rootObjects().isEmpty()) {
        // No splash is better than a hung splash process.
        return 1;
    }

    // Portable safety net — self-exit after 30 s even if no terminate()
    // ever arrives (e.g. the host crashed mid-startup).
    QTimer::singleShot(30000, &app, &QCoreApplication::quit);

    return app.exec();
}

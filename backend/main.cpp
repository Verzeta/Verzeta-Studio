// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file main.cpp
 * @brief Application entry point. Spawns the startup splash
 *        process, creates QApplication + QQmlApplicationEngine,
 *        initializes AppController, and runs the event loop.
 * @layer API (application bootstrap)
 * @dependencies Qt6::Quick, Qt6::Widgets, KF6::Kirigami2, AppController
 */

#include <QTimer>

#include <iostream>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QUrl>

// CMake-generated header carrying the project version from /VERSION.
// Single source of truth: edit /VERSION at the repo root; CMake's
// configure_file regenerates this on the next configure.
#include "verzeta-version.h"

#ifdef Q_OS_WIN
// Windows-only icon-theme bootstrap. Linux relies on the host's
// installed Breeze (or AppImage's bundled one) discovered via
// XDG_DATA_DIRS — these KF6 headers are never included on Linux.
//
// KIconTheme is the documented KF6 entry point for icon-theme
// initialisation on platforms without a system-provided theme. It
// auto-registers the bundled `icontheme.rcc` (which MSYS2 ships at
// $MSYS2/bin/data/icontheme.rcc and our deploy script lands at
// <exe-dir>/data/icontheme.rcc) and sets the active theme name +
// fallback chain. Replaces the prior `BreezeIcons::initIcons()` +
// manual `QResource::registerResource` + manual `setThemeName` /
// `setThemeSearchPaths` hack.
//
// KQuickIconProvider exposes KF6 icon resolution to QML via
// `image://icon/<name>` URLs (palette-aware mono-icon recolouring
// for dark mode comes for free — no separate `breeze-dark` theme
// directory is needed).
#include <KIconTheme>
#include <KQuickIconProvider>
// QStyle + QStyleFactory — used post-gate to log the QStyle Qt
// actually wound up with (see truth-telling block in main()).
#include <QStyle>
#include <QStyleFactory>
// Windows-only OS-theme bridge — applies a QPalette + native QStyle
// matching the registry's AppsUseLightTheme so Kirigami components
// track Windows light/dark instead of stranding on Qt's default.
#include "utils/windows-theme-bridge.h"
#endif

#include "app-controller.h"
#include "utils/command-line-options.h"
#include "utils/logger.h"
#include "utils/notification-manager.h"

/**
 * @brief Application entry point.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on clean exit, 1 on initialization failure, -1 if QML fails to load.
 */
int main(int argc, char* argv[]) {
    // -----------------------------------------------------------------------
    // Platform integration init (must run BEFORE QApplication construction).
    //
    // These calls cover three classes of cross-platform Qt+Kirigami issue
    // that surfaced when running the AppImage on non-KDE environments
    // (WSL Windows, GNOME desktop, GNOME Mobile):
    //
    //   AB-4: HiDPI scaling and antialised text. Without
    //         setHighDpiScaleFactorRoundingPolicy(PassThrough), Qt 6's
    //         default rounding (RoundPreferFloor) drops 1.25x / 1.5x
    //         scales down to 1.0x — UI rendered tiny on hidpi displays.
    //         PassThrough honors fractional scales exactly.
    //
    //   AB-5: Qt Quick Controls 2 style. Without an explicit pick, Qt
    //         defaults to the "Default" style — flat blue/white, ignores
    //         the host system theme. Setting "org.kde.desktop" tells
    //         Quick Controls to use the QStyle-backed style which
    //         respects the system palette (KStyle on Plasma, GtkStyle
    //         on GNOME, Fusion on Windows/WSL). The user can still
    //         override at run time via the QT_QUICK_CONTROLS_STYLE
    //         env var; we only set the fallback when it isn't set.
    //
    //   AB-3: Icon theme. The .desktop file references `Icon=verzeta-studio`
    //         and Kirigami widgets reference standard FreeDesktop icon
    //         names ("list-add", "configure", etc.). On non-KDE hosts
    //         the system theme may not provide these names; setting
    //         "breeze" as the fallback theme means QIcon::fromTheme()
    //         can resolve via the bundled Breeze icons (when shipped
    //         in the AppImage) or via a system-installed Breeze if one
    //         is present.
    // -----------------------------------------------------------------------
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));
    }
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

#ifdef Q_OS_WIN
    // KF6 icon-theme initialisation MUST run before QApplication
    // construction (per KIconTheme docs + Elisa reference).
    // Registers the bundled icontheme.rcc, sets the default theme
    // name, hooks icon-engine palette tracking. Linux skips this
    // entirely — system Plasma's icon theme is already authoritative
    // there via Kirigami's normal chain. Verify Linux untouched:
    //   nm verzeta-studio | grep -ciE "KIconTheme|KQuickIconProvider"
    // must return 0 on the Linux build.
    //
    // Plugin path note: MSYS2 ships KIconEnginePlugin.dll at the
    // non-standard `share/qt6/plugins/kiconthemes6/iconengines/`
    // path. Earlier attempt to QCoreApplication::addLibraryPath the
    // deploy-relative analogue did NOT take effect (verified by
    // QT_DEBUG_PLUGINS=1 -- the kiconthemes6 dir was never scanned).
    // Reliable fix is in deploy-windows.ps1: it now also copies
    // KIconEnginePlugin.dll into <DeployDir>/iconengines/ and
    // <DeployDir>/plugins/iconengines/ so Qt's standard plugin scan
    // finds it alongside qsvgicon.dll. No code-side libraryPaths
    // hack needed.
    KIconTheme::initTheme();
#endif

    // QApplication required for QSystemTrayIcon and widget fallbacks
    QApplication app(argc, argv);

#ifdef Q_OS_WIN
    const QByteArray rawMode = qgetenv("VERZETA_THEME_MODE");
    const QByteArray themeMode = rawMode.trimmed();
    if (rawMode != themeMode) {
        qWarning("Verzeta: VERZETA_THEME_MODE had whitespace ('%s' -> '%s'); "
                 "common cause: `set X=Y && cmd` cmd.exe quirk. Trimmed.",
                 rawMode.constData(),
                 themeMode.constData());
    }
    // The kdeglobals merge is independent of QStyle / QPalette setup
    // and is what makes KIconEngine recolour mono SVG icons correctly
    // in dark mode (it writes [Colors:*] + [Icons]/Theme=breeze that
    // KIconLoader reads at icon-paint time). Run UNCONDITIONALLY in
    // both bridge mode and qt-override mode -- icons need to recolour
    // regardless of which QStyle/palette path is active. Skipping
    // this in qt-mode was a logic bug user surfaced when running with
    // VERZETA_THEME_MODE=qt + QT_STYLE_OVERRIDE=Breeze: the breeze
    // QStyle loaded but icons stayed black-on-black because no
    // kdeglobals merge ran.
    Verzeta::mergeBreezeColorsIntoKdeglobals();

    if (themeMode == "qt") {
        qInfo("Verzeta: VERZETA_THEME_MODE=qt -- skipping windows-theme-bridge "
              "(Qt picks QStyle from QT_STYLE_OVERRIDE='%s' / --style / default)",
              qgetenv("QT_STYLE_OVERRIDE").constData());
    } else {
        if (!themeMode.isEmpty()) {
            qWarning("Verzeta: VERZETA_THEME_MODE='%s' is not recognised "
                     "(only 'qt' is honoured); running default bridge",
                     themeMode.constData());
        }
        Verzeta::applyWindowsNativeTheme(app);
    }

    // Final QStyle truth-telling. After the bridge / qt-mode gate AND
    // Qt's own QT_STYLE_OVERRIDE / --style processing, app.style() holds
    // the QStyle that's actually rendering everything. Log:
    //   - active style: the lower-case key Qt assigns (windows11, fusion,
    //                   breeze, windowsvista, ...). Authoritative.
    //   - C++ class:    QWindows11Style / QFusionStyle / Breeze::Style /
    //                   etc. Tells you whether the breeze QStyle PLUGIN
    //                   actually loaded (look for "Breeze::Style" or
    //                   anything from the Breeze namespace) vs Qt fell
    //                   back to a built-in style because the plugin
    //                   couldn't be instantiated.
    //   - available:    QStyleFactory::keys() -- everything Qt's plugin
    //                   loader could discover. If "Breeze" appears here
    //                   but the active style is NOT Breeze, the plugin
    //                   metadata loaded but instantiation failed
    //                   (usually missing runtime deps for the plugin).
    if (QStyle* s = app.style()) {
        qInfo("Verzeta: active QStyle = '%s' (class='%s')",
              qPrintable(s->objectName()),
              s->metaObject()->className());
    } else {
        qWarning("Verzeta: app.style() is null after gate -- this should never happen");
    }
    qInfo("Verzeta: QStyleFactory::keys() = %s",
          qPrintable(QStyleFactory::keys().join(QStringLiteral(", "))));
#endif

    // Native (FreeType-based) text rendering. Distance-field rendering
    // (Qt 6 default) gives crisper text at most scales but has known
    // hinting issues with small body text on hidpi screens. Native
    // rendering matches what the rest of the desktop draws.
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);

    // Application identity.
    //
    // Qt derives every QStandardPaths location and the QSettings file from
    // organizationName/applicationName, as "<organization>/<application>".
    // Those two must therefore be PATH IDENTIFIERS, not display strings:
    // setting both to "Verzeta Studio" produced the nested
    // "Verzeta Studio/Verzeta Studio" directory, and a data path containing
    // a space that every documented shell command had to escape.
    //
    // organizationName is the brand, applicationName is the slug that
    // matches the binary, the desktop entry, the icons and the artifacts.
    // The human-readable name lives in applicationDisplayName, which is
    // what Qt shows in window titles and dialogs and which never reaches
    // the filesystem.
    //
    // verzeta-remote sets the same organization/application pair so both
    // processes resolve the same database. Changing either one here without
    // changing it there makes the daemon open a different, empty database.
    app.setOrganizationName(QStringLiteral("Verzeta"));
    app.setApplicationName(QStringLiteral("verzeta-studio"));
    app.setApplicationDisplayName(QStringLiteral("Verzeta Studio"));
    app.setOrganizationDomain(QStringLiteral("verzeta.com"));
    app.setApplicationVersion(QStringLiteral(VERZETA_VERSION_STR));

    // -----------------------------------------------------------------------
    // Parse command-line options. The only mode flag is --headless-remote
    // (skip the QML window load AND auto-start the wire daemon for paired
    // clients). Remaining flags override individual remote-daemon settings
    // with precedence CLI > persisted QSettings > built-in default.
    //
    // Parsed AFTER QApplication is constructed so app.arguments() reflects
    // any platform-specific argv munging Qt does, and so QSettings reads
    // from the correct organisation / application scope set above.
    // -----------------------------------------------------------------------
    QSettings cliSettings;
    const auto cliOptions = Verzeta::Utils::parseCommandLine(app.arguments(), cliSettings);
    if (cliOptions.hasError) {
        std::cerr << "verzeta-studio: " << cliOptions.errorMessage.toStdString()
                  << "\nRun with --help for usage." << std::endl;
        return 1;
    }

    // Loud warning when running headless without TLS — operator deploying
    // on a non-loopback interface without TLS is exposing the pairing
    // surface in plaintext. Not refused (loopback deployments are
    // legitimate); printed to stderr + journald so accidental no-TLS
    // surfaces in the operator's logs.
    if (cliOptions.headlessRemote && !cliOptions.remoteTls) {
        std::cerr << "verzeta-studio: WARNING: running --headless-remote "
                  << "without TLS.\n"
                  << "  Pairing codes and bearer tokens travel in plaintext.\n"
                  << "  For any non-loopback address, pass "
                  << "--remote-tls --remote-cert <path> --remote-key <path>,\n"
                  << "  The Remote Access dialog's TLS setting does not "
                  << "apply to --headless-remote." << std::endl;
    }

    // Icon-theme fallback chain. Order of resolution for any
    // QIcon::fromTheme("name") call:
    //   1. The system's active icon theme (via QIcon::themeName()).
    //   2. The fallback theme set here ("breeze") — used when (1) is
    //      empty or doesn't supply the requested name.
    //   3. The bundled qrc /icons/* resources for app-specific icons
    //      we ship.
    // Setting the fallback to "breeze" means apps running under GNOME
    // / Cinnamon / WSL pick up Breeze-named icons (`configure`,
    // `list-add`, `face-smile`, etc.) when Breeze is installed
    // system-wide OR bundled in the AppImage's
    // `usr/share/icons/breeze/`.
    // Icon-theme name on Windows is set by KIconTheme::initTheme()
    // (called pre-QApplication above). KIconTheme registers the
    // bundled icontheme.rcc, sets the active theme, and gives KF6
    // icon engines palette-aware mono-icon recolouring — so the
    // single `breeze` theme adapts to OS dark mode without needing
    // a separate `breeze-dark` directory. Linux relies on the
    // system theme (Plasma / GNOME / Cinnamon) and only needs the
    // fallback below to find Breeze names.
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));

    // Window + application icon. Try the bundled Qt resource first (shipped
    // with the executable, works regardless of desktop theme), then fall
    // back to a system theme lookup.
    QIcon appIcon(QStringLiteral(":/icons/verzeta-studio.svg"));
    if (appIcon.isNull()) {
        appIcon = QIcon(QStringLiteral(":/icons/verzeta-studio.png"));
    }
    if (appIcon.isNull()) {
        appIcon = QIcon::fromTheme(QStringLiteral("verzeta-studio"));
    }
    if (!appIcon.isNull()) {
        app.setWindowIcon(appIcon);
    }

    // Clean shutdown: without this, the QSystemTrayIcon owned by
    // NotificationManager keeps the app alive after the main window is
    // closed. quitOnLastWindowClosed is the default on QApplication but
    // we set it explicitly for clarity and hook aboutToQuit below.
    app.setQuitOnLastWindowClosed(true);

    // -----------------------------------------------------------------------
    // Startup splash — spawn the standalone `verzeta-splash` process.
    //
    // The splash lives in its OWN process so it animates smoothly on its
    // own GUI thread / event loop while THIS process does its blocking
    // startup (backend init + the QML engine load). It is spawned here,
    // before any heavy work, and terminated below once MainWindow has
    // painted its first frame. A missing splash binary is non-fatal —
    // startup simply proceeds without it.
    //
    // SKIPPED entirely in headless mode — there's no main window to
    // hand off to and no display to render the splash on.
    // -----------------------------------------------------------------------
    QProcess splashProcess;
    if (!cliOptions.headlessRemote) {
        QString splashName = QStringLiteral("verzeta-splash");
#ifdef Q_OS_WIN
        splashName += QStringLiteral(".exe");
#endif
        const QString splashPath =
            QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(splashName);
        if (QFileInfo::exists(splashPath)) {
            splashProcess.setProgram(splashPath);
            splashProcess.start();
        }
    }

    // Initialize all services (opens DB, runs migrations, creates service objects).
    // The CommandLineOptions go in BEFORE initialize() so AppController can fire
    // the autostart-remote hook at the end of its setup graph.
    AppController controller;
    controller.setCommandLineOptions(cliOptions);
    if (!controller.initialize()) {
        qCritical("Failed to initialize Verzeta Studio — exiting");
        return 1;
    }

    // Set up the QML engine and register C++ types. Even in headless mode
    // we construct the engine + run registerTypes() — services that ship
    // QML singletons (Chat, Tasks, Conversations, AgentSettings, Export,
    // Canvas*, Heartbeat*, etc.) register here, and the wire daemon's
    // SessionRouter relies on the local-session ChatController being
    // wired up the same way wire-side sessions are. Only the final
    // engine.loadFromModule() call (which actually opens the window)
    // is gated below.
    QQmlApplicationEngine engine;
    controller.registerTypes(engine);

    // Hook QML's Qt.quit() calls to the app's quit path so closing via
    // Ctrl+Q from a QML shortcut also tears down cleanly.
    QObject::connect(&engine, &QQmlApplicationEngine::quit, &app, &QCoreApplication::quit);

    // Hide the tray icon when the event loop is about to exit — without
    // this, some platforms (Wayland especially) keep the process alive
    // because the tray icon is still considered a "top-level window".
    QObject::connect(
        &app, &QCoreApplication::aboutToQuit, []() { NotificationManager::instance().shutdown(); });

#ifdef Q_OS_WIN
    // Windows-only: expose KF6 icon resolution to QML via
    // `image://icon/<name>` URLs. Kirigami.Icon resolves through
    // QIcon::fromTheme on its own, but any QML that uses
    // `Image { source: "image://icon/document-open" }` requires this
    // image provider. Linux gets palette-aware icons through Plasma's
    // pipeline directly; no provider registration needed.
    engine.addImageProvider(QStringLiteral("icon"), new KQuickIconProvider);
#endif

    if (!cliOptions.headlessRemote) {
        // Load the application shell — module registered in frontend/CMakeLists.txt
        engine.loadFromModule(QStringLiteral("org.verzeta.studio"), QStringLiteral("MainWindow"));

        if (engine.rootObjects().isEmpty()) {
            qCritical("QML engine failed to load MainWindow — exiting");
            if (splashProcess.state() != QProcess::NotRunning) {
                splashProcess.terminate();
            }
            return -1;
        }

        // Dismiss the splash once MainWindow has rendered its first frame, so
        // there is no visible gap between the splash and the main window.
        if (auto* mainWin = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst())) {
            QObject::connect(
                mainWin,
                &QQuickWindow::frameSwapped,
                &app,
                [&splashProcess]() {
                    if (splashProcess.state() != QProcess::NotRunning) {
                        splashProcess.terminate();
                    }
                },
                Qt::SingleShotConnection);
        } else if (splashProcess.state() != QProcess::NotRunning) {
            // Root object is not a QQuickWindow — close the splash now rather
            // than waiting for its self-timeout.
            splashProcess.terminate();
        }

        // Backstop: if `frameSwapped` never arrives (unusual render-loop or
        // compositor combinations), dismiss the splash a few seconds into the
        // event loop regardless — it must never linger as a ghost window over
        // the running app. Harmless no-op when the splash is already gone.
        QTimer::singleShot(4000, &app, [&splashProcess]() {
            if (splashProcess.state() != QProcess::NotRunning) {
                splashProcess.terminate();
            }
        });
    } else {
        // Headless mode — no window to render, no splash to dismiss.
        // AppController::initialize() has already fired the wire-daemon
        // autostart hook via the CommandLineOptions handed to it above.
        // We simply enter the event loop and serve paired clients until
        // the process is killed.
        qInfo("verzeta-studio: running --headless-remote (no QML loaded). "
              "Wire daemon listens for paired clients; signal SIGTERM to exit.");
    }

    return app.exec();
}

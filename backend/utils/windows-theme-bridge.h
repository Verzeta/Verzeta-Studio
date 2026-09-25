// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file windows-theme-bridge.h
 * @brief Windows-only OS-theme -> Qt/Kirigami palette bridge.
 *
 *        On Linux, Kirigami inherits its colour scheme from Plasma's
 *        QPalette pipeline, so light/dark/accent track the system
 *        without explicit work. Windows has no equivalent service, so
 *        the QPalette stays at whatever the default Qt
 *        `windows`/`windowsvista` style decides, and the Windows 10
 *        / 11 light / dark preference (registry: AppsUseLightTheme)
 *        is silently ignored. Kirigami components, which read the
 *        QPalette directly for their colour roles, end up rendering
 *        the wrong scheme regardless of how the user has Windows
 *        configured.
 *
 *        applyWindowsNativeTheme() reads the OS preference from the
 *        registry, sets the most native available QStyle
 *        (windows11 -> windowsvista -> Fusion), and applies a
 *        matching QPalette so Kirigami's colour-role lookups resolve
 *        sensibly.  This is the Windows analogue of "let Kirigami
 *        inherit from Plasma", NOT a user-driven palette override.
 *        Hard distinction:
 *
 *          - Avoided: qApp->setPalette(<custom>) driven by an in-app
 *            user-toggleable theme setting (fights Kirigami inheritance).
 *          - This file: qApp->setPalette(<OS-derived>) driven by the
 *            same OS theme signal Kirigami WOULD have inherited from
 *            on Linux.  There is no in-app toggle; the OS is the
 *            single source of truth.
 *
 *        The header is unconditionally compilable but the function
 *        is only declared / defined under #ifdef Q_OS_WIN, and the
 *        .cpp is added to the build via `if(WIN32)` in
 *        backend/CMakeLists.txt. The Linux binary contains zero
 *        Windows-theme symbols (verifiable with
 *        `nm verzeta-studio | grep -c WindowsNativeTheme` -> 0).
 *
 * @layer Utility (presentation)
 * @dependencies Qt6::Widgets (QApplication, QPalette, QStyle), Qt6::Core
 *               (QSettings registry read).
 */


#pragma once

// Pull in <qsystemdetection.h> via QtGlobal so Q_OS_WIN is actually
// defined when the guard below is evaluated. main.cpp happens to
// include QApplication before including this header, which would
// also make Q_OS_WIN available, but relying on consumer include
// order is a footgun -- include it ourselves so the header is
// self-contained.
#include <QtGlobal>

#ifdef Q_OS_WIN

class QApplication;

namespace Verzeta {

/**
 * @brief Probes the Windows registry for the OS-level light/dark
 *        preference. Returns true when AppsUseLightTheme == 0 (dark),
 *        false otherwise (light, or registry key missing). Public so
 *        main.cpp can pick the matching Breeze icon theme variant
 *        (breeze vs breeze-dark) at the same time the QPalette is
 *        applied -- without it the Windows app on dark mode renders
 *        light Breeze icons against a dark background, which is
 *        the user's actual reported bug.
 */
bool isWindowsDarkTheme();

}  // namespace Verzeta

class QColor;

namespace Verzeta {

/**
 * @brief Probes the Windows DWM registry for the user's per-account
 *        accent colour and returns it as a QColor. Reads
 *        HKCU\Software\Microsoft\Windows\DWM\AccentColor (DWORD,
 *        ABGR encoded -- yes Windows stores it byte-reversed vs
 *        RGBA). Falls back to ColorizationColor (older Windows 10
 *        versions) if AccentColor is missing. Returns an INVALID
 *        QColor (i.e. `.isValid() == false`) when both registry
 *        reads fail; callers should fall back to a hard-coded
 *        Microsoft default in that case.
 *
 *        Windows 10 1607+ and Windows 11 both populate AccentColor.
 *        Pre-1607 Windows 10 only exposes ColorizationColor in a
 *        slightly different format; we fall back to that and convert.
 */
QColor windowsAccentColor();

/**
 * @brief Merge the bundled Breeze{Dark,Light}.colors file into
 *        kdeglobals + write [Icons]/Theme=breeze + [General]/
 *        ColorScheme=BreezeDark|BreezeLight + [General]/Name +
 *        [General]/shadeSortColumn.
 *
 *        Independent of QStyle / QPalette setup. Needed for KIconLoader
 *        + KIconEngine to recolour mono SVG icons correctly --
 *        KIconEngine queries default KSharedConfig (kdeglobals) at
 *        icon-paint time, NOT QApplication::palette(). Without
 *        kdeglobals being written, Kirigami mono icons paint with the
 *        default-light foreground colour and end up black-on-dark in
 *        Windows dark mode regardless of QPalette.
 *
 *        Call site: main.cpp, UNCONDITIONALLY on Windows (both bridge
 *        and qt-override modes need icons to recolour, so the merge
 *        must run regardless of which QStyle/palette path is taken).
 *
 *        Reads dark/light from `isWindowsDarkTheme()`. .colors files
 *        come from <exe-dir>/data/color-schemes/. If the file is
 *        missing the call is a no-op (logs a warning, doesn't crash).
 */
void mergeBreezeColorsIntoKdeglobals();

/**
 * @brief Applies the OS-theme-driven QStyle + QPalette pair that
 *        the rest of the app (Kirigami included) uses for colour-role
 *        lookups. Idempotent and safe to call once per app start.
 *
 *        Resolution order:
 *          1. QStyle: windows11 (Qt 6.7+) -> windowsvista -> Fusion.
 *             First style that QStyleFactory::create() returns
 *             non-null wins.
 *          2. QPalette: built from a hard-coded approximation of
 *             Windows 11 light / dark colours. Choice between the
 *             two is driven by the AppsUseLightTheme DWORD at
 *             HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\
 *             Personalize. Missing key defaults to light.
 *
 *        Live theme switching (toggling Windows light/dark while the
 *        app is running) is NOT handled here, since it would require a
 *        QAbstractNativeEventFilter listening for WM_SETTINGCHANGE
 *        with `ImmersiveColorSet`. Out of scope for v1; restart-to-
 *        apply is the documented behaviour. Users who toggle dark
 *        mode while the app is open need to relaunch.
 *
 * @param app Reference to the QApplication that owns the global
 *            QStyle + QPalette. Must be constructed before this call.
 */
void applyWindowsNativeTheme(QApplication& app);

}  // namespace Verzeta

#endif  // Q_OS_WIN

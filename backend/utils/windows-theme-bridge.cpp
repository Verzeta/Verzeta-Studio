// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file windows-theme-bridge.cpp
 * @brief Implementation of the Windows-only OS-theme bridge.
 *        See windows-theme-bridge.h for design rationale + the
 *        OS-derived-palette-only distinction vs user-driven
 *        palette overrides.
 * @layer Utility (presentation)
 * @dependencies Qt6::Widgets (QApplication, QPalette, QStyle),
 *               Qt6::Core (QSettings registry read).  Compiles to
 *               an empty translation unit on non-Windows.
 */

// QtGlobal MUST come before the Q_OS_WIN check -- the macro is
// defined inside <qsystemdetection.h> which QtGlobal pulls in. With
// no Qt header included first, Q_OS_WIN is undefined when the
// preprocessor evaluates the guard, so the entire file compiles to
// an empty translation unit even on Windows. Symptom: the linker
// reports `undefined reference to applyWindowsNativeTheme` because
// the function definition was elided. Same guard hazard exists in
// the header -- the include chain in main.cpp happens to work today
// because main.cpp pulls in QApplication first, but defensive
// coding here keeps the .cpp self-contained.
#include <QtGlobal>

#ifdef Q_OS_WIN

#include "logger.h"
#include "windows-theme-bridge.h"

#include <KColorScheme>
#include <KConfigGroup>
#include <KSharedConfig>
#include <optional>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFileInfo>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QVariant>

namespace {

// Implementation moved out of anonymous namespace + renamed to the
// public Verzeta::isWindowsDarkTheme() defined further down. The
// previous local anonymous-namespace function survives only as a
// thin shim for backward source-stability inside this .cpp.
bool windowsIsDarkTheme();

/**
 * @brief Windows 11 light-theme palette approximation. Colour values
 *        are visually-matched against the WinUI 3 "Light" theme
 *        documented at
 *        https://learn.microsoft.com/en-us/windows/apps/design/style/color
 *        (exact decimal mappings, not interpolated). Highlight colour
 *        uses Microsoft's default accent (#0067C0) rather than the
 *        user's per-account accent because reading per-user accent
 *        out of HKCU\Software\Microsoft\Windows\DWM is brittle
 *        across Windows versions. Per-user accent integration is a
 *        future enhancement.
 */
// Resolve the Windows-native highlight + link colours. Reads the
// per-account accent from DWM\AccentColor; falls back to Microsoft's
// default (#0067C0 light / #4CC2FF dark) when the registry probe
// returns an invalid colour. On dark schemes we lighten the OS
// accent slightly so it stays legible against the dark window
// background -- matches how Windows 11's own controls render
// accent-on-dark.
struct AccentPair {
    QColor highlight;
    QColor link;
    QColor highlightedText;
};
AccentPair resolveAccent(bool dark) {
    AccentPair out;
    const QColor osAccent = Verzeta::windowsAccentColor();
    QColor base;
    if (osAccent.isValid()) {
        base = osAccent;
    } else {
        // Microsoft default accent (Windows 11 default blue).
        base = dark ? QColor(0x4C, 0xC2, 0xFF) : QColor(0x00, 0x67, 0xC0);
    }
    if (dark) {
        // Lighten 1.25x on dark theme so the accent reads against
        // the 0x202020 window background. lighter() uses HSL so the
        // hue stays anchored to the user's pick.
        out.highlight = base.lighter(125);
        out.highlightedText = QColor(0x00, 0x00, 0x00);
    } else {
        out.highlight = base;
        out.highlightedText = QColor(0xFF, 0xFF, 0xFF);
    }
    out.link = out.highlight;
    return out;
}

QPalette windowsLightPalette() {
    const AccentPair accent = resolveAccent(/*dark=*/false);
    QPalette p;
    p.setColor(QPalette::Window, QColor(0xF3, 0xF3, 0xF3));
    p.setColor(QPalette::WindowText, QColor(0x1F, 0x1F, 0x1F));
    p.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::AlternateBase, QColor(0xF7, 0xF7, 0xF7));
    p.setColor(QPalette::Text, QColor(0x1F, 0x1F, 0x1F));
    p.setColor(QPalette::Button, QColor(0xFB, 0xFB, 0xFB));
    p.setColor(QPalette::ButtonText, QColor(0x1F, 0x1F, 0x1F));
    p.setColor(QPalette::BrightText, QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::Mid, QColor(0xC8, 0xC8, 0xC8));
    p.setColor(QPalette::Midlight, QColor(0xE8, 0xE8, 0xE8));
    p.setColor(QPalette::Dark, QColor(0x9C, 0x9C, 0x9C));
    p.setColor(QPalette::Shadow, QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Highlight, accent.highlight);
    p.setColor(QPalette::HighlightedText, accent.highlightedText);
    p.setColor(QPalette::Link, accent.link);
    p.setColor(QPalette::LinkVisited, QColor(0x68, 0x21, 0x7A));
    p.setColor(QPalette::ToolTipBase, QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::ToolTipText, QColor(0x1F, 0x1F, 0x1F));
    p.setColor(QPalette::PlaceholderText, QColor(0x60, 0x60, 0x60));

    // Disabled-state colours match Windows native (greyed-out).
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0xA0, 0xA0, 0xA0));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0xA0, 0xA0, 0xA0));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0xA0, 0xA0, 0xA0));
    return p;
}

/**
 * @brief Windows 11 dark-theme palette approximation. Sourced from
 *        the same WinUI 3 reference (the "Dark" theme block).
 *        Highlight colour is the dark-theme variant of Microsoft's
 *        default accent (#4CC2FF).
 */
/**
 * @brief Resolve the absolute path to the bundled
 *        Breeze{Dark,Light}.colors file shipped at
 *        <exe-dir>/data/color-schemes/. Empty QString if the file
 *        is missing.
 */
QString breezeColorsFilePath(bool dark) {
    const QString fileName =
        dark ? QStringLiteral("BreezeDark.colors") : QStringLiteral("BreezeLight.colors");
    const QString fullPath =
        QCoreApplication::applicationDirPath() + QStringLiteral("/data/color-schemes/") + fileName;
    return QFileInfo::exists(fullPath) ? fullPath : QString();
}

/**
 * @brief Build a QPalette from a bundled Breeze*.colors file. NO
 *        side effects on kdeglobals -- that's
 *        Verzeta::mergeBreezeColorsIntoKdeglobals's job.
 *
 * Open the .colors file via KSharedConfig (KConfig::SimpleConfig so
 * it doesn't merge kdeglobals or hit XDG paths) and feed it to
 * KColorScheme::createApplicationPalette, which builds a full QPalette
 * covering every ColorRole + ColorGroup tuple the way Plasma builds
 * them on Linux. Layer the OS accent colour from
 * HKCU\Software\Microsoft\Windows\DWM\AccentColor on top of Breeze's
 * default Plasma-blue accent.
 *
 * Returns nullopt when the .colors file isn't present, signalling
 * the caller to fall back to the hardcoded WinUI-3 approximation.
 */
std::optional<QPalette> breezePaletteFromColorsFile(bool dark) {
    const QString fullPath = breezeColorsFilePath(dark);
    if (fullPath.isEmpty()) {
        qCWarning(verzetaUi) << "breezePaletteFromColorsFile: .colors file missing under"
                             << "<exe-dir>/data/color-schemes/ -- falling back to"
                             << "hardcoded WinUI-3 palette";
        return std::nullopt;
    }

    KSharedConfigPtr breezeConfig = KSharedConfig::openConfig(fullPath, KConfig::SimpleConfig);
    QPalette pal = KColorScheme::createApplicationPalette(breezeConfig);

    const AccentPair accent = resolveAccent(dark);
    pal.setColor(QPalette::Highlight, accent.highlight);
    pal.setColor(QPalette::HighlightedText, accent.highlightedText);
    pal.setColor(QPalette::Link, accent.link);

    qCInfo(verzetaUi) << "breezePaletteFromColorsFile: built QPalette from"
                      << QFileInfo(fullPath).fileName() << "with OS accent override";
    return pal;
}

QPalette windowsDarkPalette() {
    const AccentPair accent = resolveAccent(/*dark=*/true);
    QPalette p;
    p.setColor(QPalette::Window, QColor(0x20, 0x20, 0x20));
    p.setColor(QPalette::WindowText, QColor(0xF0, 0xF0, 0xF0));
    p.setColor(QPalette::Base, QColor(0x2D, 0x2D, 0x2D));
    p.setColor(QPalette::AlternateBase, QColor(0x32, 0x32, 0x32));
    p.setColor(QPalette::Text, QColor(0xF0, 0xF0, 0xF0));
    p.setColor(QPalette::Button, QColor(0x2D, 0x2D, 0x2D));
    p.setColor(QPalette::ButtonText, QColor(0xF0, 0xF0, 0xF0));
    p.setColor(QPalette::BrightText, QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::Mid, QColor(0x4A, 0x4A, 0x4A));
    p.setColor(QPalette::Midlight, QColor(0x35, 0x35, 0x35));
    p.setColor(QPalette::Dark, QColor(0x18, 0x18, 0x18));
    p.setColor(QPalette::Shadow, QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Highlight, accent.highlight);
    p.setColor(QPalette::HighlightedText, accent.highlightedText);
    p.setColor(QPalette::Link, accent.link);
    p.setColor(QPalette::LinkVisited, QColor(0xC0, 0x84, 0xD0));
    p.setColor(QPalette::ToolTipBase, QColor(0x32, 0x32, 0x32));
    p.setColor(QPalette::ToolTipText, QColor(0xF0, 0xF0, 0xF0));
    p.setColor(QPalette::PlaceholderText, QColor(0xA0, 0xA0, 0xA0));

    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x70, 0x70, 0x70));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x70, 0x70, 0x70));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x70, 0x70, 0x70));
    return p;
}

}  // anonymous namespace

namespace Verzeta {

// Public probe — see header.
bool isWindowsDarkTheme() {
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\"
                                 "CurrentVersion\\Themes\\Personalize"),
                  QSettings::NativeFormat);
    const QVariant v = reg.value(QStringLiteral("AppsUseLightTheme"));
    if (!v.isValid()) {
        return false;
    }
    return v.toInt() == 0;
}

// Public accent-colour probe — see header.
QColor windowsAccentColor() {
    QSettings dwm(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\DWM"),
                  QSettings::NativeFormat);

    // AccentColor: DWORD encoded as ABGR (Microsoft byte-reversed
    // their RGBA -- a long-standing Windows quirk). Bit layout:
    //   0xAABBGGRR
    //   ^^         alpha
    //     ^^       blue
    //       ^^     green
    //         ^^   red
    auto unpackAbgr = [](quint32 abgr) -> QColor {
        const int r = static_cast<int>(abgr & 0xFF);
        const int g = static_cast<int>((abgr >> 8) & 0xFF);
        const int b = static_cast<int>((abgr >> 16) & 0xFF);
        // Alpha is per-pixel-blend metadata Windows uses for the
        // taskbar; for our QPalette we want fully-opaque, so ignore
        // the alpha byte rather than honour its 0x00 default.
        return QColor(r, g, b, 255);
    };

    const QVariant accent = dwm.value(QStringLiteral("AccentColor"));
    if (accent.isValid()) {
        bool ok = false;
        const quint32 raw = accent.toUInt(&ok);
        if (ok) {
            const QColor c = unpackAbgr(raw);
            if (c.isValid())
                return c;
        }
    }

    // Fallback for pre-1607 Windows 10: ColorizationColor uses a
    // similar DWORD but in 0xAARRGGBB order (NOT byte-reversed).
    // Mostly historical; modern Windows always populates AccentColor.
    const QVariant coloriz = dwm.value(QStringLiteral("ColorizationColor"));
    if (coloriz.isValid()) {
        bool ok = false;
        const quint32 raw = coloriz.toUInt(&ok);
        if (ok) {
            const int a = static_cast<int>((raw >> 24) & 0xFF);
            const int r = static_cast<int>((raw >> 16) & 0xFF);
            const int g = static_cast<int>((raw >> 8) & 0xFF);
            const int b = static_cast<int>(raw & 0xFF);
            Q_UNUSED(a);
            return QColor(r, g, b, 255);
        }
    }

    return QColor();  // invalid -- caller falls back to MS default
}

}  // namespace Verzeta

// Anonymous-namespace shim that delegates to the public probe so
// existing call sites further down don't need to be rewritten.
namespace {
bool windowsIsDarkTheme() {
    return Verzeta::isWindowsDarkTheme();
}
}  // namespace

namespace Verzeta {

void mergeBreezeColorsIntoKdeglobals() {
    const bool dark = isWindowsDarkTheme();
    const QString fullPath = breezeColorsFilePath(dark);
    const QString schemeName = dark ? QStringLiteral("BreezeDark") : QStringLiteral("BreezeLight");

    if (fullPath.isEmpty()) {
        qCWarning(verzetaUi) << "mergeBreezeColorsIntoKdeglobals: Breeze .colors file"
                             << "missing under <exe-dir>/data/color-schemes/ -- icons"
                             << "won't recolour correctly in dark mode. Re-run deploy.";
        return;
    }

    KSharedConfigPtr breezeConfig = KSharedConfig::openConfig(fullPath, KConfig::SimpleConfig);
    KSharedConfigPtr kdeglobals = KSharedConfig::openConfig();

    int copiedGroups = 0;
    int copiedKeys = 0;
    const QStringList groups = breezeConfig->groupList();
    for (const QString& groupName : groups) {
        KConfigGroup src = breezeConfig->group(groupName);
        KConfigGroup dst = kdeglobals->group(groupName);
        const QStringList keys = src.keyList();
        for (const QString& key : keys) {
            // Copy as raw QString -- every KColorScheme key
            // (RGB tuples, floats, ints, strings) round-trips through
            // QString without type-specific readEntry overloads.
            const QString value = src.readEntry(key, QString());
            dst.writeEntry(key, value);
            ++copiedKeys;
        }
        ++copiedGroups;
    }

    // [General]/ColorScheme + Name + shadeSortColumn -- match the
    // shape of Plasma's bigscreen kdeglobals reference.
    KConfigGroup general(kdeglobals, QStringLiteral("General"));
    general.writeEntry(QStringLiteral("ColorScheme"), schemeName);
    general.writeEntry(QStringLiteral("Name"),
                       dark ? QStringLiteral("Breeze Dark") : QStringLiteral("Breeze"));
    general.writeEntry(QStringLiteral("shadeSortColumn"), true);

    // [Icons]/Theme tells KIconLoader (used by KIconEngine) which
    // icon theme to load+recolour. Without this entry, KIconLoader
    // falls back to a hardcoded default which doesn't match the
    // theme KIconTheme::initTheme registered, and the recolour path
    // misses entirely. .colors files do NOT include this key (it
    // lives in kdeglobals proper, not in colour schemes), so we
    // write it explicitly. Reference: Plasma's bigscreen kdeglobals.
    KConfigGroup icons(kdeglobals, QStringLiteral("Icons"));
    icons.writeEntry(QStringLiteral("Theme"), QStringLiteral("breeze"));

    kdeglobals->sync();

    qCInfo(verzetaUi) << "mergeBreezeColorsIntoKdeglobals: scheme=" << schemeName << "merged"
                      << copiedKeys << "keys across" << copiedGroups
                      << "groups + Icons.Theme=breeze";
}

void applyWindowsNativeTheme(QApplication& app) {
    // Probe styles in descending order of "nativeness". The
    // windows11 style was added in Qt 6.7 and respects the OS accent
    // colour internally for control rendering. windowsvista is the
    // pre-6.7 native style. Fusion is the cross-platform fallback —
    // works everywhere but doesn't pick up OS-specific fit-and-finish
    // (rounded corners, mica, etc.).
    const QStringList preferred = {
        QStringLiteral("windows11"),
        QStringLiteral("windowsvista"),
        QStringLiteral("Fusion"),
    };
    QString chosenStyleName;
    for (const QString& name : preferred) {
        QStyle* style = QStyleFactory::create(name);
        if (style) {
            app.setStyle(style);
            chosenStyleName = name;
            break;
        }
    }
    if (chosenStyleName.isEmpty()) {
        // QStyleFactory's "windows" style is always present on
        // Windows builds of Qt; if even that isn't there we have a
        // bigger problem than this bridge can fix. Log + return,
        // leaving Qt's default style as-is.
        qCWarning(verzetaUi) << "applyWindowsNativeTheme: no preferred QStyle available;"
                             << "available styles:" << QStyleFactory::keys();
        return;
    }

    // Apply the OS-theme-matched colour scheme. Two layers:
    //
    //   QPalette       -- consumed by Kirigami components for colour
    //                     roles. We set this via QApplication::setPalette.
    //   kdeglobals     -- consumed by KIconEngine when recolouring mono
    //                     SVG icons. KIconEngine constructs
    //                     KColorScheme(Active, Window) without a config
    //                     arg, which reads default KSharedConfig
    //                     (kdeglobals). Without the merge, this is
    //                     empty and KColorScheme returns default-light
    //                     colours -- mono icons paint black-on-dark.
    //
    // breezePaletteFromColorsFile does BOTH: builds the QPalette via
    // KColorScheme::createApplicationPalette + merges the .colors
    // content into kdeglobals via KConfigGroup::writeEntry. Returns
    // nullopt if the .colors file isn't bundled (deploy regression);
    // we fall back to the hardcoded WinUI-3 palette in that case.
    // Note: even the WinUI-3 fallback won't make icons recolour
    // correctly because it doesn't write kdeglobals. The fallback is
    // for "deploy is broken, keep the app usable" not for "icons work".
    const bool dark = windowsIsDarkTheme();
    QString paletteSource;
    QPalette pal;
    if (auto maybe = breezePaletteFromColorsFile(dark); maybe.has_value()) {
        pal = *maybe;
        paletteSource = QStringLiteral("Breeze.colors+kdeglobals-merge");
    } else {
        pal = dark ? windowsDarkPalette() : windowsLightPalette();
        paletteSource = QStringLiteral("WinUI-3-hardcoded-fallback");
    }
    app.setPalette(pal);

    qCInfo(verzetaUi) << "applyWindowsNativeTheme: style=" << chosenStyleName
                      << "scheme=" << (dark ? "dark" : "light") << "palette=" << paletteSource;
}

}  // namespace Verzeta

#endif  // Q_OS_WIN

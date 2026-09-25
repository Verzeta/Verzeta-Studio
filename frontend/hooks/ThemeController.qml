// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
pragma Singleton
import QtQuick
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

QtObject {
    id: themeController

    property real fontSize: SettingsService.fontSize

    property string fontFamily: {
        const f = SettingsService.fontFamily;
        return f.length > 0 ? f : "Sans Serif";
    }

    property string codeFontFamily: {
        const f = SettingsService.codeFontFamily;
        return f.length > 0 ? f : "Monospace";
    }

    readonly property bool isDarkMode: Kirigami.Theme.backgroundColor.hslLightness < 0.5

    readonly property color codeBackground: "#1e1e1e"

    readonly property color codeText: "#d4d4d4"

    readonly property color surfacePage: Kirigami.Theme.backgroundColor

    readonly property color surfaceCard: isDarkMode ? Qt.lighter(Kirigami.Theme.backgroundColor, 1.40) : Qt.darker(Kirigami.Theme.backgroundColor, 1.04)

    readonly property color surfaceSunken: isDarkMode ? Qt.darker(Kirigami.Theme.backgroundColor, 1.25) : Qt.darker(Kirigami.Theme.backgroundColor, 1.12)

    readonly property color borderSubtle: isDarkMode ? Qt.lighter(Kirigami.Theme.backgroundColor, 1.80) : Qt.darker(Kirigami.Theme.backgroundColor, 1.18)

    readonly property color borderStrong: isDarkMode ? Qt.lighter(Kirigami.Theme.backgroundColor, 2.40) : Qt.darker(Kirigami.Theme.backgroundColor, 1.32)

    readonly property color hoverTint: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.10)

    readonly property color pressTint: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.18)

    readonly property color selectionTint: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.15)

    readonly property int radius: 6

    signal themeChanged

    onFontFamilyChanged: themeChanged()
    onCodeFontFamilyChanged: themeChanged()
    onFontSizeChanged: themeChanged()
}

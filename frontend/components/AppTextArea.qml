// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.TextArea {
    id: control

    background: Rectangle {
        color: ThemeController.surfaceSunken
        radius: ThemeController.radius
        border.width: 1
        border.color: {
            if (control.activeFocus === true)
                return Kirigami.Theme.highlightColor;
            return ThemeController.borderSubtle;
        }
    }
}

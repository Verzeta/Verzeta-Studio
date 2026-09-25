// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.AbstractButton {
    id: root

    property string label: ""

    property bool selected: false

    property bool compactTouchTarget: false

    hoverEnabled: true
    padding: 0

    implicitHeight: compactTouchTarget ? Math.max(Kirigami.Units.gridUnit * 2.4, 44) : Math.max(Kirigami.Units.gridUnit * 1.8, 32)
    implicitWidth: contentLabel.implicitWidth + Kirigami.Units.largeSpacing * 2

    background: Rectangle {
        radius: root.implicitHeight * 0.5
        color: {
            if (root.selected) {
                return ThemeController.selectionTint;
            }
            if (root.hovered) {
                return ThemeController.hoverTint;
            }
            return "transparent";
        }
        border.color: root.selected ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
        border.width: 1
        Behavior on color  {
            ColorAnimation {
                duration: 100
            }
        }
        Behavior on border.color  {
            ColorAnimation {
                duration: 100
            }
        }
    }

    contentItem: Controls.Label {
        id: contentLabel
        text: root.label
        color: root.selected ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
        font.bold: root.selected
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}

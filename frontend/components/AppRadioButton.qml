// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Controls.RadioButton {
    id: control

    indicator: Item {
        id: indicatorRoot

        readonly property real diameter: Math.round(Kirigami.Units.gridUnit * 0.9)

        implicitWidth: diameter + control.spacing
        implicitHeight: diameter
        width: implicitWidth
        height: implicitHeight

        x: control.leftPadding
        y: control.topPadding + Math.round((control.availableHeight - height) / 2)

        Rectangle {
            id: ring

            width: indicatorRoot.diameter
            height: indicatorRoot.diameter
            radius: width / 2
            antialiasing: true
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter

            color: {
                if (!control.enabled)
                    return Kirigami.Theme.alternateBackgroundColor;
                return Kirigami.Theme.backgroundColor;
            }

            border.width: 1
            border.color: {
                if (!control.enabled)
                    return Kirigami.Theme.disabledTextColor;
                if (control.checked)
                    return Kirigami.Theme.highlightColor;
                if (control.hovered)
                    return Kirigami.Theme.highlightColor;
                if (control.visualFocus)
                    return Kirigami.Theme.highlightColor;
                return Kirigami.Theme.disabledTextColor;
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.round(indicatorRoot.diameter * 0.5)
                height: width
                radius: width / 2
                antialiasing: true
                visible: control.checked
                color: {
                    if (!control.enabled)
                        return Kirigami.Theme.disabledTextColor;
                    return Kirigami.Theme.highlightColor;
                }
            }
        }
    }
}

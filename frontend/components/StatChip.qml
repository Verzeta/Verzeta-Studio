// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: chip

    property string iconSource: ""
    property string iconFallback: ""
    property string value: ""
    property string tip: ""
    property color valueColor: Kirigami.Theme.textColor

    implicitWidth: chipContent.implicitWidth + 16
    implicitHeight: 26
    radius: ThemeController.radius
    color: ThemeController.surfaceSunken
    border.width: 1
    border.color: ThemeController.borderSubtle

    RowLayout {
        id: chipContent
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 5

        Kirigami.Icon {
            source: chip.iconSource
            fallback: chip.iconFallback
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            Layout.alignment: Qt.AlignVCenter
            color: Kirigami.Theme.disabledTextColor
            visible: chip.iconSource.length > 0
        }

        Controls.Label {
            text: chip.value
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            color: chip.valueColor
            elide: Text.ElideMiddle
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
        }
    }

    HoverHandler {
        id: chipHover
    }
    Controls.ToolTip.text: chip.tip
    Controls.ToolTip.visible: chip.tip.length > 0 && chipHover.hovered
    Controls.ToolTip.delay: 500
}

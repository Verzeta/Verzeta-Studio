// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: root

    property string status: "idle"

    readonly property color _accent: {
        if (root.status === "active")
            return Kirigami.Theme.positiveTextColor;
        if (root.status === "archived")
            return Kirigami.Theme.disabledTextColor;
        return Kirigami.Theme.neutralTextColor;
    }

    color: Qt.rgba(_accent.r, _accent.g, _accent.b, 0.18)
    border.color: _accent
    border.width: 1

    implicitHeight: Kirigami.Units.gridUnit * 1.2
    implicitWidth: rowLayout.implicitWidth + Kirigami.Units.smallSpacing * 3
    radius: implicitHeight * 0.5

    RowLayout {
        id: rowLayout
        anchors.centerIn: parent
        spacing: Kirigami.Units.smallSpacing

        Rectangle {
            visible: root.status === "active"
            implicitWidth: root.implicitHeight * 0.32
            implicitHeight: implicitWidth
            radius: implicitWidth * 0.5
            color: root._accent
            Layout.alignment: Qt.AlignVCenter
        }

        Controls.Label {
            text: root.status
            color: root._accent
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.78
            font.bold: true
            Layout.alignment: Qt.AlignVCenter
        }
    }
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Rectangle {
    id: root

    property string agentName: ""

    property real diameter: Kirigami.Units.gridUnit * 1.6

    width: diameter
    height: diameter
    radius: diameter * 0.5

    border.color: Kirigami.Theme.backgroundColor
    border.width: Math.max(1, diameter * 0.07)

    color: {
        if (root.agentName.length === 0) {
            return Kirigami.Theme.disabledTextColor;
        }
        var hash = 0;
        for (var i = 0; i < root.agentName.length; ++i) {
            hash = ((hash << 5) - hash) + root.agentName.charCodeAt(i);
            hash |= 0;
        }
        var hue = Math.abs(hash) % 360;
        return Qt.hsla(hue / 360.0, 0.50, 0.55, 1.0);
    }

    Controls.Label {
        anchors.centerIn: parent
        text: root.agentName.length > 0 ? root.agentName.charAt(0).toUpperCase() : "?"
        color: "white"
        font.bold: true
        font.pixelSize: root.diameter * 0.5
    }
}

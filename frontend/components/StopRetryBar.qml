// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

RowLayout {
    id: root

    signal stopClicked

    signal retryClicked

    property bool showRetry: false

    spacing: 8
    Layout.alignment: Qt.AlignHCenter

    Controls.Button {
        id: stopBtn
        text: qsTr("Stop Generating")
        icon.name: "media-playback-stop"
        visible: ChatController.isGenerating
        highlighted: true
        onClicked: root.stopClicked()

        Controls.ToolTip {
            text: qsTr("Stop the current generation")
            visible: stopBtn.hovered
        }
    }

    Controls.Button {
        id: retryBtn
        text: qsTr("Retry")
        icon.name: "view-refresh"
        visible: root.showRetry && !ChatController.isGenerating
        flat: true
        onClicked: root.retryClicked()

        Controls.ToolTip {
            text: qsTr("Remove last response and retry")
            visible: retryBtn.hovered
        }
    }
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: root

    required property string fileName

    required property string mimeType

    required property int index

    signal removeClicked(int index)

    color: ThemeController.surfaceCard
    radius: height / 2
    implicitWidth: chipRow.implicitWidth + 16
    implicitHeight: 32

    RowLayout {
        id: chipRow
        anchors.centerIn: parent
        spacing: 4

        Kirigami.Icon {
            source: {
                const m = root.mimeType.toLowerCase();
                if (m.startsWith("image/"))
                    return "image-x-generic";
                if (m.startsWith("audio/"))
                    return "audio-x-generic";
                if (m.startsWith("video/"))
                    return "video-x-generic";
                if (m.startsWith("text/"))
                    return "text-x-generic";
                if (m === "application/pdf")
                    return "application-pdf";
                return "application-x-generic";
            }
            fallback: "application-x-generic"
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
        }

        Controls.Label {
            id: nameLabel
            text: root.fileName.length > 20 ? root.fileName.substring(0, 17) + "\u2026" : root.fileName
            font.pointSize: 9
            elide: Text.ElideRight

            HoverHandler {
                id: nameHover
            }

            Controls.ToolTip {
                text: root.fileName
                visible: root.fileName.length > 20 && nameHover.hovered
                delay: 500
            }
        }

        Controls.ToolButton {
            icon.name: "window-close"
            icon.width: 12
            icon.height: 12
            implicitWidth: 20
            implicitHeight: 20
            padding: 0
            flat: true

            onClicked: root.removeClicked(root.index)

            Controls.ToolTip.text: qsTr("Remove attachment")
            Controls.ToolTip.visible: hovered
            Controls.ToolTip.delay: 500
        }
    }
}

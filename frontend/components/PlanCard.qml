// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: root

    property var steps: []

    color: ThemeController.surfaceCard
    radius: ThemeController.radius
    implicitHeight: planLayout.implicitHeight + 16
    Layout.fillWidth: true

    ColumnLayout {
        id: planLayout
        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        Controls.Label {
            text: "Execution Plan"
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Repeater {
            model: root.steps

            delegate: RowLayout {
                spacing: 8
                Layout.fillWidth: true

                Controls.Label {
                    text: (index + 1) + "."
                    font.bold: true
                    Layout.preferredWidth: 20
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }

                Kirigami.Icon {
                    source: {
                        switch (modelData.status) {
                        case "pending":
                            return "chronometer";
                        case "running":
                            return "view-refresh";
                        case "complete":
                            return "dialog-ok-apply";
                        case "error":
                            return "dialog-error";
                        default:
                            return "chronometer";
                        }
                    }
                    fallback: "chronometer"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small

                    RotationAnimator on rotation  {
                        running: modelData.status === "running"
                        from: 0
                        to: 360
                        duration: 1200
                        loops: Animation.Infinite
                    }
                }

                Controls.Label {
                    text: modelData.description
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    font.strikeout: modelData.status === "complete"
                    color: modelData.status === "error" ? Kirigami.Theme.negativeTextColor : (modelData.status === "pending" ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.textColor)
                }
            }
        }

        Controls.Label {
            visible: root.steps.length === 0
            text: "Plan not yet generated"
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
        }
    }
}

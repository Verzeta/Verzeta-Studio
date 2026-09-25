// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: root

    property alias model: stepRepeater.model

    color: ThemeController.surfaceCard
    radius: ThemeController.radius
    implicitHeight: panelLayout.implicitHeight + 16
    Layout.fillWidth: true

    ColumnLayout {
        id: panelLayout
        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Kirigami.Icon {
                source: "system-run-symbolic"
                fallback: "system-run"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }

            Controls.Label {
                text: "Agent Progress"
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Item {
                Layout.fillWidth: true
            }

            Controls.Label {
                text: "Step " + AgentService.currentIteration
                font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                color: Kirigami.Theme.disabledTextColor
                visible: AgentService.isRunning
            }
        }

        Repeater {
            id: stepRepeater

            delegate: RowLayout {
                spacing: 8
                Layout.fillWidth: true

                Kirigami.Icon {
                    source: {
                        switch (model.status) {
                        case "running":
                            return "view-refresh";
                        case "success":
                            return "dialog-ok-apply";
                        case "error":
                            return "dialog-error";
                        default:
                            return "chronometer";
                        }
                    }
                    fallback: "dialog-information"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small

                    RotationAnimator on rotation  {
                        running: model.status === "running"
                        from: 0
                        to: 360
                        duration: 1200
                        loops: Animation.Infinite
                    }
                }

                Controls.Label {
                    text: model.description
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: model.status === "error" ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                    opacity: model.status === "pending" ? 0.5 : 1.0
                }
            }
        }

        Controls.BusyIndicator {
            visible: AgentService.isRunning
            running: visible
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 24
            implicitHeight: 24
        }
    }
}

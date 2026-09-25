// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: root

    required property string toolName

    required property string arguments

    required property string result

    required property string status

    color: ThemeController.surfaceCard
    radius: ThemeController.radius

    border.width: 2
    border.color: {
        switch (root.status) {
        case "running":
            return Kirigami.Theme.highlightColor;
        case "success":
            return Kirigami.Theme.positiveTextColor;
        case "error":
            return Kirigami.Theme.negativeTextColor;
        default:
            return ThemeController.borderSubtle;
        }
    }

    implicitHeight: cardLayout.implicitHeight + Kirigami.Units.largeSpacing
    Layout.fillWidth: true

    ColumnLayout {
        id: cardLayout
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true

            Kirigami.Icon {
                source: {
                    switch (root.status) {
                    case "pending":
                        return "chronometer";
                    case "running":
                        return "view-refresh";
                    case "success":
                        return "dialog-ok-apply";
                    case "error":
                        return "dialog-error";
                    default:
                        return "dialog-question";
                    }
                }
                fallback: "dialog-information"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }

            Controls.Label {
                text: root.toolName
                font.bold: true
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Controls.ToolButton {
                id: expandBtn
                icon.name: detailsArea.visible ? "arrow-up" : "arrow-down"
                visible: root.status === "success" || root.status === "error"
                onClicked: detailsArea.visible = !detailsArea.visible

                Controls.ToolTip {
                    text: detailsArea.visible ? qsTr("Collapse result") : qsTr("Expand result")
                    visible: expandBtn.hovered
                    delay: 400
                }
            }
        }

        Controls.Label {
            text: root.arguments
            font.family: ThemeController.codeFontFamily
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            color: Kirigami.Theme.disabledTextColor
            wrapMode: Text.Wrap
            elide: Text.ElideRight
            maximumLineCount: 2
            Layout.fillWidth: true
        }

        ColumnLayout {
            id: detailsArea
            visible: root.status === "success" || root.status === "error"
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            Controls.Label {
                text: root.status === "error" ? qsTr("Error:") : qsTr("Result:")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                color: root.status === "error" ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.positiveTextColor
            }

            Controls.TextArea {
                text: root.result
                readOnly: true
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: TextEdit.Wrap
                Layout.fillWidth: true
                Layout.maximumHeight: 200
                background: Rectangle {
                    color: ThemeController.surfaceSunken
                    radius: ThemeController.radius
                }
            }
        }

        Controls.BusyIndicator {
            visible: root.status === "running"
            running: visible
            implicitWidth: Kirigami.Units.iconSizes.medium
            implicitHeight: Kirigami.Units.iconSizes.medium
            Layout.alignment: Qt.AlignHCenter
        }
    }
}

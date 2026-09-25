// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: card

    property string displayName: ""
    property string baseUrl: ""
    property int modelCount: 0
    property bool supportsStreaming: true
    property bool supportsToolCalling: true
    property bool supportsVision: false

    signal editClicked
    signal deleteClicked

    Layout.fillWidth: true
    implicitHeight: 64
    radius: ThemeController.radius
    color: hoverHandler.hovered ? ThemeController.hoverTint : ThemeController.surfaceCard
    border.color: ThemeController.borderSubtle
    border.width: 1
    Behavior on color  {
        ColorAnimation {
            duration: 80
        }
    }

    HoverHandler {
        id: hoverHandler
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        Kirigami.Icon {
            source: "network-server"
            fallback: "preferences-system"
            implicitWidth: Kirigami.Units.iconSizes.medium
            implicitHeight: Kirigami.Units.iconSizes.medium
            color: Kirigami.Theme.textColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Controls.Label {
                text: card.displayName
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                color: Kirigami.Theme.textColor
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            RowLayout {
                spacing: 10
                Layout.fillWidth: true
                Controls.Label {
                    text: card.baseUrl
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    color: Kirigami.Theme.disabledTextColor
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Controls.Label {
                    visible: applicationWindow().isCompact ? false : card.supportsStreaming
                    text: qsTr("· streaming")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                    color: Kirigami.Theme.disabledTextColor
                }
                Controls.Label {
                    visible: applicationWindow().isCompact ? false : card.supportsToolCalling
                    text: qsTr("· tools")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                    color: Kirigami.Theme.disabledTextColor
                }
                Controls.Label {
                    visible: applicationWindow().isCompact ? false : card.supportsVision
                    text: qsTr("· vision")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                    color: Kirigami.Theme.disabledTextColor
                }
            }
        }

        Rectangle {
            implicitHeight: modelsLbl.implicitHeight + 8
            implicitWidth: modelsLbl.implicitWidth + 16
            radius: implicitHeight / 2
            color: card.modelCount > 0 ? Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.18) : Qt.rgba(Kirigami.Theme.disabledTextColor.r, Kirigami.Theme.disabledTextColor.g, Kirigami.Theme.disabledTextColor.b, 0.18)
            Controls.Label {
                id: modelsLbl
                anchors.centerIn: parent
                text: card.modelCount > 0 ? qsTr("%1 model%2").arg(card.modelCount).arg(card.modelCount === 1 ? "" : "s") : qsTr("No models")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                color: card.modelCount > 0 ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
            }
        }

        AppButton {
            text: qsTr("Edit")
            onClicked: card.editClicked()
            implicitWidth: 72
        }

        AppButton {
            text: qsTr("Delete")
            onClicked: card.deleteClicked()
            implicitWidth: 80
        }
    }
}

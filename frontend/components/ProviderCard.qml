// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: card

    property string providerName: ""
    property string iconName: "preferences-system"
    property string status: ""
    property string statusKind: "info"
    property string subtitle: ""

    signal configure

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
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: card.configure()
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        Kirigami.Icon {
            source: card.iconName
            fallback: "preferences-system"
            implicitWidth: Kirigami.Units.iconSizes.medium
            implicitHeight: Kirigami.Units.iconSizes.medium
            color: Kirigami.Theme.textColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Controls.Label {
                text: card.providerName
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                color: Kirigami.Theme.textColor
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Controls.Label {
                visible: card.subtitle.length > 0
                text: card.subtitle
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                color: Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        Rectangle {
            visible: applicationWindow().isCompact ? false : (card.status.length > 0)
            implicitHeight: statusLbl.implicitHeight + 8
            implicitWidth: statusLbl.implicitWidth + 16
            radius: implicitHeight / 2
            color: {
                if (card.statusKind === "connected") {
                    return Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.18);
                }
                if (card.statusKind === "unconfigured") {
                    return Qt.rgba(Kirigami.Theme.disabledTextColor.r, Kirigami.Theme.disabledTextColor.g, Kirigami.Theme.disabledTextColor.b, 0.18);
                }
                return Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.18);
            }
            Controls.Label {
                id: statusLbl
                anchors.centerIn: parent
                text: card.status
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                color: {
                    if (card.statusKind === "connected")
                        return Kirigami.Theme.positiveTextColor;
                    if (card.statusKind === "unconfigured")
                        return Kirigami.Theme.disabledTextColor;
                    return Kirigami.Theme.textColor;
                }
            }
        }

        Controls.Button {
            text: card.statusKind === "unconfigured" ? qsTr("Set up") : qsTr("Edit")
            onClicked: card.configure()
            implicitWidth: 80
        }
    }
}

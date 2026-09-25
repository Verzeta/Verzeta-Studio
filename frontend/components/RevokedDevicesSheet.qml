// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: sheet
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 40)

    readonly property bool _isMobile: applicationWindow().width < Kirigami.Units.gridUnit * 36
    readonly property color _cPanel: Qt.darker(Kirigami.Theme.backgroundColor, 0.6)
    readonly property color _cWell: Qt.darker(Kirigami.Theme.backgroundColor, 1.15)

    property ListModel clientsModel

    title: qsTr("Revoked devices")
    subtitle: qsTr("HOST · AUDIT HISTORY")
    dialogIcon: "view-history"

    footer: RowLayout {
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Close")
            onClicked: sheet.close()
        }
    }

    ColumnLayout {
        Layout.margins: Kirigami.Units.gridUnit
        spacing: Kirigami.Units.gridUnit

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: sheet._cPanel
            antialiasing: true
            implicitHeight: explainerCol.implicitHeight + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: explainerCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Devices whose tokens have been invalidated. " + "Listed here for audit; they cannot reconnect " + "without re-pairing.")
                    color: Kirigami.Theme.disabledTextColor
                    font.family: ThemeController.fontFamily
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: sheet._cPanel
            antialiasing: true
            implicitHeight: listCol.implicitHeight + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: listCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Revoked (%1)").arg(sheet.clientsModel ? sheet.clientsModel.count : 0)
                    font.family: ThemeController.fontFamily
                }

                Controls.Label {
                    Layout.fillWidth: true
                    visible: !sheet.clientsModel || sheet.clientsModel.count === 0
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    text: qsTr("No revoked devices.")
                }

                Repeater {
                    model: sheet.clientsModel
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: index === 0 ? Kirigami.Units.smallSpacing : 0
                        radius: ThemeController.radius
                        color: sheet._cWell
                        antialiasing: true
                        implicitHeight: rowCol.implicitHeight + Kirigami.Units.gridUnit

                        ColumnLayout {
                            id: rowCol
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: parent.top
                                margins: Kirigami.Units.smallSpacing * 2
                            }
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                Kirigami.Icon {
                                    source: "view-history"
                                    fallback: "user-trash"
                                    color: Kirigami.Theme.disabledTextColor
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                                }
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: model.name && model.name.length > 0 ? model.name : qsTr("(unnamed)")
                                    font.bold: true
                                    font.family: ThemeController.fontFamily
                                    elide: Text.ElideRight
                                    wrapMode: Text.WrapAnywhere
                                }
                                Controls.Label {
                                    text: qsTr("Revoked")
                                    color: Kirigami.Theme.negativeTextColor
                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                    font.family: ThemeController.fontFamily
                                }
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                Layout.leftMargin: Kirigami.Units.iconSizes.smallMedium + Kirigami.Units.smallSpacing
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                wrapMode: Text.WordWrap
                                text: {
                                    const last = Number(model.last_seen_at || 0);
                                    return last > 0 ? qsTr("Last seen %1").arg(new Date(last).toLocaleString(Qt.locale(), Locale.ShortFormat)) : qsTr("Never connected");
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

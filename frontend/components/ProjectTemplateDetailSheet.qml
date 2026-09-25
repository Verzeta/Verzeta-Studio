// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: detail
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: Math.min(applicationWindow().width * 0.92, Kirigami.Units.gridUnit * 48)
    implicitHeight: Math.min(applicationWindow().height * 0.88, Kirigami.Units.gridUnit * 46)

    readonly property bool _isMobile: applicationWindow().width < Kirigami.Units.gridUnit * 36
    readonly property color _cPanel: Qt.darker(Kirigami.Theme.backgroundColor, 0.6)

    property var _template: ({})
    property var _roster: []

    signal quickStartRequested(var templateData)

    function openForTemplate(t) {
        detail._template = t || ({});
        detail._roster = (t && t.id) ? ProjectTemplates.templateRoster(t.id) : [];
        detail.open();
    }

    title: (detail._template && detail._template.name) ? detail._template.name : qsTr("Template")
    subtitle: (detail._template && detail._template.tagLabel) ? detail._template.tagLabel : ""
    dialogIcon: "folder-templates"

    footer: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Close")
            onClicked: detail.close()
        }
        AppButton {
            text: qsTr("⚡  Quick start")
            highlighted: true
            onClicked: {
                var t = detail._template;
                detail.close();
                detail.quickStartRequested(t);
            }
        }
    }

    Item {
        id: bodyRoot
        implicitWidth: Math.min(applicationWindow().width * 0.92, Kirigami.Units.gridUnit * 46)
        implicitHeight: contentCol.implicitHeight

        ColumnLayout {
            id: contentCol
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            spacing: Kirigami.Units.gridUnit

            Rectangle {
                Layout.fillWidth: true
                radius: ThemeController.radius
                color: detail._cPanel
                antialiasing: true
                implicitHeight: panel1.implicitHeight + Kirigami.Units.gridUnit * 2

                RowLayout {
                    id: panel1
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                        margins: Kirigami.Units.gridUnit
                    }
                    spacing: Kirigami.Units.gridUnit

                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        visible: !detail._isMobile
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                        radius: ThemeController.radius
                        clip: true
                        color: "transparent"

                        ProjectTemplateBanner {
                            anchors.fill: parent
                            cornerRadius: 10
                            geometryKind: (detail._template && detail._template.geometryKind) ? detail._template.geometryKind : "circles"
                            baseHue: (detail._template && detail._template.baseHue !== undefined) ? detail._template.baseHue : 210
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: qsTr("SCENARIO")
                            color: Kirigami.Theme.disabledTextColor
                            font.family: ThemeController.fontFamily
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.76
                            font.letterSpacing: 0.8
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            text: (detail._template && detail._template.scenario) ? detail._template.scenario : qsTr("-")
                            color: Kirigami.Theme.textColor
                            font.family: ThemeController.fontFamily
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.05
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                radius: ThemeController.radius
                color: detail._cPanel
                antialiasing: true
                implicitHeight: panel2.implicitHeight + Kirigami.Units.gridUnit * 2

                ColumnLayout {
                    id: panel2
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                        margins: Kirigami.Units.gridUnit
                    }
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        text: qsTr("GOAL")
                        color: Kirigami.Theme.disabledTextColor
                        font.family: ThemeController.fontFamily
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.76
                        font.letterSpacing: 0.8
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: (detail._template && detail._template.goal) ? detail._template.goal : qsTr("-")
                        color: Kirigami.Theme.textColor
                        font.family: ThemeController.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    Item {
                        Layout.preferredHeight: Kirigami.Units.smallSpacing
                    }

                    Controls.Label {
                        text: qsTr("HOW IT WORKS")
                        color: Kirigami.Theme.disabledTextColor
                        font.family: ThemeController.fontFamily
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.76
                        font.letterSpacing: 0.8
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: (detail._template && detail._template.description) ? detail._template.description : qsTr("-")
                        color: Kirigami.Theme.textColor
                        opacity: 0.85
                        font.family: ThemeController.fontFamily
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                radius: ThemeController.radius
                color: detail._cPanel
                antialiasing: true
                implicitHeight: panel3.implicitHeight + Kirigami.Units.gridUnit * 2

                ColumnLayout {
                    id: panel3
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                        margins: Kirigami.Units.gridUnit
                    }
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        text: qsTr("THE TEAM · %1").arg(detail._roster.length)
                        color: Kirigami.Theme.disabledTextColor
                        font.family: ThemeController.fontFamily
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.76
                        font.letterSpacing: 0.8
                        Layout.bottomMargin: Kirigami.Units.smallSpacing
                    }

                    Repeater {
                        model: detail._roster
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.largeSpacing

                            AgentAvatar {
                                agentName: modelData.alias || ""
                                diameter: Kirigami.Units.gridUnit * 1.9
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Controls.Label {
                                    text: modelData.alias || ""
                                    color: Kirigami.Theme.textColor
                                    font.family: ThemeController.fontFamily
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Controls.Label {
                                    text: modelData.agentName || ""
                                    color: Kirigami.Theme.disabledTextColor
                                    font.family: ThemeController.fontFamily
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            Controls.Label {
                                visible: modelData.isCoordinator === true
                                text: qsTr("COORDINATOR")
                                color: Kirigami.Theme.highlightColor
                                font.family: ThemeController.fontFamily
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.72
                                font.letterSpacing: 0.8
                            }
                        }
                    }
                }
            }
        }
    }
}

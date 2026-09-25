// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: card

    property var templateData: ({})

    property real cardWidth: Kirigami.Units.gridUnit * 27

    property bool compactTouchTarget: false

    signal readMoreClicked
    signal quickStartClicked

    property bool showPinControl: false

    property bool isPinned: false

    signal pinToggleClicked

    property bool showDeleteControl: false

    signal deleteClicked

    property bool _animationsArmed: false
    Component.onCompleted: Qt.callLater(function () {
            if (card)
                card._animationsArmed = true;
        })

    width: cardWidth
    height: card.compactTouchTarget ? Kirigami.Units.gridUnit * 16 : Kirigami.Units.gridUnit * 15

    color: Kirigami.Theme.backgroundColor
    radius: ThemeController.radius
    antialiasing: true
    clip: true
    border.width: 0

    readonly property color _accent: {
        var h = (card.templateData && card.templateData.baseHue !== undefined) ? card.templateData.baseHue : 210;
        return Qt.hsla(h / 360.0, 0.55, 0.62, 1.0);
    }

    MouseArea {
        id: _hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Kirigami.Units.largeSpacing * 1.5
            spacing: Kirigami.Units.largeSpacing

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Kirigami.Units.smallSpacing

                Rectangle {
                    Layout.alignment: Qt.AlignLeft
                    visible: tagText.text.length > 0
                    radius: height * 0.5
                    color: Qt.rgba(card._accent.r, card._accent.g, card._accent.b, 0.18)
                    border.color: Qt.rgba(card._accent.r, card._accent.g, card._accent.b, 0.55)
                    border.width: 1
                    implicitHeight: Kirigami.Units.gridUnit * 1.35
                    implicitWidth: tagText.implicitWidth + Kirigami.Units.largeSpacing * 1.6

                    Controls.Label {
                        id: tagText
                        anchors.centerIn: parent
                        text: (card.templateData && card.templateData.tagLabel) ? card.templateData.tagLabel : ""
                        color: card._accent
                        font.family: ThemeController.fontFamily
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.68
                        font.letterSpacing: 0.7
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing * 0.5
                    text: (card.templateData && card.templateData.name) ? card.templateData.name : ""
                    color: Kirigami.Theme.textColor
                    font.family: ThemeController.fontFamily
                    font.bold: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.2
                    wrapMode: Text.WordWrap
                    elide: Text.ElideRight
                    maximumLineCount: 2
                }

                Controls.Label {
                    Layout.fillWidth: true
                    text: (card.templateData && card.templateData.description) ? card.templateData.description : ""
                    color: Kirigami.Theme.textColor
                    opacity: 0.68
                    wrapMode: Text.WordWrap
                    maximumLineCount: card.compactTouchTarget ? 3 : 2
                    elide: Text.ElideRight
                    font.family: ThemeController.fontFamily
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.92
                }

                Item {
                    Layout.fillHeight: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: -(Kirigami.Units.gridUnit * 1.5 * 0.34)

                    Repeater {
                        model: {
                            if (!card.templateData)
                                return [];
                            var out = [];
                            if (card.templateData.coordinator) {
                                out.push(card.templateData.coordinator);
                            }
                            var mem = card.templateData.members || [];
                            for (var i = 0; i < mem.length; ++i) {
                                out.push(mem[i].agentTemplate || mem[i].alias || "");
                            }
                            return out;
                        }
                        delegate: AgentAvatar {
                            agentName: modelData
                            diameter: Kirigami.Units.gridUnit * 1.5
                        }
                    }

                    Item {
                        Layout.preferredWidth: Kirigami.Units.smallSpacing * 2
                    }

                    Controls.Label {
                        text: {
                            if (!card.templateData)
                                return "";
                            var n = (card.templateData.members || []).length + (card.templateData.coordinator ? 1 : 0);
                            return n + qsTr(" agents");
                        }
                        color: Kirigami.Theme.disabledTextColor
                        font.family: ThemeController.fontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.82
                    }

                    Item {
                        Layout.fillWidth: true
                    }
                }
            }

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: card.compactTouchTarget ? Kirigami.Units.gridUnit * 6.5 : Kirigami.Units.gridUnit * 8.5
                Layout.preferredHeight: Layout.preferredWidth
                radius: ThemeController.radius
                clip: true
                color: "transparent"

                ProjectTemplateBanner {
                    anchors.fill: parent
                    cornerRadius: 10
                    geometryKind: (card.templateData && card.templateData.geometryKind) ? card.templateData.geometryKind : "circles"
                    baseHue: (card.templateData && card.templateData.baseHue !== undefined) ? card.templateData.baseHue : 210
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 3.6
            color: Qt.darker(Kirigami.Theme.backgroundColor, 0.82)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Kirigami.Units.largeSpacing * 1.5
                anchors.rightMargin: Kirigami.Units.largeSpacing * 1.5
                spacing: Kirigami.Units.largeSpacing

                AppButton {
                    text: qsTr("⚡  Quick start")
                    highlighted: true
                    onClicked: card.quickStartClicked()
                }

                Item {
                    Layout.fillWidth: true
                }

                AppButton {
                    visible: card.showPinControl
                    text: card.isPinned ? qsTr("📌  Pinned") : qsTr("📌  Pin")
                    flat: true
                    onClicked: card.pinToggleClicked()
                }

                AppButton {
                    text: qsTr("Read more")
                    flat: true
                    onClicked: card.readMoreClicked()
                }

                AppButton {
                    visible: card.showDeleteControl
                    text: qsTr("🗑  Delete")
                    flat: true
                    onClicked: card.deleteClicked()
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        radius: card.radius
        antialiasing: true
        border.width: 1
        border.color: _hoverArea.containsMouse ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
        Behavior on border.color  {
            enabled: card._animationsArmed
            ColorAnimation {
                duration: 110
            }
        }
    }
}

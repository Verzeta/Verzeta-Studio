// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: card

    property string folderId: ""
    property string folderName: ""
    property string folderType: "project"
    property string goalText: ""
    property string status: "idle"
    property var memberNames: []

    property real cardWidth: Kirigami.Units.gridUnit * 27

    property bool compactTouchTarget: false

    signal openRequested

    width: cardWidth
    height: card.compactTouchTarget ? Kirigami.Units.gridUnit * 16 : Kirigami.Units.gridUnit * 15

    color: Kirigami.Theme.backgroundColor
    radius: ThemeController.radius
    antialiasing: true
    clip: true
    border.width: 0

    MouseArea {
        id: _hover
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: card.openRequested()
    }

    function _hashOf(s, salt) {
        var h = 0;
        var src = (s || "") + (salt || "");
        for (var i = 0; i < src.length; ++i) {
            h = ((h << 5) - h) + src.charCodeAt(i);
            h |= 0;
        }
        return Math.abs(h);
    }
    readonly property var _geomKinds: ["circles", "grid", "triangle", "wave", "bars", "arrows", "spiral", "dots"]
    readonly property string _bannerGeom: _geomKinds[_hashOf(card.folderName, "geom") % _geomKinds.length]
    readonly property int _bannerHue: _hashOf(card.folderName, "hue") % 360

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

                ProjectStatusPill {
                    status: card.status
                    Layout.alignment: Qt.AlignLeft
                }

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing * 0.5
                    text: card.folderName
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
                    text: card.goalText.length > 0 ? card.goalText : qsTr("No goal set yet: open the room to " + "describe what success looks like.")
                    color: Kirigami.Theme.textColor
                    opacity: card.goalText.length > 0 ? 0.68 : 0.5
                    font.italic: card.goalText.length === 0
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
                        model: card.memberNames
                        delegate: AgentAvatar {
                            agentName: modelData
                            diameter: Kirigami.Units.gridUnit * 1.5
                        }
                    }

                    Item {
                        Layout.preferredWidth: Kirigami.Units.smallSpacing * 2
                    }

                    Controls.Label {
                        text: card.memberNames.length + qsTr(" members")
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
                    geometryKind: card._bannerGeom
                    baseHue: card._bannerHue
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

                Controls.Label {
                    visible: card.folderType.length > 0 && card.folderType !== "regular"
                    text: card.folderType.toUpperCase()
                    color: Kirigami.Theme.disabledTextColor
                    font.family: ThemeController.fontFamily
                    font.bold: true
                    font.letterSpacing: 0.8
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.72
                }

                Item {
                    Layout.fillWidth: true
                }

                AppButton {
                    text: qsTr("Open")
                    highlighted: true
                    onClicked: card.openRequested()
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
        border.color: _hover.containsMouse ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
        Behavior on border.color  {
            ColorAnimation {
                duration: 110
            }
        }
    }
}

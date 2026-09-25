// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Item {
    id: disclosure

    property string thinkingContent: ""

    property bool startExpanded: false

    property bool _expanded: startExpanded

    readonly property bool _has: disclosure.thinkingContent.length > 0

    visible: _has
    implicitHeight: _has ? (_expanded ? (header.height + 4 + body.height) : header.height) : 0

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: headerLbl.implicitHeight + 8
        color: hoverHandler.hovered ? ThemeController.hoverTint : ThemeController.surfaceSunken
        radius: ThemeController.radius
        Behavior on color  {
            ColorAnimation {
                duration: 80
            }
        }

        HoverHandler {
            id: hoverHandler
        }
        TapHandler {
            onTapped: disclosure._expanded = !disclosure._expanded
        }

        Row {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 6

            Controls.Label {
                text: disclosure._expanded ? "▾" : "▸"
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                color: Kirigami.Theme.disabledTextColor
                anchors.verticalCenter: parent.verticalCenter
            }
            Controls.Label {
                id: headerLbl
                text: qsTr("Reasoning (%1 char%2)").arg(disclosure.thinkingContent.length).arg(disclosure.thinkingContent.length === 1 ? "" : "s")
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                color: Kirigami.Theme.disabledTextColor
                font.italic: true
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    Rectangle {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.topMargin: 4
        visible: disclosure._expanded && disclosure._has
        height: visible ? Math.min(220, thinkingBody.implicitHeight + 16) : 0
        color: ThemeController.surfaceSunken
        radius: ThemeController.radius
        border.color: ThemeController.borderSubtle
        border.width: 1

        Controls.ScrollView {
            id: bodyScroll
            anchors.fill: parent
            anchors.margins: 8
            clip: true
            Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded
            Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff

            TextEdit {
                id: thinkingBody
                width: bodyScroll.availableWidth
                text: disclosure.thinkingContent
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                readOnly: true
                selectByMouse: true
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                color: Kirigami.Theme.textColor
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            z: 10
            onPressed: function (mouse) {
                copyMenu.popup();
            }
        }

        Controls.Menu {
            id: copyMenu

            Controls.MenuItem {
                text: qsTr("Copy selection")
                enabled: thinkingBody.selectedText.length > 0
                onTriggered: thinkingBody.copy()
            }
            Controls.MenuItem {
                text: qsTr("Copy all")
                onTriggered: {
                    var anchorWas = thinkingBody.selectionStart;
                    var positionWas = thinkingBody.selectionEnd;
                    thinkingBody.selectAll();
                    thinkingBody.copy();
                    thinkingBody.select(anchorWas, positionWas);
                }
            }
        }
    }
}

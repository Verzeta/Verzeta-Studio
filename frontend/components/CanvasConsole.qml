// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: root

    signal closeRequested

    color: ThemeController.surfaceCard
    border.width: 0

    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 36
        color: ThemeController.surfaceCard

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 6
            spacing: 6

            Kirigami.Icon {
                source: "utilities-terminal"
                fallback: "terminal"
                width: Kirigami.Units.iconSizes.small
                height: Kirigami.Units.iconSizes.small
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignVCenter
            }
            Controls.Label {
                text: qsTr("Console")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                font.bold: true
                color: Kirigami.Theme.textColor
                Layout.alignment: Qt.AlignVCenter
            }
            Controls.Label {
                text: CanvasConsoleModel.count > 0 ? qsTr("(%1)").arg(CanvasConsoleModel.count) : ""
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignVCenter
            }
            Item {
                Layout.fillWidth: true
            }

            Controls.ToolButton {
                icon.name: "edit-clear"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.alignment: Qt.AlignVCenter
                enabled: CanvasConsoleModel.count > 0
                onClicked: CanvasConsoleModel.clear()
                Controls.ToolTip.text: qsTr("Clear")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }
            Controls.ToolButton {
                icon.name: "window-close-symbolic"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.alignment: Qt.AlignVCenter
                onClicked: root.closeRequested()
                Controls.ToolTip.text: qsTr("Hide console")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }
        }
    }

    Kirigami.Separator {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
    }

    function _submitInput() {
        if (!CanvasRunner.running)
            return;
        CanvasRunner.sendInput(inputField.text);
        inputField.text = "";
        inputField.forceActiveFocus();
    }

    ListView {
        id: rowsView
        anchors.top: header.bottom
        anchors.topMargin: 1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: inputBar.top
        clip: true
        model: CanvasConsoleModel

        property bool _stickToBottom: true
        onContentYChanged: {
            if (rowsView.atYEnd)
                rowsView._stickToBottom = true;
            else if (rowsView.movingVertically)
                rowsView._stickToBottom = false;
        }
        onCountChanged: {
            if (rowsView._stickToBottom) {
                Qt.callLater(rowsView.positionViewAtEnd);
            }
        }

        delegate: Rectangle {
            id: rowRoot
            required property string kind
            required property string text
            required property string timestamp
            required property int index

            width: rowsView.width
            implicitHeight: rowLine.implicitHeight + 4
            color: rowHover.hovered ? ThemeController.hoverTint : "transparent"

            HoverHandler {
                id: rowHover
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                z: 10
                onPressed: function (mouse) {
                    rowMenu.popup();
                }
            }

            Controls.Menu {
                id: rowMenu

                Controls.MenuItem {
                    text: qsTr("Copy line")
                    onTriggered: {
                        consoleClipHelper.text = rowRoot.timestamp + "  " + rowRoot.text;
                        consoleClipHelper.selectAll();
                        consoleClipHelper.copy();
                        consoleClipHelper.deselect();
                    }
                }
            }

            RowLayout {
                id: rowLine
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                anchors.topMargin: 2
                anchors.bottomMargin: 2
                spacing: 8

                Controls.Label {
                    text: rowRoot.timestamp
                    font.family: ThemeController ? ThemeController.codeFontFamily : "monospace"
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                    color: Kirigami.Theme.disabledTextColor
                    Layout.alignment: Qt.AlignVCenter
                }
                Controls.Label {
                    text: rowRoot.text
                    font.family: ThemeController ? ThemeController.codeFontFamily : "monospace"
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    color: {
                        switch (rowRoot.kind) {
                        case "stdout":
                            return Kirigami.Theme.textColor;
                        case "stderr":
                            return Kirigami.Theme.negativeTextColor;
                        case "system":
                            return Kirigami.Theme.neutralTextColor;
                        case "input":
                            return Kirigami.Theme.highlightColor;
                        case "exit-ok":
                            return Kirigami.Theme.positiveTextColor;
                        case "exit-err":
                            return Kirigami.Theme.negativeTextColor;
                        default:
                            return Kirigami.Theme.textColor;
                        }
                    }
                }
            }
        }

        Controls.Label {
            anchors.centerIn: parent
            visible: rowsView.count === 0
            text: qsTr("Console is empty.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
        }
    }

    Rectangle {
        id: inputBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: CanvasRunner.running
        height: visible ? 38 : 0
        color: ThemeController.surfaceSunken

        onVisibleChanged: if (visible)
            inputField.forceActiveFocus()

        Kirigami.Separator {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
        }

        RowLayout {
            anchors.fill: parent
            anchors.topMargin: 1
            anchors.leftMargin: 10
            anchors.rightMargin: 6
            spacing: 6

            Controls.Label {
                text: "›"
                font.family: ThemeController.codeFontFamily
                color: Kirigami.Theme.highlightColor
                Layout.alignment: Qt.AlignVCenter
            }

            AppTextField {
                id: inputField
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                font.family: ThemeController.codeFontFamily
                placeholderText: qsTr("Type input for the script, then press Enter")
                onAccepted: root._submitInput()
            }

            Controls.ToolButton {
                icon.name: "go-next"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.alignment: Qt.AlignVCenter
                onClicked: root._submitInput()
                Controls.ToolTip.text: qsTr("Send input")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }

            Controls.ToolButton {
                icon.name: "media-playback-stop"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.alignment: Qt.AlignVCenter
                onClicked: CanvasRunner.sendEof()
                Controls.ToolTip.text: qsTr("End input (EOF)")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }
        }
    }

    TextEdit {
        id: consoleClipHelper
        visible: false
    }
}

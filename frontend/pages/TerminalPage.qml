// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.Page {
    id: terminalPage

    title: qsTr("Terminal")

    actions: [
        Kirigami.Action {
            icon.name: "edit-clear-all"
            text: qsTr("Clear")
            onTriggered: {
                outputArea.text = "";
                blockedBanner.visible = false;
            }
        }
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Kirigami.InlineMessage {
            id: blockedBanner
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            visible: false
            showCloseButton: true

            Connections {
                target: TerminalController
                function onCommandBlocked(command, reason) {
                    blockedBanner.text = qsTr("Blocked: \"%1\": %2").arg(command).arg(reason);
                    blockedBanner.visible = true;
                }
            }
        }

        Controls.ScrollView {
            id: outputScrollView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Controls.TextArea {
                id: outputArea
                readOnly: true
                font.family: ThemeController.codeFontFamily
                font.pointSize: ThemeController.fontSize > 1 ? ThemeController.fontSize - 1 : 9
                color: ThemeController.codeText
                background: Rectangle {
                    color: ThemeController.codeBackground
                }
                wrapMode: TextEdit.Wrap
                text: ""
                selectByMouse: true

                Connections {
                    target: TerminalController
                    function onOutputLine(line) {
                        outputArea.append(line);
                        Qt.callLater(function () {
                                outputScrollView.ScrollBar.vertical.position = 1.0;
                            });
                    }
                }
            }
        }

        Connections {
            target: TerminalController
            function onCommandFinished(result) {
                if (result.timedOut) {
                    outputArea.append(qsTr("[Process killed: timed out]"));
                } else if (result.exitCode !== 0) {
                    outputArea.append(qsTr("[Exited with code %1]").arg(result.exitCode));
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 6
            spacing: 6

            Controls.Label {
                text: "$"
                font.family: ThemeController.codeFontFamily
                font.bold: true
                color: "#4ec9b0"
            }

            Controls.TextField {
                id: cmdInput
                Layout.fillWidth: true

                font.family: ThemeController.codeFontFamily
                font.pointSize: ThemeController.fontSize > 1 ? ThemeController.fontSize - 1 : 9
                placeholderText: qsTr("Enter command\u2026")
                color: ThemeController.codeText

                background: Rectangle {
                    color: "#2d2d2d"
                    radius: ThemeController.radius
                }

                Keys.onReturnPressed: terminalPage.submitCommand()

                Keys.onUpPressed: terminalPage.navigateHistory(-1)
                Keys.onDownPressed: terminalPage.navigateHistory(1)
            }

            Controls.ToolButton {
                icon.name: "media-playback-start"
                onClicked: terminalPage.submitCommand()
                Controls.ToolTip.text: qsTr("Run command")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 500
            }
        }
    }

    property var commandHistory: []
    property int historyIndex: -1

    function submitCommand() {
        const text = cmdInput.text.trim();
        if (text.length === 0) {
            return;
        }
        outputArea.append("$ " + text);
        if (commandHistory.length === 0 || commandHistory[commandHistory.length - 1] !== text) {
            commandHistory.push(text);
        }
        historyIndex = commandHistory.length;
        cmdInput.text = "";
        blockedBanner.visible = false;
        TerminalController.executeCommand(text);
    }

    function navigateHistory(delta) {
        if (commandHistory.length === 0) {
            return;
        }
        historyIndex = Math.max(0, Math.min(commandHistory.length, historyIndex + delta));
        if (historyIndex < commandHistory.length) {
            cmdInput.text = commandHistory[historyIndex];
        } else {
            cmdInput.text = "";
        }
    }
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.PromptDialog {
    id: dialog

    property string toolName: ""

    property string toolArgs: ""

    property string callId: ""

    title: "Tool Call Confirmation"
    subtitle: "The assistant wants to run a tool. Review it before approving."

    ColumnLayout {
        spacing: 8

        Controls.Label {
            text: "Tool: " + dialog.toolName
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }

        Controls.TextArea {
            id: argsArea
            text: dialog.toolArgs
            readOnly: true
            font.family: ThemeController.codeFontFamily
            font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
            Layout.fillWidth: true
            Layout.preferredHeight: 100
            wrapMode: Text.Wrap
            selectByMouse: true

            background: Rectangle {
                color: ThemeController.codeBackground
                radius: ThemeController.radius
            }
            color: ThemeController.codeText
        }

        Controls.Label {
            text: "Approving will execute this tool and return the result to the assistant."
            font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
            color: Kirigami.Theme.disabledTextColor
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }

    standardButtons: Controls.Dialog.Yes | Controls.Dialog.No

    onAccepted: {
        AgentService.approveToolCall(dialog.callId);
    }

    onRejected: {
        AgentService.denyToolCall(dialog.callId);
    }
}

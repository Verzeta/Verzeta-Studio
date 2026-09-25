// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.Dialog {
    id: exportDialog

    title: qsTr("Export Conversation")
    preferredWidth: Kirigami.Units.gridUnit * 24

    standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            text: qsTr("Export format:")
            font.bold: true
        }

        Controls.ButtonGroup {
            id: formatGroup
        }

        AppRadioButton {
            id: markdownRadio

            text: qsTr("Markdown (.md)")
            checked: true
            Controls.ButtonGroup.group: formatGroup

            readonly property string format: "markdown"
        }

        AppRadioButton {
            id: jsonRadio

            text: qsTr("JSON (.json)")
            Controls.ButtonGroup.group: formatGroup

            readonly property string format: "json"
        }

        Kirigami.Separator {
        }

        Controls.CheckBox {
            id: attachmentsCheck

            text: qsTr("Include attachments (base64-encoded)")
            checked: false
        }

        Controls.Label {
            text: qsTr("Files are saved to your Downloads folder.")
            wrapMode: Text.Wrap
            font.italic: true
            color: Kirigami.Theme.disabledTextColor
            Layout.fillWidth: true
        }
    }

    onAccepted: {
        const format = formatGroup.checkedButton ? formatGroup.checkedButton.format : "markdown";
        Export.exportConversation(ChatController.activeConversationId, format, attachmentsCheck.checked);
    }

    Connections {
        target: Export

        function onExportCompleted(filePath) {
            exportBanner.type = Kirigami.MessageType.Positive;
            exportBanner.text = qsTr("Exported to: %1").arg(filePath);
            exportBanner.visible = true;
        }

        function onExportFailed(error) {
            exportBanner.type = Kirigami.MessageType.Error;
            exportBanner.text = qsTr("Export failed: %1").arg(error);
            exportBanner.visible = true;
        }
    }
}

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

    implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 42)

    implicitHeight: Math.min(applicationWindow().height * 0.9, Kirigami.Units.gridUnit * 44)

    closePolicy: Controls.Popup.CloseOnEscape

    property string editingSlug: ""
    property string testToken: ""
    property string testState: ""
    property string testMessage: ""
    property var previewModels: []

    readonly property var prefillCatalogue: [{
            "label": qsTr("vLLM"),
            "baseUrl": "http://localhost:8000/v1",
            "streaming": true,
            "tools": true,
            "vision": false
        }, {
            "label": qsTr("LM Studio"),
            "baseUrl": "http://localhost:1234/v1",
            "streaming": true,
            "tools": true,
            "vision": false
        }, {
            "label": qsTr("Jan"),
            "baseUrl": "http://localhost:1337/v1",
            "streaming": true,
            "tools": true,
            "vision": false
        }, {
            "label": qsTr("Llamafile"),
            "baseUrl": "http://localhost:8080/v1",
            "streaming": true,
            "tools": false,
            "vision": false
        }, {
            "label": qsTr("TabbyAPI"),
            "baseUrl": "http://localhost:5000/v1",
            "streaming": true,
            "tools": true,
            "vision": false
        }, {
            "label": qsTr("KoboldCpp"),
            "baseUrl": "http://localhost:5001/v1",
            "streaming": true,
            "tools": false,
            "vision": false
        }, {
            "label": qsTr("LocalAI"),
            "baseUrl": "http://localhost:8080/v1",
            "streaming": true,
            "tools": true,
            "vision": false
        }, {
            "label": qsTr("text-generation-webui"),
            "baseUrl": "http://localhost:5000/v1",
            "streaming": true,
            "tools": false,
            "vision": false
        }, {
            "label": qsTr("SGLang"),
            "baseUrl": "http://localhost:30000/v1",
            "streaming": true,
            "tools": true,
            "vision": false
        }]

    function applyPrefill(idx) {
        if (idx < 0 || idx >= sheet.prefillCatalogue.length) {
            return;
        }
        var p = sheet.prefillCatalogue[idx];
        if (nameField.text.length === 0) {
            nameField.text = p.label;
        }
        urlField.text = p.baseUrl;
        streamingCheck.checked = p.streaming;
        toolsCheck.checked = p.tools;
        visionCheck.checked = p.vision;
        sheet.testState = "";
        sheet.testMessage = "";
        sheet.previewModels = [];
    }

    function openForAdd() {
        sheet.editingSlug = "";
        nameField.text = "";
        urlField.text = "";
        keyField.text = "";
        requiresKeyCheck.checked = false;
        streamingCheck.checked = true;
        toolsCheck.checked = true;
        visionCheck.checked = false;
        prefillCombo.currentIndex = -1;
        sheet.testState = "";
        sheet.testMessage = "";
        sheet.previewModels = [];
        sheet.open();
    }

    function openForEdit(slug) {
        var row = CustomServers.byslug(slug);
        if (!row || !row.slug) {
            return;
        }
        sheet.editingSlug = slug;
        nameField.text = row.displayName || "";
        urlField.text = row.baseUrl || "";
        keyField.text = "";
        requiresKeyCheck.checked = row.requiresApiKeyFlag === true;
        streamingCheck.checked = row.supportsStreaming === true;
        toolsCheck.checked = row.supportsToolCalling === true;
        visionCheck.checked = row.supportsVision === true;
        prefillCombo.currentIndex = -1;
        sheet.testState = "";
        sheet.testMessage = "";
        sheet.previewModels = [];
        sheet.open();
    }

    function runTestConnection() {
        if (urlField.text.trim().length === 0) {
            sheet.testState = "error";
            sheet.testMessage = qsTr("Enter a base URL first.");
            return;
        }
        sheet.testToken = "setup-sheet-" + Date.now();
        sheet.testState = "running";
        sheet.testMessage = qsTr("Probing the server…");
        sheet.previewModels = [];
        CustomServers.testConnection(sheet.testToken, urlField.text.trim(), requiresKeyCheck.checked, keyField.text);
    }

    Connections {
        target: CustomServers
        function onConnectionTestResult(correlationToken, ok, verdictKey, humanMessage, models) {
            if (correlationToken !== sheet.testToken) {
                return;
            }
            sheet.testState = ok ? "ok" : "error";
            sheet.testMessage = humanMessage;
            sheet.previewModels = (ok && models) ? models : [];
        }
    }

    function persistAndClose() {
        var trimmedName = nameField.text.trim();
        var trimmedUrl = urlField.text.trim();
        if (trimmedName.length === 0 || trimmedUrl.length === 0) {
            sheet.testState = "error";
            sheet.testMessage = qsTr("Display name and base URL are both required.");
            return;
        }
        var dupSlug = CustomServers.slugForBaseUrl(trimmedUrl);
        if (dupSlug.length > 0 && dupSlug !== sheet.editingSlug) {
            var existing = CustomServers.byslug(dupSlug);
            var existingName = (existing && existing.displayName) ? existing.displayName : dupSlug;
            sheet.testState = "error";
            sheet.testMessage = qsTr("A provider for this URL is already configured: %1. Edit that one instead.").arg(existingName);
            return;
        }
        if (sheet.editingSlug.length === 0) {
            var slug = CustomServers.add(trimmedName, trimmedUrl, requiresKeyCheck.checked, keyField.text, streamingCheck.checked, toolsCheck.checked, visionCheck.checked);
            if (slug.length === 0) {
                sheet.testState = "error";
                sheet.testMessage = qsTr("Save failed: check that the URL is valid (http or https scheme, non-empty host).");
                return;
            }
        } else {
            var ok = CustomServers.update(sheet.editingSlug, trimmedName, trimmedUrl, requiresKeyCheck.checked, keyField.text, streamingCheck.checked, toolsCheck.checked, visionCheck.checked);
            if (!ok) {
                sheet.testState = "error";
                sheet.testMessage = qsTr("Update failed: the server is no longer in the catalogue.");
                return;
            }
        }
        sheet.close();
    }

    title: sheet.editingSlug.length === 0 ? qsTr("Add Custom OpenAI-Compatible Server") : qsTr("Edit: %1").arg(nameField.text)
    dialogIcon: "network-server"

    footer: RowLayout {
        spacing: 8

        Rectangle {
            visible: sheet.testState.length > 0
            implicitHeight: pillLabel.implicitHeight + 8
            implicitWidth: Math.min(pillLabel.implicitWidth + 16, 360)
            radius: implicitHeight / 2
            color: {
                if (sheet.testState === "ok") {
                    return Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.18);
                }
                if (sheet.testState === "error") {
                    return Qt.rgba(Kirigami.Theme.negativeTextColor.r, Kirigami.Theme.negativeTextColor.g, Kirigami.Theme.negativeTextColor.b, 0.18);
                }
                return Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.18);
            }
            Controls.Label {
                id: pillLabel
                anchors.centerIn: parent
                text: {
                    if (sheet.testState === "ok")
                        return qsTr("OK");
                    if (sheet.testState === "error")
                        return qsTr("Failed");
                    return qsTr("Testing…");
                }
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                color: {
                    if (sheet.testState === "ok")
                        return Kirigami.Theme.positiveTextColor;
                    if (sheet.testState === "error")
                        return Kirigami.Theme.negativeTextColor;
                    return Kirigami.Theme.textColor;
                }
            }
        }

        Item {
            Layout.fillWidth: true
        }

        AppButton {
            text: qsTr("Test Connection")
            enabled: sheet.testState !== "running" && urlField.text.trim().length > 0
            onClicked: sheet.runTestConnection()
        }
        AppButton {
            text: qsTr("Cancel")
            onClicked: sheet.close()
        }
        AppButton {
            text: sheet.editingSlug.length === 0 ? qsTr("Add") : qsTr("Save")
            highlighted: true
            onClicked: sheet.persistAndClose()
        }
    }

    ColumnLayout {
        spacing: 14

        Controls.Label {
            text: qsTr("Identity")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.topMargin: 4
        }

        Controls.Label {
            text: qsTr("Display name (Required)")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            color: Kirigami.Theme.disabledTextColor
        }
        AppTextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("e.g. LM Studio at home, vLLM lab box")
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }

        Controls.Label {
            text: qsTr("Start from a known stack…")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            color: Kirigami.Theme.disabledTextColor
        }
        Controls.ComboBox {
            id: prefillCombo
            Layout.fillWidth: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            textRole: "label"
            model: sheet.prefillCatalogue
            displayText: currentIndex >= 0 ? sheet.prefillCatalogue[currentIndex].label : qsTr("(pick a stack to fill in the URL and flags)")
            onActivated: sheet.applyPrefill(currentIndex)
        }
        Controls.Label {
            text: qsTr("Picking a stack fills the base URL and capability flags with that stack's defaults. You can edit every field afterward.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Controls.Label {
            text: qsTr("Connection")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.topMargin: 8
        }

        Controls.Label {
            text: qsTr("Base URL (Required)")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            color: Kirigami.Theme.disabledTextColor
        }
        AppTextField {
            id: urlField
            Layout.fillWidth: true
            placeholderText: qsTr("http://localhost:1234/v1")
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }
        Controls.Label {
            text: qsTr("Include `/v1` at the end if your server expects it. For example LM Studio's default is `http://localhost:1234/v1`, not `http://localhost:1234`.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Controls.CheckBox {
            id: requiresKeyCheck
            text: qsTr("Server requires an API key")
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            checked: false
        }

        Controls.Label {
            text: qsTr("API key")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            color: Kirigami.Theme.disabledTextColor
            visible: requiresKeyCheck.checked || keyField.text.length > 0
        }
        AppTextField {
            id: keyField
            Layout.fillWidth: true
            placeholderText: qsTr("(leave blank for auth-less servers)")
            echoMode: TextInput.Password
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            visible: requiresKeyCheck.checked || keyField.text.length > 0
        }

        Rectangle {
            visible: sheet.testMessage.length > 0
            Layout.fillWidth: true
            implicitHeight: testMessageLbl.implicitHeight + 16
            color: ThemeController.surfaceSunken
            radius: ThemeController.radius
            border.color: ThemeController.borderSubtle
            border.width: 1
            Controls.Label {
                id: testMessageLbl
                anchors.fill: parent
                anchors.margins: 8
                text: sheet.testMessage
                wrapMode: Text.WordWrap
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.textColor
            }
        }

        Rectangle {
            visible: sheet.previewModels.length > 0
            Layout.fillWidth: true
            implicitHeight: Math.min(160, previewCol.implicitHeight + 16)
            color: ThemeController.surfaceSunken
            radius: ThemeController.radius
            border.color: ThemeController.borderSubtle
            border.width: 1

            Controls.ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded
                Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
                ColumnLayout {
                    id: previewCol
                    spacing: 2
                    Controls.Label {
                        text: qsTr("Models the server reported:")
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        color: Kirigami.Theme.disabledTextColor
                    }
                    Repeater {
                        model: sheet.previewModels
                        delegate: Controls.Label {
                            text: "• " + modelData
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            color: Kirigami.Theme.textColor
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }

        Controls.Label {
            text: qsTr("Capabilities")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.topMargin: 8
        }
        Controls.Label {
            text: qsTr("These flags are advertised to the rest of Verzeta. Agents that need tool calling are routed only to servers with the flag on. Set them to match what your server supports.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Controls.CheckBox {
            id: streamingCheck
            text: qsTr("Server supports SSE streaming")
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            checked: true
        }
        Controls.CheckBox {
            id: toolsCheck
            text: qsTr("Server supports tool calling (OpenAI `tools` schema)")
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            checked: true
        }
        Controls.CheckBox {
            id: visionCheck
            text: qsTr("Server accepts image INPUT (vision) via `image_url`")
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            checked: false
        }
        Controls.Label {
            text: qsTr("This is image INPUT to a chat model (sending images for the model to read). It is separate from Image Providers / image generation, which is configured under Settings → Image Generation.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}

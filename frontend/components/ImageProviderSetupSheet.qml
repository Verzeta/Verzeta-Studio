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
    implicitHeight: Math.min(applicationWindow().height * 0.9, Kirigami.Units.gridUnit * 46)

    closePolicy: Controls.Popup.CloseOnEscape

    property string editingId: ""

    readonly property var endpointTypes: [{
            "label": qsTr("OpenAI Images (DALL·E / gpt-image)"),
            "value": "openai_images"
        }, {
            "label": qsTr("OpenAI-compatible chat-image (OpenRouter)"),
            "value": "openai_chat_image"
        }, {
            "label": qsTr("Automatic1111 / SD WebUI"),
            "value": "a1111"
        }, {
            "label": qsTr("Local CLI"),
            "value": "local_cli"
        }]

    function shapeIndex(shape) {
        for (var i = 0; i < endpointTypes.length; ++i) {
            if (endpointTypes[i].value === shape)
                return i;
        }
        return 0;
    }
    function currentShape() {
        var idx = typeCombo.currentIndex;
        if (idx < 0 || idx >= endpointTypes.length)
            return "openai_images";
        return endpointTypes[idx].value;
    }
    function isHttpShape(shape) {
        return shape === "openai_images" || shape === "openai_chat_image" || shape === "a1111";
    }

    function openForAdd() {
        sheet.editingId = "";
        nameField.text = "";
        typeCombo.currentIndex = 0;
        urlField.text = "";
        pathField.text = "";
        modelField.text = "";
        keyField.text = "";
        authLocationCombo.currentIndex = 0;
        authHeaderField.text = "Authorization";
        authPrefixField.text = "Bearer ";
        authQueryField.text = "";
        authBodyFieldInput.text = "api_key";
        sizeField.text = "1024x1024";
        qualityField.text = "standard";
        outputCombo.currentIndex = 0;
        sheet.open();
    }

    function openForEdit(id) {
        var row = ImageProviders.byId(id);
        if (!row || !row.id) {
            return;
        }
        sheet.editingId = id;
        nameField.text = row.displayName || "";
        typeCombo.currentIndex = shapeIndex(row.endpointShape || "openai_images");
        urlField.text = row.baseUrl || "";
        pathField.text = row.sdPath || "";
        modelField.text = row.model || "";
        keyField.text = "";
        authLocationCombo.currentIndex = authLocationCombo.indexOfValue(row.authLocation || "header");
        authHeaderField.text = row.authHeaderName || "Authorization";
        authPrefixField.text = (row.authValuePrefix !== undefined) ? row.authValuePrefix : "Bearer ";
        authQueryField.text = row.authQueryParam || "";
        authBodyFieldInput.text = row.authBodyField || "api_key";
        sizeField.text = row.size || "1024x1024";
        qualityField.text = row.quality || "standard";
        outputCombo.currentIndex = (row.outputModalities === "image_text") ? 1 : 0;
        sheet.open();
    }

    property string saveError: ""

    function persistAndClose() {
        var name = nameField.text.trim();
        var shape = currentShape();
        if (name.length === 0) {
            sheet.saveError = qsTr("Display name is required.");
            return;
        }
        if (isHttpShape(shape) && urlField.text.trim().length === 0) {
            sheet.saveError = qsTr("Base URL is required for this endpoint type.");
            return;
        }
        if (shape === "local_cli" && pathField.text.trim().length === 0) {
            sheet.saveError = qsTr("Executable path is required for Local CLI.");
            return;
        }
        if (shape === "openai_chat_image" && modelField.text.trim().length === 0) {
            sheet.saveError = qsTr("Model is required for chat-image providers (e.g. google/gemini-2.5-flash-image-preview).");
            return;
        }
        var m = {};
        if (sheet.editingId.length > 0)
            m.id = sheet.editingId;
        m.displayName = name;
        m.endpointShape = shape;
        m.baseUrl = isHttpShape(shape) ? urlField.text.trim() : "";
        m.model = isHttpShape(shape) ? modelField.text.trim() : "";
        m.sdPath = (shape === "local_cli") ? pathField.text.trim() : "";
        m.authLocation = authLocationCombo.currentValue;
        m.authHeaderName = authHeaderField.text.length > 0 ? authHeaderField.text : "Authorization";
        m.authValuePrefix = authPrefixField.text;
        m.authQueryParam = authQueryField.text.trim();
        m.authBodyField = authBodyFieldInput.text.trim().length > 0 ? authBodyFieldInput.text.trim() : "api_key";
        m.size = sizeField.text.trim().length > 0 ? sizeField.text.trim() : "1024x1024";
        m.quality = qualityField.text.trim().length > 0 ? qualityField.text.trim() : "standard";
        m.outputModalities = (shape === "openai_chat_image") ? outputCombo.currentValue : "image";
        m.requiresApiKey = keyField.text.length > 0;
        if (keyField.text.length > 0)
            m.apiKey = keyField.text;
        var id = ImageProviders.upsert(m);
        if (!id || id.length === 0) {
            sheet.saveError = qsTr("Save failed: check the fields and try again.");
            return;
        }
        sheet.saveError = "";
        sheet.close();
    }

    title: sheet.editingId.length === 0 ? qsTr("Add Image Provider") : qsTr("Edit: %1").arg(nameField.text)
    dialogIcon: "image-x-generic"

    footer: RowLayout {
        spacing: 8
        Controls.Label {
            visible: sheet.saveError.length > 0
            text: sheet.saveError
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
        }
        Item {
            Layout.fillWidth: true
            visible: sheet.saveError.length === 0
        }
        AppButton {
            text: qsTr("Cancel")
            onClicked: sheet.close()
        }
        AppButton {
            text: sheet.editingId.length === 0 ? qsTr("Add") : qsTr("Save")
            highlighted: true
            onClicked: sheet.persistAndClose()
        }
    }

    ColumnLayout {
        spacing: 14

        Controls.Label {
            text: qsTr("Display name (Required)")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            color: Kirigami.Theme.disabledTextColor
        }
        AppTextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("e.g. OpenAI DALL·E, OpenRouter image, SD box")
        }

        Controls.Label {
            text: qsTr("Endpoint type (Required)")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            color: Kirigami.Theme.disabledTextColor
        }
        Controls.ComboBox {
            id: typeCombo
            Layout.fillWidth: true
            textRole: "label"
            model: sheet.endpointTypes
        }
        Controls.Label {
            text: {
                switch (sheet.currentShape()) {
                case "openai_images":
                    return qsTr("POSTs to <base URL>/images/generations. Works with OpenAI and any server speaking the OpenAI Images API.");
                case "openai_chat_image":
                    return qsTr("POSTs to <base URL>/chat/completions with image output enabled. Use for OpenRouter image models (e.g. Gemini image).");
                case "a1111":
                    return qsTr("POSTs to <base URL>/sdapi/v1/txt2img. Works with Automatic1111, vladmandic/automatic, and forks.");
                case "local_cli":
                    return qsTr("Spawns a local Stable Diffusion CLI with --prompt / --output / --steps arguments.");
                }
                return "";
            }
            wrapMode: Text.WordWrap
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            Layout.fillWidth: true
        }

        ColumnLayout {
            visible: sheet.isHttpShape(sheet.currentShape())
            spacing: 14
            Layout.fillWidth: true

            Controls.Label {
                text: qsTr("Base URL (Required)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
            }
            AppTextField {
                id: urlField
                Layout.fillWidth: true
                placeholderText: {
                    switch (sheet.currentShape()) {
                    case "openai_images":
                        return "https://api.openai.com/v1";
                    case "openai_chat_image":
                        return "https://openrouter.ai/api/v1";
                    case "a1111":
                        return "http://192.168.0.10:7860";
                    }
                    return "https://…";
                }
            }
            Controls.Label {
                text: {
                    switch (sheet.currentShape()) {
                    case "openai_images":
                        return qsTr("Enter the API base only (e.g. https://api.openai.com/v1). The app appends /images/generations. Pasting the full endpoint also works.");
                    case "openai_chat_image":
                        return qsTr("Enter the API base only (e.g. https://openrouter.ai/api/v1). The app appends /chat/completions automatically. Pasting the full endpoint URL also works.");
                    case "a1111":
                        return qsTr("Enter the server base (e.g. http://192.168.0.10:7860). The app appends /sdapi/v1/txt2img automatically.");
                    }
                    return "";
                }
                visible: text.length > 0
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                Layout.fillWidth: true
            }

            Controls.Label {
                text: qsTr("Model")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
                visible: sheet.currentShape() !== "a1111"
            }
            AppTextField {
                id: modelField
                Layout.fillWidth: true
                visible: sheet.currentShape() !== "a1111"
                placeholderText: {
                    switch (sheet.currentShape()) {
                    case "openai_images":
                        return "dall-e-3 / gpt-image-1";
                    case "openai_chat_image":
                        return "google/gemini-2.5-flash-image-preview";
                    }
                    return qsTr("(model id)");
                }
            }
            Controls.Label {
                text: qsTr("Use an image-generation model, e.g. google/gemini-2.5-flash-image-preview, black-forest-labs/flux.2-pro, or x-ai/grok-imagine-image-quality.")
                visible: sheet.currentShape() === "openai_chat_image"
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                Layout.fillWidth: true
            }

            Controls.Label {
                text: qsTr("Output")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
                visible: sheet.currentShape() === "openai_chat_image"
            }
            Controls.ComboBox {
                id: outputCombo
                Layout.fillWidth: true
                visible: sheet.currentShape() === "openai_chat_image"
                textRole: "label"
                valueRole: "value"
                model: [{
                        "label": qsTr("Image only (most image models)"),
                        "value": "image"
                    }, {
                        "label": qsTr("Image + text (dual-output models e.g. Gemini)"),
                        "value": "image_text"
                    }]
            }
            Controls.Label {
                text: qsTr("Most dedicated image models output only an image. Choose \"Image + text\" only for models that also return text, or you may get \"No endpoints found that support the requested output modalities\".")
                visible: sheet.currentShape() === "openai_chat_image"
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                Layout.fillWidth: true
            }
        }

        ColumnLayout {
            visible: sheet.currentShape() === "local_cli"
            spacing: 14
            Layout.fillWidth: true

            Controls.Label {
                text: qsTr("Executable path (Required)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
            }
            AppTextField {
                id: pathField
                Layout.fillWidth: true
                placeholderText: qsTr("/path/to/stable-diffusion-cli")
            }
        }

        ColumnLayout {
            visible: sheet.isHttpShape(sheet.currentShape())
            spacing: 14
            Layout.fillWidth: true

            Controls.Label {
                text: qsTr("API key")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
            }
            AppTextField {
                id: keyField
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: sheet.editingId.length > 0 ? qsTr("Leave blank to keep the stored key") : qsTr("(leave blank for auth-less servers)")
            }
        }

        Controls.Label {
            text: qsTr("Send credential in")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.topMargin: 4
            visible: sheet.isHttpShape(sheet.currentShape())
        }
        ColumnLayout {
            visible: sheet.isHttpShape(sheet.currentShape())
            spacing: 14
            Layout.fillWidth: true

            Controls.ComboBox {
                id: authLocationCombo
                Layout.fillWidth: true
                textRole: "label"
                valueRole: "value"
                model: [{
                        "label": qsTr("Header  (e.g. Authorization: Bearer …)"),
                        "value": "header"
                    }, {
                        "label": qsTr("URL query parameter  (?key=…)"),
                        "value": "query"
                    }, {
                        "label": qsTr("Request body  (JSON field)"),
                        "value": "body"
                    }]
            }

            Controls.Label {
                text: qsTr("Header name")
                visible: authLocationCombo.currentValue === "header"
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
            }
            AppTextField {
                id: authHeaderField
                visible: authLocationCombo.currentValue === "header"
                Layout.fillWidth: true
                placeholderText: "Authorization"
            }
            Controls.Label {
                text: qsTr("Value prefix")
                visible: authLocationCombo.currentValue === "header"
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
            }
            AppTextField {
                id: authPrefixField
                visible: authLocationCombo.currentValue === "header"
                Layout.fillWidth: true
                placeholderText: "Bearer "
            }

            Controls.Label {
                text: qsTr("Query parameter name")
                visible: authLocationCombo.currentValue === "query"
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
            }
            AppTextField {
                id: authQueryField
                visible: authLocationCombo.currentValue === "query"
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. key (sends ?key=<API key>)")
            }

            Controls.Label {
                text: qsTr("Body field name")
                visible: authLocationCombo.currentValue === "body"
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
            }
            AppTextField {
                id: authBodyFieldInput
                visible: authLocationCombo.currentValue === "body"
                Layout.fillWidth: true
                placeholderText: "api_key"
            }

            Controls.Label {
                text: qsTr("Where the API key is placed on each request. Most cloud providers (OpenAI, OpenRouter) use Header.")
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                Layout.fillWidth: true
            }
        }

        ColumnLayout {
            visible: sheet.isHttpShape(sheet.currentShape())
            spacing: 14
            Layout.fillWidth: true

            Controls.Label {
                text: qsTr("Image size")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
            }
            AppTextField {
                id: sizeField
                Layout.fillWidth: true
                placeholderText: "1024x1024"
            }

            Controls.Label {
                text: qsTr("Quality")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                color: Kirigami.Theme.disabledTextColor
                visible: sheet.currentShape() === "openai_images"
            }
            AppTextField {
                id: qualityField
                Layout.fillWidth: true
                visible: sheet.currentShape() === "openai_images"
                placeholderText: "standard / hd"
            }
        }
    }
}

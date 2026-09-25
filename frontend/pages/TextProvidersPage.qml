// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: page
    color: ThemeController.surfacePage

    signal back

    property int _refreshTick: 0
    Connections {
        target: SettingsService
        function onSettingsChanged(key) {
            page._refreshTick++;
        }
        function onOllamaBaseUrlChanged() {
            page._refreshTick++;
        }
        function onProviderBaseUrlChanged(p, u) {
            page._refreshTick++;
        }
    }

    function cloudStatus(providerId) {
        var _ = page._refreshTick;
        return SettingsService.hasApiKey(providerId) ? {
            "text": qsTr("Connected"),
            "kind": "connected"
        } : {
            "text": qsTr("Not configured"),
            "kind": "unconfigured"
        };
    }
    function ollamaStatus() {
        var _ = page._refreshTick;
        var url = SettingsService.ollamaBaseUrl;
        return url.length > 0 ? {
            "text": url,
            "kind": "info"
        } : {
            "text": qsTr("Not configured"),
            "kind": "unconfigured"
        };
    }
    function llamaRemoteStatus() {
        var _ = page._refreshTick;
        var url = SettingsService.providerBaseUrl("llamacpp_remote");
        return url.length > 0 ? {
            "text": url,
            "kind": "info"
        } : {
            "text": qsTr("Default (localhost:8080)"),
            "kind": "info"
        };
    }
    function llamaLocalStatus() {
        var _ = page._refreshTick;
        var path = SettingsService.llamaCppModelPath;
        return path.length > 0 ? {
            "text": qsTr("On-Device"),
            "kind": "connected"
        } : {
            "text": qsTr("Not configured"),
            "kind": "unconfigured"
        };
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            color: ThemeController.surfaceCard
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 16
                spacing: 8
                Controls.ToolButton {
                    icon.name: "go-previous"
                    text: qsTr("Back")
                    display: Controls.AbstractButton.IconOnly
                    onClicked: page.back()
                }
                Kirigami.Heading {
                    text: qsTr("Text Providers")
                    level: 3
                    Layout.fillWidth: true
                }
            }
        }
        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Controls.ScrollView {
            id: textProvScroll
            contentWidth: applicationWindow().isCompact ? availableWidth : -1
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ColumnLayout {
                width: applicationWindow().isCompact ? Math.min(textProvScroll.availableWidth - 32, 720) : Math.min(page.width - 32, 720)
                x: applicationWindow().isCompact ? Math.max(16, (textProvScroll.availableWidth - width) / 2) : Math.max(16, (page.width - width) / 2)
                spacing: 16

                Item {
                    Layout.preferredHeight: 16
                }

                Kirigami.Heading {
                    text: qsTr("Cloud Providers")
                    level: 4
                }
                Controls.Label {
                    text: qsTr("Use hosted API providers such as OpenAI, Anthropic, Gemini, OpenRouter, or DeepSeek.")
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    Layout.fillWidth: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                }

                ProviderCard {
                    providerName: "OpenAI"
                    iconName: "internet-services"
                    subtitle: qsTr("GPT models. Also powers DALL-E image generation.")
                    status: cloudStatus("openai").text
                    statusKind: cloudStatus("openai").kind
                    onConfigure: setupSheet.openFor("openai")
                }
                ProviderCard {
                    providerName: "Anthropic"
                    iconName: "internet-services"
                    subtitle: qsTr("Claude models.")
                    status: cloudStatus("anthropic").text
                    statusKind: cloudStatus("anthropic").kind
                    onConfigure: setupSheet.openFor("anthropic")
                }
                ProviderCard {
                    providerName: "Google Gemini"
                    iconName: "internet-services"
                    subtitle: qsTr("Gemini models.")
                    status: cloudStatus("gemini").text
                    statusKind: cloudStatus("gemini").kind
                    onConfigure: setupSheet.openFor("gemini")
                }
                ProviderCard {
                    providerName: "OpenRouter"
                    iconName: "internet-services"
                    subtitle: qsTr("Routes to many models through one API.")
                    status: cloudStatus("openrouter").text
                    statusKind: cloudStatus("openrouter").kind
                    onConfigure: setupSheet.openFor("openrouter")
                }
                ProviderCard {
                    providerName: "DeepSeek"
                    iconName: "internet-services"
                    subtitle: qsTr("DeepSeek-Chat and DeepSeek-Reasoner.")
                    status: cloudStatus("deepseek").text
                    statusKind: cloudStatus("deepseek").kind
                    onConfigure: setupSheet.openFor("deepseek")
                }

                Item {
                    Layout.preferredHeight: 12
                }

                Kirigami.Heading {
                    text: qsTr("Remote Servers")
                    level: 4
                }
                Controls.Label {
                    text: qsTr("Connect to your own Ollama or llama.cpp server running on the same machine or another one.")
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    Layout.fillWidth: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                }

                ProviderCard {
                    providerName: "Ollama"
                    iconName: "network-server"
                    subtitle: qsTr("Local or LAN-hosted Ollama server.")
                    status: ollamaStatus().text
                    statusKind: ollamaStatus().kind
                    onConfigure: setupSheet.openFor("ollama")
                }
                ProviderCard {
                    providerName: "llama.cpp (Remote)"
                    iconName: "network-server"
                    subtitle: qsTr("HTTP client for an externally-running llama.cpp server.")
                    status: llamaRemoteStatus().text
                    statusKind: llamaRemoteStatus().kind
                    onConfigure: setupSheet.openFor("llamacpp_remote")
                }

                Item {
                    Layout.preferredHeight: 12
                }

                Kirigami.Heading {
                    text: qsTr("Local Models")
                    level: 4
                }
                Controls.Label {
                    text: qsTr("Run llama.cpp directly on this device with a local GGUF model file.")
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    Layout.fillWidth: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                }

                ProviderCard {
                    providerName: "llama.cpp (Local)"
                    iconName: "computer"
                    subtitle: qsTr("On-device inference with a GGUF model on this machine.")
                    status: llamaLocalStatus().text
                    statusKind: llamaLocalStatus().kind
                    onConfigure: setupSheet.openFor("llamacpp_local")
                }

                Item {
                    Layout.preferredHeight: 12
                }

                Kirigami.Heading {
                    text: qsTr("Custom OpenAI-Compatible Servers")
                    level: 4
                }
                Controls.Label {
                    text: qsTr("Point Verzeta at any server that speaks the OpenAI /v1/chat/completions and /v1/models API, such as vLLM, LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI, SGLang, or text-generation-webui. Add several, and each agent in a team can use a different one.")
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    Layout.fillWidth: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                }

                Repeater {
                    model: CustomServers.servers
                    delegate: CustomServerCard {
                        displayName: modelData.displayName
                        baseUrl: modelData.baseUrl
                        modelCount: modelData.modelCount
                        supportsStreaming: modelData.supportsStreaming
                        supportsToolCalling: modelData.supportsToolCalling
                        supportsVision: modelData.supportsVision
                        onEditClicked: customServerSheet.openForEdit(modelData.slug)
                        onDeleteClicked: customServerDeleteConfirm.confirmFor(modelData.slug, modelData.displayName)
                    }
                }

                AppButton {
                    text: qsTr("+ Add Custom Server")
                    Layout.fillWidth: true
                    onClicked: customServerSheet.openForAdd()
                }

                Item {
                    Layout.preferredHeight: 32
                }
            }
        }
    }

    CustomServerSetupSheet {
        id: customServerSheet
    }

    Kirigami.PromptDialog {
        id: customServerDeleteConfirm
        title: qsTr("Delete custom server?")
        property string targetSlug: ""
        property string targetName: ""
        subtitle: qsTr("Permanently remove “%1” from the catalogue. The API key (if any) will be cleared from the keychain. Conversations already routing to this server will need a different provider before sending again.").arg(targetName)
        standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.Cancel
        onAccepted: CustomServers.remove(targetSlug)
        function confirmFor(slug, name) {
            targetSlug = slug;
            targetName = name;
            open();
        }
    }

    AppOverlayDialog {
        id: setupSheet
        parent: applicationWindow().overlay
        closePolicy: Controls.Popup.CloseOnEscape

        implicitWidth: Math.min(applicationWindow().width * 0.85, Kirigami.Units.gridUnit * 32)

        property string providerId: ""
        property string providerLabel: ""
        property string fieldKey: ""
        property string fieldUrl: ""
        property bool llamaRemoteTools: false
        property string testStatus: ""
        property string testMessage: ""

        function subtitleFor(id) {
            switch (id) {
            case "openai":
                return qsTr("OpenAI cloud API key. Powers GPT models AND DALL-E image generation.");
            case "anthropic":
                return qsTr("Anthropic cloud API key for Claude models.");
            case "gemini":
                return qsTr("Google Gemini cloud API key.");
            case "openrouter":
                return qsTr("OpenRouter routes to many models through one API.");
            case "deepseek":
                return qsTr("DeepSeek-Chat and DeepSeek-Reasoner cloud API.");
            case "ollama":
                return qsTr("Self-hosted Ollama server that runs on this machine or your network.");
            case "llamacpp_remote":
                return qsTr("HTTP client for an externally-running llama.cpp server.");
            case "llamacpp_local":
                return qsTr("Run a GGUF model with the bundled on-device engine. Memory use is about the size of the GGUF file.");
            }
            return "";
        }

        function openFor(id) {
            providerId = id;
            switch (id) {
            case "openai":
                providerLabel = "OpenAI";
                break;
            case "anthropic":
                providerLabel = "Anthropic";
                break;
            case "gemini":
                providerLabel = "Google Gemini";
                break;
            case "openrouter":
                providerLabel = "OpenRouter";
                break;
            case "deepseek":
                providerLabel = "DeepSeek";
                break;
            case "ollama":
                providerLabel = "Ollama";
                break;
            case "llamacpp_remote":
                providerLabel = "llama.cpp (Remote)";
                break;
            case "llamacpp_local":
                providerLabel = "llama.cpp (Local)";
                break;
            default:
                providerLabel = id;
            }
            fieldKey = "";
            fieldUrl = "";
            llamaRemoteTools = false;
            testStatus = "";
            testMessage = "";
            if (id === "ollama") {
                fieldUrl = SettingsService.ollamaBaseUrl;
            } else if (id === "llamacpp_remote") {
                fieldUrl = SettingsService.providerBaseUrl("llamacpp_remote");
                llamaRemoteTools = SettingsService.llamaCppRemoteSupportsTools();
            } else if (id === "openrouter" || id === "deepseek") {
                fieldUrl = SettingsService.providerBaseUrl(id);
            } else if (id === "llamacpp_local") {
                fieldUrl = SettingsService.llamaCppModelPath;
            }
            open();
        }

        Connections {
            target: SettingsService
            function onOllamaConnectionResult(success, message) {
                if (!setupSheet.visible)
                    return;
                setupSheet.testStatus = success ? "success" : "error";
                setupSheet.testMessage = message;
            }
        }

        title: setupSheet.providerLabel.length > 0 ? qsTr("Configure %1").arg(setupSheet.providerLabel) : qsTr("Configure provider")
        subtitle: setupSheet.subtitleFor(setupSheet.providerId)
        dialogIcon: "settings-configure"

        footer: RowLayout {
            spacing: Kirigami.Units.smallSpacing

            AppButton {
                text: qsTr("Remove")
                icon.name: "edit-delete"
                visible: {
                    var id = setupSheet.providerId;
                    if (id === "openai" || id === "anthropic" || id === "gemini" || id === "openrouter" || id === "deepseek" || id === "llamacpp_remote") {
                        return SettingsService.hasApiKey(id);
                    }
                    if (id === "ollama") {
                        return SettingsService.ollamaBaseUrl.length > 0;
                    }
                    if (id === "llamacpp_local") {
                        return SettingsService.llamaCppModelPath.length > 0;
                    }
                    return false;
                }
                onClicked: {
                    var id = setupSheet.providerId;
                    if (id === "openai" || id === "anthropic" || id === "gemini") {
                        SettingsService.setApiKey(id, "");
                    } else if (id === "openrouter" || id === "deepseek") {
                        SettingsService.setApiKey(id, "");
                        SettingsService.setProviderBaseUrl(id, "");
                    } else if (id === "ollama") {
                        SettingsService.ollamaBaseUrl = "";
                    } else if (id === "llamacpp_remote") {
                        SettingsService.setApiKey("llamacpp_remote", "");
                        SettingsService.setProviderBaseUrl("llamacpp_remote", "");
                        SettingsService.setLlamaCppRemoteSupportsTools(false);
                    } else if (id === "llamacpp_local") {
                        SettingsService.llamaCppModelPath = "";
                    }
                    setupSheet.close();
                }
            }

            Item {
                Layout.fillWidth: true
            }

            AppButton {
                text: qsTr("Cancel")
                onClicked: setupSheet.close()
            }
            AppButton {
                text: qsTr("Save")
                highlighted: true
                onClicked: {
                    var id = setupSheet.providerId;
                    if (id === "openai" || id === "anthropic" || id === "gemini") {
                        if (setupSheet.fieldKey.length > 0) {
                            SettingsService.setApiKey(id, setupSheet.fieldKey);
                        }
                    } else if (id === "openrouter" || id === "deepseek") {
                        if (setupSheet.fieldKey.length > 0) {
                            SettingsService.setApiKey(id, setupSheet.fieldKey);
                        }
                        SettingsService.setProviderBaseUrl(id, setupSheet.fieldUrl);
                    } else if (id === "ollama") {
                        SettingsService.ollamaBaseUrl = setupSheet.fieldUrl;
                    } else if (id === "llamacpp_remote") {
                        if (setupSheet.fieldKey.length > 0) {
                            SettingsService.setApiKey("llamacpp_remote", setupSheet.fieldKey);
                        }
                        SettingsService.setProviderBaseUrl("llamacpp_remote", setupSheet.fieldUrl);
                        SettingsService.setLlamaCppRemoteSupportsTools(setupSheet.llamaRemoteTools);
                    } else if (id === "llamacpp_local") {
                        SettingsService.llamaCppModelPath = setupSheet.fieldUrl;
                    }
                    setupSheet.close();
                }
            }
        }

        ColumnLayout {
            spacing: 14

            ColumnLayout {
                spacing: 4
                visible: setupSheet.providerId === "openai" || setupSheet.providerId === "anthropic" || setupSheet.providerId === "gemini" || setupSheet.providerId === "openrouter" || setupSheet.providerId === "deepseek"
                Layout.fillWidth: true

                Controls.Label {
                    text: qsTr("API Key")
                    font.bold: true
                }
                AppTextField {
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    text: setupSheet.fieldKey
                    onTextChanged: setupSheet.fieldKey = text
                    placeholderText: SettingsService.hasApiKey(setupSheet.providerId) ? qsTr("Stored. Type to replace, or leave blank to keep") : qsTr("Paste your API key")
                }
            }

            ColumnLayout {
                spacing: 4
                visible: setupSheet.providerId === "llamacpp_remote"
                Layout.fillWidth: true

                Controls.Label {
                    text: qsTr("Bearer Token (optional)")
                    font.bold: true
                }
                AppTextField {
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    text: setupSheet.fieldKey
                    onTextChanged: setupSheet.fieldKey = text
                    placeholderText: SettingsService.hasApiKey("llamacpp_remote") ? qsTr("Stored. Leave blank to keep") : qsTr("Only needed if behind a reverse proxy")
                }
                Controls.Label {
                    text: qsTr("llama.cpp servers are usually auth-less. Set a token only if you put one behind a reverse proxy.")
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    Layout.fillWidth: true
                }
            }

            ColumnLayout {
                spacing: 4
                visible: setupSheet.providerId === "ollama" || setupSheet.providerId === "llamacpp_remote" || setupSheet.providerId === "llamacpp_local" || setupSheet.providerId === "openrouter" || setupSheet.providerId === "deepseek"
                Layout.fillWidth: true

                Controls.Label {
                    text: {
                        if (setupSheet.providerId === "llamacpp_local")
                            return qsTr("GGUF Model Path");
                        if (setupSheet.providerId === "openrouter" || setupSheet.providerId === "deepseek")
                            return qsTr("Base URL (optional)");
                        return qsTr("Server URL");
                    }
                    font.bold: true
                }
                AppTextField {
                    Layout.fillWidth: true
                    text: setupSheet.fieldUrl
                    onTextChanged: setupSheet.fieldUrl = text
                    placeholderText: {
                        switch (setupSheet.providerId) {
                        case "ollama":
                            return "http://localhost:11434";
                        case "llamacpp_remote":
                            return "http://localhost:8080/v1";
                        case "llamacpp_local":
                            return "/path/to/model.gguf";
                        case "openrouter":
                            return "https://openrouter.ai/api/v1";
                        case "deepseek":
                            return "https://api.deepseek.com/v1";
                        }
                        return "";
                    }
                }
                Controls.Label {
                    visible: setupSheet.providerId === "openrouter" || setupSheet.providerId === "deepseek"
                    text: qsTr("Leave blank to use the default endpoint.")
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    Layout.fillWidth: true
                }
                Controls.Label {
                    visible: setupSheet.providerId === "ollama"
                    text: qsTr("Defaults to localhost. Point at a LAN address to use Ollama on another machine.")
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    Layout.fillWidth: true
                }
            }

            Controls.CheckBox {
                visible: setupSheet.providerId === "llamacpp_remote"
                text: qsTr("My llama.cpp build supports tool calling")
                checked: setupSheet.llamaRemoteTools
                onToggled: setupSheet.llamaRemoteTools = checked
            }

            ColumnLayout {
                visible: setupSheet.providerId === "ollama"
                spacing: 6
                Layout.fillWidth: true

                Kirigami.Separator {
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppButton {
                        text: setupSheet.testStatus === "testing" ? qsTr("Testing…") : qsTr("Test Connection")
                        icon.name: "network-connect"
                        enabled: setupSheet.testStatus !== "testing" && setupSheet.fieldUrl.length > 0
                        onClicked: {
                            setupSheet.testStatus = "testing";
                            setupSheet.testMessage = qsTr("Contacting %1…").arg(setupSheet.fieldUrl);
                            SettingsService.testOllamaConnection(setupSheet.fieldUrl);
                        }
                    }
                    Kirigami.Icon {
                        visible: setupSheet.testStatus === "success"
                        source: "emblem-success"
                        fallback: "dialog-ok"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        color: Kirigami.Theme.positiveTextColor
                    }
                    Kirigami.Icon {
                        visible: setupSheet.testStatus === "error"
                        source: "dialog-error"
                        fallback: "dialog-warning"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        color: Kirigami.Theme.negativeTextColor
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                }
                Controls.Label {
                    visible: setupSheet.testMessage.length > 0
                    text: setupSheet.testMessage
                    color: setupSheet.testStatus === "success" ? Kirigami.Theme.positiveTextColor : setupSheet.testStatus === "error" ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }
    }
}

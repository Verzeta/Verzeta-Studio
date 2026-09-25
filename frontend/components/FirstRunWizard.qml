// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: wizard

    signal closed

    color: ThemeController.surfacePage

    readonly property bool _isMobile: wizard.width < 720

    readonly property var _steps: [{
            "id": "providers",
            "title": qsTr("Providers"),
            "description": qsTr("Connect the AI providers you want to use, " + "whether cloud services or local model servers. " + "Add as many or as few as you like, and switch " + "between them anytime, per conversation or per " + "agent."),
            "kind": "providers"
        }, {
            "id": "ragp",
            "title": qsTr("Local Intelligence"),
            "description": qsTr("Optionally set up two on-device models for " + "private routing and search. Pick a model file " + "you have, or download a recommended one. You " + "can skip this and use your remote providers " + "instead."),
            "kind": "ragp"
        }, {
            "id": "tour-team",
            "title": qsTr("Tour: Multi-Agent Teams"),
            "description": qsTr("Several agents with their own roles, models, " + "and tools work together in one chat, with " + "per-agent settings, polls, projects, and a " + "full history."),
            "kind": "tour",
            "tagline": qsTr("A team of agents in one chat"),
            "bullets": [{
                    "iconName": "user-group-new",
                    "title": qsTr("Build a team"),
                    "detail": qsTr("Multiple agents in one chat, each with its " + "own alias, role, and system prompt. Add or " + "remove members at any time: every change " + "is captured in the activity timeline.")
                }, {
                    "iconName": "network-server",
                    "title": qsTr("Per-member providers + models"),
                    "detail": qsTr("@Alice on Ollama (qwen3), @Bob on Claude, " + "@Carol on Gemini, all in " + "the same conversation. Each member's reply " + "uses the provider you assigned to them.")
                }, {
                    "iconName": "mail-forward",
                    "title": qsTr("@mention routing"),
                    "detail": qsTr("Agents @mention each other to hand off " + "work. Verzeta reads each reply and routes " + "the next turn automatically, with no manual " + "scripting.")
                }, {
                    "iconName": "view-form-list",
                    "title": qsTr("Polls for decisions"),
                    "detail": qsTr("When the team needs a decision, they open " + "a poll. Members vote on the record instead " + "of arguing in circles, so the chat keeps " + "moving and the decision is recorded.")
                }, {
                    "iconName": "folder",
                    "title": qsTr("Projects & coordinator"),
                    "detail": qsTr("Group conversations under a project folder " + "with shared goal docs. The coordinator " + "agent drives task assignment and decisions. " + "Each project has one member roster, shared " + "by every chat in it.")
                }, {
                    "iconName": "documentation",
                    "title": qsTr("Activity timeline"),
                    "detail": qsTr("Every agent turn, tool call, file write, " + "and poll vote is recorded. Open the " + "timeline from any project's Folder " + "Settings or any conversation's Chat " + "Settings.")
                }]
        }, {
            "id": "tour-capabilities",
            "title": qsTr("Tour: What Agents Can Do"),
            "description": qsTr("Your agents do real work: use tools, edit in " + "a canvas, run shared tasks, apply skills, and " + "check in on their own."),
            "kind": "tour",
            "tagline": qsTr("What your agents can do"),
            "bullets": [{
                    "iconName": "tools",
                    "title": qsTr("Tools"),
                    "detail": qsTr("Read and write files, run shell commands " + "in a sandbox, search the web, and more. A " + "per-agent tool list controls exactly what " + "each member is allowed to do.")
                }, {
                    "iconName": "document-edit",
                    "title": qsTr("Canvas"),
                    "detail": qsTr("A side editor where agents open code, " + "configs, or specs for live editing. " + "Python and Bash run inline in a sandbox; " + "other languages get an Open in IDE button. " + "An AI action bar adapts to the language.")
                }, {
                    "iconName": "view-task",
                    "title": qsTr("Tasks"),
                    "detail": qsTr("Anyone on the team can open a shared task, " + "everyone works on it, and anyone can close it. " + "Tasks survive app restarts and conversation " + "switches, keep an optional checklist, and " + "collect every file produced.")
                }, {
                    "iconName": "package-x-generic",
                    "title": qsTr("Skills"),
                    "detail": qsTr("Instruction packs your agents " + "follow when they are relevant. " + "Install them from ClawHub or a local " + "folder, and approve each one first.")
                }, {
                    "iconName": "preferences-system-time",
                    "title": qsTr("Heartbeats"),
                    "detail": qsTr("Per-agent background routines that " + "periodically check on the project. When " + "something needs attention (stale plan, " + "missed deadline, drift), the agent " + "surfaces a message into the chat.")
                }]
        }, {
            "id": "tour-settings-remote",
            "title": qsTr("Tour: Settings & Remote"),
            "description": qsTr("Where the settings live, from global defaults " + "to per-conversation and per-agent, plus " + "pairing the Android app."),
            "kind": "tour",
            "tagline": qsTr("Configure once, use everywhere"),
            "bullets": [{
                    "iconName": "preferences-system",
                    "title": qsTr("Global settings"),
                    "detail": qsTr("Providers, API keys, web search, RAGP " + "and embeddings. Set them once here and " + "the rest of the app uses them.")
                }, {
                    "iconName": "preferences-desktop",
                    "title": qsTr("Per-conversation settings"),
                    "detail": qsTr("Override the model, system prompt, agent " + "pattern, RAG, tools, or Thinking Mode for " + "one chat in Chat Settings, without leaving " + "the conversation.")
                }, {
                    "iconName": "preferences-desktop-user",
                    "title": qsTr("Per-member settings"),
                    "detail": qsTr("The same overrides work per team " + "member, so three Writer agents built " + "from one template can each use " + "a different " + "provider.")
                }, {
                    "iconName": "network-wireless",
                    "title": qsTr("Remote pairing"),
                    "detail": qsTr("Pair an Android phone or VS Code once with a " + "pairing code and keep chatting there. " + "Each paired client gets its own session, " + "so clients never collide with the desktop or " + "with each other.")
                }, {
                    "iconName": "documentation",
                    "title": qsTr("Activity log access"),
                    "detail": qsTr("For a project: Folder Settings → View " + "Activity Log. For a chat: " + "Chat Settings → Activity. " + "Filter by actor (user / agent / system) " + "and event type.")
                }]
        }]

    property int currentStep: 0

    function formatBytes(bytes) {
        if (bytes >= 1000 * 1000 * 1000)
            return (bytes / (1000 * 1000 * 1000)).toFixed(1) + " GB";
        if (bytes >= 1000 * 1000)
            return Math.round(bytes / (1000 * 1000)) + " MB";
        return Math.round(bytes / 1000) + " KB";
    }

    function _next() {
        if (currentStep + 1 < _steps.length) {
            currentStep += 1;
        } else {
            _finish();
        }
    }

    function _skipAll() {
        _finish();
    }

    function _finish() {
        SettingsService.firstRunComplete = true;
        wizard.closed();
    }

    property int _refreshTick: 0
    property int _customServerRefreshTick: 0
    Connections {
        target: CustomServers
        function onServersChanged() {
            wizard._customServerRefreshTick++;
        }
    }
    Connections {
        target: SettingsService
        function onSettingsChanged(key) {
            wizard._refreshTick++;
        }
        function onOllamaBaseUrlChanged() {
            wizard._refreshTick++;
        }
        function onProviderBaseUrlChanged(p, u) {
            wizard._refreshTick++;
        }
        function onLlamaCppModelPathChanged() {
            wizard._refreshTick++;
        }
    }

    function cloudStatus(providerId) {
        var _ = wizard._refreshTick;
        return SettingsService.hasApiKey(providerId) ? {
            "text": qsTr("Connected"),
            "kind": "connected"
        } : {
            "text": qsTr("Not configured"),
            "kind": "unconfigured"
        };
    }
    function ollamaStatus() {
        var _ = wizard._refreshTick;
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
        var _ = wizard._refreshTick;
        var url = SettingsService.providerBaseUrl("llamacpp_remote");
        return url.length > 0 ? {
            "text": url,
            "kind": "info"
        } : {
            "text": qsTr("Default (localhost:8080)"),
            "kind": "info"
        };
    }
    function customServerStatus() {
        var _ = wizard._refreshTick;
        var __ = wizard._customServerRefreshTick;
        var n = CustomServers.list().length;
        if (n === 0) {
            return {
                "text": qsTr("Click Configure to add one"),
                "kind": "unconfigured"
            };
        }
        return {
            "text": qsTr("%1 configured").arg(n),
            "kind": "connected"
        };
    }
    function llamaLocalStatus() {
        var _ = wizard._refreshTick;
        if (!SettingsService.hasLocalLlama) {
            return {
                "text": qsTr("Unavailable in this build"),
                "kind": "unconfigured"
            };
        }
        var path = SettingsService.llamaCppModelPath;
        return path.length > 0 ? {
            "text": qsTr("On-Device"),
            "kind": "connected"
        } : {
            "text": qsTr("Not configured"),
            "kind": "unconfigured"
        };
    }

    Rectangle {
        id: headerStrip
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 88
        color: ThemeController.surfaceCard

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: ThemeController.borderSubtle
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            spacing: 16

            Kirigami.Icon {
                source: "qrc:/icons/verzeta-studio.svg"
                fallback: "applications-multimedia"
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
                Layout.alignment: Qt.AlignVCenter
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Controls.Label {
                    text: qsTr("Get Started with Verzeta Studio")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize + 4
                    font.bold: true
                    color: Kirigami.Theme.textColor
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Controls.Label {
                    text: qsTr("A few quick steps to make the workspace yours.")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                    color: Kirigami.Theme.disabledTextColor
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            Controls.Button {
                text: qsTr("Skip All")
                icon.name: "dialog-close"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: 36
                onClicked: wizard._skipAll()
            }
        }
    }

    RowLayout {
        anchors.top: headerStrip.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 0
        visible: !wizard._isMobile

        Rectangle {
            Layout.preferredWidth: 320
            Layout.fillHeight: true
            color: "transparent"

            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                width: 1
                color: ThemeController.borderSubtle
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8

                Repeater {
                    model: wizard._steps.length
                    delegate: Rectangle {
                        required property int index
                        readonly property var step: wizard._steps[index]
                        readonly property bool _isActive: wizard.currentStep === index
                        readonly property bool _isDone: wizard.currentStep > index

                        Layout.fillWidth: true
                        implicitHeight: _isActive ? activeContent.implicitHeight + 28 : 44
                        radius: ThemeController.radius
                        color: _isActive ? ThemeController.selectionTint : "transparent"
                        border.color: _isActive ? Kirigami.Theme.highlightColor : "transparent"
                        border.width: _isActive ? 1 : 0
                        Behavior on implicitHeight  {
                            NumberAnimation {
                                duration: 120
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: wizard.currentStep = index
                        }

                        ColumnLayout {
                            id: activeContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            anchors.topMargin: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Rectangle {
                                    width: 22
                                    height: 22
                                    radius: height / 2
                                    Layout.alignment: Qt.AlignVCenter
                                    color: _isDone ? Kirigami.Theme.highlightColor : (_isActive ? Kirigami.Theme.highlightColor : "transparent")
                                    border.color: _isDone || _isActive ? Kirigami.Theme.highlightColor : Qt.darker(Kirigami.Theme.backgroundColor, 1.5)
                                    border.width: 1.5

                                    Controls.Label {
                                        anchors.centerIn: parent
                                        visible: _isDone
                                        text: "✓"
                                        font.bold: true
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                        color: "white"
                                    }
                                }

                                Controls.Label {
                                    text: step.title
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                    font.bold: _isActive
                                    color: _isActive || _isDone ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }

                            Controls.Label {
                                visible: _isActive
                                text: step.description
                                color: Kirigami.Theme.textColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                Layout.leftMargin: 32
                            }

                            RowLayout {
                                visible: _isActive
                                Layout.fillWidth: true
                                Layout.leftMargin: 32
                                Layout.topMargin: 4
                                spacing: 8

                                Controls.Button {
                                    text: wizard.currentStep + 1 < wizard._steps.length ? qsTr("Next") : qsTr("Done")
                                    highlighted: true
                                    onClicked: wizard._next()
                                }
                                Controls.Button {
                                    text: qsTr("Skip")
                                    flat: true
                                    enabled: wizard.currentStep + 1 < wizard._steps.length
                                    onClicked: wizard._next()
                                }
                            }
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: parent ? parent.width : 0
                spacing: 16

                Loader {
                    Layout.fillWidth: true
                    Layout.topMargin: 24
                    Layout.leftMargin: 32
                    Layout.rightMargin: 32
                    Layout.bottomMargin: 24
                    sourceComponent: {
                        const k = wizard._steps[wizard.currentStep].kind;
                        if (k === "providers")
                            return providersComponent;
                        if (k === "ragp")
                            return ragpComponent;
                        return tourComponent;
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.top: headerStrip.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 0
        visible: wizard._isMobile

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 36
            color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.08)

            Controls.Label {
                anchors.fill: parent
                anchors.leftMargin: 16
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Step %1 of %2: %3").arg(wizard.currentStep + 1).arg(wizard._steps.length).arg(wizard._steps[wizard.currentStep].title)
                font.bold: true
                color: Kirigami.Theme.highlightColor
                elide: Text.ElideRight
            }
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: parent ? parent.width : 0
                spacing: 12

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    text: wizard._steps[wizard.currentStep].description
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.WordWrap
                }

                Loader {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.bottomMargin: 16
                    sourceComponent: {
                        const k = wizard._steps[wizard.currentStep].kind;
                        if (k === "providers")
                            return providersComponent;
                        if (k === "ragp")
                            return ragpComponent;
                        return tourComponent;
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 60
            color: ThemeController.surfaceCard
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: ThemeController.borderSubtle
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                Controls.Button {
                    text: qsTr("Back")
                    icon.name: "go-previous"
                    enabled: wizard.currentStep > 0
                    onClicked: wizard.currentStep -= 1
                }

                Item {
                    Layout.fillWidth: true
                }

                Controls.Button {
                    text: qsTr("Skip")
                    enabled: wizard.currentStep + 1 < wizard._steps.length
                    onClicked: wizard._next()
                }

                Controls.Button {
                    text: wizard.currentStep + 1 < wizard._steps.length ? qsTr("Next") : qsTr("Done")
                    icon.name: wizard.currentStep + 1 < wizard._steps.length ? "go-next" : "dialog-ok"
                    highlighted: true
                    Layout.preferredWidth: 120
                    onClicked: wizard._next()
                }
            }
        }
    }

    Component {
        id: providersComponent
        ColumnLayout {
            spacing: 16

            Controls.Label {
                text: qsTr("Configure providers")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize + 2
                font.bold: true
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                text: qsTr("Click any provider to configure it. Add " + "as many or as few as you like. Keys are stored " + "locally, and you can switch providers per " + "conversation or per member later.")
            }

            Kirigami.Heading {
                text: qsTr("Cloud Providers")
                level: 4
                Layout.topMargin: 8
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                text: qsTr("Hosted API providers. Only an API key " + "is needed.")
            }

            ProviderCard {
                providerName: "OpenAI"
                iconName: "internet-services"
                subtitle: qsTr("GPT models. Also powers DALL-E image generation.")
                status: wizard.cloudStatus("openai").text
                statusKind: wizard.cloudStatus("openai").kind
                onConfigure: setupSheet.openFor("openai")
            }
            ProviderCard {
                providerName: "Anthropic"
                iconName: "internet-services"
                subtitle: qsTr("Claude models.")
                status: wizard.cloudStatus("anthropic").text
                statusKind: wizard.cloudStatus("anthropic").kind
                onConfigure: setupSheet.openFor("anthropic")
            }
            ProviderCard {
                providerName: "Google Gemini"
                iconName: "internet-services"
                subtitle: qsTr("Gemini models.")
                status: wizard.cloudStatus("gemini").text
                statusKind: wizard.cloudStatus("gemini").kind
                onConfigure: setupSheet.openFor("gemini")
            }
            ProviderCard {
                providerName: "OpenRouter"
                iconName: "internet-services"
                subtitle: qsTr("Routes to many models through one API.")
                status: wizard.cloudStatus("openrouter").text
                statusKind: wizard.cloudStatus("openrouter").kind
                onConfigure: setupSheet.openFor("openrouter")
            }
            ProviderCard {
                providerName: "DeepSeek"
                iconName: "internet-services"
                subtitle: qsTr("DeepSeek-Chat and DeepSeek-Reasoner.")
                status: wizard.cloudStatus("deepseek").text
                statusKind: wizard.cloudStatus("deepseek").kind
                onConfigure: setupSheet.openFor("deepseek")
            }

            Kirigami.Heading {
                text: qsTr("Remote Servers")
                level: 4
                Layout.topMargin: 12
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                text: qsTr("Connect to your own Ollama or llama.cpp server " + "running on this machine or another on your LAN.")
            }

            ProviderCard {
                providerName: "Ollama"
                iconName: "network-server"
                subtitle: qsTr("Local or LAN-hosted Ollama server.")
                status: wizard.ollamaStatus().text
                statusKind: wizard.ollamaStatus().kind
                onConfigure: setupSheet.openFor("ollama")
            }
            ProviderCard {
                providerName: "llama.cpp (Remote)"
                iconName: "network-server"
                subtitle: qsTr("HTTP client for an externally-running llama.cpp server.")
                status: wizard.llamaRemoteStatus().text
                statusKind: wizard.llamaRemoteStatus().kind
                onConfigure: setupSheet.openFor("llamacpp_remote")
            }

            Kirigami.Heading {
                text: qsTr("Local Models")
                level: 4
                Layout.topMargin: 12
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                text: qsTr("On-device inference using llama.cpp. The bundled " + "engine runs GGUF model files on this machine. " + "Pick the GGUF model file and RAGP backend in the " + "next step.")
            }

            ProviderCard {
                providerName: "llama.cpp (Local)"
                iconName: "computer"
                subtitle: qsTr("On-device inference with a GGUF model on this machine.")
                status: wizard.llamaLocalStatus().text
                statusKind: wizard.llamaLocalStatus().kind
                onConfigure: setupSheet.openFor("llamacpp_local")
            }

            Kirigami.Heading {
                text: qsTr("Custom OpenAI-Compatible Servers")
                level: 4
                Layout.topMargin: 12
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                text: qsTr("Already running vLLM, LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI, SGLang, or text-generation-webui? Point Verzeta at it. Add as many as you like, and each agent can use its own server.")
            }

            ProviderCard {
                providerName: qsTr("Custom OpenAI-Compatible Server")
                iconName: "network-server"
                subtitle: qsTr("vLLM, LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI, SGLang, text-generation-webui, or anything that speaks /v1/chat/completions.")
                status: wizard.customServerStatus().text
                statusKind: wizard.customServerStatus().kind
                onConfigure: customServerSheet.openForAdd()
            }
        }
    }

    CustomServerSetupSheet {
        id: customServerSheet
    }

    Component {
        id: ragpComponent
        ColumnLayout {
            spacing: 16

            Controls.Label {
                text: qsTr("Local intelligence: routing and retrieval")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize + 2
                font.bold: true
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                text: qsTr("Routing decides who replies next in a group " + "chat. Run it on-device for speed and privacy, or " + "use one of your providers.")
            }

            AppRadioButton {
                id: remoteRadio
                text: qsTr("Use a remote provider")
                checked: !SettingsService.ragpLocalEnabled
                onToggled: if (checked)
                    SettingsService.ragpLocalEnabled = false
            }
            AppRadioButton {
                id: localRadio
                text: SettingsService.hasLocalLlama ? qsTr("Use the on-device model") : qsTr("Use the on-device model (not in this build)")
                enabled: SettingsService.hasLocalLlama
                checked: SettingsService.ragpLocalEnabled
                onToggled: if (checked)
                    SettingsService.ragpLocalEnabled = true
            }

            Controls.Label {
                visible: !SettingsService.hasLocalLlama
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                text: qsTr("This build does not include the on-device " + "engine. Download the Local AI edition to run " + "models on this machine, or keep using your " + "remote providers. Everything else works the same.")
            }

            Rectangle {
                visible: SettingsService.ragpLocalEnabled && SettingsService.hasLocalLlama
                Layout.fillWidth: true
                Layout.topMargin: 8
                radius: ThemeController.radius
                color: ThemeController.surfaceCard
                border.color: ThemeController.borderSubtle
                border.width: 1
                implicitHeight: ragpModelCol.implicitHeight + 24

                ColumnLayout {
                    id: ragpModelCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 8

                    Controls.Label {
                        text: qsTr("Default GGUF model")
                        font.bold: true
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                        text: qsTr("Required when local llama.cpp is " + "selected. Without a model picked, " + "RAGP silently falls back to the " + "remote provider.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Controls.ComboBox {
                            id: ragpModelCombo
                            Layout.fillWidth: true
                            enabled: count > 0
                            model: _availableModels
                            property var _availableModels: []

                            function _refresh() {
                                _availableModels = SettingsService.ragpAvailableModels();
                                const idx = _availableModels.indexOf(SettingsService.ragpDefaultModelFilename);
                                currentIndex = (idx >= 0) ? idx : 0;
                            }

                            Component.onCompleted: _refresh()

                            onActivated: {
                                if (currentIndex >= 0 && currentIndex < _availableModels.length) {
                                    SettingsService.ragpDefaultModelFilename = _availableModels[currentIndex];
                                }
                            }
                        }

                        Controls.Button {
                            text: qsTr("Browse…")
                            icon.name: "folder-open"
                            onClicked: wizardRagpModelDialog.open()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: !ModelDownloads.recommendedPresent("ragp")
                        Controls.Button {
                            icon.name: "download"
                            enabled: !ModelDownloads.active
                            text: qsTr("Download recommended (%1, %2)").arg(ModelDownloads.recommendedFilename("ragp")).arg(wizard.formatBytes(ModelDownloads.recommendedBytes("ragp")))
                            onClicked: ModelDownloads.start("ragp")
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: ModelDownloads.active && ModelDownloads.activeKind === "ragp"
                        Controls.ProgressBar {
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            value: ModelDownloads.progress
                        }
                        Controls.Label {
                            text: qsTr("%1%  ·  %2 / %3").arg(Math.round(ModelDownloads.progress * 100)).arg(wizard.formatBytes(ModelDownloads.receivedBytes)).arg(wizard.formatBytes(ModelDownloads.totalBytes))
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                        }
                        Controls.ToolButton {
                            icon.name: "process-stop"
                            Controls.ToolTip.text: qsTr("Cancel download")
                            Controls.ToolTip.visible: hovered
                            onClicked: ModelDownloads.cancel()
                        }
                    }
                    Connections {
                        target: ModelDownloads
                        function onDownloadFinished(kind, filename) {
                            if (kind !== "ragp")
                                return;
                            SettingsService.ragpDefaultModelFilename = filename;
                            ragpModelCombo._refresh();
                            ragpAddStatus.statusKind = "ok";
                            ragpAddStatus.statusMessage = qsTr("Downloaded, verified and selected: %1").arg(filename);
                        }
                        function onDownloadFailed(kind, error) {
                            if (kind !== "ragp")
                                return;
                            ragpAddStatus.statusKind = "err";
                            ragpAddStatus.statusMessage = error;
                        }
                    }

                    Controls.Label {
                        visible: ragpModelCombo._availableModels.length === 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                        text: qsTr("No .gguf models yet. Use Browse… to " + "pick a file anywhere on disk (it will " + "be symlinked into the models folder, " + "your original file is not moved or " + "copied).")
                    }

                    Rectangle {
                        id: ragpAddStatus
                        Layout.fillWidth: true
                        visible: statusKind !== "idle"
                        radius: ThemeController.radius
                        implicitHeight: ragpAddStatusRow.implicitHeight + 12

                        property string statusKind: "idle"
                        property string statusMessage: ""

                        readonly property color _accent: statusKind === "ok" ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor

                        color: Qt.rgba(_accent.r, _accent.g, _accent.b, 0.12)
                        border.color: _accent
                        border.width: 1

                        RowLayout {
                            id: ragpAddStatusRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 8

                            Controls.Label {
                                text: ragpAddStatus.statusKind === "ok" ? "✓" : "✗"
                                font.bold: true
                                color: ragpAddStatus._accent
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Controls.Label {
                                text: ragpAddStatus.statusMessage
                                color: ragpAddStatus._accent
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                    }
                }

                FileDialog {
                    id: wizardRagpModelDialog
                    title: qsTr("Add GGUF Model for RAGP")
                    nameFilters: ["GGUF Models (*.gguf)", "All Files (*)"]
                    onAccepted: {
                        const path = PathUtils.toLocalFile(selectedFile);
                        if (!path || path.length === 0)
                            return;
                        const err = SettingsService.ragpAddModelFromPath(path);
                        if (err.length > 0) {
                            ragpAddStatus.statusKind = "err";
                            ragpAddStatus.statusMessage = err;
                            return;
                        }
                        const basename = path.substring(path.lastIndexOf("/") + 1);
                        SettingsService.ragpDefaultModelFilename = basename;
                        ragpModelCombo._refresh();
                        ragpAddStatus.statusKind = "ok";
                        ragpAddStatus.statusMessage = qsTr("Added and selected: %1").arg(basename);
                    }
                }
            }

            Rectangle {
                visible: SettingsService.hasLocalLlama
                Layout.fillWidth: true
                Layout.topMargin: 8
                radius: ThemeController.radius
                color: ThemeController.surfaceCard
                border.color: ThemeController.borderSubtle
                border.width: 1
                implicitHeight: embedModelCol.implicitHeight + 24

                ColumnLayout {
                    id: embedModelCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 8

                    Controls.Label {
                        text: qsTr("Embedding model (RAG retrieval)")
                        font.bold: true
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                        text: qsTr("Turns messages and documents into " + "vectors for retrieval, entirely " + "on-device. Optional: skip it and " + "configure a remote embeddings " + "endpoint later in Settings.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Controls.Switch {
                            id: wizardEmbedLocalSwitch
                            text: qsTr("Use a local embedding model")
                            checked: SettingsService.embeddingLocalEnabled
                            onToggled: SettingsService.embeddingLocalEnabled = checked
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: wizardEmbedLocalSwitch.checked

                        Controls.ComboBox {
                            id: wizardEmbedModelCombo
                            Layout.fillWidth: true
                            enabled: count > 0
                            model: _availableModels
                            property var _availableModels: []

                            function _refresh() {
                                _availableModels = SettingsService.embeddingAvailableModels();
                                const idx = _availableModels.indexOf(SettingsService.embeddingLocalModelFilename);
                                currentIndex = (idx >= 0) ? idx : 0;
                            }

                            Component.onCompleted: _refresh()

                            onActivated: {
                                if (currentIndex >= 0 && currentIndex < _availableModels.length) {
                                    SettingsService.embeddingLocalModelFilename = _availableModels[currentIndex];
                                }
                            }
                        }

                        Controls.Button {
                            text: qsTr("Browse…")
                            icon.name: "folder-open"
                            onClicked: wizardEmbedModelDialog.open()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: wizardEmbedLocalSwitch.checked && !ModelDownloads.recommendedPresent("embed")
                        Controls.Button {
                            icon.name: "download"
                            enabled: !ModelDownloads.active
                            text: qsTr("Download recommended (%1, %2)").arg(ModelDownloads.recommendedFilename("embed")).arg(wizard.formatBytes(ModelDownloads.recommendedBytes("embed")))
                            onClicked: ModelDownloads.start("embed")
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: ModelDownloads.active && ModelDownloads.activeKind === "embed"
                        Controls.ProgressBar {
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            value: ModelDownloads.progress
                        }
                        Controls.Label {
                            text: qsTr("%1%  ·  %2 / %3").arg(Math.round(ModelDownloads.progress * 100)).arg(wizard.formatBytes(ModelDownloads.receivedBytes)).arg(wizard.formatBytes(ModelDownloads.totalBytes))
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                        }
                        Controls.ToolButton {
                            icon.name: "process-stop"
                            Controls.ToolTip.text: qsTr("Cancel download")
                            Controls.ToolTip.visible: hovered
                            onClicked: ModelDownloads.cancel()
                        }
                    }

                    Rectangle {
                        id: embedAddStatus
                        Layout.fillWidth: true
                        visible: statusKind !== "idle"
                        radius: ThemeController.radius
                        implicitHeight: embedAddStatusRow.implicitHeight + 12

                        property string statusKind: "idle"
                        property string statusMessage: ""

                        readonly property color _accent: statusKind === "ok" ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor

                        color: Qt.rgba(_accent.r, _accent.g, _accent.b, 0.12)
                        border.color: _accent
                        border.width: 1

                        RowLayout {
                            id: embedAddStatusRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 8

                            Controls.Label {
                                text: embedAddStatus.statusKind === "ok" ? "✓" : "✗"
                                font.bold: true
                                color: embedAddStatus._accent
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Controls.Label {
                                text: embedAddStatus.statusMessage
                                color: embedAddStatus._accent
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                    }

                    Connections {
                        target: ModelDownloads
                        function onDownloadFinished(kind, filename) {
                            if (kind !== "embed")
                                return;
                            SettingsService.embeddingLocalModelFilename = filename;
                            wizardEmbedModelCombo._refresh();
                            embedAddStatus.statusKind = "ok";
                            embedAddStatus.statusMessage = qsTr("Downloaded, verified and selected: %1").arg(filename);
                        }
                        function onDownloadFailed(kind, error) {
                            if (kind !== "embed")
                                return;
                            embedAddStatus.statusKind = "err";
                            embedAddStatus.statusMessage = error;
                        }
                    }
                }

                FileDialog {
                    id: wizardEmbedModelDialog
                    title: qsTr("Add GGUF Embedding Model")
                    nameFilters: ["GGUF Models (*.gguf)", "All Files (*)"]
                    onAccepted: {
                        const path = PathUtils.toLocalFile(selectedFile);
                        if (!path || path.length === 0)
                            return;
                        const err = SettingsService.embeddingAddModelFromPath(path);
                        if (err.length > 0) {
                            embedAddStatus.statusKind = "err";
                            embedAddStatus.statusMessage = err;
                            return;
                        }
                        const basename = path.substring(path.lastIndexOf("/") + 1);
                        SettingsService.embeddingLocalModelFilename = basename;
                        wizardEmbedModelCombo._refresh();
                        embedAddStatus.statusKind = "ok";
                        embedAddStatus.statusMessage = qsTr("Added and selected: %1").arg(basename);
                    }
                }
            }
        }
    }

    Component {
        id: tourComponent
        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing * 2

            Kirigami.Heading {
                text: wizard._steps[wizard.currentStep].tagline || ""
                level: 3
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                visible: text.length > 0
            }

            Repeater {
                model: wizard._steps[wizard.currentStep].bullets || []
                delegate: Rectangle {
                    required property var modelData

                    Layout.fillWidth: true
                    implicitHeight: Math.max(56, bulletRow.implicitHeight + Kirigami.Units.largeSpacing * 2)
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: ThemeController.borderSubtle
                    border.width: 1

                    RowLayout {
                        id: bulletRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Kirigami.Units.largeSpacing + 4
                        anchors.rightMargin: Kirigami.Units.largeSpacing + 4
                        spacing: Kirigami.Units.largeSpacing + 4

                        Kirigami.Icon {
                            source: modelData.iconName || "preferences-system"
                            fallback: "preferences-system"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 2
                            color: Kirigami.Theme.highlightColor
                            opacity: 0.85
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Controls.Label {
                                text: modelData.title || ""
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: Kirigami.Theme.textColor
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: modelData.detail || ""
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
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
                        fallback: "emblem-error"
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

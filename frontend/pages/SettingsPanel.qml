// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: settingsPanel
    color: ThemeController.surfacePage
    clip: true

    property string activeSubPage: "main"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: applicationWindow().isWide ? 56 : 88
            color: ThemeController.surfaceCard
            visible: settingsPanel.activeSubPage === "main"

            Kirigami.Heading {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: applicationWindow().isWide ? 0 : 16
                anchors.leftMargin: 24
                anchors.rightMargin: 24
                level: 2
                elide: Text.ElideRight
                text: {
                    if (mainTabs.currentIndex === 0)
                        return qsTr("Settings");
                    if (diagSubTabs.currentIndex === 0)
                        return qsTr("Heartbeat Diagnostics");
                    return qsTr("Self-Configuration Audit");
                }
            }

            Kirigami.Separator {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
            }
        }

        Controls.TabBar {
            id: mainTabs
            Layout.fillWidth: true
            visible: settingsPanel.activeSubPage === "main"

            Controls.TabButton {
                text: qsTr("Settings")
                icon.name: "settings-configure"
                Layout.fillWidth: true
            }
            Controls.TabButton {
                text: qsTr("Diagnostics")
                icon.name: "utilities-system-monitor"
                Layout.fillWidth: true
            }
        }

        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            active: settingsPanel.activeSubPage !== "main"
            visible: active
            sourceComponent: {
                if (settingsPanel.activeSubPage === "text_providers")
                    return textProvidersComp;
                if (settingsPanel.activeSubPage === "image_providers")
                    return imageProvidersComp;
                if (settingsPanel.activeSubPage === "search_providers")
                    return searchProvidersComp;
                if (settingsPanel.activeSubPage === "ragp")
                    return ragpProvidersComp;
                if (settingsPanel.activeSubPage === "embeddings")
                    return embeddingsProvidersComp;
                return null;
            }
        }
        Component {
            id: textProvidersComp
            TextProvidersPage {
                onBack: settingsPanel.activeSubPage = "main"
            }
        }
        Component {
            id: imageProvidersComp
            ImageProvidersPage {
                onBack: settingsPanel.activeSubPage = "main"
            }
        }
        Component {
            id: searchProvidersComp
            SearchProvidersPage {
                onBack: settingsPanel.activeSubPage = "main"
            }
        }
        Component {
            id: ragpProvidersComp
            RagpProvidersPage {
                onBack: settingsPanel.activeSubPage = "main"
            }
        }
        Component {
            id: embeddingsProvidersComp
            EmbeddingsProvidersPage {
                onBack: settingsPanel.activeSubPage = "main"
            }
        }

        Controls.SwipeView {
            id: mainSwipe
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: mainTabs.currentIndex
            interactive: false
            clip: true
            visible: settingsPanel.activeSubPage === "main"

            Item {
                clip: true
                Controls.ScrollView {
                    id: settingsScroll
                    contentWidth: applicationWindow().isCompact ? availableWidth : -1
                    anchors.fill: parent
                    Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
                    clip: true

                    ColumnLayout {
                        id: defaultsColumn
                        width: applicationWindow().isCompact ? Math.min(settingsScroll.availableWidth - 32, 720) : Math.min(settingsPanel.width - 32, 720)
                        x: applicationWindow().isCompact ? Math.max(16, (settingsScroll.availableWidth - width) / 2) : Math.max(16, (settingsPanel.width - width) / 2)
                        spacing: 4

                        Item {
                            Layout.preferredHeight: 16
                        }

                        Kirigami.Heading {
                            text: qsTr("Defaults")
                            level: 4
                            Layout.bottomMargin: 8
                        }

                        Controls.Label {
                            text: qsTr("Default Text Provider")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        }
                        Controls.ComboBox {
                            id: providerCombo
                            model: AgentSettings.availableProviders
                            textRole: "displayName"
                            valueRole: "providerId"
                            Layout.fillWidth: true
                            currentIndex: {
                                var providers = AgentSettings.availableProviders;
                                for (var i = 0; i < providers.length; ++i) {
                                    if (providers[i].providerId === AgentSettings.activeProvider)
                                        return i;
                                }
                                return 0;
                            }
                            onActivated: {
                                SettingsService.defaultProvider = currentValue;
                                AgentSettings.setModel(currentValue, "");
                            }
                        }
                        Controls.Label {
                            text: qsTr("Active: %1 / %2").arg(AgentSettings.activeProvider).arg(AgentSettings.activeModel.length > 0 ? AgentSettings.activeModel : qsTr("(none)"))
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            Layout.bottomMargin: 8
                        }

                        Controls.Label {
                            text: qsTr("Default Image Provider")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        }
                        ListModel {
                            id: imageProviderOptions
                            ListElement {
                                providerId: ""
                                displayName: qsTr("None")
                            }
                        }
                        function rebuildImageProviderOptions() {
                            imageProviderOptions.clear();
                            imageProviderOptions.append({
                                    "providerId": "",
                                    "displayName": qsTr("None (disabled)")
                                });
                            var rows = ImageProviders.providers;
                            for (var i = 0; i < rows.length; ++i) {
                                imageProviderOptions.append({
                                        "providerId": rows[i].id,
                                        "displayName": rows[i].displayName
                                    });
                            }
                        }
                        Connections {
                            target: ImageProviders
                            function onProvidersChanged() {
                                defaultsColumn.rebuildImageProviderOptions();
                                imageProviderCombo.currentIndex = imageProviderCombo.indexForBackend(ImageProviders.activeProviderId);
                            }
                            function onActiveProviderChanged() {
                                imageProviderCombo.currentIndex = imageProviderCombo.indexForBackend(ImageProviders.activeProviderId);
                            }
                        }
                        Component.onCompleted: defaultsColumn.rebuildImageProviderOptions()
                        Controls.ComboBox {
                            id: imageProviderCombo
                            model: imageProviderOptions
                            textRole: "displayName"
                            valueRole: "providerId"
                            Layout.fillWidth: true
                            enabled: imageProviderOptions.count > 1
                            function indexForBackend(b) {
                                for (var i = 0; i < imageProviderOptions.count; ++i) {
                                    if (imageProviderOptions.get(i).providerId === b)
                                        return i;
                                }
                                return 0;
                            }
                            currentIndex: indexForBackend(ImageProviders.activeProviderId)
                            onActivated: ImageProviders.activeProviderId = currentValue
                        }
                        Controls.Label {
                            text: imageProviderOptions.count > 1 ? qsTr("Used by the `generate_image` tool and the Generate Image button in chat.") : qsTr("Configure an image provider below to enable image generation.")
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                        }

                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                        }

                        Kirigami.Heading {
                            text: qsTr("Providers")
                            level: 4
                            Layout.bottomMargin: 8
                        }

                        ProviderCard {
                            providerName: qsTr("Text Providers")
                            iconName: "applications-development"
                            subtitle: qsTr("Cloud APIs (OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek), self-hosted servers (Ollama, llama.cpp), and local GGUF models.")
                            status: qsTr("Configure")
                            statusKind: "info"
                            onConfigure: settingsPanel.activeSubPage = "text_providers"
                        }

                        ProviderCard {
                            providerName: qsTr("Image Generation")
                            iconName: "image-x-generic"
                            subtitle: (ImageProviders.activeProviderId && ImageProviders.activeProviderId.length > 0) ? qsTr("Active provider configured. The Generate Image button is enabled in chat.") : qsTr("No image provider active. The Generate Image button is hidden in chat.")
                            status: qsTr("Configure")
                            statusKind: "info"
                            onConfigure: settingsPanel.activeSubPage = "image_providers"
                        }

                        ProviderCard {
                            providerName: qsTr("Web Search")
                            iconName: "internet-web-browser"
                            subtitle: qsTr("Backend for the search_web tool: DuckDuckGo (no key), Tavily / Exa / LangSearch (API key), a self-hosted SearXNG instance, or a custom JSON endpoint.")
                            status: qsTr("Configure")
                            statusKind: "info"
                            onConfigure: settingsPanel.activeSubPage = "search_providers"
                        }

                        ProviderCard {
                            providerName: qsTr("RAGP (Routing & Gating)")
                            iconName: "view-process-tree"
                            subtitle: qsTr("On-device classifier that decides which agent replies next in group chats. Local GGUF for fast, private routing; remote Ollama fallback.")
                            status: qsTr("Configure")
                            statusKind: "info"
                            onConfigure: settingsPanel.activeSubPage = "ragp"
                        }

                        ProviderCard {
                            providerName: qsTr("Embeddings (RAG)")
                            iconName: "search"
                            subtitle: qsTr("Vector provider for retrieval: local (on-device llama.cpp) or a remote OpenAI-compatible endpoint, plus the knowledge-base corpus.")
                            status: qsTr("Configure")
                            statusKind: "info"
                            onConfigure: settingsPanel.activeSubPage = "embeddings"
                        }

                        Item {
                            Layout.preferredHeight: 16
                        }
                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                        }

                        Kirigami.Heading {
                            text: qsTr("Remote Access")
                            level: 4
                            Layout.bottomMargin: 8
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Kirigami.Theme.disabledTextColor
                            text: qsTr("Pair Verzeta for Android or Verzeta for VS " + "Code to use this host over your local " + "network.")
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            Kirigami.Icon {
                                source: RemoteAccess.remoteRunning ? "network-connect" : "network-disconnect"
                                color: RemoteAccess.remoteRunning ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
                                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: RemoteAccess.remoteRunning ? qsTr("Online: %1").arg(RemoteAccess.remoteUrl) : qsTr("Offline")
                                color: RemoteAccess.remoteRunning ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
                            }
                        }
                        Controls.Button {
                            Layout.fillWidth: true
                            text: qsTr("Manage Remote Access")
                            icon.name: "preferences-system-network"
                            onClicked: remoteAccessDialog.open()
                        }

                        Item {
                            Layout.preferredHeight: 16
                        }
                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                        }

                        Kirigami.Heading {
                            text: qsTr("Execution & Permissions")
                            level: 4
                            Layout.bottomMargin: 8
                        }

                        Rectangle {
                            id: execPermsCard
                            Layout.fillWidth: true
                            radius: ThemeController.radius
                            color: execPermsHover.hovered ? ThemeController.hoverTint : ThemeController.surfaceCard
                            border.width: 1
                            border.color: ThemeController.borderSubtle
                            antialiasing: true
                            implicitHeight: execPermsRow.implicitHeight + 24

                            RowLayout {
                                id: execPermsRow
                                anchors {
                                    left: parent.left
                                    right: parent.right
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: 12
                                    rightMargin: 12
                                }
                                spacing: 12

                                Kirigami.Icon {
                                    source: "security-medium"
                                    fallback: "system-run"
                                    color: Kirigami.Theme.highlightColor
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Controls.Label {
                                        text: qsTr("Shell command allow-list")
                                        font.bold: true
                                        font.family: ThemeController.fontFamily
                                    }
                                    Controls.Label {
                                        text: qsTr("Agents may run %1 programs.").arg(SettingsService.shellAllowList.length)
                                        color: Kirigami.Theme.disabledTextColor
                                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                }
                                Kirigami.Icon {
                                    source: "go-next"
                                    color: Kirigami.Theme.disabledTextColor
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                }
                            }

                            HoverHandler {
                                id: execPermsHover
                            }
                            TapHandler {
                                onTapped: execPermsOverlay.open()
                            }
                        }

                        Item {
                            Layout.preferredHeight: 16
                        }
                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                        }

                        Kirigami.Heading {
                            text: qsTr("Voice Calls")
                            level: 4
                            Layout.bottomMargin: 8
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Kirigami.Theme.disabledTextColor
                            text: qsTr("Talk to your agents over push-to-talk " + "voice calls. Requires the Verzeta-Voice " + "add-on, installed separately.")
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            Kirigami.Icon {
                                source: VoiceCallService.running ? "audio-headset" : "audio-volume-muted"
                                color: VoiceCallService.running ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
                                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: VoiceCallService.running ? qsTr("Running: pack %1, %2 voices").arg(VoiceCallService.packVersion).arg(VoiceCallService.voices.length) : (VoiceCallService.detected ? qsTr("Installed, not running") : qsTr("Not installed"))
                                color: VoiceCallService.running ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
                            }
                        }
                        Controls.Button {
                            Layout.fillWidth: true
                            text: qsTr("Manage Voice Calls")
                            icon.name: "audio-headset"
                            onClicked: voiceCallsDialog.open()
                        }

                        Item {
                            Layout.preferredHeight: 16
                        }
                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                        }

                        Kirigami.Heading {
                            text: qsTr("Appearance")
                            level: 4
                            Layout.bottomMargin: 8
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Controls.Label {
                                text: qsTr("Theme")
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                font.bold: true
                            }
                            Controls.Label {
                                text: qsTr("Verzeta follows your system colour scheme. " + "Currently in %1 mode.").arg(ThemeController.isDarkMode ? qsTr("dark") : qsTr("light"))
                                color: Kirigami.Theme.disabledTextColor
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: qsTr("On KDE Plasma, change colours in " + "System Settings → Colours.")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }

                        Item {
                            Layout.preferredHeight: 16
                        }
                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                        }

                        Kirigami.Heading {
                            text: qsTr("Onboarding")
                            level: 4
                            Layout.bottomMargin: 8
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Kirigami.Theme.disabledTextColor
                            text: qsTr("Re-run the welcome wizard at any time.")
                        }

                        Controls.Button {
                            text: qsTr("Re-run Welcome Wizard")
                            icon.name: "tools-wizard"
                            onClicked: SettingsService.firstRunComplete = false
                        }

                        Item {
                            Layout.preferredHeight: 16
                        }
                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                        }

                        Kirigami.Heading {
                            text: qsTr("Storage")
                            level: 4
                            Layout.bottomMargin: 8
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Controls.Label {
                                text: qsTr("Database: %1").arg(SettingsService.databasePath)
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }

                        Item {
                            Layout.preferredHeight: 8
                        }

                        Controls.Button {
                            text: qsTr("Clear All Conversations")
                            icon.name: "edit-delete"
                            onClicked: clearConfirmDialog.open()
                        }

                        Item {
                            Layout.preferredHeight: 40
                        }
                    }
                }
            }

            Item {
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Controls.TabBar {
                        id: diagSubTabs
                        Layout.fillWidth: true

                        Controls.TabButton {
                            text: qsTr("Heartbeat")
                            icon.name: "media-playback-start"
                            Layout.fillWidth: true
                        }
                        Controls.TabButton {
                            text: qsTr("Self-config Audit")
                            icon.name: "view-list-text"
                            Layout.fillWidth: true
                        }
                    }

                    Controls.SwipeView {
                        id: diagSwipe
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: diagSubTabs.currentIndex
                        interactive: false
                        clip: true

                        HeartbeatDiagnosticsTab {
                        }
                        HeartbeatAuditTab {
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: SettingsService
        function onOllamaConnectionResult(success, message) {
            ollamaStatus.statusKind = success ? "ok" : "err";
            ollamaStatus.statusMessage = message.length > 0 ? message : (success ? qsTr("Connected") : qsTr("Connection failed"));
        }
    }

    RemoteAccessDialog {
        id: remoteAccessDialog
    }

    VoiceCallsDialog {
        id: voiceCallsDialog
    }

    Dialogs.FileDialog {
        id: ggufDialog
        title: qsTr("Select GGUF Model")
        nameFilters: ["GGUF Models (*.gguf)", "All Files (*)"]
        onAccepted: {
            var path = PathUtils.toLocalFile(selectedFile);
            if (path.length === 0)
                return;
            llamaPathField.text = path;
            SettingsService.llamaCppModelPath = path;
        }
    }

    Kirigami.PromptDialog {
        id: clearConfirmDialog
        title: qsTr("Clear All Conversations")
        subtitle: qsTr("This will permanently delete all conversations. This cannot be undone.")
        standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.No
        onAccepted: Conversations.clearAllConversations()
    }

    ExecutionPermissionsOverlay {
        id: execPermsOverlay
    }
}

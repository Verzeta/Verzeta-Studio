// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: page
    color: ThemeController.surfacePage

    signal back

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
                    text: qsTr("Embeddings (RAG)")
                    level: 3
                    Layout.fillWidth: true
                }
            }
        }
        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Controls.ScrollView {
            id: embedScroll
            contentWidth: applicationWindow().isCompact ? availableWidth : -1
            Layout.fillWidth: true
            Layout.fillHeight: true
            Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
            clip: true

            ColumnLayout {
                id: embedColumn
                width: applicationWindow().isCompact ? Math.min(embedScroll.availableWidth - 32, 720) : Math.min(page.width - 32, 720)
                x: applicationWindow().isCompact ? Math.max(16, (embedScroll.availableWidth - width) / 2) : Math.max(16, (page.width - width) / 2)
                spacing: 16

                Item {
                    Layout.preferredHeight: 8
                }

                Controls.Label {
                    text: qsTr("Embeddings turn your messages and documents into vectors for retrieval. Pick a Local (on-device) or Remote provider. Turn RAG on per conversation in Chat Settings.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: providerCardCol.implicitHeight + 24
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: ThemeController.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        id: providerCardCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        anchors.topMargin: 12
                        spacing: 6

                        Controls.Label {
                            text: qsTr("Provider")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Controls.Switch {
                                id: embedLocalSwitch
                                text: SettingsService.hasLocalLlama ? qsTr("Use local model (llama.cpp)") : qsTr("Use local model (llama.cpp, not in this build)")
                                enabled: SettingsService.hasLocalLlama
                                checked: SettingsService.embeddingLocalEnabled && SettingsService.hasLocalLlama
                                onToggled: SettingsService.embeddingLocalEnabled = checked
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                        }
                        Controls.Label {
                            visible: !SettingsService.hasLocalLlama
                            text: qsTr("This build does not include the on-device engine, so local embeddings are unavailable. Download the Local AI edition, or use a Remote provider below.")
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.82
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            visible: embedLocalSwitch.checked && SettingsService.hasLocalLlama
                            Controls.Label {
                                text: qsTr("Local embedding model")
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            }
                            Controls.ComboBox {
                                id: embedModelCombo
                                Layout.fillWidth: true
                                enabled: count > 0
                                model: _availableModels
                                property var _availableModels: []
                                property string _selected: SettingsService.embeddingLocalModelFilename
                                function _refresh() {
                                    _availableModels = SettingsService.embeddingAvailableModels();
                                    const idx = _availableModels.indexOf(_selected);
                                    currentIndex = (idx >= 0) ? idx : 0;
                                }
                                Component.onCompleted: _refresh()
                                onActivated: {
                                    if (currentIndex >= 0 && currentIndex < _availableModels.length) {
                                        SettingsService.embeddingLocalModelFilename = _availableModels[currentIndex];
                                    }
                                }
                            }
                            Controls.Label {
                                visible: embedModelCombo._availableModels.length === 0
                                text: qsTr("No embedding model selected yet. Click “Add model…” to pick a .gguf file. It is linked into the folder below, exactly like the RAGP model picker.")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Controls.Label {
                                    text: SettingsService.embeddingModelsDir
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }
                                Controls.Button {
                                    text: qsTr("Add model…")
                                    icon.name: "list-add"
                                    onClicked: embedModelDialog.open()
                                }
                                Controls.Button {
                                    text: qsTr("Open")
                                    icon.name: "folder"
                                    onClicked: {
                                        const dir = SettingsService.embeddingModelsDir;
                                        if (!Qt.openUrlExternally(PathUtils.fromLocalFile(dir))) {
                                            embedStatusLabel.text = qsTr("Could not open %1").arg(dir);
                                            embedStatusLabel.isError = true;
                                        }
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("Refresh")
                                    icon.name: "view-refresh"
                                    onClicked: {
                                        embedModelCombo._refresh();
                                        const n = embedModelCombo._availableModels.length;
                                        embedStatusLabel.text = n === 1 ? qsTr("Rescanned: 1 model found") : qsTr("Rescanned: %1 models found").arg(n);
                                        embedStatusLabel.isError = false;
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                visible: !ModelDownloads.recommendedPresent("embed")
                                Controls.Button {
                                    icon.name: "download"
                                    enabled: !ModelDownloads.active
                                    text: qsTr("Download recommended: %1").arg(ModelDownloads.recommendedFilename("embed"))
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
                                    text: {
                                        const pct = Math.round(ModelDownloads.progress * 100);
                                        const got = (ModelDownloads.receivedBytes / 1000000).toFixed(1);
                                        const tot = (ModelDownloads.totalBytes / 1000000).toFixed(1);
                                        return qsTr("%1%  ·  %2 / %3 MB").arg(pct).arg(got).arg(tot);
                                    }
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
                                    if (kind !== "embed")
                                        return;
                                    SettingsService.embeddingLocalModelFilename = filename;
                                    embedModelCombo._selected = filename;
                                    embedModelCombo._refresh();
                                    embedStatusLabel.text = qsTr("Downloaded, verified and selected: %1").arg(filename);
                                    embedStatusLabel.isError = false;
                                }
                                function onDownloadFailed(kind, error) {
                                    if (kind !== "embed")
                                        return;
                                    embedStatusLabel.text = error;
                                    embedStatusLabel.isError = true;
                                }
                            }

                            Controls.Label {
                                id: embedStatusLabel
                                property bool isError: false
                                visible: text.length > 0
                                color: isError ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.positiveTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            visible: !(embedLocalSwitch.checked && SettingsService.hasLocalLlama)
                            Controls.Label {
                                text: qsTr("Remote provider (OpenAI-compatible endpoint)")
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            }
                            Controls.Label {
                                text: qsTr("Any OpenAI-compatible /embeddings endpoint. OpenAI, a local Ollama / LM Studio, or a llama.cpp server. (Anthropic has no embeddings API.)")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: qsTr("Embeddings endpoint (base URL)")
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            }
                            Controls.TextField {
                                id: embedUrlField
                                Layout.fillWidth: true
                                text: SettingsService.embeddingEndpointBaseUrl()
                                placeholderText: qsTr("https://api.openai.com/v1  (or http://localhost:11434/v1)")
                                onEditingFinished: SettingsService.setEmbeddingEndpointBaseUrl(text.trim())
                            }
                            Controls.Label {
                                text: qsTr("Embedding model")
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            }
                            Controls.TextField {
                                id: embedModelField
                                Layout.fillWidth: true
                                text: SettingsService.embeddingModel()
                                placeholderText: qsTr("text-embedding-3-small  (or nomic-embed-text)")
                                onEditingFinished: SettingsService.setEmbeddingModel(text.trim())
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: kbCardCol.implicitHeight + 24
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: ThemeController.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        id: kbCardCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        anchors.topMargin: 12
                        spacing: 6

                        Controls.Label {
                            text: qsTr("Knowledge base")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Controls.Button {
                                text: qsTr("Add document to knowledge base")
                                icon.name: "document-open"
                                onClicked: ragDocDialog.open()
                            }
                            Controls.Button {
                                text: qsTr("Clear all")
                                icon.name: "edit-clear-all"
                                onClicked: RagService.clearAllEmbeddings()
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                id: corpusLabel
                                property int chunks: RagService.embeddingCount()
                                text: qsTr("%1 text chunks indexed").arg(chunks)
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                Connections {
                                    target: RagService
                                    function onDocumentIndexed(docId) {
                                        corpusLabel.chunks = RagService.embeddingCount();
                                    }
                                    function onEmbeddingsCleared() {
                                        corpusLabel.chunks = RagService.embeddingCount();
                                    }
                                }
                            }
                        }
                        Controls.Label {
                            text: qsTr("Documents added here form a global knowledge base. Turn RAG on for a conversation (the \"RAG\" pill or its Chat Settings) to retrieve from this knowledge base and index that conversation's messages.")
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.82
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                Item {
                    Layout.preferredHeight: 24
                }
            }
        }
    }

    Dialogs.FileDialog {
        id: ragDocDialog
        title: qsTr("Add document to knowledge base")
        fileMode: Dialogs.FileDialog.OpenFile
        onAccepted: RagService.indexDocumentFile(selectedFile)
    }

    Connections {
        target: SettingsService
        function onEmbeddingAvailableModelsChanged() {
            embedModelCombo._refresh();
        }
    }

    Dialogs.FileDialog {
        id: embedModelDialog
        title: qsTr("Add GGUF Embedding Model")
        nameFilters: ["GGUF Models (*.gguf)", "All Files (*)"]
        onAccepted: {
            const path = PathUtils.toLocalFile(selectedFile);
            if (path.length === 0) {
                embedStatusLabel.text = qsTr("That file is not on this machine.");
                embedStatusLabel.isError = true;
                return;
            }
            const err = SettingsService.embeddingAddModelFromPath(path);
            if (err.length > 0) {
                embedStatusLabel.text = err;
                embedStatusLabel.isError = true;
                return;
            }
            const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
            const basename = path.substring(cut + 1);
            SettingsService.embeddingLocalModelFilename = basename;
            embedModelCombo._selected = basename;
            embedModelCombo._refresh();
            embedStatusLabel.text = qsTr("Added and selected: %1").arg(basename);
            embedStatusLabel.isError = false;
        }
    }
}

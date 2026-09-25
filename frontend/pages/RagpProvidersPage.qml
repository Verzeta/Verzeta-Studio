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
                    text: qsTr("RAGP (Routing & Gating)")
                    level: 3
                    Layout.fillWidth: true
                }
            }
        }
        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Controls.ScrollView {
            id: ragpScroll
            contentWidth: applicationWindow().isCompact ? availableWidth : -1
            Layout.fillWidth: true
            Layout.fillHeight: true
            Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
            clip: true

            ColumnLayout {
                width: applicationWindow().isCompact ? Math.min(ragpScroll.availableWidth - 32, 720) : Math.min(page.width - 32, 720)
                x: applicationWindow().isCompact ? Math.max(16, (ragpScroll.availableWidth - width) / 2) : Math.max(16, (page.width - width) / 2)
                spacing: 16

                Item {
                    Layout.preferredHeight: 8
                }

                Controls.Label {
                    text: qsTr("Decides which agent responds next in a " + "group chat. Local inference is fast and " + "private; the remote fallback runs via " + "the configured Ollama server.")
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: ragpCardCol.implicitHeight + 24
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: ThemeController.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        id: ragpCardCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        anchors.topMargin: 12
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Controls.Switch {
                                id: ragpLocalSwitch
                                text: qsTr("Use local model")
                                checked: SettingsService.ragpLocalEnabled
                                onToggled: SettingsService.ragpLocalEnabled = checked
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: {
                                    var live = AppController.ragpAccelerationLive;
                                    if (live.length > 0)
                                        return qsTr("Acceleration: %1").arg(live);
                                    return SettingsService.hasLocalLlama ? qsTr("Acceleration: determined at first model load") : qsTr("Acceleration: %1").arg(SettingsService.ragpAccelerationBackend);
                                }
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            }
                        }

                        Item {
                            Layout.preferredHeight: 4
                        }

                        Controls.Label {
                            text: qsTr("Default model")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Controls.ComboBox {
                                id: ragpModelCombo
                                enabled: ragpLocalSwitch.checked && count > 0
                                Layout.fillWidth: true
                                model: _availableModels
                                property var _availableModels: []
                                property string _defaultFilename: SettingsService.ragpDefaultModelFilename

                                function _refresh() {
                                    _availableModels = SettingsService.ragpAvailableModels();
                                    const idx = _availableModels.indexOf(_defaultFilename);
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
                                enabled: ragpLocalSwitch.checked
                                onClicked: ragpModelDialog.open()
                            }
                            Controls.Button {
                                text: qsTr("Remove")
                                icon.name: "edit-delete"
                                enabled: ragpLocalSwitch.checked && ragpModelCombo.currentIndex >= 0 && ragpModelCombo._availableModels.length > 0
                                onClicked: {
                                    const filename = ragpModelCombo._availableModels[ragpModelCombo.currentIndex];
                                    const err = SettingsService.ragpRemoveModel(filename);
                                    if (err.length > 0) {
                                        ragpStatusLabel.text = err;
                                        ragpStatusLabel.isError = true;
                                    } else {
                                        ragpStatusLabel.text = qsTr("Removed from list: %1").arg(filename);
                                        ragpStatusLabel.isError = false;
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            visible: SettingsService.hasLocalLlama && !ModelDownloads.recommendedPresent("ragp")
                            Controls.Button {
                                icon.name: "download"
                                enabled: ragpLocalSwitch.checked && !ModelDownloads.active
                                text: qsTr("Download recommended: %1").arg(ModelDownloads.recommendedFilename("ragp"))
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
                                if (kind !== "ragp")
                                    return;
                                SettingsService.ragpDefaultModelFilename = filename;
                                ragpModelCombo._refresh();
                                ragpStatusLabel.text = qsTr("Downloaded, verified and selected: %1").arg(filename);
                                ragpStatusLabel.isError = false;
                            }
                            function onDownloadFailed(kind, error) {
                                if (kind !== "ragp")
                                    return;
                                ragpStatusLabel.text = error;
                                ragpStatusLabel.isError = true;
                            }
                        }

                        Controls.Label {
                            visible: ragpModelCombo._availableModels.length === 0
                            text: qsTr("No .gguf models yet. Use Browse… to " + "pick a file anywhere on disk (it will " + "be symlinked into the models folder, " + "your original file is not moved or " + "copied).")
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            id: ragpStatusLabel
                            property bool isError: false
                            text: ""
                            visible: text.length > 0
                            color: isError ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.positiveTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Item {
                            Layout.preferredHeight: 4
                        }

                        Controls.Label {
                            text: qsTr("Models folder")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Controls.Label {
                                text: SettingsService.ragpModelsDir
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: qsTr("Open")
                                icon.name: "folder"
                                onClicked: {
                                    const dir = SettingsService.ragpModelsDir;
                                    if (!Qt.openUrlExternally(PathUtils.fromLocalFile(dir))) {
                                        ragpStatusLabel.text = qsTr("Could not open %1").arg(dir);
                                        ragpStatusLabel.isError = true;
                                    }
                                }
                            }
                            Controls.Button {
                                text: qsTr("Refresh")
                                icon.name: "view-refresh"
                                onClicked: {
                                    ragpModelCombo._refresh();
                                    const n = ragpModelCombo._availableModels.length;
                                    ragpStatusLabel.text = n === 1 ? qsTr("Rescanned: 1 model found") : qsTr("Rescanned: %1 models found").arg(n);
                                    ragpStatusLabel.isError = false;
                                }
                            }
                        }

                        Item {
                            Layout.preferredHeight: 4
                        }

                        Controls.Label {
                            text: qsTr("Current backend: %1").arg(AppController.ragpBackendLive && AppController.ragpBackendLive.length > 0 ? AppController.ragpBackendLive : qsTr("(not yet initialised)"))
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Connections {
                            target: SettingsService
                            function onRagpAvailableModelsChanged() {
                                ragpModelCombo._refresh();
                            }
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
        id: ragpModelDialog
        title: qsTr("Add GGUF Model for RAGP")
        nameFilters: ["GGUF Models (*.gguf)", "All Files (*)"]
        onAccepted: {
            const path = PathUtils.toLocalFile(selectedFile);
            if (path.length === 0) {
                ragpStatusLabel.text = qsTr("That file is not on this machine.");
                ragpStatusLabel.isError = true;
                return;
            }
            const err = SettingsService.ragpAddModelFromPath(path);
            if (err.length > 0) {
                ragpStatusLabel.text = err;
                ragpStatusLabel.isError = true;
                return;
            }
            const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
            const basename = path.substring(cut + 1);
            SettingsService.ragpDefaultModelFilename = basename;
            ragpStatusLabel.text = qsTr("Added and selected: %1").arg(basename);
            ragpStatusLabel.isError = false;
        }
    }
}

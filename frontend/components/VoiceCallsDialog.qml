// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: dialog
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: Math.min(applicationWindow().width * 0.9,
                             Kirigami.Units.gridUnit * 40)

    readonly property bool _isMobile:
        applicationWindow().width < Kirigami.Units.gridUnit * 36

    readonly property color _cPanel:
        Qt.darker(Kirigami.Theme.backgroundColor, 0.6)
    readonly property color _cWell:
        Qt.darker(Kirigami.Theme.backgroundColor, 1.15)

    component FieldLabel: Controls.Label {
        font.family: ThemeController.fontFamily
        font.bold: true
        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.76
        font.letterSpacing: 0.8
        color: Kirigami.Theme.disabledTextColor
    }

    title: qsTr("Voice Calls")
    subtitle: qsTr("VERZETA-VOICE · SPEECH")
    dialogIcon: "audio-headset"

    footer: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Controls.Label {
            text: qsTr("Runs as a separate process.")
            color: Kirigami.Theme.disabledTextColor
            font.family: ThemeController.fontFamily
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            visible: !dialog._isMobile
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        Item { visible: dialog._isMobile; Layout.fillWidth: true }

        AppButton {
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    }

    ColumnLayout {
        Layout.margins: Kirigami.Units.gridUnit
        spacing: Kirigami.Units.gridUnit

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: heroLayout.implicitHeight
                + Kirigami.Units.gridUnit * 2

            RowLayout {
                id: heroLayout
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.gridUnit

                Kirigami.Icon {
                    source: VoiceCallService.running
                        ? "audio-headset" : "audio-headset-symbolic"
                    fallback: "audio-headset"
                    color: VoiceCallService.running
                        ? Kirigami.Theme.positiveTextColor
                        : Kirigami.Theme.disabledTextColor
                    Layout.preferredWidth: Kirigami.Units.iconSizes.large
                    Layout.preferredHeight: Kirigami.Units.iconSizes.large
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: VoiceCallService.running
                            ? qsTr("Voice service is running")
                            : (VoiceCallService.detected
                               ? qsTr("Voice service is stopped")
                               : qsTr("Verzeta-Voice is not installed"))
                        font.bold: true
                        font.family: ThemeController.fontFamily
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        visible: VoiceCallService.running
                        wrapMode: Text.WordWrap
                        text: qsTr("Pack %1 · %2 voices installed")
                            .arg(VoiceCallService.packVersion)
                            .arg(VoiceCallService.voices.length)
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        visible: VoiceCallService.lastError.length > 0
                        wrapMode: Text.WordWrap
                        text: VoiceCallService.lastError
                        color: Kirigami.Theme.negativeTextColor
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                    }
                }

                Controls.Switch {
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                    enabled: VoiceCallService.detected
                    checked: VoiceCallService.running
                    onToggled: checked ? VoiceCallService.enable()
                                       : VoiceCallService.disable()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: installCol.implicitHeight
                + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: installCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Installation")
                    font.family: ThemeController.fontFamily
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                }

                Controls.Label {
                    visible: !VoiceCallService.detected
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    text: VoiceCallService.explicitPath.length > 0
                        ? qsTr("The custom path is not a runnable "
                               + "Verzeta-Voice binary. Fix or clear it, "
                               + "then press Detect.")
                        : qsTr("Install the Verzeta-Voice add-on to %1 or "
                               + "your PATH, then press Detect. Use Locate "
                               + "for an install somewhere unusual.")
                              .arg(VoiceCallService.wellKnownDir)
                }

                Rectangle {
                    visible: VoiceCallService.detected
                    Layout.fillWidth: true
                    radius: ThemeController.radius
                    color: dialog._cWell
                    implicitHeight: pathCol.implicitHeight
                        + Kirigami.Units.gridUnit

                    ColumnLayout {
                        id: pathCol
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                            margins: Kirigami.Units.smallSpacing * 2
                        }
                        spacing: 2

                        FieldLabel { text: qsTr("DETECTED AT") }
                        Controls.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WrapAnywhere
                            font.family: "monospace"
                            color: Kirigami.Theme.activeTextColor
                            text: VoiceCallService.detectedPath
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing

                    AppButton {
                        text: qsTr("Detect")
                        icon.name: "view-refresh"
                        onClicked: VoiceCallService.detect()
                    }
                    AppButton {
                        text: qsTr("Locate…")
                        icon.name: "document-open"
                        onClicked: voiceBinaryDialog.open()
                    }
                    AppButton {
                        visible: VoiceCallService.explicitPath.length > 0
                        text: qsTr("Clear custom path")
                        icon.name: "edit-clear"
                        onClicked: VoiceCallService.explicitPath = ""
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.gridUnit

                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Start the voice service with the "
                                   + "application")
                    }
                    Controls.Switch {
                        enabled: VoiceCallService.detected
                        checked: VoiceCallService.autostart
                        onToggled: VoiceCallService.autostart = checked
                    }
                }
            }
        }

        Rectangle {
            visible: VoiceCallService.detected
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: pttCol.implicitHeight
                + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: pttCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Push to talk")
                    font.family: ThemeController.fontFamily
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    text: qsTr("Hold this key during a call to talk, exactly "
                               + "like holding the on-screen button. The key "
                               + "works only while a call is running and "
                               + "never while you are typing.")
                }

                FieldLabel {
                    text: qsTr("HOTKEY")
                    Layout.topMargin: Kirigami.Units.smallSpacing
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Rectangle {
                        id: hotkeyCapture
                        Layout.fillWidth: true
                        implicitHeight: Kirigami.Units.gridUnit * 2
                        radius: ThemeController.radius
                        color: dialog._cWell
                        border.width: 1
                        border.color: hotkeyCapture.activeFocus
                            ? Kirigami.Theme.highlightColor
                            : ThemeController.borderSubtle

                        activeFocusOnTab: true

                        Controls.Label {
                            anchors.centerIn: parent
                            font.family: "monospace"
                            color: {
                                if (hotkeyCapture.activeFocus)
                                    return Kirigami.Theme.highlightColor
                                if (VoiceCallService.pttHotkey.length > 0)
                                    return Kirigami.Theme.activeTextColor
                                return Kirigami.Theme.disabledTextColor
                            }
                            text: hotkeyCapture.activeFocus
                                  ? qsTr("Press a key…")
                                  : (VoiceCallService.pttHotkey.length > 0
                                     ? VoiceCallService.pttHotkey
                                     : qsTr("Not set"))
                        }

                        TapHandler {
                            onTapped: hotkeyCapture.forceActiveFocus()
                        }

                        Keys.onPressed: (event) => {
                            event.accepted = true
                            if (event.key === Qt.Key_Escape) {
                                hotkeyCapture.focus = false
                                return
                            }
                            var seq = VoiceCallService.hotkeyFromEvent(
                                          event.key, event.modifiers)
                            if (seq.length === 0)
                                return  
                            VoiceCallService.pttHotkey = seq
                            hotkeyCapture.focus = false
                        }
                    }

                    AppButton {
                        visible: VoiceCallService.pttHotkey.length > 0
                        text: qsTr("Clear")
                        icon.name: "edit-clear"
                        onClicked: VoiceCallService.pttHotkey = ""
                    }
                }
            }
        }

        Rectangle {
            visible: VoiceCallService.running
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: voicesCol.implicitHeight
                + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: voicesCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Voices")
                    font.family: ThemeController.fontFamily
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                }

                FieldLabel { text: qsTr("DEFAULT VOICE") }
                Controls.ComboBox {
                    Layout.fillWidth: true
                    model: VoiceCallService.voices
                    currentIndex: VoiceCallService.voices.indexOf(
                                      VoiceCallService.defaultVoice)
                    onActivated: VoiceCallService.defaultVoice = currentText
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    text: qsTr("Used by any agent without a voice of its "
                               + "own. Per-agent voices are set in the "
                               + "conversation sidebar.")
                }
                Controls.Label {
                    visible: VoiceCallService.voices.length === 1
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    text: qsTr("Only one voice is installed, so every "
                               + "agent will sound the same until more "
                               + "voices are added.")
                }
            }
        }

        Rectangle {
            visible: VoiceCallService.running
                     && VoiceModelDownloader.targetDir.length > 0
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: dlCol.implicitHeight
                + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: dlCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                property var rows: VoiceModelDownloader.catalog()
                property string lastError: ""
                Connections {
                    target: VoiceModelDownloader
                    function onFinished(id, success, error) {
                        dlCol.rows = VoiceModelDownloader.catalog()
                        dlCol.lastError = success ? "" : error
                        if (success && !VoiceCallService.callActive)
                            VoiceCallService.restartService()
                    }
                    function onTargetDirChanged() {
                        dlCol.rows = VoiceModelDownloader.catalog()
                    }
                }

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Downloads")
                    font.family: ThemeController.fontFamily
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    text: qsTr("Every download is checked against a known "
                               + "fingerprint before it is installed. The "
                               + "voice service restarts by itself when an "
                               + "install finishes, so new voices appear "
                               + "right away.")
                }

                Controls.Label {
                    visible: dlCol.lastError.length > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.negativeTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    text: dlCol.lastError
                }

                FieldLabel {
                    text: qsTr("VOICES")
                    Layout.topMargin: Kirigami.Units.smallSpacing
                }
                Repeater {
                    model: dlCol.rows.filter(r => r.kind === "voice")
                    delegate: downloadRow
                }

                FieldLabel {
                    text: qsTr("SPEECH RECOGNITION")
                    Layout.topMargin: Kirigami.Units.smallSpacing
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    text: qsTr("A larger model understands you better. The "
                               + "best installed one is used automatically.")
                }
                Repeater {
                    model: dlCol.rows.filter(r => r.kind === "stt")
                    delegate: downloadRow
                }
            }
        }
    }

    Component {
        id: downloadRow

        RowLayout {
            id: row
            required property var modelData
            readonly property bool active:
                VoiceModelDownloader.activeId === modelData.id

            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Controls.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: row.modelData.displayName
                }
                Controls.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    text: qsTr("%1 MB · %2")
                        .arg((row.modelData.sizeBytes / 1048576).toFixed(0))
                        .arg(row.modelData.license)
                }
            }

            Controls.Label {
                visible: row.active
                color: Kirigami.Theme.activeTextColor
                text: {
                    var total = VoiceModelDownloader.totalBytes
                    if (total <= 0) return ""
                    var done = VoiceModelDownloader.receivedBytes
                    return qsTr("%1%  ·  %2 MB / %3 MB")
                        .arg(Math.floor(done * 100 / total))
                        .arg((done / 1048576).toFixed(1))
                        .arg((total / 1048576).toFixed(1))
                }
            }
            AppButton {
                visible: row.active
                flat: true
                iconOnly: true
                icon.name: "process-stop"
                text: qsTr("Cancel download")
                onClicked: VoiceModelDownloader.cancel()
            }

            Kirigami.Icon {
                visible: row.modelData.installed && !row.active
                source: "checkmark"
                fallback: "dialog-ok"
                color: Kirigami.Theme.positiveTextColor
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }
            Controls.Label {
                visible: row.modelData.installed && !row.active
                color: Kirigami.Theme.positiveTextColor
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                text: qsTr("Installed")
            }

            AppButton {
                visible: !row.modelData.installed && !row.active
                enabled: VoiceModelDownloader.activeId.length === 0
                icon.name: "download"
                text: qsTr("Get")
                onClicked: VoiceModelDownloader.download(row.modelData.id)
            }
        }
    }

    Dialogs.FileDialog {
        id: voiceBinaryDialog
        title: qsTr("Locate the Verzeta-Voice program")
        nameFilters: [qsTr("Verzeta-Voice (verzeta-voice verzeta-voice.exe *.AppImage)"),
                      qsTr("All Files (*)")]
        onAccepted: {
            var path = PathUtils.toLocalFile(selectedFile)
            if (path.length === 0) return
            VoiceCallService.explicitPath = path
        }
    }
}

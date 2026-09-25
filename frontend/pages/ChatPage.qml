// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


import QtQuick
import QtQuick.Controls as Controls
import Qt.labs.platform as Platform
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.ScrollablePage {
    id: chatPage

    title: ChatController.activeConversationTitle.length > 0
           ? ChatController.activeConversationTitle
           : qsTr("Chat")

    actions: [
        Kirigami.Action {
            text: qsTr("Conversation Settings")
            icon.name: "configure"
            onTriggered: convSettingsSheet.open()
        },
        Kirigami.Action {
            text: qsTr("Export…")
            icon.name: "document-export"
            enabled: ChatController.activeConversationId.length > 0
            onTriggered: chatExportDialog.open()
        },
        Kirigami.Action {
            text: qsTr("Generate Image")
            icon.name: "image-x-generic"
            enabled: inputField.text.trim().length > 0
                     && ChatController.activeConversationId.length > 0
            onTriggered: {
                var prompt = inputField.text.trim()
                if (prompt.length > 0) {
                    ImageService.generateImage(
                        ChatController.activeConversationId,
                        prompt,
                        { apiKey: SettingsService.apiKey("openai") }
                    )
                    inputField.text = ""
                }
            }
        },
        Kirigami.Action {
            text: qsTr("Read Aloud")
            icon.name: "audio-volume-high"
            enabled: ChatController.activeConversationId.length > 0
            onTriggered: {
                var lastMsg = ChatController.lastAssistantMessage()
                if (lastMsg && lastMsg.length > 0) {
                    AudioService.generateAudio(
                        ChatController.activeConversationId,
                        lastMsg,
                        { apiKey: SettingsService.apiKey("openai") }
                    )
                }
            }
        },
        Kirigami.Action {
            text: qsTr("Artifacts")
            icon.name: "folder-pictures-symbolic"
            enabled: ChatController.activeConversationId.length > 0
            onTriggered: pageStack.push(artifactViewerComponent)
        }
    ]

    Component {
        id: artifactViewerComponent
        ArtifactViewerPage {}
    }

    Connections {
        target: ImageService
        function onImageGenerated(convId, imagePath) {
            if (convId === ChatController.activeConversationId) {
                applicationWindow().showPassiveNotification(
                    qsTr("Image saved: ") + imagePath.split("/").pop(), 4000)
            }
        }
        function onError(convId, message) {
            if (convId === ChatController.activeConversationId) {
                applicationWindow().showPassiveNotification(
                    qsTr("Image generation failed: ") + message, 5000)
            }
        }
    }

    Connections {
        target: AudioService
        function onAudioGenerated(convId, audioPath) {
            if (convId === ChatController.activeConversationId) {
                applicationWindow().showPassiveNotification(
                    qsTr("Audio ready. Open Artifacts to play."), 4000)
            }
        }
        function onError(convId, message) {
            if (convId === ChatController.activeConversationId) {
                applicationWindow().showPassiveNotification(
                    qsTr("Audio generation failed: ") + message, 5000)
            }
        }
    }

    ListView {
        id: messageList
        model: ChatController.messages
        spacing: 12
        clip: true

        bottomMargin: 8
        topMargin:    8
        leftMargin:   12
        rightMargin:  12

        delegate: MessageBubble {
            messageId:    model.id
            role:         model.role
            content:      model.content
            contentHtml:  model.contentHtml
            isStreaming:  model.isStreaming
            tokenCount:   model.tokenCount
            modelUsed:    model.modelUsed
            finishReason: model.finishReason
            metadata:     model.metadata
            thinkingContent: model.thinkingContent
            width:        messageList.width - messageList.leftMargin - messageList.rightMargin
        }

        onCountChanged: Qt.callLater(function() {
            messageList.positionViewAtEnd()
        })

        Connections {
            target: ChatController.messages
            function onDataChanged(topLeft, bottomRight, roles) {
                Qt.callLater(function() {
                    messageList.positionViewAtEnd()
                })
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            visible: messageList.count === 0 && !ChatController.isGenerating
            text: qsTr("Send a message to start chatting")
            icon.name: "dialog-messages"
            width: parent.width - (Kirigami.Units.largeSpacing * 4)
        }
    }

    footer: Controls.Pane {
        id: footerPane
        padding: 0
        background: Rectangle {
            color: Kirigami.Theme.backgroundColor
            Kirigami.Separator { anchors.top: parent.top; width: parent.width }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            StopRetryBar {
                visible: ChatController.isGenerating
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                onStopClicked:  ChatController.stopGeneration()
                onRetryClicked: ChatController.retryLastMessage()
            }


            Flow {
                id: pendingAttachmentsFlow
                Layout.fillWidth: true
                Layout.leftMargin:  8
                Layout.rightMargin: 8
                Layout.bottomMargin: 4
                spacing: 6
                visible: pendingAttachments.count > 0

                Repeater {
                    model: ListModel { id: pendingAttachments }
                    delegate: FileAttachmentChip {
                        fileName: model.name
                        mimeType: model.mime
                        index:    model.index

                        onRemoveClicked: (idx) => pendingAttachments.remove(idx)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 8
                spacing: 8

                Controls.TextArea {
                    id: inputField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Type a message…")
                    wrapMode: TextEdit.Wrap
                    font.family:    ThemeController.fontFamily
                    font.pointSize: ThemeController.fontSize

                    Keys.onReturnPressed: (event) => {
                        if (!(event.modifiers & Qt.ShiftModifier)) {
                            event.accepted = true
                            chatPage.sendAction()
                        }
                    }
                }

                Controls.ToolButton {
                    id: attachBtn
                    icon.name: "mail-attachment"
                    enabled:   !ChatController.isGenerating

                    onClicked: attachDialog.open()

                    Controls.ToolTip {
                        text:    qsTr("Attach files")
                        visible: attachBtn.hovered
                        delay:   500
                    }
                }

                Controls.ToolButton {
                    id: sendBtn
                    icon.name: ChatController.isGenerating
                               ? "media-playback-stop"
                               : "document-send"
                    icon.color: Kirigami.Theme.highlightColor

                    onClicked: {
                        if (ChatController.isGenerating) {
                            ChatController.stopGeneration()
                        } else {
                            chatPage.sendAction()
                        }
                    }

                    Controls.ToolTip {
                        text: ChatController.isGenerating
                              ? qsTr("Stop generation")
                              : qsTr("Send message (Enter)")
                        visible: sendBtn.hovered
                    }
                }
            }

            TokenCounter {
                Layout.alignment: Qt.AlignRight
                Layout.rightMargin: 8
                Layout.bottomMargin: 4
            }
        }
    }


    ConversationSettingsSheet {
        id: convSettingsSheet
    }


    ExportDialog {
        id: chatExportDialog
    }


    Platform.FileDialog {
        id: attachDialog
        fileMode: Platform.FileDialog.OpenFiles
        title:    qsTr("Attach Files")

        onAccepted: {
            for (const fileUrl of attachDialog.files) {
                const path = PathUtils.toLocalFile(fileUrl)
                if (path.length === 0)
                    continue
                const name = path.split("/").pop()
                const mime = FileService.mimeType(path)
                pendingAttachments.append({
                    path:  path,
                    name:  name,
                    mime:  mime,
                    index: pendingAttachments.count
                })
            }
        }
    }


    function sendAction() {
        const text = inputField.text.trim()
        if (text.length === 0 && pendingAttachments.count === 0) {
            return
        }

        const attachPaths = []
        for (let i = 0; i < pendingAttachments.count; ++i) {
            attachPaths.push(pendingAttachments.get(i).path)
        }

        if (attachPaths.length > 0) {
            let combined = text
            if (combined.length === 0) {
                combined = qsTr("[Attached %1 file(s)]").arg(attachPaths.length)
            }
            ChatController.sendMessage(combined)
        } else if (text.length > 0) {
            ChatController.sendMessage(text)
        }

        inputField.text = ""
        pendingAttachments.clear()
    }
}

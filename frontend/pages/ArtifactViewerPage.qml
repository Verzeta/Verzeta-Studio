// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.ScrollablePage {
    id: artifactPage
    title: qsTr("Artifacts")

    onVisibleChanged: {
        if (visible) {
            imageModel.reload();
            audioModel.reload();
        }
    }

    ListModel {
        id: imageModel

        function reload() {
            clear();
            var paths = ImageService.conversationImages(ChatController.activeConversationId);
            for (var i = 0; i < paths.length; ++i) {
                append({
                        "path": paths[i]
                    });
            }
        }

        Component.onCompleted: reload()
    }

    ListModel {
        id: audioModel

        function reload() {
            clear();
            var paths = AudioService.conversationAudio(ChatController.activeConversationId);
            for (var i = 0; i < paths.length; ++i) {
                append({
                        "path": paths[i]
                    });
            }
        }

        Component.onCompleted: reload()
    }

    Connections {
        target: ImageService
        function onImageGenerated(convId, path) {
            if (convId === ChatController.activeConversationId) {
                imageModel.reload();
            }
        }
    }

    Connections {
        target: AudioService
        function onAudioGenerated(convId, path) {
            if (convId === ChatController.activeConversationId) {
                audioModel.reload();
            }
        }
    }

    ColumnLayout {
        spacing: 16
        width: artifactPage.width - Kirigami.Units.largeSpacing * 2

        Kirigami.Heading {
            text: qsTr("Images")
            level: 3
            visible: imageGrid.count > 0
        }

        GridLayout {
            id: imageGrid
            property int count: imageRepeater.count

            columns: artifactPage.width > 600 ? 3 : 2
            rowSpacing: 8
            columnSpacing: 8
            Layout.fillWidth: true
            visible: count > 0

            Repeater {
                id: imageRepeater
                model: imageModel

                delegate: Rectangle {
                    Layout.preferredWidth: 150
                    Layout.preferredHeight: 150
                    color: ThemeController.surfaceCard
                    radius: ThemeController.radius
                    clip: true

                    Image {
                        anchors.fill: parent
                        source: PathUtils.fromLocalFile(model.path)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true

                        Controls.BusyIndicator {
                            anchors.centerIn: parent
                            running: parent.status === Image.Loading
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            fullImageViewer.imagePath = model.path;
                            fullImageViewer.open();
                        }
                    }
                }
            }
        }

        Kirigami.Heading {
            text: qsTr("Audio")
            level: 3
            visible: audioRepeater.count > 0
        }

        Repeater {
            id: audioRepeater
            model: audioModel

            delegate: ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Controls.Label {
                    text: model.path.split("/").pop()
                    font.pointSize: 9
                    color: Kirigami.Theme.disabledTextColor
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }

                AudioPlayer {
                    audioUrl: PathUtils.fromLocalFile(model.path)
                    Layout.fillWidth: true
                }
            }
        }

        Kirigami.PlaceholderMessage {
            id: emptyMsg
            visible: imageGrid.count === 0 && audioRepeater.count === 0
            text: qsTr("No artifacts generated yet")
            explanation: qsTr("Use \"Generate Image\" or \"Read Aloud\" from the chat toolbar to create media.")
            icon.name: "folder-pictures-symbolic"
            Layout.fillWidth: true
        }
    }

    AppOverlayDialog {
        id: fullImageViewer

        property string imagePath: ""

        title: fullImageViewer.imagePath.split("/").pop()
        dialogIcon: "image-x-generic"

        footer: RowLayout {
            AppButton {
                text: qsTr("Save As…")
                icon.name: "document-save-as"
                onClicked: {
                    FileService.saveGeneratedFile(fullImageViewer.imagePath.split("/").pop(), "", "");
                    fullImageViewer.close();
                }
            }
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                text: qsTr("Close")
                onClicked: fullImageViewer.close()
            }
        }

        Image {
            id: fullImage
            source: PathUtils.fromLocalFile(fullImageViewer.imagePath)
            fillMode: Image.PreserveAspectFit
            width: Math.min(artifactPage.width - Kirigami.Units.largeSpacing * 4, 800)
            height: Math.min(artifactPage.Window.height * 0.7, 600)
            asynchronous: true
        }
    }
}

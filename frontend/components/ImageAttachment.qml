// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Item {
    id: root

    required property string imagePath

    property string caption: ""

    property int maxThumbWidth: 320

    readonly property int _pad: 6

    implicitWidth: frame.implicitWidth
    implicitHeight: frame.implicitHeight

    readonly property url _fileUrl: PathUtils.fromLocalFile(imagePath)

    readonly property bool _canRefine: {
        const id = ImageProviders.activeProviderId;
        if (!id || id.length === 0)
            return false;
        const row = ImageProviders.byId(id);
        return !!row && row.endpointShape === "openai_chat_image";
    }

    function _refine(instruction) {
        const p = (instruction || "").trim();
        if (p.length === 0 || !root._canRefine)
            return;
        ImageService.refineImage(ChatController.activeConversationId, root.imagePath, p);
        refineField.text = "";
        applicationWindow().showPassiveNotification(qsTr("Refining image: the result will appear in the chat."), 3500);
        fullScreenViewer.close();
    }

    Rectangle {
        id: frame
        radius: ThemeController.radius
        color: ThemeController.surfaceCard
        border.color: ThemeController.borderSubtle
        border.width: 1
        implicitWidth: thumbCol.implicitWidth + root._pad * 2
        implicitHeight: thumbCol.implicitHeight + root._pad * 2

        ColumnLayout {
            id: thumbCol
            anchors.fill: parent
            anchors.margins: root._pad
            spacing: 4

            Image {
                id: img
                source: root._fileUrl
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: true

                readonly property real _aspect: (implicitWidth > 0 && implicitHeight > 0) ? implicitHeight / implicitWidth : 0.66

                Layout.preferredWidth: (implicitWidth > 0) ? Math.min(root.maxThumbWidth, implicitWidth) : root.maxThumbWidth
                Layout.preferredHeight: Layout.preferredWidth * _aspect

                Controls.BusyIndicator {
                    anchors.centerIn: parent
                    running: img.status === Image.Loading
                    visible: running
                }

                Kirigami.Icon {
                    anchors.centerIn: parent
                    source: "image-missing"
                    fallback: "image-x-generic"
                    visible: img.status === Image.Error
                    implicitWidth: Kirigami.Units.iconSizes.large
                    implicitHeight: Kirigami.Units.iconSizes.large
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: fullScreenViewer.open()
                }
            }

            Controls.Label {
                text: root.caption
                visible: root.caption.length > 0
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.maximumWidth: img.Layout.preferredWidth
            }
        }
    }

    AppOverlayDialog {
        id: fullScreenViewer

        parent: applicationWindow().overlay
        title: root.caption.length > 0 ? root.caption : qsTr("Image")
        dialogIcon: "image-x-generic"

        implicitWidth: Math.min(applicationWindow().width * 0.9, Math.max(imgWrap.implicitWidth + root._pad * 3, Kirigami.Units.gridUnit * 22))

        Item {
            id: imgWrap

            readonly property real _aspect: (img.implicitWidth > 0 && img.implicitHeight > 0) ? img.implicitHeight / img.implicitWidth : 0.66
            readonly property real _boxW: Math.min(applicationWindow().width * 0.82, Kirigami.Units.gridUnit * 62)
            readonly property real _boxH: applicationWindow().height * 0.56
            readonly property real _natW: img.implicitWidth > 0 ? img.implicitWidth : _boxW

            implicitWidth: Math.min(_boxW, _boxH / _aspect, _natW)
            implicitHeight: implicitWidth * _aspect

            Image {
                id: fullImg
                anchors.fill: parent
                source: root._fileUrl
                fillMode: Image.PreserveAspectFit
                asynchronous: true

                Controls.BusyIndicator {
                    anchors.centerIn: parent
                    running: fullImg.status === Image.Loading
                    visible: running
                }
            }
        }

        footer: ColumnLayout {
            spacing: 10

            Rectangle {
                Layout.fillWidth: true
                visible: root._canRefine
                radius: ThemeController.radius
                color: ThemeController.surfaceCard
                border.color: ThemeController.borderSubtle
                border.width: 1
                implicitHeight: refineCol.implicitHeight + 24

                ColumnLayout {
                    id: refineCol
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        Controls.Label {
                            text: qsTr("AI refine")
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.smallFont.pointSize
                            height: variationBtn.height
                            verticalAlignment: Text.AlignVCenter
                        }
                        Controls.Button {
                            id: variationBtn
                            text: qsTr("Variation")
                            icon.name: "view-refresh"
                            onClicked: root._refine(qsTr("Create a variation of this image, keeping the same subject and overall style."))
                        }
                        Controls.Button {
                            text: qsTr("Enhance")
                            icon.name: "tools-wizard"
                            onClicked: root._refine(qsTr("Enhance this image: sharper details, cleaner lighting and colour, higher quality. Keep the composition."))
                        }
                        Controls.Button {
                            text: qsTr("Remove background")
                            icon.name: "layer-visible-off"
                            onClicked: root._refine(qsTr("Remove the background, leaving the main subject on a clean plain background."))
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        AppTextField {
                            id: refineField
                            Layout.fillWidth: true
                            Layout.minimumWidth: Kirigami.Units.gridUnit * 4
                            placeholderText: qsTr("Describe changes… (e.g. make it night, add a hat)")
                            onAccepted: root._refine(text)
                        }
                        Controls.Button {
                            text: qsTr("Refine")
                            icon.name: "edit-image"
                            highlighted: true
                            enabled: refineField.text.trim().length > 0
                            onClicked: root._refine(refineField.text)
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item {
                    Layout.fillWidth: true
                }
                Controls.Button {
                    text: qsTr("Save As…")
                    icon.name: "document-save-as"
                    onClicked: saveDialog.open()
                }
                Controls.Button {
                    text: qsTr("Close")
                    icon.name: "window-close"
                    onClicked: fullScreenViewer.close()
                }
            }
        }
    }

    Platform.FileDialog {
        id: saveDialog
        fileMode: Platform.FileDialog.SaveFile
        title: qsTr("Save Image As")
        currentFile: root._fileUrl

        onAccepted: {
            const dest = PathUtils.toLocalFile(saveDialog.currentFile);
            if (dest.length > 0)
                FileService.copyFile(root.imagePath, dest);
        }
    }
}

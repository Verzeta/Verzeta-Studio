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

    function shapeLabel(shape) {
        switch (shape) {
        case "openai_images":
            return qsTr("OpenAI Images");
        case "openai_chat_image":
            return qsTr("OpenAI-compatible chat-image (OpenRouter)");
        case "a1111":
            return qsTr("Automatic1111 / SD WebUI");
        case "local_cli":
            return qsTr("Local CLI");
        }
        return shape;
    }
    function iconFor(shape) {
        if (shape === "local_cli")
            return "computer";
        if (shape === "a1111")
            return "network-server";
        return "image-x-generic";
    }
    function activeLabel() {
        var active = ImageProviders.activeProviderId;
        if (!active || active.length === 0)
            return qsTr("None (image generation disabled)");
        var row = ImageProviders.byId(active);
        return (row && row.displayName) ? row.displayName : qsTr("None (image generation disabled)");
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
                    text: qsTr("Image Generation")
                    level: 3
                    Layout.fillWidth: true
                }
                AppButton {
                    text: qsTr("Add Image Provider")
                    icon.name: "list-add"
                    highlighted: true
                    onClicked: setupSheet.openForAdd()
                }
            }
        }
        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Controls.ScrollView {
            id: imageProvScroll
            contentWidth: applicationWindow().isCompact ? availableWidth : -1
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ColumnLayout {
                width: applicationWindow().isCompact ? Math.min(imageProvScroll.availableWidth - 32, 720) : Math.min(page.width - 32, 720)
                x: applicationWindow().isCompact ? Math.max(16, (imageProvScroll.availableWidth - width) / 2) : Math.max(16, (page.width - width) / 2)
                spacing: 16

                Item {
                    Layout.preferredHeight: 16
                }

                Rectangle {
                    id: banner
                    Layout.fillWidth: true
                    implicitHeight: bannerRow.implicitHeight + 24
                    radius: ThemeController.radius
                    readonly property bool hasActive: ImageProviders.activeProviderId && ImageProviders.activeProviderId.length > 0
                    color: hasActive ? Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.10) : Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.10)
                    border.color: hasActive ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                    border.width: 1

                    RowLayout {
                        id: bannerRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Kirigami.Icon {
                            source: banner.hasActive ? "emblem-success" : "dialog-information"
                            fallback: "dialog-ok"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            color: banner.hasActive ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Controls.Label {
                                text: qsTr("Active: %1").arg(page.activeLabel())
                                font.bold: true
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            Controls.Label {
                                text: banner.hasActive ? qsTr("The `generate_image` tool and the Generate Image button are enabled.") : qsTr("Add a provider below and set it active to enable image generation.")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    visible: ImageProviders.providers.length === 0
                    implicitHeight: emptyCol.implicitHeight + 32
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: ThemeController.borderSubtle
                    border.width: 1
                    ColumnLayout {
                        id: emptyCol
                        anchors.centerIn: parent
                        width: parent.width - 32
                        spacing: 8
                        Controls.Label {
                            text: qsTr("No image providers configured yet.")
                            font.bold: true
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Controls.Label {
                            text: qsTr("Add an OpenAI Images provider, an OpenRouter chat-image provider, an Automatic1111 server, or a local Stable Diffusion CLI.")
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            Layout.fillWidth: true
                        }
                        AppButton {
                            text: qsTr("Add Image Provider")
                            icon.name: "list-add"
                            highlighted: true
                            Layout.alignment: Qt.AlignHCenter
                            onClicked: setupSheet.openForAdd()
                        }
                    }
                }

                Repeater {
                    model: ImageProviders.providers
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: cardCol.implicitHeight + 24
                        radius: ThemeController.radius
                        color: ThemeController.surfaceCard
                        border.color: isActive ? Kirigami.Theme.positiveTextColor : ThemeController.borderSubtle
                        border.width: isActive ? 2 : 1

                        readonly property bool isActive: ImageProviders.activeProviderId === modelData.id

                        ColumnLayout {
                            id: cardCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                Kirigami.Icon {
                                    source: page.iconFor(modelData.endpointShape)
                                    fallback: "image-x-generic"
                                    implicitWidth: Kirigami.Units.iconSizes.medium
                                    implicitHeight: Kirigami.Units.iconSizes.medium
                                    color: Kirigami.Theme.textColor
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Controls.Label {
                                            text: modelData.displayName
                                            font.bold: true
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Rectangle {
                                            visible: isActive
                                            implicitHeight: activeLbl.implicitHeight + 6
                                            implicitWidth: activeLbl.implicitWidth + 14
                                            radius: implicitHeight / 2
                                            color: Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.18)
                                            Controls.Label {
                                                id: activeLbl
                                                anchors.centerIn: parent
                                                text: qsTr("ACTIVE")
                                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.75
                                                font.bold: true
                                                color: Kirigami.Theme.positiveTextColor
                                            }
                                        }
                                    }
                                    Controls.Label {
                                        text: page.shapeLabel(modelData.endpointShape)
                                        color: Kirigami.Theme.disabledTextColor
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    Controls.Label {
                                        text: modelData.endpointShape === "local_cli" ? modelData.sdPath : (modelData.baseUrl + (modelData.model && modelData.model.length > 0 ? "  •  " + modelData.model : ""))
                                        color: Kirigami.Theme.disabledTextColor
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                        elide: Text.ElideMiddle
                                        Layout.fillWidth: true
                                        visible: text.length > 0
                                    }
                                    Controls.Label {
                                        visible: modelData.endpointShape !== "local_cli"
                                        text: modelData.hasApiKey ? qsTr("API key set") : qsTr("No API key")
                                        color: Kirigami.Theme.disabledTextColor
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Item {
                                    Layout.fillWidth: true
                                }
                                Controls.Button {
                                    text: isActive ? qsTr("In Use") : qsTr("Set as active")
                                    enabled: !isActive
                                    onClicked: ImageProviders.activeProviderId = modelData.id
                                }
                                Controls.Button {
                                    text: qsTr("Edit")
                                    onClicked: setupSheet.openForEdit(modelData.id)
                                }
                                Controls.Button {
                                    text: qsTr("Delete")
                                    icon.name: "edit-delete"
                                    onClicked: {
                                        deleteConfirm.targetId = modelData.id;
                                        deleteConfirm.targetName = modelData.displayName;
                                        deleteConfirm.open();
                                    }
                                }
                            }
                        }
                    }
                }

                Item {
                    Layout.preferredHeight: 32
                }
            }
        }
    }

    ImageProviderSetupSheet {
        id: setupSheet
    }

    AppOverlayDialog {
        id: deleteConfirm
        parent: applicationWindow().overlay
        closePolicy: Controls.Popup.CloseOnEscape
        implicitWidth: Math.min(applicationWindow().width * 0.8, Kirigami.Units.gridUnit * 26)

        property string targetId: ""
        property string targetName: ""

        title: qsTr("Remove image provider")
        dialogIcon: "edit-delete"
        footer: RowLayout {
            spacing: 8
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                text: qsTr("Cancel")
                onClicked: deleteConfirm.close()
            }
            AppButton {
                text: qsTr("Remove")
                icon.name: "edit-delete"
                highlighted: true
                onClicked: {
                    ImageProviders.remove(deleteConfirm.targetId);
                    deleteConfirm.close();
                }
            }
        }
        ColumnLayout {
            spacing: 8
            Controls.Label {
                text: qsTr("Remove \"%1\"? This cannot be undone.").arg(deleteConfirm.targetName)
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }
}

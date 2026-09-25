// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: homePage
    color: ThemeController.surfacePage

    signal openConversation(string convId)
    signal newConversation
    signal newGroupChat
    signal openSettings

    readonly property bool _isCompact: homePage.width < 720
    readonly property int _quickStartCols: _isCompact ? 1 : (homePage.width < 1100 ? 2 : 4)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: parent ? parent.width : 0
                spacing: 24

                Item {
                    Layout.fillWidth: true
                    Layout.topMargin: 32
                    Layout.leftMargin: 32
                    Layout.rightMargin: 32
                    implicitHeight: heroRow.implicitHeight

                    RowLayout {
                        id: heroRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 16

                        Image {
                            source: "qrc:/icons/verzeta-studio.svg"
                            Layout.preferredWidth: 72
                            Layout.preferredHeight: 54
                            fillMode: Image.PreserveAspectFit
                            asynchronous: false
                            sourceSize.width: 144
                            sourceSize.height: 108
                            Layout.alignment: Qt.AlignVCenter
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 2

                            Kirigami.Heading {
                                text: qsTr("Verzeta Studio")
                                level: 1
                            }

                            Controls.Label {
                                text: qsTr("A team of AI agents with different " + "models and tools in one chat, " + "plus skills, polls, canvas, projects, " + "and remote pairing.")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }

                        Rectangle {
                            visible: !homePage._isCompact
                            Layout.alignment: Qt.AlignVCenter
                            radius: ThemeController.radius
                            color: ThemeController.surfaceCard
                            border.color: ThemeController.borderSubtle
                            border.width: 1
                            implicitWidth: providerRow.implicitWidth + 24
                            implicitHeight: 40

                            RowLayout {
                                id: providerRow
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8

                                Kirigami.Icon {
                                    source: "network-server"
                                    fallback: "network-connect"
                                    implicitWidth: Kirigami.Units.iconSizes.small
                                    implicitHeight: Kirigami.Units.iconSizes.small
                                    color: Kirigami.Theme.disabledTextColor
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                Controls.Label {
                                    text: qsTr("Provider: %1").arg((AgentSettings.activeProvider || qsTr("none")).toUpperCase())
                                    color: Kirigami.Theme.textColor
                                    font.bold: true
                                    Layout.alignment: Qt.AlignVCenter
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: homePage.openSettings()
                                Controls.ToolTip.text: qsTr("Click to switch provider / model in Settings")
                                Controls.ToolTip.visible: containsMouse
                                Controls.ToolTip.delay: 600
                                hoverEnabled: true
                            }
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.leftMargin: 32
                    Layout.rightMargin: 32
                    implicitHeight: quickStart.implicitHeight

                    ColumnLayout {
                        id: quickStart
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 12

                        Kirigami.Heading {
                            text: qsTr("Quick Start")
                            level: 3
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: homePage._quickStartCols
                            columnSpacing: 12
                            rowSpacing: 12

                            QuickCard {
                                iconName: "folder-projects"
                                iconFallback: "folder-documents"
                                featured: true
                                title: qsTr("Project Rooms")
                                subtitle: qsTr("Multi-agent workspaces" + ": pick a template, assemble " + "your team, ship the deliverable.")
                                onActivated: {
                                    projectRoomLoader.active = true;
                                    projectRoomLoader.item.openSheet();
                                }
                            }
                            QuickCard {
                                iconName: "list-add"
                                title: qsTr("New Chat")
                                subtitle: qsTr("Solo conversation with one model, " + "per-chat overrides for system " + "prompt, tools, and RAG.")
                                onActivated: homePage.newConversation()
                            }
                            QuickCard {
                                iconName: "user-group-new"
                                title: qsTr("New Group Chat")
                                subtitle: qsTr("Build a team of agents: " + "different providers and tools " + "per member, @mention routing, " + "polls for decisions.")
                                onActivated: homePage.newGroupChat()
                            }
                            QuickCard {
                                iconName: "configure"
                                title: qsTr("Settings")
                                subtitle: qsTr("Providers, web search, RAGP, " + "embeddings, remote access, and " + "voice calls.")
                                onActivated: homePage.openSettings()
                            }
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.leftMargin: 32
                    Layout.rightMargin: 32
                    visible: recentModel.count > 0
                    implicitHeight: recentCol.implicitHeight

                    ColumnLayout {
                        id: recentCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 12

                        Kirigami.Heading {
                            text: qsTr("Recent Conversations")
                            level: 3
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: homePage._isCompact ? 1 : 3
                            columnSpacing: 12
                            rowSpacing: 12

                            Repeater {
                                model: recentModel
                                delegate: Rectangle {
                                    required property var model
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 72
                                    radius: ThemeController.radius
                                    color: convMouse.containsMouse ? ThemeController.hoverTint : ThemeController.surfaceCard
                                    border.color: ThemeController.borderSubtle
                                    border.width: 1
                                    Behavior on color  {
                                        ColorAnimation {
                                            duration: 80
                                        }
                                    }

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 4

                                        Controls.Label {
                                            text: model.convTitle || qsTr("(untitled)")
                                            font.bold: true
                                            color: Kirigami.Theme.textColor
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Controls.Label {
                                            text: {
                                                if (!model.convUpdatedAt || model.convUpdatedAt.length === 0)
                                                    return "";
                                                var d = new Date(model.convUpdatedAt);
                                                if (isNaN(d.getTime()))
                                                    return model.convUpdatedAt;
                                                return d.toLocaleDateString(Qt.locale(), Locale.ShortFormat);
                                            }
                                            color: Kirigami.Theme.disabledTextColor
                                            font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                                            Layout.fillWidth: true
                                        }
                                    }
                                    MouseArea {
                                        id: convMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: homePage.openConversation(model.convId)
                                    }
                                }
                            }
                        }
                    }
                }

                Item {
                    Layout.preferredHeight: 24
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
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
                anchors.leftMargin: 32
                anchors.rightMargin: 32
                spacing: 16

                Controls.Label {
                    text: qsTr("v%1 (%2)").arg(BuildInfo.version).arg(BuildInfo.commit)
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                    Layout.alignment: Qt.AlignVCenter
                }
                Item {
                    Layout.fillWidth: true
                }
                Controls.Button {
                    text: qsTr("Help")
                    icon.name: "help-contents"
                    onClicked: helpOverlay.openForOverview()
                    Layout.alignment: Qt.AlignVCenter
                }
                Controls.Button {
                    text: qsTr("About")
                    icon.name: "help-about"
                    onClicked: aboutDialog.open()
                    Layout.alignment: Qt.AlignVCenter
                }
            }
        }
    }

    component QuickCard: Rectangle {
        property string iconName: ""
        property string iconFallback: ""
        property string title: ""
        property string subtitle: ""
        property bool featured: false
        signal activated

        Layout.fillWidth: true
        Layout.preferredHeight: 110
        radius: ThemeController.radius
        clip: true
        color: cardMouse.containsMouse ? ThemeController.hoverTint : ThemeController.surfaceCard
        border.color: cardMouse.containsMouse ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
        border.width: 1
        Behavior on color  {
            ColorAnimation {
                duration: 80
            }
        }
        Behavior on border.color  {
            ColorAnimation {
                duration: 80
            }
        }

        ProjectRoomTexture {
            anchors.fill: parent
            visible: featured
            stripeAlpha: 0.08
            stripeStep: 10
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Kirigami.Icon {
                    source: iconName
                    fallback: iconFallback
                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                    implicitHeight: Kirigami.Units.iconSizes.smallMedium
                    color: Kirigami.Theme.highlightColor
                    Layout.alignment: Qt.AlignVCenter
                }
                Controls.Label {
                    text: title
                    font.family: ThemeController.fontFamily
                    font.bold: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize + 1
                    color: Kirigami.Theme.textColor
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            Controls.Label {
                text: subtitle
                color: Kirigami.Theme.disabledTextColor
                font.family: ThemeController.fontFamily
                wrapMode: Text.WordWrap
                font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
            }
        }

        MouseArea {
            id: cardMouse
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: parent.activated()
        }
    }

    AboutDialog {
        id: aboutDialog
    }

    HelpOverlay {
        id: helpOverlay
    }

    Loader {
        id: projectRoomLoader
        active: false
        sourceComponent: Component {
            ProjectRoomOverlay {
                onOpenConversationRequested: function (convId) {
                    homePage.openConversation(convId);
                }
            }
        }
    }

    ListModel {
        id: recentModel
    }

    function refreshRecent() {
        recentModel.clear();
        var mdl = ConversationListModel;
        var rootCount = mdl.rowCount();
        var count = 0;
        for (var i = 0; i < rootCount && count < 12; ++i) {
            var idx = mdl.index(i, 0);
            var itemType = mdl.data(idx, 259);
            if (itemType === "folder") {
                var childCount = mdl.rowCount(idx);
                for (var j = 0; j < childCount && count < 12; ++j) {
                    var childIdx = mdl.index(j, 0, idx);
                    recentModel.append({
                            "convId": mdl.data(childIdx, 257),
                            "convTitle": mdl.data(childIdx, 258),
                            "convUpdatedAt": mdl.data(childIdx, 261) || ""
                        });
                    ++count;
                }
            } else {
                recentModel.append({
                        "convId": mdl.data(idx, 257),
                        "convTitle": mdl.data(idx, 258),
                        "convUpdatedAt": mdl.data(idx, 261) || ""
                    });
                ++count;
            }
        }
    }

    Component.onCompleted: refreshRecent()

    Connections {
        target: ConversationListModel
        function onModelReset() {
            homePage.refreshRecent();
        }
        function onRowsInserted() {
            homePage.refreshRecent();
        }
        function onRowsRemoved() {
            homePage.refreshRecent();
        }
        function onTotalCountChanged() {
            homePage.refreshRecent();
        }
    }
}

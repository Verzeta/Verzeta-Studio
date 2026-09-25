// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.ApplicationWindow {
    id: root
    title: qsTr("Verzeta Studio")
    width: 1280
    height: 800
    minimumWidth: 360
    minimumHeight: 480

    Component.onCompleted: {
        if (pageStack)
            pageStack.visible = false;
        refreshCanvasAvailable();
    }

    Shortcut {
        sequence: "Ctrl+Shift+C"
        enabled: root.canvasAvailable
        context: Qt.ApplicationShortcut
        onActivated: {
            if (root.isMobile) {
                root.canvasMobileTabActive = !root.canvasMobileTabActive;
            } else {
                root.canvasShown = !root.canvasShown;
            }
        }
    }

    readonly property bool isWide: width > 900
    readonly property bool isCompact: width < 600
    readonly property bool isMobile: !root.isWide
    property bool rightPanelVisible: false

    property bool canvasAvailable: false
    property bool canvasShown: false
    property bool canvasMobileTabActive: false

    function refreshCanvasAvailable() {
        var convId = ChatController.activeConversationId;
        if (!convId || convId.length === 0) {
            root.canvasAvailable = false;
            root.canvasShown = false;
            root.canvasMobileTabActive = false;
            return;
        }
        var m = Canvas.activeCanvasFor(convId);
        var has = (m && m.id !== undefined && m.id !== "");
        root.canvasAvailable = has;
        if (!has) {
            root.canvasShown = false;
            root.canvasMobileTabActive = false;
        }
    }

    Connections {
        target: ChatController
        function onActiveConversationChanged() {
            root.refreshCanvasAvailable();
        }
    }

    Connections {
        target: Canvas
        function onCanvasOpened(convId, canvasId) {
            if (convId === ChatController.activeConversationId) {
                root.canvasAvailable = true;
                root.canvasShown = true;
                if (root.isMobile) {
                    root.canvasMobileTabActive = true;
                }
            }
        }
        function onCanvasClosed(convId, canvasId) {
            if (convId === ChatController.activeConversationId) {
                root.refreshCanvasAvailable();
            }
        }
    }

    property string activeSection: "home"

    property alias overlayBackdrop: appContentRoot

    RowLayout {
        id: appContentRoot
        anchors.fill: parent
        spacing: 0

        IconNavBar {
            id: navBar
            visible: root.isWide
            Layout.fillHeight: true
            activeSection: root.activeSection
            onSectionChanged: function (section) {
                root.activeSection = section;
            }
        }

        Rectangle {
            Layout.fillHeight: true
            width: 1
            visible: root.isWide
            color: ThemeController.borderSubtle
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            AppButton {
                id: mobileMenuBtn
                visible: !root.isWide && root.activeSection !== "conversations"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: 8
                z: 50
                icon.name: "application-menu"
                iconOnly: true
                flat: true
                onClicked: sidebarDrawer.open()
                Controls.ToolTip.text: qsTr("Menu")
                Controls.ToolTip.visible: hovered
            }

            HomePage {
                id: homePageView
                anchors.fill: parent
                visible: root.activeSection === "home"
                onOpenConversation: function (convId) {
                    ChatController.switchConversation(convId);
                    root.activeSection = "conversations";
                    navBar.activeSection = "conversations";
                }
                onNewConversation: {
                    var id = Conversations.newConversation();
                    if (id.length > 0)
                        ChatController.switchConversation(id);
                    root.activeSection = "conversations";
                    navBar.activeSection = "conversations";
                }
                onNewGroupChat: {
                    root.activeSection = "conversations";
                    navBar.activeSection = "conversations";
                    rootGroupChatDialog.folderId = "";
                    rootGroupChatDialog.allowedAgentIds = [];
                    rootGroupChatDialog.preloadedMembers = [];
                    rootGroupChatDialog.open();
                }
                onOpenSettings: {
                    root.activeSection = "settings";
                    navBar.activeSection = "settings";
                }
            }

            RowLayout {
                anchors.fill: parent
                visible: root.activeSection === "conversations"
                spacing: 0

                LeftSidebar {
                    id: sidebarPanel
                    visible: root.isWide
                    Layout.preferredWidth: 260
                    Layout.minimumWidth: 200
                    Layout.maximumWidth: 400
                    Layout.fillHeight: true
                    onExportRequested: function (convId) {
                        exportDialog.open();
                    }
                }

                Rectangle {
                    Layout.fillHeight: true
                    width: 1
                    visible: root.isWide
                    color: ThemeController.borderSubtle
                }

                Item {
                    id: contentArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Controls.SplitView {
                        id: desktopSplit
                        anchors.fill: parent
                        visible: !root.isMobile
                        orientation: Qt.Horizontal

                        handle: Rectangle {
                            implicitWidth: 6
                            implicitHeight: 6
                            HoverHandler {
                                id: handleHover
                                cursorShape: Qt.SplitHCursor
                            }
                            color: handleHover.hovered ? ThemeController.surfaceCard : ThemeController.borderSubtle
                            border.width: handleHover.hovered ? 1 : 0
                            border.color: ThemeController.borderStrong
                            Column {
                                anchors.centerIn: parent
                                spacing: 3
                                Repeater {
                                    model: 3
                                    delegate: Rectangle {
                                        width: 2
                                        height: 2
                                        radius: 1
                                        color: Qt.darker(Kirigami.Theme.textColor, 1.5)
                                        opacity: handleHover.hovered ? 0.9 : 0.5
                                    }
                                }
                            }
                        }

                        ChatPanel {
                            id: chatPaneDesktop
                            Controls.SplitView.fillWidth: true
                            Controls.SplitView.minimumWidth: 360
                            showMenuButton: false
                            settingsActive: root.rightPanelVisible
                            canvasAvailable: root.canvasAvailable
                            canvasActive: root.canvasShown
                            onToggleSettings: root.rightPanelVisible = !root.rightPanelVisible
                            onToggleCanvas: root.canvasShown = !root.canvasShown
                        }

                        RightSettingsPanel {
                            id: rightPanelDesktop
                            visible: root.rightPanelVisible
                            Controls.SplitView.preferredWidth: 380
                            Controls.SplitView.minimumWidth: 320
                            Controls.SplitView.maximumWidth: 520
                            onCloseRequested: root.rightPanelVisible = false
                        }

                        CanvasPanel {
                            id: canvasPaneDesktop
                            visible: root.canvasAvailable && root.canvasShown
                            Controls.SplitView.preferredWidth: Math.round(root.width * 0.5)
                            Controls.SplitView.minimumWidth: 380
                            conversationId: ChatController.activeConversationId
                        }
                    }

                    ColumnLayout {
                        id: mobileLayout
                        anchors.fill: parent
                        visible: root.isMobile
                        spacing: 0

                        Rectangle {
                            id: mobileTabStrip
                            Layout.fillWidth: true
                            implicitHeight: visible ? 40 : 0
                            visible: root.canvasAvailable
                            color: ThemeController.surfaceCard

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: 4

                                AppButton {
                                    icon.name: "application-menu"
                                    flat: true
                                    iconOnly: true
                                    onClicked: sidebarDrawer.open()
                                    Controls.ToolTip.text: qsTr("Open sidebar")
                                    Controls.ToolTip.visible: hovered
                                }

                                Controls.Button {
                                    Layout.fillWidth: true
                                    text: qsTr("Chat")
                                    flat: true
                                    checkable: true
                                    checked: !root.canvasMobileTabActive
                                    onClicked: root.canvasMobileTabActive = false
                                }

                                Controls.Button {
                                    Layout.fillWidth: true
                                    text: qsTr("Canvas")
                                    flat: true
                                    checkable: true
                                    checked: root.canvasMobileTabActive && root.canvasShown
                                    onClicked: {
                                        root.canvasShown = true;
                                        root.canvasMobileTabActive = true;
                                    }
                                }
                            }
                        }

                        Kirigami.Separator {
                            Layout.fillWidth: true
                            visible: mobileTabStrip.visible
                        }

                        ChatPanel {
                            id: chatPaneMobile
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: !(root.canvasAvailable && root.canvasShown && root.canvasMobileTabActive)
                            showMenuButton: !mobileTabStrip.visible
                            settingsActive: root.rightPanelVisible
                            canvasAvailable: root.canvasAvailable
                            canvasActive: root.canvasShown
                            canvasButtonHidden: true
                            onMenuClicked: sidebarDrawer.open()
                            onToggleSettings: rightDrawer.opened ? rightDrawer.close() : rightDrawer.open()
                            onToggleCanvas: {
                                if (root.canvasMobileTabActive && root.canvasShown) {
                                    root.canvasMobileTabActive = false;
                                } else {
                                    root.canvasShown = true;
                                    root.canvasMobileTabActive = true;
                                }
                            }
                        }

                        CanvasPanel {
                            id: canvasPaneMobile
                            visible: root.canvasAvailable && root.canvasShown && root.canvasMobileTabActive
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            conversationId: ChatController.activeConversationId
                        }
                    }
                }
            }

            AgentsPage {
                id: agentsPageView
                anchors.fill: parent
                visible: root.activeSection === "agents"
            }

            ToolsPage {
                id: toolsPageView
                anchors.fill: parent
                visible: root.activeSection === "tools"
            }

            SkillsPage {
                anchors.fill: parent
                visible: root.activeSection === "skills"
            }

            SettingsPanel {
                id: settingsPageView
                anchors.fill: parent
                visible: root.activeSection === "settings"
            }
        }
    }

    Controls.Drawer {
        id: sidebarDrawer
        width: Math.min(root.width * 0.8, 360)
        height: root.height
        edge: Qt.LeftEdge
        modal: true
        dim: true
        dragMargin: 0

        RowLayout {
            anchors.fill: parent
            spacing: 0

            IconNavBar {
                id: drawerNavBar
                Layout.fillHeight: true
                activeSection: root.activeSection
                onSectionChanged: function (section) {
                    root.activeSection = section;
                    navBar.activeSection = section;
                    if (section !== "conversations") {
                        sidebarDrawer.close();
                    }
                }
            }

            Rectangle {
                Layout.fillHeight: true
                width: 1
                color: ThemeController.borderSubtle
            }

            LeftSidebar {
                Layout.fillWidth: true
                Layout.fillHeight: true
                onConversationActivated: sidebarDrawer.close()
                onExportRequested: function (convId) {
                    sidebarDrawer.close();
                    exportDialog.open();
                }
            }
        }
    }

    Controls.Drawer {
        id: rightDrawer
        width: Math.min(root.width * 0.9, 380)
        height: root.height
        edge: Qt.RightEdge
        modal: true
        dim: true
        dragMargin: 0

        RightSettingsPanel {
            anchors.fill: parent
            onCloseRequested: rightDrawer.close()
        }
    }

    Loader {
        id: firstRunLoader
        anchors.fill: parent
        z: 200
        active: !SettingsService.firstRunComplete
        sourceComponent: active ? firstRunComponent : null

        Component {
            id: firstRunComponent
            FirstRunWizard {
                onClosed: firstRunLoader.active = false
            }
        }
    }

    Kirigami.InlineMessage {
        id: errorBanner
        type: Kirigami.MessageType.Error
        visible: false
        position: Kirigami.InlineMessage.Position.Header
        showCloseButton: true
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            margins: 8
        }
        z: 100
    }

    Kirigami.InlineMessage {
        id: exportBanner
        visible: false
        position: Kirigami.InlineMessage.Position.Header
        showCloseButton: true
        anchors {
            top: errorBanner.visible ? errorBanner.bottom : parent.top
            left: parent.left
            right: parent.right
            margins: 8
        }
        z: 100
        Timer {
            id: exportBannerHide
            interval: 5000
            onTriggered: exportBanner.visible = false
        }
    }

    Kirigami.InlineMessage {
        id: mentionBanner
        type: Kirigami.MessageType.Information
        visible: false
        position: Kirigami.InlineMessage.Position.Header
        showCloseButton: true
        anchors {
            top: {
                if (errorBanner.visible)
                    return errorBanner.bottom;
                if (exportBanner.visible)
                    return exportBanner.bottom;
                return parent.top;
            }
            left: parent.left
            right: parent.right
            margins: 8
        }
        z: 100

        property string targetConvId: ""

        actions: Kirigami.Action {
            text: qsTr("Open conversation")
            icon.name: "go-jump"
            onTriggered: {
                if (mentionBanner.targetConvId.length > 0 && mentionBanner.targetConvId !== ChatController.activeConversationId) {
                    ChatController.switchConversation(mentionBanner.targetConvId);
                }
                root.activeSection = "conversations";
                navBar.activeSection = "conversations";
                mentionBanner.visible = false;
            }
        }

        Timer {
            id: mentionBannerHide
            interval: 15000
            onTriggered: mentionBanner.visible = false
        }
    }

    ExportDialog {
        id: exportDialog
    }

    NewGroupChatDialog {
        id: rootGroupChatDialog
    }

    Shortcut {
        sequences: [StandardKey.New]
        onActivated: {
            var _id = Conversations.newConversation();
            if (_id.length > 0)
                ChatController.switchConversation(_id);
            root.activeSection = "conversations";
            navBar.activeSection = "conversations";
        }
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: {
            root.activeSection = "settings";
            navBar.activeSection = "settings";
        }
    }
    Shortcut {
        sequence: "Ctrl+Shift+S"
        onActivated: {
            if (root.activeSection === "conversations") {
                if (root.isWide)
                    root.rightPanelVisible = !root.rightPanelVisible;
                else
                    rightDrawer.opened ? rightDrawer.close() : rightDrawer.open();
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+E"
        enabled: ChatController.activeConversationId.length > 0
        onActivated: exportDialog.open()
    }
    Shortcut {
        sequence: "Ctrl+H"
        onActivated: {
            root.activeSection = "home";
            navBar.activeSection = "home";
        }
    }

    Connections {
        target: ChatController
        function onErrorOccurred(message) {
            errorBanner.text = message;
            errorBanner.visible = true;
        }
        function onUserMessageQueued(text) {
            var preview = text.replace(/\n/g, " ").trim();
            if (preview.length > 80)
                preview = preview.substring(0, 80) + "…";
            exportBanner.text = qsTr("Queued \"%1\". It sends when the current response finishes.").arg(preview);
            exportBanner.type = Kirigami.MessageType.Information;
            exportBanner.visible = true;
            exportBannerHide.start();
        }
        function onUserMentionedInGroup(convId, agentAlias, messageText) {
            mentionBanner.targetConvId = convId;
            var preview = messageText.replace(/\n/g, " ").trim();
            if (preview.length > 120)
                preview = preview.substring(0, 120) + "…";
            mentionBanner.text = qsTr("@%1 needs your input: %2").arg(agentAlias).arg(preview);
            mentionBanner.visible = true;
            mentionBannerHide.restart();
        }
    }

    Connections {
        target: Conversations
        function onErrorOccurred(message) {
            errorBanner.text = message;
            errorBanner.visible = true;
        }
    }

    Connections {
        target: Export
        function onExportCompleted(filePath) {
            exportBanner.text = qsTr("Exported to: %1").arg(filePath);
            exportBanner.type = Kirigami.MessageType.Positive;
            exportBanner.visible = true;
            exportBannerHide.start();
        }
        function onExportFailed(error) {
            exportBanner.text = qsTr("Export failed: %1").arg(error);
            exportBanner.type = Kirigami.MessageType.Error;
            exportBanner.visible = true;
        }
    }
}

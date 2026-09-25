// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: chatPanel
    color: ThemeController.surfacePage

    property bool showMenuButton: false
    property bool settingsActive: false

    property bool canvasAvailable: false
    property bool canvasActive: false
    property bool canvasButtonHidden: false

    signal menuClicked
    signal toggleSettings
    signal toggleCanvas

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ModelSelectorBar {
            Layout.fillWidth: true
            showMenuButton: chatPanel.showMenuButton
            settingsActive: chatPanel.settingsActive
            canvasAvailable: chatPanel.canvasAvailable
            canvasActive: chatPanel.canvasActive
            canvasButtonHidden: chatPanel.canvasButtonHidden
            onMenuClicked: chatPanel.menuClicked()
            onToggleSettings: chatPanel.toggleSettings()
            onToggleCanvas: chatPanel.toggleCanvas()
            onOpenArtifacts: artifactsDialog.open()
            onOpenTerminal: terminalDialog.open()
            onOpenPlans: plansOverlay.open()
            onStartCall: callOverlay.startCall()
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Item {
            id: modelGate
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: AgentSettings.activeModel.length === 0

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 20
                width: applicationWindow().isCompact ? Math.min(parent.width - 32, 400) : Math.min(parent.width - 80, 400)

                Kirigami.Icon {
                    source: "computer"
                    fallback: "computer-laptop"
                    implicitWidth: Kirigami.Units.iconSizes.huge
                    implicitHeight: Kirigami.Units.iconSizes.huge
                    Layout.alignment: Qt.AlignHCenter
                    color: Kirigami.Theme.disabledTextColor
                }

                Kirigami.Heading {
                    text: qsTr("Select a Model to Start")
                    level: 2
                    Layout.alignment: Qt.AlignHCenter
                }

                Controls.Label {
                    text: qsTr("Choose a provider and model below, then start chatting.")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: modelGateSelector.implicitHeight + 24
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    Layout.alignment: Qt.AlignHCenter

                    ModelSelector {
                        id: modelGateSelector
                        anchors.centerIn: parent
                        width: applicationWindow().isCompact ? Math.min(implicitWidth, parent.width - 24) : implicitWidth
                    }
                }
            }
        }

        ListView {
            id: messageList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: AgentSettings.activeModel.length > 0
            model: ChatController.messages
            spacing: 16
            clip: true
            topMargin: 16
            bottomMargin: 16
            leftMargin: 24
            rightMargin: 24

            cacheBuffer: 1200

            Controls.ScrollBar.vertical: Controls.ScrollBar {
                id: chatVBar
                policy: Controls.ScrollBar.AsNeeded
                onPressedChanged: {
                    if (pressed) {
                        messageList.followTail = false;
                    } else {
                        messageList.followTail = messageList.atYEnd;
                    }
                }
            }

            delegate: Item {
                id: delegateRoot
                width: {
                    var v = ListView.view;
                    if (!v)
                        return 0;
                    return Math.max(0, v.width - v.leftMargin - v.rightMargin);
                }
                height: msgBubble.implicitHeight

                required property string id
                required property string role
                required property string content
                required property string contentHtml
                required property bool isStreaming
                required property int tokenCount
                required property string modelUsed
                required property string finishReason
                required property string agentId
                required property string agentName
                required property string agentRoleName
                required property string agentIcon
                required property var metadata
                required property var attachments
                required property string thinkingContent

                MessageBubble {
                    id: msgBubble
                    width: parent.width

                    messageId: delegateRoot.id
                    role: delegateRoot.role
                    content: delegateRoot.content
                    contentHtml: delegateRoot.contentHtml
                    isStreaming: delegateRoot.isStreaming
                    tokenCount: delegateRoot.tokenCount
                    modelUsed: delegateRoot.modelUsed
                    finishReason: delegateRoot.finishReason
                    agentId: delegateRoot.agentId
                    agentName: delegateRoot.agentName
                    agentRoleName: delegateRoot.agentRoleName
                    agentIcon: delegateRoot.agentIcon
                    metadata: delegateRoot.metadata
                    attachments: delegateRoot.attachments
                    thinkingContent: delegateRoot.thinkingContent
                }
            }

            property bool followTail: true

            onMovementEnded: followTail = atYEnd

            Connections {
                target: ChatController
                function onActiveConversationChanged() {
                    messageList.followTail = true;
                    Qt.callLater(messageList.scrollToEnd);
                }
            }

            onCountChanged: Qt.callLater(scrollToEnd)
            onContentHeightChanged: {
                if (followTail && !moving && !dragging && !chatVBar.pressed)
                    contentY = Math.max(originY, originY + contentHeight + bottomMargin + topMargin - height);
            }

            function scrollToEnd() {
                if (count > 0 && followTail)
                    positionViewAtEnd();
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                visible: messageList.count === 0 && !ChatController.isGenerating
                text: qsTr("Start a Conversation")
                explanation: qsTr("Type a message below and press Enter to chat with %1.").arg(AgentSettings.activeModel)
                icon.name: "dialog-messages"
                width: parent.width - Kirigami.Units.largeSpacing * 4
            }
        }

        Flow {
            id: pendingAttachmentsFlow
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 4
            spacing: 6
            visible: pendingAttachments.count > 0 && AgentSettings.activeModel.length > 0

            Repeater {
                model: pendingAttachments
                delegate: FileAttachmentChip {
                    onRemoveClicked: pendingAttachments.remove(index)
                }
            }
        }

        Rectangle {
            id: groupHintBanner
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 4
            implicitHeight: groupHintRow.implicitHeight + 12
            visible: AgentSettings.activeModel.length > 0 && Conversations.isGroup(ChatController.activeConversationId)
            radius: ThemeController.radius
            color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.10)
            border.color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.35)
            border.width: 1

            property int _membersRev: 0

            Connections {
                target: MembershipService
                function onConversationMembersChanged(convId) {
                    if (convId === ChatController.activeConversationId) {
                        groupHintBanner._membersRev++;
                    }
                }
            }

            RowLayout {
                id: groupHintRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8

                Kirigami.Icon {
                    source: "user-group-new"
                    fallback: "system-users"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                    color: Kirigami.Theme.highlightColor
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Controls.Label {
                        text: {
                            var _rev = groupHintBanner._membersRev;
                            var members = Conversations.groupMembers(ChatController.activeConversationId);
                            if (members.length === 0)
                                return qsTr("Group chat");
                            var names = [];
                            for (var i = 0; i < members.length; ++i) {
                                var alias = "@" + (members[i].alias || "").replace(/ /g, "_");
                                if (members[i].isCoordinator) {
                                    names.push("⭐ " + alias);
                                } else {
                                    names.push(alias);
                                }
                            }
                            return qsTr("Group chat: ") + names.join(", ") + qsTr("   •   use @everyone to broadcast");
                        }
                        color: Kirigami.Theme.textColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        text: qsTr("Verify agent work: click Artifacts (%1) for files, Tool Log (%2) for actions").arg(ArtifactsModel.count).arg(ToolCallLogModel.count)
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        font.italic: true
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
        }

        ChatInputBar {
            id: inputBar
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 4
            visible: AgentSettings.activeModel.length > 0

            onSendMessage: function (text) {
                chatPanel.sendAction(text);
            }
            onAttachRequested: attachDialog.open()
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 38
            visible: AgentSettings.activeModel.length > 0
            color: ThemeController.surfaceCard

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                TokenCounter {
                    Layout.alignment: Qt.AlignVCenter
                }

                Rectangle {
                    visible: AgentSettings.activeModel.length > 0 && ChatController.messages && ChatController.messages.count > 0
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 18
                    Layout.alignment: Qt.AlignVCenter
                    color: ThemeController.borderSubtle
                }

                StatChip {
                    visible: AgentSettings.activeModel.length > 0
                    iconSource: "network-server"
                    iconFallback: "computer"
                    value: AgentSettings.activeModel
                    valueColor: Kirigami.Theme.textColor
                    tip: qsTr("Active model")
                    Layout.alignment: Qt.AlignVCenter
                    Layout.maximumWidth: 340
                }

                Item {
                    Layout.fillWidth: true
                }

                ToolActivityIndicator {
                    active: ChatController.isGenerating
                    Layout.alignment: Qt.AlignVCenter
                }
            }
        }
    }

    CallOverlay {
        id: callOverlay
        conversationId: ChatController.activeConversationId
    }

    ListModel {
        id: pendingAttachments
    }

    function sendAction(text) {
        if (text.length === 0 && pendingAttachments.count === 0)
            return;
        if (ChatController.activeConversationId.length === 0) {
            var _newId = Conversations.newConversation();
            if (_newId.length > 0)
                ChatController.switchConversation(_newId);
        }
        messageList.followTail = true;
        if (pendingAttachments.count > 0) {
            var paths = [];
            for (var i = 0; i < pendingAttachments.count; ++i)
                paths.push(pendingAttachments.get(i).filePath);
            ChatController.sendMessageWithAttachments(text, paths);
            pendingAttachments.clear();
        } else {
            ChatController.sendMessage(text);
        }
        inputBar.clear();
    }

    Dialogs.FileDialog {
        id: attachDialog
        title: qsTr("Attach Files")
        fileMode: Dialogs.FileDialog.OpenFiles

        onAccepted: {
            for (var i = 0; i < selectedFiles.length; ++i) {
                var url = selectedFiles[i];
                var path = PathUtils.toLocalFile(url);
                if (path.length === 0)
                    continue;
                var cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
                var name = path.substring(cut + 1);
                var mime = FileService.mimeType(path);
                pendingAttachments.append({
                        "filePath": path,
                        "fileName": name,
                        "mimeType": mime
                    });
            }
        }
    }

    Connections {
        target: ImageService
        function onImageGenerated(convId, path) {
            if (convId === ChatController.activeConversationId)
                applicationWindow().showPassiveNotification(qsTr("Image generated"), 3000);
        }
        function onError(convId, message) {
            if (convId === ChatController.activeConversationId)
                applicationWindow().showPassiveNotification(qsTr("Image error: %1").arg(message), 5000);
        }
    }

    Connections {
        target: AudioService
        function onAudioGenerated(convId, path) {
            if (convId === ChatController.activeConversationId)
                applicationWindow().showPassiveNotification(qsTr("Audio generated"), 3000);
        }
        function onError(convId, message) {
            if (convId === ChatController.activeConversationId)
                applicationWindow().showPassiveNotification(qsTr("Audio error: %1").arg(message), 5000);
        }
    }

    PlansOverlay {
        id: plansOverlay
    }

    AppOverlayDialog {
        id: artifactsDialog
        parent: applicationWindow().overlay
        implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 40)

        property string artifactsPath: ""

        onOpened: artifactsPath = ChatController.activeArtifactsPath()

        function resolveArtifactPath(p) {
            if (!p || p.length === 0)
                return "";
            if (p.charAt(0) === "/" || p.indexOf("://") > 0)
                return p;
            if (Qt.platform.os === "windows" && (/^[A-Za-z]:[\\/]/.test(p) || p.substring(0, 2) === "\\\\"))
                return p;
            if (artifactsPath.length === 0)
                return p;
            return artifactsPath + "/" + p;
        }

        function isCanvasOpenable(name) {
            return /\.(md|markdown|txt|text|log|json|jsonl|yaml|yml|xml|toml|ini|cfg|conf|csv|py|js|mjs|ts|tsx|jsx|cpp|cxx|cc|c|h|hpp|hh|rs|go|java|kt|kts|rb|php|sh|bash|zsh|sql|html|htm|css|scss|qml|cmake)$/i.test(name || "");
        }

        title: qsTr("Artifacts: %1").arg(ChatController.activeConversationTitle)
        dialogIcon: "folder-documents"

        footer: RowLayout {
            Controls.Label {
                text: qsTr("%1 file(s)").arg(artifactsRepeater.count)
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                visible: artifactsRepeater.count > 0
                Layout.fillWidth: true
            }
            Item {
                Layout.fillWidth: true
                visible: artifactsRepeater.count === 0
            }
            AppButton {
                text: qsTr("Open Folder")
                icon.name: "folder-open"
                flat: true
                visible: artifactsDialog.artifactsPath.length > 0
                onClicked: ChatController.openActiveArtifactsFolder()
            }
            AppButton {
                text: qsTr("Close")
                onClicked: artifactsDialog.close()
            }
        }

        ColumnLayout {
            spacing: 8

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 4
                implicitHeight: pathRow.implicitHeight + 14
                radius: ThemeController.radius
                color: ThemeController.surfaceSunken
                border.color: ThemeController.borderSubtle
                border.width: 1
                visible: artifactsDialog.artifactsPath.length > 0

                RowLayout {
                    id: pathRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    spacing: 8

                    Kirigami.Icon {
                        source: "folder"
                        fallback: "inode-directory"
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: Kirigami.Units.iconSizes.small
                        color: Kirigami.Theme.textColor
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Controls.Label {
                            text: qsTr("Saved to")
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                            color: Kirigami.Theme.disabledTextColor
                        }
                        Controls.Label {
                            text: artifactsDialog.artifactsPath
                            font.family: ThemeController.codeFontFamily
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            color: Kirigami.Theme.textColor
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            Controls.Label {
                visible: artifactsRepeater.count === 0
                Layout.fillWidth: true
                Layout.topMargin: 20
                Layout.bottomMargin: 20
                text: qsTr("No artifacts yet.\nTool calls that generate files will appear here.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Repeater {
                id: artifactsRepeater
                model: ArtifactsModel

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: artifactRowLayout.implicitHeight + 20
                    radius: ThemeController.radius
                    color: {
                        if (artifactMouseArea.containsMouse === true)
                            return ThemeController.hoverTint;
                        return ThemeController.surfaceCard;
                    }
                    border.color: ThemeController.borderSubtle
                    border.width: 1

                    required property string path
                    required property string fileName
                    required property string toolName
                    required property string planId
                    required property string stepId
                    required property string stepTitle
                    required property string planGoal
                    required property string submittedBy
                    required property int index

                    RowLayout {
                        id: artifactRowLayout
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 10
                        spacing: 10

                        Kirigami.Icon {
                            source: "text-x-script"
                            fallback: "text-x-generic"
                            implicitWidth: Kirigami.Units.iconSizes.smallMedium
                            implicitHeight: Kirigami.Units.iconSizes.smallMedium
                            color: Kirigami.Theme.textColor
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Controls.Label {
                                text: fileName || ""
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: Kirigami.Theme.textColor
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }

                            RowLayout {
                                visible: stepId !== undefined && stepId !== ""
                                Layout.fillWidth: true
                                spacing: 6

                                Rectangle {
                                    Layout.preferredHeight: tagText.implicitHeight + 4
                                    Layout.preferredWidth: tagText.implicitWidth + 12
                                    radius: height / 2
                                    color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.18)
                                    border.color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.45)
                                    border.width: 1
                                    Controls.Label {
                                        id: tagText
                                        anchors.centerIn: parent
                                        text: qsTr("Task: %1 · %2").arg(planGoal || "").arg(stepTitle || "")
                                        color: Kirigami.Theme.highlightColor
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                        elide: Text.ElideRight
                                    }
                                }

                                Controls.Label {
                                    visible: submittedBy !== undefined && submittedBy !== ""
                                    text: qsTr("by @%1").arg(submittedBy || "")
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                }

                                Item {
                                    Layout.fillWidth: true
                                }
                            }

                            Controls.Label {
                                text: path || ""
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: Kirigami.Theme.disabledTextColor
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }

                        Controls.ToolButton {
                            icon.name: "document-edit"
                            visible: artifactsDialog.isCanvasOpenable(fileName)
                            Controls.ToolTip.text: qsTr("Open in Canvas")
                            Controls.ToolTip.visible: hovered
                            onClicked: {
                                var abs = artifactsDialog.resolveArtifactPath(path);
                                var cid = Canvas.openCanvasFromFile(ChatController.activeConversationId, abs);
                                if (cid.length > 0) {
                                    artifactsDialog.close();
                                } else {
                                    applicationWindow().showPassiveNotification(qsTr("Could not open %1 in Canvas").arg(fileName));
                                }
                            }
                        }

                        Controls.ToolButton {
                            icon.name: "document-open"
                            Controls.ToolTip.text: qsTr("Open with default application")
                            Controls.ToolTip.visible: hovered
                            onClicked: {
                                var abs = artifactsDialog.resolveArtifactPath(path);
                                if (!ChatController.openPathExternally(abs)) {
                                    applicationWindow().showPassiveNotification(qsTr("File not found: %1").arg(abs));
                                }
                            }
                        }

                        Controls.ToolButton {
                            icon.name: "edit-copy"
                            Controls.ToolTip.text: qsTr("Copy full path")
                            Controls.ToolTip.visible: hovered
                            onClicked: {
                                artifactClipHelper.text = artifactsDialog.resolveArtifactPath(path);
                                artifactClipHelper.selectAll();
                                artifactClipHelper.copy();
                            }
                        }
                    }

                    MouseArea {
                        id: artifactMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }
                }
            }
        }

        TextEdit {
            id: artifactClipHelper
            visible: false
        }
    }

    AppOverlayDialog {
        id: terminalDialog
        parent: applicationWindow().overlay
        implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 48)

        title: qsTr("Tool Activity Log")
        dialogIcon: "view-list-details"

        footer: RowLayout {
            Controls.Label {
                text: qsTr("%1 tool call(s)").arg(toolLogRepeater.count)
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                visible: toolLogRepeater.count > 0
                Layout.fillWidth: true
            }
            Item {
                Layout.fillWidth: true
                visible: toolLogRepeater.count === 0
            }
            AppButton {
                text: qsTr("Close")
                onClicked: terminalDialog.close()
            }
        }

        ColumnLayout {
            spacing: 10

            Controls.Label {
                visible: toolLogRepeater.count === 0
                Layout.fillWidth: true
                Layout.topMargin: 40
                Layout.bottomMargin: 40
                text: qsTr("No tool calls yet.\nWhen the assistant uses tools (shell commands, file operations),\ntheir execution history will appear here.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Repeater {
                id: toolLogRepeater
                model: ToolCallLogModel

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: entryCol.implicitHeight + 20
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: {
                        if (status === "error")
                            return Kirigami.Theme.negativeTextColor;
                        if (status === "running")
                            return Kirigami.Theme.neutralTextColor;
                        return ThemeController.borderSubtle;
                    }
                    border.width: 1

                    required property string id
                    required property string toolName
                    required property string args
                    required property string result
                    required property string status
                    required property string timestamp
                    required property int index

                    ColumnLayout {
                        id: entryCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 10
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Rectangle {
                                width: 8
                                height: 8
                                radius: height / 2
                                Layout.alignment: Qt.AlignVCenter
                                color: {
                                    if (status === "running")
                                        return Kirigami.Theme.neutralTextColor;
                                    if (status === "error")
                                        return Kirigami.Theme.negativeTextColor;
                                    return Kirigami.Theme.positiveTextColor;
                                }
                            }

                            Controls.Label {
                                text: toolName
                                font.bold: true
                                font.family: ThemeController.codeFontFamily
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: Kirigami.Theme.textColor
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            Controls.Label {
                                text: {
                                    if (!timestamp)
                                        return "";
                                    var d = new Date(timestamp);
                                    return d.toLocaleTimeString(Qt.locale(), Locale.ShortFormat);
                                }
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: Kirigami.Theme.disabledTextColor
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: argsEdit.implicitHeight + 12
                            radius: ThemeController.radius
                            color: ThemeController.codeBackground
                            visible: args && args.length > 0

                            HoverHandler {
                                id: argsHover
                            }

                            TextEdit {
                                id: argsEdit
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 6
                                anchors.rightMargin: 30
                                text: {
                                    if (!args)
                                        return "";
                                    try {
                                        return JSON.stringify(JSON.parse(args), null, 2);
                                    } catch (e) {
                                        return args;
                                    }
                                }
                                readOnly: true
                                selectByMouse: true
                                persistentSelection: true
                                wrapMode: TextEdit.Wrap
                                textFormat: TextEdit.PlainText
                                font.family: ThemeController.codeFontFamily
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: ThemeController.codeText
                                selectionColor: Kirigami.Theme.highlightColor

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.RightButton
                                    z: 10
                                    onPressed: argsMenu.popup()
                                }
                                Controls.Menu {
                                    id: argsMenu
                                    Controls.MenuItem {
                                        text: qsTr("Copy selection")
                                        enabled: argsEdit.selectedText.length > 0
                                        onTriggered: argsEdit.copy()
                                    }
                                    Controls.MenuItem {
                                        text: qsTr("Copy all")
                                        onTriggered: {
                                            argsEdit.selectAll();
                                            argsEdit.copy();
                                            argsEdit.deselect();
                                        }
                                    }
                                }
                            }

                            Controls.ToolButton {
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.margins: 2
                                implicitWidth: 24
                                implicitHeight: 24
                                visible: argsHover.hovered
                                icon.name: "edit-copy"
                                icon.width: Kirigami.Units.iconSizes.small
                                icon.height: Kirigami.Units.iconSizes.small
                                onClicked: {
                                    argsEdit.selectAll();
                                    argsEdit.copy();
                                    argsEdit.deselect();
                                }
                                Controls.ToolTip.text: qsTr("Copy command")
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.delay: 600
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: resultEdit.implicitHeight + 12
                            radius: ThemeController.radius
                            color: ThemeController.codeBackground
                            visible: result && result.length > 0

                            HoverHandler {
                                id: resultHover
                            }

                            TextEdit {
                                id: resultEdit
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 6
                                anchors.rightMargin: 30
                                text: {
                                    if (!result)
                                        return "";
                                    try {
                                        return JSON.stringify(JSON.parse(result), null, 2);
                                    } catch (e) {
                                        return result;
                                    }
                                }
                                readOnly: true
                                selectByMouse: true
                                persistentSelection: true
                                wrapMode: TextEdit.Wrap
                                textFormat: TextEdit.PlainText
                                font.family: ThemeController.codeFontFamily
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: status === "error" ? "#f48771" : "#4ec9b0"
                                selectionColor: Kirigami.Theme.highlightColor

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.RightButton
                                    z: 10
                                    onPressed: resultMenu.popup()
                                }
                                Controls.Menu {
                                    id: resultMenu
                                    Controls.MenuItem {
                                        text: qsTr("Copy selection")
                                        enabled: resultEdit.selectedText.length > 0
                                        onTriggered: resultEdit.copy()
                                    }
                                    Controls.MenuItem {
                                        text: qsTr("Copy all")
                                        onTriggered: {
                                            resultEdit.selectAll();
                                            resultEdit.copy();
                                            resultEdit.deselect();
                                        }
                                    }
                                }
                            }

                            Controls.ToolButton {
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.margins: 2
                                implicitWidth: 24
                                implicitHeight: 24
                                visible: resultHover.hovered
                                icon.name: "edit-copy"
                                icon.width: Kirigami.Units.iconSizes.small
                                icon.height: Kirigami.Units.iconSizes.small
                                onClicked: {
                                    resultEdit.selectAll();
                                    resultEdit.copy();
                                    resultEdit.deselect();
                                }
                                Controls.ToolTip.text: qsTr("Copy result")
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.delay: 600
                            }
                        }
                    }
                }
            }
        }
    }

    ToolConfirmationDialog {
        id: toolConfirmDialog
    }
}

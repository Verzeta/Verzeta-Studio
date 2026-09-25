// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Shapes
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: inputBar
    radius: ThemeController.radius
    color: ThemeController.surfaceCard
    border.color: {
        if (inputField.activeFocus === true)
            return Kirigami.Theme.highlightColor;
        return ThemeController.borderSubtle;
    }
    border.width: 1
    implicitHeight: inputColumn.implicitHeight + 24

    Behavior on border.color  {
        ColorAnimation {
            duration: 120
        }
    }

    signal sendMessage(string text)
    signal attachRequested

    readonly property bool _compact: width < 640
    readonly property bool _veryCompact: width < 380

    function clear() {
        inputField.text = "";
    }

    function focusInput() {
        inputField.forceActiveFocus();
    }

    property var busyMembers: PlansModel.busyMembers

    QtObject {
        id: _picker
        property string mode: ""
        property int triggerIndex: -1
        property string partial: ""
    }

    property var _members: []
    function _refreshMembers() {
        var cid = ChatController.activeConversationId;
        _members = (cid && cid.length > 0) ? MembershipService.conversationMembersList(cid) : [];
        if (pickerPopup.opened)
            _rebuildPickerModel();
    }
    readonly property bool _isGroupChat: _members.length > 0

    property var _pickerModel: []
    function _rebuildPickerModel() {
        var mode = _picker.mode;
        var partial = _picker.partial.toLowerCase();
        var rows = [];
        if (mode === "mention") {
            var members = inputBar._members;
            for (var i = 0; i < members.length; ++i) {
                var m = members[i];
                var alias = m.alias || "";
                if (partial.length === 0 || alias.toLowerCase().indexOf(partial) >= 0) {
                    rows.push({
                            "insertText": "@" + alias + " ",
                            "title": alias,
                            "subtitle": m.agentName ? (m.agentName + (m.isCoordinator ? qsTr(" • coordinator") : "")) : (m.agentDescription || ""),
                            "iconName": m.agentIconName || "user"
                        });
                }
            }
        } else if (mode === "command") {
            var cmds = SlashCommands.availableCommands();
            for (var j = 0; j < cmds.length; ++j) {
                var c = cmds[j];
                var name = c.name || "";
                var bare = name.charAt(0) === "/" ? name.substring(1) : name;
                if (partial.length === 0 || bare.toLowerCase().indexOf(partial) >= 0) {
                    rows.push({
                            "insertText": name + " ",
                            "title": name,
                            "subtitle": c.summary || "",
                            "iconName": ""
                        });
                }
            }
        }
        _pickerModel = rows;
    }

    readonly property bool _pickerOpen: pickerPopup.opened

    function _updatePickerToken() {
        var text = inputField.text;
        var pos = inputField.cursorPosition;
        if (pos > text.length)
            pos = text.length;
        if (text.length > 0 && text.charAt(0) === "/") {
            var firstSpace = text.indexOf(" ");
            var firstNl = text.indexOf("\n");
            var tokenEnd = text.length;
            if (firstSpace >= 0)
                tokenEnd = Math.min(tokenEnd, firstSpace);
            if (firstNl >= 0)
                tokenEnd = Math.min(tokenEnd, firstNl);
            if (pos <= tokenEnd) {
                _picker.mode = "command";
                _picker.triggerIndex = 0;
                _picker.partial = text.substring(1, pos);
                _openPicker();
                return;
            }
        }
        if (inputBar._isGroupChat) {
            var i = pos - 1;
            while (i >= 0) {
                var ch = text.charAt(i);
                if (ch === "@") {
                    var before = i > 0 ? text.charAt(i - 1) : " ";
                    if (before === " " || before === "\n" || before === "\t") {
                        _picker.mode = "mention";
                        _picker.triggerIndex = i;
                        _picker.partial = text.substring(i + 1, pos);
                        _openPicker();
                        return;
                    }
                    break;
                }
                if (ch === " " || ch === "\n" || ch === "\t")
                    break;
                --i;
            }
        }
        _closePicker();
    }

    function _partialIsTokenLike(s) {
        return /^[A-Za-z0-9_\-]*$/.test(s);
    }

    function _openPicker() {
        if (!_partialIsTokenLike(_picker.partial)) {
            _closePicker();
            return;
        }
        _rebuildPickerModel();
        if (pickerListView.count === 0) {
            _closePicker();
            return;
        }
        if (!pickerPopup.opened)
            pickerPopup.open();
        if (pickerListView.currentIndex < 0 || pickerListView.currentIndex >= pickerListView.count)
            pickerListView.currentIndex = 0;
    }

    function _closePicker() {
        if (pickerPopup.opened)
            pickerPopup.close();
        _picker.mode = "";
        _picker.triggerIndex = -1;
        _picker.partial = "";
    }

    function _acceptPicker() {
        if (!pickerPopup.opened)
            return false;
        var idx = pickerListView.currentIndex;
        if (idx < 0 || idx >= pickerListView.count)
            return false;
        var insert = pickerListView.model[idx] ? pickerListView.model[idx].insertText : "";
        if (!insert || insert.length === 0)
            return false;
        var text = inputField.text;
        var cursor = inputField.cursorPosition;
        var start = _picker.triggerIndex;
        var before = text.substring(0, start);
        var after = text.substring(cursor);
        inputField.text = before + insert + after;
        inputField.cursorPosition = before.length + insert.length;
        _closePicker();
        return true;
    }

    ColumnLayout {
        id: inputColumn
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: busyRow.implicitHeight + 8
            visible: inputBar.busyMembers.length > 0
            radius: ThemeController.radius
            color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.12)
            border.color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.35)
            border.width: 1

            RowLayout {
                id: busyRow
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                Kirigami.Icon {
                    source: "media-playback-start"
                    fallback: "system-run"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    color: Kirigami.Theme.neutralTextColor
                }
                Controls.Label {
                    Layout.fillWidth: true
                    text: qsTr("Working: %1  •  Your message will be sent when this turn ends.").arg(inputBar.busyMembers.join(", "))
                    color: Kirigami.Theme.neutralTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    elide: Text.ElideRight
                    wrapMode: Text.NoWrap
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: queuedRow.implicitHeight + 8
            visible: ChatController.queuedUserText.length > 0
            radius: ThemeController.radius
            color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.12)
            border.color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.35)
            border.width: 1

            RowLayout {
                id: queuedRow
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                Kirigami.Icon {
                    source: "mail-queue"
                    fallback: "documentinfo"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    color: Kirigami.Theme.highlightColor
                }
                Controls.Label {
                    Layout.fillWidth: true
                    text: qsTr("Queued. Next: %1").arg(ChatController.queuedUserText)
                    color: Kirigami.Theme.highlightColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    elide: Text.ElideRight
                    wrapMode: Text.NoWrap
                }
            }
        }

        Controls.ScrollView {
            id: inputScroll
            Layout.fillWidth: true
            Layout.minimumHeight: 24
            Layout.maximumHeight: 160
            clip: true
            Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
            Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded

            Controls.TextArea {
                id: inputField
                placeholderText: qsTr("Message…")
                wrapMode: TextEdit.Wrap
                font.family: ThemeController.fontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                color: Kirigami.Theme.textColor
                placeholderTextColor: Kirigami.Theme.disabledTextColor
                background: null

                onTextChanged: inputBar._updatePickerToken()
                onCursorPositionChanged: inputBar._updatePickerToken()
                onActiveFocusChanged: {
                    if (!activeFocus)
                        inputBar._closePicker();
                }

                Keys.onPressed: function (event) {
                    if (!inputBar._pickerOpen)
                        return;
                    if (event.key === Qt.Key_Down) {
                        pickerListView.incrementCurrentIndex();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Up) {
                        pickerListView.decrementCurrentIndex();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Tab) {
                        event.accepted = inputBar._acceptPicker();
                    } else if (event.key === Qt.Key_Escape) {
                        inputBar._closePicker();
                        event.accepted = true;
                    }
                }

                Keys.onReturnPressed: function (event) {
                    if (inputBar._pickerOpen && !(event.modifiers & Qt.ShiftModifier)) {
                        event.accepted = inputBar._acceptPicker();
                        return;
                    }
                    if (event.modifiers & Qt.ShiftModifier) {
                        event.accepted = false;
                    } else {
                        event.accepted = true;
                        inputBar.doSend();
                    }
                }
                Keys.onEnterPressed: function (event) {
                    if (inputBar._pickerOpen && !(event.modifiers & Qt.ShiftModifier)) {
                        event.accepted = inputBar._acceptPicker();
                        return;
                    }
                    if (event.modifiers & Qt.ShiftModifier) {
                        event.accepted = false;
                    } else {
                        event.accepted = true;
                        inputBar.doSend();
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            AppButton {
                icon.name: "mail-attachment"
                flat: true
                iconOnly: true
                text: qsTr("Attach files")
                enabled: !ChatController.isGenerating
                onClicked: inputBar.attachRequested()
            }

            AppButton {
                icon.name: "view-task"
                flat: true
                iconOnly: true
                text: qsTr("Start a Task")
                visible: !inputBar._veryCompact
                enabled: !ChatController.isGenerating
                onClicked: startTaskDialog.open()
                Controls.ToolTip.text: qsTr("Start a task: the agent will work on your request using whatever tools it needs. Progress and artifacts are tracked in the Plans panel.")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }

            AppButton {
                icon.name: "image-x-generic"
                flat: true
                iconOnly: true
                text: qsTr("Generate Image")
                visible: !inputBar._veryCompact && SettingsService.isImageGenConfigured()
                onClicked: generateImageDialog.open()
                Controls.ToolTip.text: qsTr("Generate an image from a text prompt. Result appears as an assistant message in this conversation.")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }

            AppButton {
                id: stopTaskBtn
                icon.name: "process-stop"
                flat: false
                iconOnly: false
                text: qsTr("Stop Task")
                visible: hasTaskActive
                onClicked: {
                    Tasks.stopAllActivePlansInConversation(ChatController.activeConversationId);
                    hasTaskActive = false;
                }
                Controls.ToolTip.text: qsTr("Stop every active task in this chat. Clears queued messages and cancels any running model request.")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600

                property bool hasTaskActive: PlansModel.hasActiveTask
            }

            Item {
                Layout.fillWidth: true
            }

            FeaturePill {
                text: qsTr("Thinking")
                icon.name: "view-visible"
                iconOnly: inputBar._compact
                checked: AgentSettings.activeThinking
                onToggled: AgentSettings.activeThinking = !AgentSettings.activeThinking
                toolTipText: qsTr("Thinking Mode: asks the model to reason step-by-step before answering.\nMirrors the \"Thinking Mode\" switch in Chat Settings.\nWorks with models that support extended reasoning, for example DeepSeek-R1.")
            }

            FeaturePill {
                text: qsTr("Tools")
                icon.name: "tools"
                iconOnly: inputBar._compact
                checked: AgentSettings.toolsEnabled
                onToggled: AgentSettings.toolsEnabled = !AgentSettings.toolsEnabled
                toolTipText: qsTr("Tools: if on, the assistant can call registered tools (shell, file ops, web search, MCP tools).\nIf off, tools are hidden from the model entirely for this conversation.\nView or manage tools in the Tools page.")
            }

            FeaturePill {
                id: ragPill
                visible: !inputBar._veryCompact
                text: qsTr("RAG")
                icon.name: "database-index"
                iconOnly: inputBar._compact
                checked: AgentSettings.activeRagEnabled
                onToggled: AgentSettings.activeRagEnabled = !AgentSettings.activeRagEnabled
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        ragPill.checked = AgentSettings.activeRagEnabled;
                    }
                }
                toolTipText: qsTr("RAG (Retrieval-Augmented Generation): searches indexed documents and injects\nrelevant passages into the system prompt before each message.\nMirrors the \"Enable RAG\" switch in Chat Settings.")
            }

            AppButton {
                icon.name: "view-refresh"
                flat: true
                iconOnly: true
                text: qsTr("Retry last response")
                visible: !ChatController.isGenerating && ChatController.messageCount > 1 && !inputBar._veryCompact
                onClicked: ChatController.retryLastMessage()
            }

            AppButton {
                id: overflowBtn
                icon.name: "view-more-symbolic"
                flat: true
                iconOnly: true
                text: qsTr("More")
                visible: inputBar._veryCompact
                Controls.ToolTip.text: qsTr("More actions")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
                onClicked: overflowMenu.popup()

                Controls.Menu {
                    id: overflowMenu

                    Controls.MenuItem {
                        text: qsTr("Start a Task…")
                        icon.name: "view-task"
                        enabled: !ChatController.isGenerating
                        onTriggered: startTaskDialog.open()
                    }

                    Controls.MenuItem {
                        text: AgentSettings.activeRagEnabled ? qsTr("RAG: ON (click to disable)") : qsTr("RAG: OFF (click to enable)")
                        icon.name: "database-index"
                        onTriggered: AgentSettings.activeRagEnabled = !AgentSettings.activeRagEnabled
                    }

                    Controls.MenuItem {
                        text: qsTr("Retry last response")
                        icon.name: "view-refresh"
                        enabled: !ChatController.isGenerating && ChatController.messageCount > 1
                        onTriggered: ChatController.retryLastMessage()
                    }

                    Controls.MenuItem {
                        text: ChatController.compactionTurnsTotal > 0 ? qsTr("Compact now (~%1 turns to refresh)").arg(Math.max(0, ChatController.compactionTurnsTotal - ChatController.compactionTurnsUsed)) : (ChatController.contextFillPercent > 0 ? qsTr("Compact now (%1% context)").arg(ChatController.contextFillPercent) : qsTr("Compact conversation"))
                        icon.name: "edit-clear-history"
                        enabled: !ChatController.isGenerating && ChatController.activeConversationId.length > 0
                        onTriggered: ChatController.compactNow()
                    }
                }
            }

            CompactionGauge {
                veryCompact: inputBar._veryCompact
            }

            AppButton {
                icon.name: ChatController.isGenerating ? "media-playback-stop" : "document-send"
                highlighted: !ChatController.isGenerating
                flat: ChatController.isGenerating
                iconOnly: true
                text: ChatController.isGenerating ? qsTr("Stop") : qsTr("Send")
                onClicked: {
                    if (ChatController.isGenerating) {
                        ChatController.stopGeneration();
                    } else {
                        inputBar.doSend();
                    }
                }
            }
        }
    }

    Component.onCompleted: inputBar._refreshMembers()

    Connections {
        target: ChatController
        function onActiveConversationChanged() {
            inputBar._refreshMembers();
            inputBar._closePicker();
        }
    }

    Connections {
        target: MembershipService
        function onConversationMembersChanged(conversationId) {
            if (conversationId === ChatController.activeConversationId)
                inputBar._refreshMembers();
        }
    }

    Controls.Popup {
        id: pickerPopup
        parent: inputScroll
        x: 0
        y: -height - Kirigami.Units.smallSpacing
        width: Math.min(Math.max(inputScroll.width, Kirigami.Units.gridUnit * 16), applicationWindow().width - Kirigami.Units.largeSpacing * 2)
        height: Math.min(Math.max(1, pickerListView.count) * Math.round(Kirigami.Units.gridUnit * 1.9) + padding * 2, Math.round(Kirigami.Units.gridUnit * 1.9) * 6 + padding * 2)
        padding: Kirigami.Units.smallSpacing
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape
        focus: false

        background: Rectangle {
            color: ThemeController.surfaceCard
            radius: ThemeController.radius
            border.width: 1
            border.color: ThemeController.borderSubtle
        }

        contentItem: ListView {
            id: pickerListView
            clip: true
            keyNavigationEnabled: false
            model: inputBar._pickerModel

            onCountChanged: {
                if (count === 0) {
                    inputBar._closePicker();
                } else if (currentIndex < 0 || currentIndex >= count) {
                    currentIndex = 0;
                }
            }

            Controls.ScrollBar.vertical: Controls.ScrollBar {
            }

            delegate: Controls.ItemDelegate {
                id: pickerRow
                required property int index
                required property var modelData
                width: pickerListView.width
                implicitHeight: Math.round(Kirigami.Units.gridUnit * 1.9)
                highlighted: pickerListView.currentIndex === pickerRow.index
                onClicked: {
                    pickerListView.currentIndex = pickerRow.index;
                    inputBar._acceptPicker();
                }

                contentItem: RowLayout {
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        visible: pickerRow.modelData.iconName.length > 0
                        source: pickerRow.modelData.iconName
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }

                    Controls.Label {
                        text: pickerRow.modelData.title
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        font.bold: true
                        color: Kirigami.Theme.textColor
                        verticalAlignment: Text.AlignVCenter
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        text: pickerRow.modelData.subtitle
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }

    function doSend() {
        var msg = inputField.text.trim();
        if (msg.length === 0)
            return;
        inputBar.sendMessage(msg);
        inputField.text = "";
        inputField.forceActiveFocus();
    }

    Kirigami.PromptDialog {
        id: startTaskDialog
        title: qsTr("Start a Task")
        subtitle: qsTr("Describe what you want the agent to do. It will work on this using tools as needed (file writes, web search, etc.), for as many turns as it takes. Progress is tracked in the Plans panel.")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 100
            clip: true
            Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
            Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded

            AppTextArea {
                id: goalInput
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("e.g. Draft a playful launch announcement with a CTA, targeting developers, under 150 words, then save it as launch.md")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
        }

        onAccepted: {
            var g = goalInput.text.trim();
            if (g.length > 0) {
                Tasks.userInitiatedStartTask(ChatController.activeConversationId, g);
                goalInput.text = "";
            }
        }
        onRejected: goalInput.text = ""
    }

    Kirigami.PromptDialog {
        id: generateImageDialog
        title: qsTr("Generate Image")
        subtitle: qsTr("Describe the image you want. The result appears as an assistant message in this conversation a moment after you click OK. Your active image provider (Settings → Image Generation) creates it.")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        ColumnLayout {
            spacing: 6
            Layout.fillWidth: true

            Controls.ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 110
                clip: true
                Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
                Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded
                AppTextArea {
                    id: imagePromptInput
                    wrapMode: TextEdit.Wrap
                    placeholderText: qsTr("e.g. A minimalist line-art logo of a stylised owl reading a book, vector style on a cream background")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Controls.Label {
                    text: qsTr("Size")
                    font.bold: true
                }
                Controls.ComboBox {
                    id: imageSizeCombo
                    model: ["1024x1024", "1024x1792", "1792x1024"]
                    currentIndex: 0
                    Layout.preferredWidth: 140
                }

                Item {
                    Layout.fillWidth: true
                }

                Controls.Label {
                    text: qsTr("Quality")
                    font.bold: true
                }
                Controls.ComboBox {
                    id: imageQualityCombo
                    model: ["standard", "hd"]
                    currentIndex: 0
                    Layout.preferredWidth: 110
                }
            }
        }

        onAccepted: {
            var p = imagePromptInput.text.trim();
            if (p.length === 0)
                return;
            var convId = ChatController.activeConversationId;
            if (convId.length === 0) {
                var newId = Conversations.newConversation();
                if (newId.length > 0)
                    ChatController.switchConversation(newId);
                convId = ChatController.activeConversationId;
            }
            ImageService.generateImageFromMap(convId, p, {
                    "size": imageSizeCombo.currentText,
                    "quality": imageQualityCombo.currentText
                });
            imagePromptInput.text = "";
        }
        onRejected: imagePromptInput.text = ""
    }
}

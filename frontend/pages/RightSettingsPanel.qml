// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.ScrollablePage {
    id: panel

    padding: 0
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0

    background: Rectangle {
        color: ThemeController.surfaceCard
    }

    signal closeRequested

    property bool _loading: false

    property bool _isGroup: false
    property var _groupMembers: []

    property string _folderId: ""
    property bool _isProjectScoped: false
    property string _memberAlias: ""
    property var _memberOverrides: ({})

    property var _heartbeatConfigs: []
    property bool _heartbeatGate: false
    property int _heartbeatGateCap: 1

    header: Rectangle {
        implicitHeight: 48
        color: ThemeController.surfaceCard

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 8
            spacing: 6

            Controls.Label {
                text: qsTr("Chat Settings")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                font.bold: true
                color: Kirigami.Theme.textColor
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            AppButton {
                icon.name: "window-close"
                flat: true
                iconOnly: true
                text: qsTr("Close")
                onClicked: panel.closeRequested()
            }
        }

        Kirigami.Separator {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
        }
    }

    ColumnLayout {
        spacing: 0

        RowLayout {
            id: appSamplingToggle
            visible: AgentSettings.hasAppRecommendedSampling
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            spacing: 8

            Controls.CheckBox {
                id: useRecommendedSampling
                text: qsTr("Use app-recommended sampling")
                checked: AgentSettings.useAppRecommendedSampling
                onToggled: {
                    if (!panel._loading)
                        AgentSettings.useAppRecommendedSampling = checked;
                }
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        useRecommendedSampling.checked = AgentSettings.useAppRecommendedSampling;
                    }
                }
            }

            Item {
                Layout.fillWidth: true
            }
        }

        Controls.Label {
            visible: AgentSettings.hasAppRecommendedSampling && AgentSettings.samplingProfileWarning.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 16 + 24
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            text: AgentSettings.samplingProfileWarning + " " + qsTr("When ON, the values below are overridden " + "at request time.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.smallFont.pointSize
            wrapMode: Text.WordWrap
        }

        SectionHeader {
            title: qsTr("Group Members")
            visible: panel._isGroup
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8
            visible: panel._isGroup

            Controls.Label {
                text: qsTr("Agents in this group. @mention a member in the chat to " + "address them specifically. Without a mention, the ⭐ coordinator answers.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            AppButton {
                text: qsTr("Add Member")
                icon.name: "list-add"
                flat: true
                Layout.fillWidth: true
                onClicked: {
                    addMemberDialog.openFor(ChatController.activeConversationId, ChatController.activeConversationFolderId());
                }
            }

            AppButton {
                text: qsTr("Project Roster…")
                icon.name: "configure"
                flat: true
                visible: panel._isProjectScoped
                Layout.fillWidth: true
                onClicked: rightPanelFolderSettings.openFor(panel._folderId)
            }

            Repeater {
                id: groupMembersRepeater
                model: panel._groupMembers

                Rectangle {
                    id: memberRect
                    Layout.fillWidth: true
                    implicitHeight: memberRow.implicitHeight + 12
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: modelData.isCoordinator ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
                    border.width: modelData.isCoordinator ? 2 : 1

                    required property var modelData

                    readonly property var memberOverride: {
                        var o = panel._memberOverrides[modelData.alias];
                        if (o)
                            return o;
                        return {
                            "modelProvider": "",
                            "modelName": "",
                            "allowedTools": []
                        };
                    }
                    readonly property bool hasOverride: {
                        var o = memberRect.memberOverride;
                        if ((o.modelProvider || "").length > 0)
                            return true;
                        if ((o.modelName || "").length > 0)
                            return true;
                        if (o.allowedTools && o.allowedTools.length > 0)
                            return true;
                        return false;
                    }
                    readonly property string overrideSummary: {
                        var o = memberRect.memberOverride;
                        var p = o.modelProvider || "";
                        var mdl = o.modelName || "";
                        var tools = o.allowedTools || [];
                        var parts = [];
                        if (p.length > 0) {
                            parts.push(mdl.length > 0 ? (p + " / " + mdl) : p);
                        } else if (mdl.length > 0) {
                            parts.push(qsTr("model: %1").arg(mdl));
                        }
                        if (tools.length > 0)
                            parts.push(qsTr("%1 tool(s)").arg(tools.length));
                        if (parts.length === 0)
                            return qsTr("Default model · all tools");
                        return parts.join("  ·  ");
                    }

                    RowLayout {
                        id: memberRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 8
                        spacing: 8

                        Kirigami.Icon {
                            source: modelData.iconName || "face-smile"
                            fallback: "face-laugh"
                            implicitWidth: Kirigami.Units.iconSizes.smallMedium
                            implicitHeight: Kirigami.Units.iconSizes.smallMedium
                            color: Kirigami.Theme.textColor
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Controls.Label {
                                    text: "@" + (modelData.alias || "").replace(/ /g, "_")
                                    font.bold: true
                                    font.family: ThemeController.codeFontFamily
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                    color: Kirigami.Theme.highlightColor
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: implicitWidth
                                }
                                Controls.Label {
                                    visible: modelData.isCoordinator
                                    text: "⭐ " + qsTr("Coordinator")
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                    color: Kirigami.Theme.highlightColor
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: implicitWidth
                                }
                            }
                            Controls.Label {
                                text: (modelData.name || "") + (modelData.description ? " - " + modelData.description : "")
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: Kirigami.Theme.disabledTextColor
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            Controls.Label {
                                text: memberRect.overrideSummary
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                color: memberRect.hasOverride ? Kirigami.Theme.highlightColor : Kirigami.Theme.disabledTextColor
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }

                        Controls.ToolButton {
                            icon.name: "configure"
                            Controls.ToolTip.text: qsTr("Configure model / tools for this member")
                            Controls.ToolTip.visible: hovered
                            onClicked: memberConfigSheet.openForGroupMember(modelData.alias, (modelData.name || modelData.alias || ""), memberRect.memberOverride)
                        }

                        Controls.ToolButton {
                            icon.name: modelData.isCoordinator ? "starred-symbolic" : "non-starred-symbolic"
                            Controls.ToolTip.text: modelData.isCoordinator ? qsTr("This is the coordinator") : qsTr("Mark as coordinator")
                            Controls.ToolTip.visible: hovered
                            enabled: !modelData.isCoordinator
                            onClicked: {
                                MembershipService.setConversationCoordinatorAlias(ChatController.activeConversationId, modelData.alias);
                                panel.loadSettings();
                            }
                        }

                        Controls.ToolButton {
                            icon.name: "edit-delete"
                            enabled: panel._groupMembers.length > 1
                            Controls.ToolTip.text: panel._groupMembers.length > 1 ? qsTr("Remove from this chat") : qsTr("A group chat needs at least one member")
                            Controls.ToolTip.visible: hovered
                            onClicked: removeMemberDialog.openFor(modelData.alias, modelData.isCoordinator === true)
                        }
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: panel._isGroup
        }

        SectionHeader {
            title: qsTr("Primary Agent")
            visible: !panel._isGroup
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8
            visible: !panel._isGroup

            Controls.ComboBox {
                id: primaryAgentCombo
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                textRole: "name"
                valueRole: "id"
                model: {
                    var list = [{
                            "id": "",
                            "name": qsTr("(No agent)"),
                            "description": ""
                        }];
                    var agents = AgentRegistry.agentList();
                    for (var i = 0; i < agents.length; ++i) {
                        list.push(agents[i]);
                    }
                    return list;
                }
                onActivated: {
                    if (!panel._loading) {
                        Tasks.setPrimaryAgent(ChatController.activeConversationId, currentValue);
                    }
                }
            }

            Controls.Label {
                text: qsTr("Assigns a named agent to this conversation. The agent's system " + "prompt and default pattern will be used. You can still override " + "the pattern below. Manage agents in the Agents page.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: !panel._isGroup
        }

        SectionHeader {
            title: qsTr("Member Model Override")
            visible: panel._isProjectScoped && !panel._isGroup && panel._memberAlias.length > 0
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8
            visible: panel._isProjectScoped && !panel._isGroup && panel._memberAlias.length > 0

            readonly property var _ovr: {
                var o = panel._memberOverrides[panel._memberAlias];
                if (o)
                    return o;
                return {
                    "modelProvider": "",
                    "modelName": "",
                    "allowedTools": []
                };
            }
            readonly property bool _hasOverride: {
                var o = _ovr;
                if ((o.modelProvider || "").length > 0)
                    return true;
                if ((o.modelName || "").length > 0)
                    return true;
                if (o.allowedTools && o.allowedTools.length > 0)
                    return true;
                return false;
            }

            Controls.Label {
                text: qsTr("This is a direct chat with the project member @%1. Its provider, " + "model, and tool whitelist override the conversation default and " + "the agent template on every turn. The same override drives this " + "member's turns in the project's group chats.").arg((panel._memberAlias || "").replace(/ /g, "_"))
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Controls.Label {
                text: {
                    var o = parent._ovr;
                    var p = o.modelProvider || "";
                    var mdl = o.modelName || "";
                    var tools = o.allowedTools || [];
                    var parts = [];
                    if (p.length > 0) {
                        parts.push(mdl.length > 0 ? (p + " / " + mdl) : p);
                    } else if (mdl.length > 0) {
                        parts.push(qsTr("model: %1").arg(mdl));
                    }
                    if (tools.length > 0)
                        parts.push(qsTr("%1 tool(s)").arg(tools.length));
                    if (parts.length === 0)
                        return qsTr("Default model · all tools");
                    return parts.join("  ·  ");
                }
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                color: parent._hasOverride ? Kirigami.Theme.highlightColor : Kirigami.Theme.disabledTextColor
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            AppButton {
                text: qsTr("Configure Member Model / Tools…")
                icon.name: "configure"
                flat: true
                Layout.fillWidth: true
                onClicked: memberConfigSheet.openForProjectMember(panel._memberAlias, panel._memberAlias, parent._ovr)
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: panel._isProjectScoped && !panel._isGroup && panel._memberAlias.length > 0
        }

        SectionHeader {
            title: qsTr("Model")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8

            ModelSelector {
                Layout.fillWidth: true
                Layout.preferredWidth: 100
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: !panel._isGroup && VoiceCallService.running
        }

        SectionHeader {
            title: qsTr("Voice")
            visible: !panel._isGroup && VoiceCallService.running
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8
            visible: !panel._isGroup && VoiceCallService.running

            Controls.ComboBox {
                id: oneToOneVoiceCombo
                Layout.fillWidth: true
                model: [qsTr("(Use default voice)")].concat(VoiceCallService.voices)

                function sync() {
                    var assigned = VoiceCallService.assignedVoiceForMember(ChatController.activeConversationId, "");
                    var idx = assigned.length > 0 ? VoiceCallService.voices.indexOf(assigned) + 1 : 0;
                    if (!popup.visible)
                        currentIndex = Math.max(0, idx);
                }
                Component.onCompleted: sync()
                onActivated: VoiceCallService.setVoiceForMember(ChatController.activeConversationId, "", currentIndex <= 0 ? "" : currentText)

                Connections {
                    target: VoiceCallService
                    function onVoiceAssignmentsChanged() {
                        oneToOneVoiceCombo.sync();
                    }
                    function onServiceStateChanged() {
                        oneToOneVoiceCombo.sync();
                    }
                }
                Connections {
                    target: ChatController
                    function onActiveConversationChanged() {
                        oneToOneVoiceCombo.sync();
                    }
                }
            }

            Controls.Label {
                text: qsTr("How this agent sounds on voice calls in this " + "chat. Applied on its next spoken reply.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            title: qsTr("System Prompt")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8

            AppTextArea {
                id: systemPromptField
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("Enter a system prompt…")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                color: Kirigami.Theme.textColor
                placeholderTextColor: Kirigami.Theme.disabledTextColor
                onTextChanged: if (!panel._loading)
                    saveDebounce.restart()
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            title: qsTr("Parameters")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 10

            Controls.Label {
                text: qsTr("Temperature: %1").arg(tempSlider.value.toFixed(1))
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Controls.Slider {
                id: tempSlider
                Layout.fillWidth: true
                from: 0.0
                to: 2.0
                stepSize: 0.1
                onMoved: if (!panel._loading)
                    saveDebounce.restart()
            }

            Controls.Label {
                text: qsTr("Max Tokens")
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Controls.SpinBox {
                id: maxTokensSpin
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                from: -1
                to: 128000
                stepSize: 256
                editable: true
                textFromValue: function (value, locale) {
                    if (value < 0) {
                        return qsTr("Auto (no cap)");
                    }
                    return Number(value).toLocaleString(locale, 'f', 0);
                }
                valueFromText: function (text, locale) {
                    const normalised = text.trim().toLowerCase();
                    if (normalised === "" || normalised === "-1" || normalised.indexOf("auto") >= 0 || normalised.indexOf("no cap") >= 0) {
                        return -1;
                    }
                    const parsed = Number.fromLocaleString(locale, text);
                    return isNaN(parsed) ? -1 : parsed;
                }
                onValueModified: if (!panel._loading)
                    saveDebounce.restart()
            }

            Controls.Label {
                text: qsTr("Context Window (input)")
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Controls.SpinBox {
                id: contextWindowSpin
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                from: 1024
                to: 131072
                stepSize: 1024
                editable: true
                onValueModified: if (!panel._loading)
                    saveDebounce.restart()
            }
            Controls.Label {
                text: qsTr("How much input the model can see per request. Ollama defaults to 2048 unless set explicitly. Raise this for long system prompts, shared docs, or long group chats. Check your model's maximum before going past 8192.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }

            Controls.Label {
                text: qsTr("Advanced Sampling")
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                font.bold: true
                Layout.fillWidth: true
                Layout.topMargin: 8
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Controls.Label {
                    text: qsTr("Top-K")
                    Layout.preferredWidth: 100
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }
                Controls.Slider {
                    id: topKSlider
                    Layout.fillWidth: true
                    from: -1
                    to: 200
                    stepSize: 1
                    enabled: !AgentSettings.useAppRecommendedSampling
                    onMoved: if (!panel._loading)
                        saveDebounce.restart()
                }
                Controls.Label {
                    text: topKSlider.value < 0 ? qsTr("Auto") : Math.round(topKSlider.value).toString()
                    Layout.preferredWidth: 60
                    horizontalAlignment: Text.AlignRight
                    color: Kirigami.Theme.disabledTextColor
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Controls.Label {
                    text: qsTr("Top-P")
                    Layout.preferredWidth: 100
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }
                Controls.Slider {
                    id: topPSlider
                    Layout.fillWidth: true
                    from: -1.0
                    to: 1.0
                    stepSize: 0.05
                    enabled: !AgentSettings.useAppRecommendedSampling
                    onMoved: if (!panel._loading)
                        saveDebounce.restart()
                }
                Controls.Label {
                    text: topPSlider.value < 0 ? qsTr("Auto") : topPSlider.value.toFixed(2)
                    Layout.preferredWidth: 60
                    horizontalAlignment: Text.AlignRight
                    color: Kirigami.Theme.disabledTextColor
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Controls.Label {
                    text: qsTr("Repeat penalty")
                    Layout.preferredWidth: 100
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }
                Controls.Slider {
                    id: repeatPenaltySlider
                    Layout.fillWidth: true
                    from: -1.0
                    to: 2.0
                    stepSize: 0.05
                    enabled: !AgentSettings.useAppRecommendedSampling
                    onMoved: if (!panel._loading)
                        saveDebounce.restart()
                }
                Controls.Label {
                    text: repeatPenaltySlider.value < 0 ? qsTr("Auto") : repeatPenaltySlider.value.toFixed(2)
                    Layout.preferredWidth: 60
                    horizontalAlignment: Text.AlignRight
                    color: Kirigami.Theme.disabledTextColor
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Controls.Label {
                    text: qsTr("Presence penalty")
                    Layout.preferredWidth: 100
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }
                Controls.Slider {
                    id: presencePenaltySlider
                    Layout.fillWidth: true
                    from: -1.0
                    to: 2.0
                    stepSize: 0.05
                    enabled: !AgentSettings.useAppRecommendedSampling
                    onMoved: if (!panel._loading)
                        saveDebounce.restart()
                }
                Controls.Label {
                    text: presencePenaltySlider.value < 0 ? qsTr("Auto") : presencePenaltySlider.value.toFixed(2)
                    Layout.preferredWidth: 60
                    horizontalAlignment: Text.AlignRight
                    color: Kirigami.Theme.disabledTextColor
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Controls.Label {
                    text: qsTr("Frequency penalty")
                    Layout.preferredWidth: 100
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }
                Controls.Slider {
                    id: frequencyPenaltySlider
                    Layout.fillWidth: true
                    from: -1.0
                    to: 2.0
                    stepSize: 0.05
                    enabled: !AgentSettings.useAppRecommendedSampling
                    onMoved: if (!panel._loading)
                        saveDebounce.restart()
                }
                Controls.Label {
                    text: frequencyPenaltySlider.value < 0 ? qsTr("Auto") : frequencyPenaltySlider.value.toFixed(2)
                    Layout.preferredWidth: 60
                    horizontalAlignment: Text.AlignRight
                    color: Kirigami.Theme.disabledTextColor
                }
            }
            Controls.Label {
                text: qsTr("Drag any slider above fully left (\"Auto\") to use the model's own default. When the recommended-sampling toggle above is ON, these values are overridden at request time.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.bottomMargin: 8
            }

            Controls.CheckBox {
                id: streamingSwitch
                text: qsTr("Streaming")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                onCheckedChanged: if (!panel._loading)
                    saveDebounce.restart()
            }

            Controls.CheckBox {
                id: thinkingSwitch
                text: qsTr("Thinking Mode")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                onCheckedChanged: if (!panel._loading)
                    saveDebounce.restart()
            }
            Controls.Label {
                text: qsTr("Asks the model to reason step-by-step before answering. Mirrors the \"Thinking\" pill in the chat input bar. Requires a model that supports extended reasoning.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            title: qsTr("Tools")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8

            Controls.CheckBox {
                id: toolsSwitch
                text: qsTr("Enable Tools")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                checked: AgentSettings.toolsEnabled
                onCheckedChanged: AgentSettings.toolsEnabled = checked
            }
            Controls.Label {
                text: qsTr("Agents can use tools (files, shell, web, MCP). Manage them in the Tools page.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }

            Controls.Label {
                text: qsTr("Tool steps per turn")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Controls.SpinBox {
                id: toolCapSpin
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                from: 0
                to: 500
                stepSize: 5
                editable: true
                value: SettingsService.toolIterationCap
                textFromValue: function (value, locale) {
                    return value <= 0 ? qsTr("Unlimited") : Number(value).toLocaleString(locale, 'f', 0);
                }
                valueFromText: function (text, locale) {
                    const n = text.trim().toLowerCase();
                    if (n === "" || n === "0" || n.indexOf("unlim") >= 0) {
                        return 0;
                    }
                    const p = Number.fromLocaleString(locale, text);
                    return isNaN(p) ? 0 : p;
                }
                onValueModified: SettingsService.toolIterationCap = value
            }
            Controls.Label {
                text: qsTr("Max chained tool calls before a turn pauses (a runaway-loop guard, not a work limit). 0 = unlimited. A separate safeguard still stops a run after repeated tool failures.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }

            Controls.CheckBox {
                id: toolsInPromptSwitch
                text: qsTr("Describe tools in system prompt")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                checked: AgentSettings.toolsInSystemPrompt
                onToggled: if (!panel._loading)
                    AgentSettings.toolsInSystemPrompt = checked
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        toolsInPromptSwitch.checked = AgentSettings.toolsInSystemPrompt;
                    }
                }
            }
            Controls.Label {
                text: qsTr("Adds a written tool list (~3,000 tokens per request). Only needed if a model keeps forgetting its tools.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }
            Controls.CheckBox {
                id: dynamicCompactSwitch
                text: qsTr("Dynamic compaction")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                checked: AgentSettings.dynamicCompactEnabled
                onToggled: if (!panel._loading)
                    AgentSettings.dynamicCompactEnabled = checked
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        dynamicCompactSwitch.checked = AgentSettings.dynamicCompactEnabled;
                    }
                }
            }
            Controls.Label {
                text: qsTr("Summarises older messages in the background so long chats never lose the thread. Your transcript is never altered.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }
            Controls.CheckBox {
                id: implicitTaskCompletionSwitch
                text: qsTr("Auto-complete tasks when the model goes quiet")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                checked: AgentSettings.implicitTaskCompletion
                onToggled: if (!panel._loading)
                    AgentSettings.implicitTaskCompletion = checked
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        implicitTaskCompletionSwitch.checked = AgentSettings.implicitTaskCompletion;
                    }
                }
            }
            Controls.Label {
                text: qsTr("When on, a tracked task is marked done automatically if the model stops right after a tool call. Off (default) keeps the task open until the model or you explicitly complete it, which works better for multi-part projects.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }
            Controls.Label {
                text: qsTr("Refresh summary every … agent replies")
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                Layout.leftMargin: 20
                wrapMode: Text.WordWrap
                enabled: dynamicCompactSwitch.checked
            }
            Controls.SpinBox {
                id: compactTurnsSpin
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                Layout.leftMargin: 20
                enabled: dynamicCompactSwitch.checked
                from: 0
                to: 500
                stepSize: 5
                editable: true
                value: AgentSettings.compactEveryTurns
                onValueModified: if (!panel._loading)
                    AgentSettings.compactEveryTurns = value
                textFromValue: function (value) {
                    return value === 0 ? qsTr("Off") : value.toString();
                }
                valueFromText: function (text) {
                    const normalised = text.trim().toLowerCase();
                    if (normalised === "" || normalised === qsTr("Off").toLowerCase())
                        return 0;
                    const parsed = parseInt(text);
                    return isNaN(parsed) ? 0 : parsed;
                }
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        compactTurnsSpin.value = AgentSettings.compactEveryTurns;
                    }
                }
            }
            Controls.Label {
                text: qsTr("0 = only when the context window fills up.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            title: qsTr("Agent")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 10

            Controls.Label {
                text: qsTr("Pattern")
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Controls.ComboBox {
                id: agentPatternCombo
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                model: ["direct", "react", "planner", "router", "multi_agent", "memory"]
                onActivated: {
                    AgentSettings.setAgentPattern(currentText);
                }
            }

            Controls.CheckBox {
                id: confirmSwitch
                text: qsTr("Require Confirmation")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                onCheckedChanged: {
                    AgentSettings.setRequireConfirmation(checked);
                }
            }

            Controls.Label {
                text: qsTr("Team autonomy: rounds before pausing")
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                Layout.topMargin: 4
                wrapMode: Text.WordWrap
            }
            Controls.SpinBox {
                id: maxAutoRoundsSpin
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                from: 0
                to: 50
                stepSize: 1
                editable: true
                value: AgentSettings.activeMaxAutoRounds
                onValueModified: if (!panel._loading)
                    AgentSettings.activeMaxAutoRounds = value
                textFromValue: function (value) {
                    return value === 0 ? qsTr("Unlimited") : value.toString();
                }
                valueFromText: function (text) {
                    const n = text.trim().toLowerCase();
                    if (n === "" || n === qsTr("Unlimited").toLowerCase() || n.indexOf("unlim") >= 0)
                        return 0;
                    const parsed = parseInt(text);
                    return isNaN(parsed) ? 0 : parsed;
                }
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        maxAutoRoundsSpin.value = AgentSettings.activeMaxAutoRounds;
                    }
                }
            }
            Controls.Label {
                text: qsTr("In group chats, how many autonomous rounds the team " + "runs before pausing for you. Higher = more " + "autonomy; 0 = Unlimited (runs until the team stops " + "making progress). Default 6.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            id: prefSkillsHeader
            title: Conversations.isGroup(ChatController.activeConversationId) ? qsTr("Set Up Preferred Skills for Group Conversation") : qsTr("Set Up Preferred Skills for Conversation")
        }

        ColumnLayout {
            id: prefSkillsBlock
            Layout.fillWidth: true
            spacing: 8

            readonly property string convId: ChatController.activeConversationId
            readonly property string scopeType: Conversations.isGroup(convId) ? "conversation_group" : "conversation_1to1"
            readonly property string _folderId: Conversations.folderIdOf(convId)
            readonly property var _folderPref: _folderId.length > 0 ? Skills.preferredSkillsFor("folder", _folderId) : []
            readonly property bool _hasFolderPref: _folderPref.length > 0

            Controls.CheckBox {
                id: overrideCheck
                visible: prefSkillsBlock._hasFolderPref
                text: qsTr("Override Project / Organization Preferred Skills List")
                checked: Skills.overrideParentFolder(prefSkillsBlock.convId)
                onToggled: Skills.setOverrideParentFolder(prefSkillsBlock.convId, checked)
            }
            Controls.Label {
                visible: overrideCheck.visible && overrideCheck.checked
                text: qsTr("⚠ When enabled, this setting overrides your " + "Project / Organization's preferred skills list.")
                color: Kirigami.Theme.neutralTextColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            }

            Controls.Button {
                text: qsTr("Preferred Skills…")
                icon.name: "applications-education"
                enabled: !prefSkillsBlock._hasFolderPref || overrideCheck.checked
                onClicked: preferredSkillsOverlay.openFor(prefSkillsBlock.scopeType, prefSkillsBlock.convId)
            }

            Controls.CheckBox {
                id: exposeOnlyCheck
                visible: Skills.preferredSkillsFor(prefSkillsBlock.scopeType, prefSkillsBlock.convId).length > 0
                text: qsTr("Use / Expose Only Preferred Skills")
                checked: Skills.exposeOnlyPreferred(prefSkillsBlock.scopeType, prefSkillsBlock.convId)
                onToggled: Skills.setExposeOnlyPreferred(prefSkillsBlock.scopeType, prefSkillsBlock.convId, checked)
            }
            Controls.Label {
                visible: exposeOnlyCheck.visible
                text: qsTr("When on, agents in this conversation are restricted " + "to the preferred list and discover_skills is " + "disabled.")
                color: Kirigami.Theme.disabledTextColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            }
        }

        PreferredSkillsOverlay {
            id: preferredSkillsOverlay
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            title: qsTr("Heartbeat")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 10

            Controls.Label {
                text: qsTr("Heartbeats let members run on a schedule without user prompting. Reports always go to the heartbeat overlay first; chat posting requires the gate below (off by default).")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            AppButton {
                text: qsTr("View Heartbeat Activity")
                icon.name: "view-history"
                flat: true
                Layout.fillWidth: true
                onClicked: heartbeatOverlay.open()
            }

            Controls.CheckBox {
                id: hbGateSwitch
                text: qsTr("Allow auto-posted heartbeat reports")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                checked: panel._heartbeatGate
                onCheckedChanged: {
                    if (panel._loading)
                        return;
                    if (checked === panel._heartbeatGate)
                        return;
                    panel._heartbeatGate = checked;
                    var capToPersist = panel._heartbeatGateCap;
                    if (checked && capToPersist <= 1) {
                        capToPersist = HeartbeatConfig.suggestedAutoSurfaceCap(ChatController.activeConversationId);
                        panel._heartbeatGateCap = capToPersist;
                        hbCapSpin.value = capToPersist;
                    }
                    Conversations.setHeartbeatAutoSurface(ChatController.activeConversationId, checked, capToPersist);
                }
            }

            Rectangle {
                Layout.fillWidth: true
                visible: hbGateSwitch.checked
                color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.12)
                border.color: Kirigami.Theme.neutralTextColor
                border.width: 1
                radius: ThemeController.radius
                implicitHeight: hbGateWarn.implicitHeight + 16
                Controls.Label {
                    id: hbGateWarn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 8
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    text: qsTr("⚠ Autonomous chat posts. Heartbeat-enabled members may post to this conversation up to %1 times/day. Each auto-post decision is one extra LLM call. The setting is saved as soon as you turn it on.").arg(hbCapSpin.value)
                }
            }

            Controls.Label {
                text: qsTr("Daily cap (auto-posts per day)")
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                Layout.topMargin: 4
                visible: hbGateSwitch.checked
            }
            Controls.SpinBox {
                id: hbCapSpin
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                from: 1
                to: 24
                value: panel._heartbeatGateCap
                editable: true
                visible: hbGateSwitch.checked
                onValueModified: {
                    if (panel._loading)
                        return;
                    panel._heartbeatGateCap = value;
                    Conversations.setHeartbeatAutoSurface(ChatController.activeConversationId, hbGateSwitch.checked, value);
                }
            }

            Controls.Label {
                text: qsTr("Members with heartbeat (%1)").arg(panel._heartbeatConfigs.length)
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                font.bold: true
                Layout.topMargin: 8
                Layout.fillWidth: true
            }

            Controls.Label {
                visible: panel._heartbeatConfigs.length === 0
                text: qsTr("No members in this conversation have a heartbeat configured. Open the member list above (group chats) or use Add Member to set one up.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Repeater {
                id: heartbeatConfigsRepeater
                model: panel._heartbeatConfigs

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: hbRow.implicitHeight + 16
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: modelData.enabled ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
                    border.width: 1

                    required property var modelData

                    RowLayout {
                        id: hbRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 8
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Controls.Label {
                                text: modelData.alias.length > 0 ? "@" + modelData.alias.replace(/ /g, "_") : qsTr("(direct chat)")
                                font.bold: true
                                font.family: ThemeController.codeFontFamily
                                color: Kirigami.Theme.highlightColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            }
                            Controls.Label {
                                text: modelData.enabled ? qsTr("Schedule: %1").arg(modelData.schedule.length > 0 ? modelData.schedule : qsTr("(manual runs only)")) : qsTr("Disabled")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            }
                            Controls.Label {
                                visible: modelData.lastFireAt.length > 0
                                text: qsTr("Last run: %1 (%2)").arg(modelData.lastFireAt).arg(modelData.lastFireOutcome.length > 0 ? modelData.lastFireOutcome : qsTr("unknown"))
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Controls.CheckBox {
                                text: qsTr("Allow agent self-config")
                                checked: modelData.selfConfigAllowed === undefined ? false : modelData.selfConfigAllowed
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                                Layout.fillWidth: true
                                onToggled: {
                                    HeartbeatConfig.upsertConfigMap({
                                            "id": modelData.id,
                                            "agentId": modelData.agentId,
                                            "scopeType": modelData.scopeType,
                                            "scopeId": modelData.scopeId,
                                            "alias": modelData.alias,
                                            "enabled": modelData.enabled,
                                            "schedule": modelData.schedule,
                                            "goal": modelData.goal,
                                            "surfaceCriteria": modelData.surfaceCriteria,
                                            "maxRunsPerDay": modelData.maxRunsPerDay,
                                            "autoSurfaceTargetConversationId": modelData.autoSurfaceTargetConversationId,
                                            "selfConfigAllowed": checked
                                        });
                                }
                            }
                        }

                        Controls.ToolButton {
                            icon.name: "media-playback-start"
                            Controls.ToolTip.text: qsTr("Run now")
                            Controls.ToolTip.visible: hovered
                            onClicked: HeartbeatSubagent.runNow(modelData.id)
                        }
                        Controls.ToolButton {
                            icon.name: "edit-delete"
                            Controls.ToolTip.text: qsTr("Remove heartbeat for this member")
                            Controls.ToolTip.visible: hovered
                            onClicked: {
                                HeartbeatConfig.removeConfig(modelData.id);
                                panel.loadSettings();
                            }
                        }
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            title: qsTr("RAG")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8

            Controls.CheckBox {
                id: ragSwitch
                text: qsTr("Enable RAG")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                checked: AgentSettings.activeRagEnabled
                onToggled: if (!panel._loading)
                    AgentSettings.activeRagEnabled = checked
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        ragSwitch.checked = AgentSettings.activeRagEnabled;
                    }
                }
            }
            Controls.Label {
                text: qsTr("Retrieval-Augmented Generation: searches indexed documents and injects relevant passages into the system prompt before each message. Mirrors the \"RAG\" pill in the chat input bar.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }

            Controls.Button {
                text: qsTr("Clear RAG memory")
                icon.name: "edit-clear-all"
                Layout.fillWidth: true
                onClicked: {
                    const n = RagService.clearConversationEmbeddings(ChatController.activeConversationId);
                    ragClearStatus.text = qsTr("Cleared %1 indexed chunk(s).").arg(n);
                }
            }
            Controls.Label {
                id: ragClearStatus
                text: ""
                visible: text.length > 0
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            title: qsTr("Agent memory")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8

            Controls.CheckBox {
                id: aimSwitch
                text: qsTr("Use agent memory")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                checked: AgentSettings.activeAimEnabled
                onToggled: if (!panel._loading)
                    AgentSettings.activeAimEnabled = checked
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        aimSwitch.checked = AgentSettings.activeAimEnabled;
                    }
                }
            }
            Controls.Label {
                text: qsTr("When on, agents recall durable facts they've saved (and, in a plain chat, what was saved here) and have them surfaced automatically before each reply. Saving is controlled per agent by the memory tool in the agent's tool list.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        SectionHeader {
            title: qsTr("Team memory")
            visible: AgentSettings.activeAcnAvailable
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8
            visible: AgentSettings.activeAcnAvailable

            Controls.CheckBox {
                id: acnSwitch
                text: qsTr("Use team memory")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.fillWidth: true
                checked: AgentSettings.activeAcnEnabled
                onToggled: if (!panel._loading)
                    AgentSettings.activeAcnEnabled = checked
                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        acnSwitch.checked = AgentSettings.activeAcnEnabled;
                    }
                }
            }
            Controls.Label {
                text: qsTr("When on, this conversation contributes to and recalls the project/organization's shared team memory (summaries and decisions captured automatically when the chat is compacted). Turn the whole feature on or off for the project in Folder Settings.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: AgentSettings.activeAcnAvailable
        }

        SectionHeader {
            title: qsTr("Activity")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            spacing: 8

            Controls.Label {
                text: qsTr("Chronological audit log of agent turns, tool " + "invocations, polls, member changes, and file / " + "canvas writes for this conversation.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            AppButton {
                text: qsTr("View Activity Log")
                icon.name: "documentation"
                flat: true
                Layout.fillWidth: true
                enabled: ChatController.activeConversationId.length > 0
                onClicked: convActivityTimeline.openForConversation(ChatController.activeConversationId, ChatController.activeConversationTitle)
            }
        }

        Item {
            Layout.preferredHeight: 24
        }
    }

    component SectionHeader: Rectangle {
        property string title: ""
        Layout.fillWidth: true
        implicitHeight: 36
        color: "transparent"

        Controls.Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: title
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            font.capitalization: Font.AllUppercase
            color: Kirigami.Theme.disabledTextColor
            elide: Text.ElideRight
        }
    }

    Timer {
        id: saveDebounce
        interval: 400
        repeat: false
        onTriggered: {
            if (ChatController.activeConversationId.length === 0)
                return;
            AgentSettings.saveConversationConfig({
                    "systemPrompt": systemPromptField.text,
                    "temperature": tempSlider.value,
                    "maxTokens": maxTokensSpin.value,
                    "contextWindow": contextWindowSpin.value,
                    "streaming": streamingSwitch.checked,
                    "thinking": thinkingSwitch.checked
                });
            AgentSettings.activeTopK = topKSlider.value;
            AgentSettings.activeTopP = topPSlider.value;
            AgentSettings.activeRepeatPenalty = repeatPenaltySlider.value;
            AgentSettings.activePresencePenalty = presencePenaltySlider.value;
            AgentSettings.activeFrequencyPenalty = frequencyPenaltySlider.value;
        }
    }

    Connections {
        target: AgentSettings
        function onActiveConversationSettingsChanged() {
            panel.loadSettings();
        }
    }
    Connections {
        target: ChatController
        function onActiveConversationChanged() {
            panel.loadSettings();
        }
    }

    function loadSettings() {
        _loading = true;
        systemPromptField.text = AgentSettings.activeSystemPrompt;
        tempSlider.value = AgentSettings.activeTemperature;
        maxTokensSpin.value = AgentSettings.activeMaxTokens;
        contextWindowSpin.value = AgentSettings.activeContextWindow;
        streamingSwitch.checked = AgentSettings.activeStreaming;
        thinkingSwitch.checked = AgentSettings.activeThinking;
        topKSlider.value = AgentSettings.activeTopK;
        topPSlider.value = AgentSettings.activeTopP;
        repeatPenaltySlider.value = AgentSettings.activeRepeatPenalty;
        presencePenaltySlider.value = AgentSettings.activePresencePenalty;
        frequencyPenaltySlider.value = AgentSettings.activeFrequencyPenalty;
        _isGroup = Conversations.isGroup(ChatController.activeConversationId);
        _groupMembers = Conversations.groupMembers(ChatController.activeConversationId);
        var _convId = ChatController.activeConversationId;
        _folderId = _convId.length > 0 ? Conversations.folderIdOf(_convId) : "";
        var _ftype = "";
        if (_folderId.length > 0) {
            var _finfo = Conversations.folderInfo(_folderId);
            _ftype = (_finfo && _finfo.folderType) ? _finfo.folderType : "";
        }
        _isProjectScoped = (_ftype === "project" || _ftype === "organization");
        _memberAlias = _convId.length > 0 ? Conversations.conversationMemberAlias(_convId) : "";
        var _ovrMap = ({});
        if (_convId.length > 0) {
            var _ovrList = [];
            if (_isProjectScoped && _folderId.length > 0) {
                _ovrList = MembershipService.projectMembersList(_folderId);
            } else if (_isGroup) {
                _ovrList = MembershipService.conversationMembersList(_convId);
            }
            for (var _oi = 0; _oi < _ovrList.length; ++_oi) {
                var _om = _ovrList[_oi];
                _ovrMap[_om.alias] = {
                    "modelProvider": _om.modelProvider || "",
                    "modelName": _om.modelName || "",
                    "allowedTools": _om.allowedTools || []
                };
            }
        }
        _memberOverrides = _ovrMap;
        var list = [{
                "id": "",
                "name": qsTr("(No agent)"),
                "description": ""
            }];
        var agents = AgentRegistry.agentList();
        for (var i = 0; i < agents.length; ++i) {
            list.push(agents[i]);
        }
        primaryAgentCombo.model = list;
        var currentId = Tasks.primaryAgentId(ChatController.activeConversationId);
        var idx = 0;
        for (var j = 0; j < list.length; ++j) {
            if (list[j].id === currentId) {
                idx = j;
                break;
            }
        }
        primaryAgentCombo.currentIndex = idx;
        var convId = ChatController.activeConversationId;
        if (convId.length > 0) {
            _heartbeatConfigs = HeartbeatConfig.configsForConversation(convId);
            _heartbeatGate = Conversations.heartbeatAutoSurface(convId);
            _heartbeatGateCap = Conversations.heartbeatAutoSurfaceMaxPerDay(convId);
        } else {
            _heartbeatConfigs = [];
            _heartbeatGate = false;
            _heartbeatGateCap = 1;
        }
        _loading = false;
    }

    Connections {
        target: AgentRegistry
        function onAgentsChanged() {
            panel.loadSettings();
        }
    }

    Connections {
        target: MembershipService
        function onConversationMembersChanged(convId) {
            if (convId === ChatController.activeConversationId) {
                panel.loadSettings();
            }
        }
        function onProjectMembersChanged(folderId) {
            if (folderId.length > 0 && folderId === panel._folderId) {
                panel.loadSettings();
            }
        }
    }

    Connections {
        target: HeartbeatConfig
        function onConfigChanged(configId) {
            panel.loadSettings();
        }
        function onConfigRemoved(configId) {
            panel.loadSettings();
        }
    }

    AddChatMemberDialog {
        id: addMemberDialog
    }

    FolderSettingsDialog {
        id: rightPanelFolderSettings
    }

    Kirigami.PromptDialog {
        id: removeMemberDialog

        property string memberAlias: ""
        property bool wasCoordinator: false

        function openFor(alias, isCoord) {
            removeMemberDialog.memberAlias = alias;
            removeMemberDialog.wasCoordinator = (isCoord === true);
            removeMemberDialog.open();
        }

        title: qsTr("Remove Member")
        subtitle: {
            var handle = "@" + removeMemberDialog.memberAlias.replace(/ /g, "_");
            var s = qsTr("Remove %1 from this chat? Their previous messages " + "stay in the conversation.").arg(handle);
            if (removeMemberDialog.wasCoordinator === true) {
                s += " " + qsTr("They are the ⭐ coordinator. Messages without " + "an @mention go to the first remaining member " + "until you mark a new coordinator.");
            }
            if (panel._isProjectScoped === true) {
                s += " " + qsTr("They remain in the project's roster " + "(manage that under Project Roster).");
            }
            return s;
        }
        standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.No

        onAccepted: {
            MembershipService.removeConversationMemberAlias(ChatController.activeConversationId, removeMemberDialog.memberAlias);
            panel.loadSettings();
        }
    }

    HeartbeatActivityOverlay {
        id: heartbeatOverlay
    }

    ActivityTimeline {
        id: convActivityTimeline
    }

    AppOverlayDialog {
        id: memberConfigSheet
        parent: applicationWindow().overlay
        implicitWidth: Math.min(applicationWindow().width * 0.8, Kirigami.Units.gridUnit * 30)

        property string targetAlias: ""
        property string targetDisplayName: ""

        property int voiceIndexAtOpen: 0

        function _loadFrom(alias, displayName, ovr) {
            memberConfigSheet.targetAlias = alias;
            memberConfigSheet.targetDisplayName = displayName;
            var o = ovr || {
                "modelProvider": "",
                "modelName": "",
                "allowedTools": []
            };
            mcEditor.load(o.modelProvider || "", o.modelName || "", (o.allowedTools && o.allowedTools.length !== undefined) ? o.allowedTools : []);
            var assigned = VoiceCallService.running ? VoiceCallService.assignedVoiceForMember(ChatController.activeConversationId, alias) : "";
            var idx = assigned.length > 0 ? VoiceCallService.voices.indexOf(assigned) + 1 : 0;
            memberConfigSheet.voiceIndexAtOpen = Math.max(0, idx);
            memberVoiceCombo.currentIndex = memberConfigSheet.voiceIndexAtOpen;
            memberConfigSheet.open();
        }

        function openForGroupMember(alias, displayName, ovr) {
            memberConfigSheet._loadFrom(alias, displayName, ovr);
        }
        function openForProjectMember(alias, displayName, ovr) {
            memberConfigSheet._loadFrom(alias, displayName, ovr);
        }

        title: memberConfigSheet.targetDisplayName.length > 0 ? qsTr("Configure: %1").arg(memberConfigSheet.targetDisplayName) : qsTr("Configure Member")
        dialogIcon: "user-properties"

        footer: RowLayout {
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                text: qsTr("Cancel")
                onClicked: memberConfigSheet.close()
            }
            AppButton {
                text: qsTr("Apply")
                highlighted: true
                enabled: memberConfigSheet.targetAlias.length > 0
                onClicked: {
                    var alias = memberConfigSheet.targetAlias;
                    var prov = mcEditor.providerId;
                    var model = mcEditor.modelName;
                    var tools = mcEditor.toolWhitelist;
                    if (panel._isProjectScoped && panel._folderId.length > 0) {
                        MembershipService.updateProjectMemberModelOverride(panel._folderId, alias, prov, model, tools);
                    } else {
                        MembershipService.updateConversationMemberModelOverride(ChatController.activeConversationId, alias, prov, model, tools);
                    }
                    if (memberVoiceCombo.visible && memberVoiceCombo.currentIndex !== memberConfigSheet.voiceIndexAtOpen) {
                        VoiceCallService.setVoiceForMember(ChatController.activeConversationId, alias, memberVoiceCombo.currentIndex <= 0 ? "" : memberVoiceCombo.currentText);
                    }
                    panel.loadSettings();
                    memberConfigSheet.close();
                }
            }
        }

        ColumnLayout {
            spacing: 12

            Controls.Label {
                text: qsTr("This member's provider, model, and tool whitelist override the " + "conversation default and the agent template on every turn " + "this member takes. Other members in the same chat are unaffected.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            ModelOverrideEditor {
                id: mcEditor
                Layout.fillWidth: true
            }

            ColumnLayout {
                visible: VoiceCallService.running
                Layout.fillWidth: true
                spacing: 4

                Controls.Label {
                    text: qsTr("Voice")
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }
                Controls.ComboBox {
                    id: memberVoiceCombo
                    Layout.fillWidth: true
                    model: [qsTr("(Use default voice)")].concat(VoiceCallService.voices)
                }
                Controls.Label {
                    text: qsTr("How this member sounds on voice calls in " + "this chat. Applied on their next spoken reply.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }
    }

    Component.onCompleted: loadSettings()
}

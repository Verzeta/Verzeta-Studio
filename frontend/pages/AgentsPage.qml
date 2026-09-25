// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.ScrollablePage {
    id: agentsPage

    padding: 0
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0

    background: Rectangle {
        color: ThemeController.surfacePage
    }

    ColumnLayout {
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 32
            Layout.rightMargin: 32
            Layout.topMargin: 32
            Layout.bottomMargin: 32
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Kirigami.Heading {
                    text: qsTr("Agents")
                    level: 2
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                AppButton {
                    text: qsTr("New Agent")
                    icon.name: "list-add"
                    highlighted: true
                    onClicked: {
                        editDialog.openForNew();
                    }
                }
            }

            Controls.Label {
                text: qsTr("Agents are persistent roles you can assign to conversations. " + "Each agent has a name, system prompt, default reasoning pattern, " + "and optional model/tool restrictions. Click an agent to edit it, " + "or create a new one from scratch or a template.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.bottomMargin: 16
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.bottomMargin: 12
            }

            Kirigami.Heading {
                text: qsTr("Templates")
                level: 4
                Layout.bottomMargin: 4
            }

            Controls.Label {
                text: qsTr("Click any template to start a new chat with that agent.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.bottomMargin: 8
            }

            Flow {
                id: templatesFlow
                Layout.fillWidth: true
                spacing: 8

                Repeater {
                    id: templatesRepeater
                    model: AgentRegistry.builtInTemplates()

                    Rectangle {
                        width: 180
                        height: Kirigami.Units.gridUnit * 7
                        radius: ThemeController.radius
                        color: templateMouse.containsMouse ? ThemeController.hoverTint : ThemeController.surfaceCard
                        border.color: templateMouse.containsMouse ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
                        border.width: 1

                        required property var modelData
                        required property int index

                        Behavior on color  {
                            ColorAnimation {
                                duration: 80
                            }
                        }

                        ColumnLayout {
                            id: templateCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 10
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6

                                Kirigami.Icon {
                                    source: modelData.iconName || "face-smile"
                                    fallback: "face-laugh"
                                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                                    implicitHeight: Kirigami.Units.iconSizes.smallMedium
                                    color: Kirigami.Theme.textColor
                                }
                                Controls.Label {
                                    text: modelData.name
                                    font.bold: true
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                    color: Kirigami.Theme.textColor
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                            }

                            Controls.Label {
                                text: modelData.description
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                maximumLineCount: 3
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            id: templateMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                const aid = AgentRegistry.getOrCreateBuiltinByName(modelData.name);
                                if (!aid)
                                    return;
                                const cid = Conversations.newConversationWithAgent(aid, qsTr("Chat with ") + modelData.name);
                                if (cid)
                                    ChatController.switchConversation(cid);
                            }
                        }
                    }
                }
            }

            Item {
                Layout.preferredHeight: 20
            }
            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.bottomMargin: 12
            }

            Kirigami.Heading {
                text: qsTr("Your Agents")
                level: 4
                Layout.bottomMargin: 8
            }

            Controls.Label {
                visible: agentsRepeater.count === 0
                text: qsTr("No agents yet. Create one from a template above or click \"New Agent\".")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.bottomMargin: 16
            }

            Repeater {
                id: agentsRepeater
                model: AgentRegistry.agentList()

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: agentCol.implicitHeight + 20
                    radius: ThemeController.radius
                    color: agentMouseArea.containsMouse ? ThemeController.hoverTint : ThemeController.surfaceCard
                    border.color: ThemeController.borderSubtle
                    border.width: 1
                    Layout.bottomMargin: 8

                    Behavior on color  {
                        ColorAnimation {
                            duration: 80
                        }
                    }

                    required property var modelData
                    required property int index

                    ColumnLayout {
                        id: agentCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 12
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            Kirigami.Icon {
                                source: modelData.iconName || "face-smile"
                                fallback: "face-laugh"
                                implicitWidth: Kirigami.Units.iconSizes.medium
                                implicitHeight: Kirigami.Units.iconSizes.medium
                                Layout.alignment: Qt.AlignVCenter
                                color: Kirigami.Theme.textColor
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    Controls.Label {
                                        text: modelData.name
                                        font.bold: true
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                        color: Kirigami.Theme.textColor
                                        Layout.fillWidth: applicationWindow().isCompact ? true : false
                                        elide: applicationWindow().isCompact ? Text.ElideRight : Text.ElideNone
                                    }

                                    Rectangle {
                                        visible: applicationWindow().isCompact ? false : true
                                        radius: ThemeController.radius
                                        color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.15)
                                        implicitWidth: patternLabel.implicitWidth + 10
                                        implicitHeight: patternLabel.implicitHeight + 4
                                        Controls.Label {
                                            id: patternLabel
                                            anchors.centerIn: parent
                                            text: modelData.defaultPattern || "direct"
                                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                            color: Kirigami.Theme.highlightColor
                                        }
                                    }

                                    Rectangle {
                                        visible: applicationWindow().isCompact ? false : modelData.isBuiltIn
                                        radius: ThemeController.radius
                                        color: Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.15)
                                        implicitWidth: builtinLabel.implicitWidth + 10
                                        implicitHeight: builtinLabel.implicitHeight + 4
                                        Controls.Label {
                                            id: builtinLabel
                                            anchors.centerIn: parent
                                            text: qsTr("Built-in")
                                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                            color: Kirigami.Theme.positiveTextColor
                                        }
                                    }

                                    Item {
                                        Layout.fillWidth: applicationWindow().isCompact ? false : true
                                    }
                                }

                                Controls.Label {
                                    text: modelData.description
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                            }

                            Controls.ToolButton {
                                icon.name: "document-edit"
                                onClicked: editDialog.openForEdit(modelData)
                                Controls.ToolTip.text: qsTr("Edit")
                                Controls.ToolTip.visible: hovered
                            }

                            Controls.ToolButton {
                                icon.name: "edit-delete"
                                onClicked: {
                                    AgentRegistry.removeAgent(modelData.id);
                                    agentsPage.refreshList();
                                }
                                Controls.ToolTip.text: qsTr("Delete")
                                Controls.ToolTip.visible: hovered
                            }
                        }

                        Controls.Label {
                            text: modelData.systemPrompt
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            Layout.leftMargin: 42
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            font.italic: true
                        }
                    }

                    MouseArea {
                        id: agentMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        acceptedButtons: Qt.NoButton
                    }

                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: {
                            const cid = Conversations.newConversationWithAgent(modelData.id, qsTr("Chat with ") + modelData.name);
                            if (cid)
                                ChatController.switchConversation(cid);
                        }
                    }
                }
            }

            Item {
                Layout.preferredHeight: 40
            }
        }
    }

    function refreshList() {
        agentsRepeater.model = AgentRegistry.agentList();
    }

    Connections {
        target: AgentRegistry
        function onAgentsChanged() {
            agentsPage.refreshList();
        }
    }

    Component.onCompleted: refreshList()

    AppOverlayDialog {
        id: editDialog
        parent: applicationWindow().overlay
        implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 42)

        property string editingId: ""

        function openForNew() {
            editingId = "";
            nameField.text = "";
            descField.text = "";
            iconField.text = "face-smile";
            promptField.text = "";
            patternCombo.currentIndex = 0;
            agentModelEditor.load("", "", []);
            hbDefaultGoalField.text = "";
            hbDefaultScheduleField.text = "";
            hbDefaultCriteriaField.text = "";
            hbDefaultsExpander.expanded = false;
            editDialog.open();
        }

        function openForEdit(agent) {
            editingId = agent.id;
            nameField.text = agent.name;
            descField.text = agent.description || "";
            iconField.text = agent.iconName || "face-smile";
            promptField.text = agent.systemPrompt;
            var patterns = ["direct", "react", "planner", "router", "multi_agent", "memory"];
            patternCombo.currentIndex = Math.max(0, patterns.indexOf(agent.defaultPattern));
            agentModelEditor.load(agent.modelProvider || "", agent.modelName || "", agent.allowedTools ? agent.allowedTools.slice() : []);
            hbDefaultGoalField.text = agent.defaultHeartbeatGoal || "";
            hbDefaultScheduleField.text = agent.defaultHeartbeatSchedule || "";
            hbDefaultCriteriaField.text = agent.defaultHeartbeatSurfaceCriteria || "";
            hbDefaultsExpander.expanded = hbDefaultGoalField.text.length > 0 || hbDefaultScheduleField.text.length > 0 || hbDefaultCriteriaField.text.length > 0;
            editDialog.open();
        }

        title: editDialog.editingId.length > 0 ? qsTr("Edit Agent") : qsTr("New Agent")
        dialogIcon: "system-users"

        footer: RowLayout {
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                text: qsTr("Cancel")
                onClicked: editDialog.close()
            }
            AppButton {
                text: editDialog.editingId.length > 0 ? qsTr("Save") : qsTr("Create")
                highlighted: true
                enabled: nameField.text.trim().length > 0 && promptField.text.trim().length > 0
                onClicked: {
                    var map = {
                        "name": nameField.text.trim(),
                        "description": descField.text.trim(),
                        "iconName": iconField.text.trim() || "face-smile",
                        "systemPrompt": promptField.text,
                        "defaultPattern": patternCombo.currentText,
                        "modelProvider": agentModelEditor.providerId,
                        "modelName": agentModelEditor.modelName,
                        "allowedTools": agentModelEditor.toolWhitelist,
                        "defaultHeartbeatGoal": hbDefaultGoalField.text.trim(),
                        "defaultHeartbeatSchedule": hbDefaultScheduleField.text.trim(),
                        "defaultHeartbeatSurfaceCriteria": hbDefaultCriteriaField.text.trim()
                    };
                    if (editDialog.editingId.length > 0) {
                        map.id = editDialog.editingId;
                    }
                    var newId = AgentRegistry.saveAgent(map);
                    if (newId.length > 0) {
                        editDialog.close();
                        agentsPage.refreshList();
                    }
                }
            }
        }

        ColumnLayout {
            spacing: 12

            Controls.Label {
                text: qsTr("Name (Required)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.topMargin: 4
            }
            AppTextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("My Specialized Agent")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                text: qsTr("Description (one-line)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            AppTextField {
                id: descField
                Layout.fillWidth: true
                placeholderText: qsTr("Short description of what this agent does")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                text: qsTr("Icon Name (KDE icon, e.g. face-smile)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            AppTextField {
                id: iconField
                Layout.fillWidth: true
                placeholderText: "face-smile"
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                text: "face-smile"
            }

            Controls.Label {
                text: qsTr("System Prompt (Required)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            Controls.Label {
                text: qsTr("The role definition sent to the LLM. Describe who the agent is, " + "what they do, how they should behave, and what they should avoid.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            AppTextArea {
                id: promptField
                Layout.fillWidth: true
                Layout.preferredHeight: 180
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("You are a...")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                text: qsTr("Default Reasoning Pattern")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            Controls.Label {
                text: qsTr("Execution strategy used when this agent is active. You can still " + "override it per conversation in Chat Settings.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Controls.ComboBox {
                id: patternCombo
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                model: ["direct", "react", "planner", "router", "multi_agent", "memory"]
            }

            Controls.Label {
                text: qsTr("Model Override (optional)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.topMargin: 4
            }
            Controls.Label {
                text: qsTr("When set, this agent uses its own provider/model on every " + "turn, including inside a group chat where other agents use " + "different models. Leave the provider on \"(Use conversation " + "default)\" to inherit the conversation's model.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            ModelOverrideEditor {
                id: agentModelEditor
                Layout.fillWidth: true
            }

            Item {
                id: hbDefaultsExpander
                property bool expanded: false
                Layout.fillWidth: true
                Layout.topMargin: 8
                implicitHeight: hbDefaultsCol.implicitHeight

                ColumnLayout {
                    id: hbDefaultsCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 8

                    AppButton {
                        text: hbDefaultsExpander.expanded ? qsTr("▾ Default heartbeat suggestion (optional)") : qsTr("▸ Default heartbeat suggestion (optional)")
                        flat: true
                        Layout.fillWidth: true
                        onClicked: hbDefaultsExpander.expanded = !hbDefaultsExpander.expanded
                    }

                    ColumnLayout {
                        visible: hbDefaultsExpander.expanded
                        Layout.fillWidth: true
                        spacing: 8

                        Controls.Label {
                            text: qsTr("These pre-fill the heartbeat settings when this agent is added to a new chat or project. They are suggestions only: each member's heartbeat is configured and stored separately. Leave a field empty to skip it.")
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: qsTr("Default Goal")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                        }
                        AppTextField {
                            id: hbDefaultGoalField
                            Layout.fillWidth: true
                            placeholderText: qsTr("e.g. Watch competitor blogs for new product launches")
                        }

                        Controls.Label {
                            text: qsTr("Default Schedule")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                        }
                        AppTextField {
                            id: hbDefaultScheduleField
                            Layout.fillWidth: true
                            placeholderText: "@daily at 09:00"
                        }
                        Controls.Label {
                            text: qsTr("Format: @hourly | @daily at HH:MM | @weekly[ on DAY at HH:MM] | @interval N (≥ 5).")
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: qsTr("Default Surface Criteria")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                        }
                        AppTextField {
                            id: hbDefaultCriteriaField
                            Layout.fillWidth: true
                            placeholderText: qsTr("(optional; the goal is used if empty)")
                        }
                    }
                }
            }
        }
    }
}

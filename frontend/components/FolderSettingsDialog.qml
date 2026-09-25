// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: dialog
    parent: applicationWindow().overlay
    implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 42)

    closePolicy: Controls.Popup.CloseOnEscape

    property string folderId: ""
    property string folderName: ""

    property var projectDocs: []

    property var heartbeatConfigs: []
    property var folderConversations: []

    function reloadProjectDocs() {
        dialog.projectDocs = Conversations.projectDocuments(dialog.folderId);
    }

    function reloadHeartbeats() {
        if (dialog.folderId.length === 0) {
            dialog.heartbeatConfigs = [];
            dialog.folderConversations = [];
            return;
        }
        dialog.heartbeatConfigs = HeartbeatConfig.configsForFolder(dialog.folderId);
        dialog.folderConversations = Conversations.conversationsInFolder(dialog.folderId);
    }

    function openFor(fid) {
        var info = Conversations.folderInfo(fid);
        if (!info || !info.id)
            return;
        dialog.folderId = info.id;
        dialog.folderName = info.name || "";
        var type = info.folderType || "regular";
        var typeIdx = ["regular", "project", "organization"].indexOf(type);
        typeCombo.currentIndex = Math.max(0, typeIdx);
        goalField.text = info.goal || "";
        descField.text = info.description || "";
        acnMasterSwitch.checked = (info.acnEnabled !== false);
        editor.initialMembers = MembershipService.projectMembersList(fid);
        editor.reload();
        dialog.reloadProjectDocs();
        dialog.reloadHeartbeats();
        dialog.open();
    }

    function validateSchedule(s) {
        var trimmed = (s || "").trim();
        if (trimmed.length === 0)
            return "";
        if (trimmed.toLowerCase() === "@hourly")
            return "";
        if (trimmed.toLowerCase() === "@weekly")
            return "";
        if (/^@daily\s+at\s+([0-2][0-9]):([0-5][0-9])$/i.test(trimmed)) {
            var m = trimmed.match(/^@daily\s+at\s+([0-2][0-9]):([0-5][0-9])$/i);
            if (parseInt(m[1]) >= 24)
                return qsTr("Hour must be 00 to 23");
            return "";
        }
        if (/^@weekly\s+on\s+(MON|TUE|WED|THU|FRI|SAT|SUN)(\s+at\s+([0-2][0-9]):([0-5][0-9]))?$/i.test(trimmed))
            return "";
        var iv = trimmed.match(/^@interval\s+(\d+)$/i);
        if (iv) {
            var n = parseInt(iv[1]);
            if (n < 5)
                return qsTr("Interval must be at least 5 minutes");
            return "";
        }
        return qsTr("Use @hourly, @daily at HH:MM, @weekly[ on DAY at HH:MM], or @interval N (N ≥ 5)");
    }

    Connections {
        target: HeartbeatConfig
        function onConfigChanged(configId) {
            dialog.reloadHeartbeats();
        }
        function onConfigRemoved(configId) {
            dialog.reloadHeartbeats();
        }
    }

    title: qsTr("Folder Settings: %1").arg(dialog.folderName)
    dialogIcon: "settings-configure"

    footer: RowLayout {
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Cancel")
            onClicked: dialog.close()
        }
        AppButton {
            text: qsTr("Save")
            highlighted: true
            onClicked: {
                Conversations.updateFolderMetadata(dialog.folderId, typeCombo.currentValue, goalField.text, descField.text, []);
                if (typeCombo.currentValue !== "regular") {
                    Conversations.setFolderAcnEnabled(dialog.folderId, acnMasterSwitch.checked);
                }
                if (typeCombo.currentValue !== "regular") {
                    MembershipService.setProjectMembersFromList(dialog.folderId, editor.memberList());
                } else {
                    MembershipService.setProjectMembersFromList(dialog.folderId, []);
                }
                dialog.close();
                if (typeCombo.currentValue !== "regular" && editor.memberList().length > 0) {
                    kickoffDialog.folderId = dialog.folderId;
                    kickoffDialog.folderName = dialog.folderName;
                    kickoffDialog.memberCount = editor.memberList().length;
                    kickoffDialog.open();
                }
            }
        }
    }

    ColumnLayout {
        spacing: 14

        Controls.Label {
            text: qsTr("Folder Type")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.topMargin: 4
        }
        Controls.ComboBox {
            id: typeCombo
            Layout.fillWidth: true
            Layout.preferredWidth: 100
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            textRole: "text"
            valueRole: "value"
            model: [{
                    "text": qsTr("Regular Folder: an organizer only"),
                    "value": "regular"
                }, {
                    "text": qsTr("Project: team of agents with a shared goal"),
                    "value": "project"
                }, {
                    "text": qsTr("Organization: higher-level context for projects"),
                    "value": "organization"
                }]
        }
        Controls.Label {
            text: qsTr("Regular folders are plain containers. Projects and Organizations " + "inject their goal, description, and team into the system prompt of " + "any conversation inside them.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Controls.Label {
            text: qsTr("Goal")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            visible: typeCombo.currentValue !== "regular"
        }
        AppTextField {
            id: goalField
            Layout.fillWidth: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            placeholderText: qsTr("e.g. Launch Q2 marketing campaign by May 15")
            visible: typeCombo.currentValue !== "regular"
        }

        Controls.Label {
            text: qsTr("Description")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            visible: typeCombo.currentValue !== "regular"
        }
        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            visible: typeCombo.currentValue !== "regular"
            clip: true
            Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded
            Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff

            background: Rectangle {
                color: ThemeController.surfaceSunken
                radius: ThemeController.radius
                border.color: descField.activeFocus ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
                border.width: 1
            }

            Controls.TextArea {
                id: descField
                wrapMode: TextEdit.Wrap
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                placeholderText: qsTr("Longer description of the project scope, constraints, " + "deadlines, or anything else the agents should know.")
                background: null
                selectByMouse: true
                persistentSelection: true
            }
        }

        Controls.CheckBox {
            id: acnMasterSwitch
            text: qsTr("Team memory")
            checked: true
            visible: typeCombo.currentValue !== "regular"
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.fillWidth: true
        }
        Controls.Label {
            text: qsTr("Capture this project's conversation summaries and decisions into a shared team memory that any chat in the project can recall. Individual chats can opt out in their own settings.")
            visible: typeCombo.currentValue !== "regular"
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.leftMargin: 20
        }

        Controls.Label {
            text: qsTr("Team Members")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            visible: typeCombo.currentValue !== "regular"
        }
        Controls.Label {
            text: qsTr("Add agents to this project with unique aliases. The same template " + "(e.g. \"Engineer\") can be added multiple times with different aliases. " + "Mark one as coordinator to make them the default responder in group chats.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            visible: typeCombo.currentValue !== "regular"
        }

        MembershipEditor {
            id: editor
            Layout.fillWidth: true
            visible: typeCombo.currentValue !== "regular"
        }

        Controls.Label {
            text: qsTr("Shared Project Documents")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            visible: typeCombo.currentValue !== "regular"
            Layout.topMargin: 6
        }
        Controls.Label {
            text: qsTr("Upload reference documents (specs, briefs, research) that every " + "agent on this project can read. Files live with the project, not " + "inside a single chat, so they persist across conversations.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            visible: typeCombo.currentValue !== "regular"
        }

        RowLayout {
            Layout.fillWidth: true
            visible: typeCombo.currentValue !== "regular"
            spacing: 8
            AppButton {
                text: qsTr("Upload Document…")
                icon.name: "document-open"
                onClicked: docFileDialog.open()
            }
            Controls.Label {
                text: qsTr("%1 file(s)").arg(dialog.projectDocs.length)
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.alignment: Qt.AlignVCenter
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(160, Math.max(40, dialog.projectDocs.length * 34 + 8))
            visible: typeCombo.currentValue !== "regular" && dialog.projectDocs.length > 0
            color: ThemeController.surfaceCard
            radius: ThemeController.radius
            border.color: ThemeController.borderSubtle
            border.width: 1

            ListView {
                id: docList
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: dialog.projectDocs
                spacing: 2

                delegate: Rectangle {
                    width: docList.width
                    height: 30
                    color: docMouse.containsMouse ? ThemeController.hoverTint : "transparent"
                    radius: ThemeController.radius

                    MouseArea {
                        id: docMouse
                        anchors.fill: parent
                        hoverEnabled: true
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 4
                        spacing: 8

                        Kirigami.Icon {
                            source: "text-x-generic"
                            fallback: "text-plain"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            text: modelData.name
                            elide: Text.ElideMiddle
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        }
                        Controls.Label {
                            text: {
                                var s = modelData.size || 0;
                                if (s < 1024)
                                    return s + " B";
                                if (s < 1024 * 1024)
                                    return (s / 1024).toFixed(1) + " KB";
                                return (s / 1024 / 1024).toFixed(1) + " MB";
                            }
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        }
                        AppButton {
                            icon.name: "edit-delete"
                            flat: true
                            iconOnly: true
                            Controls.ToolTip.text: qsTr("Remove")
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: 500
                            onClicked: {
                                Conversations.removeProjectDocument(dialog.folderId, modelData.name);
                                dialog.reloadProjectDocs();
                            }
                        }
                    }
                }
            }
        }

        Controls.Label {
            text: qsTr("Set Up Preferred Skills for Project / Organization")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            visible: typeCombo.currentValue !== "regular"
            Layout.topMargin: 12
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            visible: typeCombo.currentValue !== "regular"

            Controls.Button {
                text: qsTr("Preferred Skills…")
                icon.name: "applications-education"
                onClicked: folderPreferredSkillsOverlay.openFor("folder", dialog.folderId)
            }
            Controls.CheckBox {
                visible: Skills.preferredSkillsFor("folder", dialog.folderId).length > 0
                text: qsTr("Use / Expose Only Preferred Skills")
                checked: Skills.exposeOnlyPreferred("folder", dialog.folderId)
                onToggled: Skills.setExposeOnlyPreferred("folder", dialog.folderId, checked)
            }
        }
        PreferredSkillsOverlay {
            id: folderPreferredSkillsOverlay
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            Layout.topMargin: 8
        }

        Controls.Label {
            text: qsTr("Project Activity")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            visible: typeCombo.currentValue !== "regular"
            Layout.topMargin: 12
        }
        Controls.Label {
            text: qsTr("Chronological audit log of agent turns, tool invocations, " + "polls, member changes, and file / canvas writes scoped " + "to this project. Useful for retracing what happened " + "and when.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            visible: typeCombo.currentValue !== "regular"
        }
        RowLayout {
            Layout.fillWidth: true
            visible: typeCombo.currentValue !== "regular"
            spacing: 8

            AppButton {
                text: qsTr("View Activity Log")
                icon.name: "documentation"
                flat: true
                onClicked: folderActivityTimeline.openForProject(dialog.folderId, dialog.folderName)
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            Layout.topMargin: 8
            visible: typeCombo.currentValue !== "regular"
        }

        Controls.Label {
            text: qsTr("Project Heartbeats")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            visible: typeCombo.currentValue !== "regular"
            Layout.topMargin: 12
        }
        Controls.Label {
            text: qsTr("Heartbeats run on a schedule for this project. Each member can have its own goal + schedule, posting into a designated chat (or staying overlay-only). Reports go to the heartbeat overlay first; chat posting requires the per-conversation auto-surface gate on the target chat.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            visible: typeCombo.currentValue !== "regular"
        }

        RowLayout {
            Layout.fillWidth: true
            visible: typeCombo.currentValue !== "regular"
            spacing: 8

            AppButton {
                text: qsTr("Add Heartbeat for Member…")
                icon.name: "list-add"
                flat: true
                enabled: !addHbExpander.expanded
                onClicked: {
                    addHbExpander.expanded = true;
                    addHbAgentCombo.model = MembershipService.projectMembersList(dialog.folderId);
                    if (addHbAgentCombo.model.length > 0) {
                        addHbAgentCombo.currentIndex = 0;
                    }
                    addHbScheduleField.text = "";
                    addHbGoalField.text = "";
                    addHbCriteriaField.text = "";
                    addHbMaxRunsSpin.value = 24;
                    addHbTargetCombo.model = [{
                            "id": "",
                            "title": qsTr("(overlay-only: no chat post)")
                        }].concat(dialog.folderConversations);
                    addHbTargetCombo.currentIndex = 0;
                }
            }
            AppButton {
                text: qsTr("View Activity")
                icon.name: "view-history"
                flat: true
                onClicked: folderHeartbeatOverlay.openForFolder(dialog.folderId)
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Item {
            id: addHbExpander
            property bool expanded: false
            Layout.fillWidth: true
            visible: expanded && typeCombo.currentValue !== "regular"
            implicitHeight: visible ? addHbCol.implicitHeight + 16 : 0

            Rectangle {
                anchors.fill: parent
                anchors.margins: 4
                color: ThemeController.surfaceCard
                radius: ThemeController.radius
                border.color: ThemeController.borderSubtle
                border.width: 1

                ColumnLayout {
                    id: addHbCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 8

                    Controls.Label {
                        text: qsTr("Configure heartbeat for a project member")
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    }

                    Controls.Label {
                        text: qsTr("Member")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                    }
                    Controls.ComboBox {
                        id: addHbAgentCombo
                        Layout.fillWidth: true
                        textRole: "alias"
                    }

                    Controls.Label {
                        text: qsTr("Schedule")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                    }
                    AppTextField {
                        id: addHbScheduleField
                        Layout.fillWidth: true
                        placeholderText: "@daily at 09:00"
                    }
                    Controls.Label {
                        text: dialog.validateSchedule(addHbScheduleField.text) === "" ? qsTr("Formats: @hourly | @daily at HH:MM | @weekly[ on DAY at HH:MM] | @interval N (≥ 5).") : qsTr("⚠ %1").arg(dialog.validateSchedule(addHbScheduleField.text))
                        color: dialog.validateSchedule(addHbScheduleField.text) === "" ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.negativeTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        text: qsTr("Goal")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                    }
                    AppTextArea {
                        id: addHbGoalField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 60
                        wrapMode: TextEdit.Wrap
                        placeholderText: qsTr("e.g. Watch competitor blogs for new product launches")
                    }

                    Controls.Label {
                        text: qsTr("Surface criteria (optional, falls back to goal)")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                    }
                    AppTextArea {
                        id: addHbCriteriaField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 50
                        wrapMode: TextEdit.Wrap
                        placeholderText: qsTr("(optional)")
                    }

                    Controls.Label {
                        text: qsTr("Post target: which conversation auto-surfaced reports go to")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                    }
                    Controls.ComboBox {
                        id: addHbTargetCombo
                        Layout.fillWidth: true
                        textRole: "title"
                        valueRole: "id"
                    }
                    Controls.Label {
                        text: qsTr("Empty target = overlay-only. Posting also requires the target chat's auto-surface gate to be ON.")
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Controls.Label {
                            text: qsTr("Max runs/day")
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                        }
                        Controls.SpinBox {
                            id: addHbMaxRunsSpin
                            from: 1
                            to: 1440
                            value: 24
                            editable: true
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }

                    Controls.Switch {
                        id: addHbSelfConfigSwitch
                        text: qsTr("Allow agent to manage its own heartbeat config")
                        checked: false
                    }
                    Controls.Label {
                        visible: addHbSelfConfigSwitch.checked
                        text: qsTr("⚠ The agent can change its own goal, schedule (within cap), surface criteria, and enable/disable itself. The schedule format and the max-runs/day cap are still enforced. Every change is audited.")
                        color: Kirigami.Theme.neutralTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.12)
                        border.color: Kirigami.Theme.neutralTextColor
                        border.width: 1
                        radius: ThemeController.radius
                        implicitHeight: addHbWarn.implicitHeight + 14
                        Controls.Label {
                            id: addHbWarn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 8
                            wrapMode: Text.WordWrap
                            color: Kirigami.Theme.textColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            text: qsTr("⚠ Autonomous activity at project scope. This member will run on its schedule (%1) without user prompting, capped at %2/day. Reports always land in the project heartbeat overlay; auto-posting also requires the target chat's auto-surface gate.").arg(addHbScheduleField.text.trim().length > 0 ? addHbScheduleField.text.trim() : qsTr("(no schedule yet)")).arg(addHbMaxRunsSpin.value)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Item {
                            Layout.fillWidth: true
                        }
                        AppButton {
                            text: qsTr("Cancel")
                            flat: true
                            onClicked: addHbExpander.expanded = false
                        }
                        AppButton {
                            text: qsTr("Add Heartbeat")
                            highlighted: true
                            enabled: addHbAgentCombo.currentIndex >= 0 && dialog.validateSchedule(addHbScheduleField.text) === "" && addHbGoalField.text.trim().length > 0
                            onClicked: {
                                var member = addHbAgentCombo.model[addHbAgentCombo.currentIndex];
                                if (!member)
                                    return;
                                HeartbeatConfig.upsertConfigMap({
                                        "agentId": member.agentId,
                                        "scopeType": "folder",
                                        "scopeId": dialog.folderId,
                                        "alias": member.alias,
                                        "enabled": true,
                                        "schedule": addHbScheduleField.text.trim(),
                                        "goal": addHbGoalField.text.trim(),
                                        "surfaceCriteria": addHbCriteriaField.text.trim(),
                                        "maxRunsPerDay": addHbMaxRunsSpin.value,
                                        "autoSurfaceTargetConversationId": addHbTargetCombo.currentValue || "",
                                        "selfConfigAllowed": addHbSelfConfigSwitch.checked
                                    });
                                addHbExpander.expanded = false;
                            }
                        }
                    }
                }
            }
        }

        Repeater {
            model: dialog.heartbeatConfigs

            Rectangle {
                Layout.fillWidth: true
                visible: typeCombo.currentValue !== "regular"
                implicitHeight: hbCfgRow.implicitHeight + 14
                radius: ThemeController.radius
                color: ThemeController.surfaceCard
                border.color: modelData.enabled ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
                border.width: 1

                required property var modelData

                RowLayout {
                    id: hbCfgRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 8
                    spacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Controls.Label {
                            text: "@" + (modelData.alias || "").replace(/ /g, "_")
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
                            text: {
                                var t = modelData.autoSurfaceTargetConversationId || "";
                                if (t.length === 0)
                                    return qsTr("Target: overlay-only");
                                for (var i = 0; i < dialog.folderConversations.length; ++i) {
                                    if (dialog.folderConversations[i].id === t) {
                                        return qsTr("Target: %1").arg(dialog.folderConversations[i].title);
                                    }
                                }
                                return qsTr("Target: unknown conversation");
                            }
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Controls.Label {
                            visible: modelData.lastFireAt && modelData.lastFireAt.length > 0
                            text: qsTr("Last run: %1 (%2)").arg(modelData.lastFireAt).arg(modelData.lastFireOutcome || qsTr("unknown"))
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
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
                        Controls.ToolTip.text: qsTr("Remove this heartbeat")
                        Controls.ToolTip.visible: hovered
                        onClicked: HeartbeatConfig.removeConfig(modelData.id)
                    }
                }
            }
        }

        Controls.Label {
            visible: typeCombo.currentValue !== "regular" && dialog.heartbeatConfigs.length === 0 && !addHbExpander.expanded
            text: qsTr("No heartbeats configured for this project yet.")
            color: Kirigami.Theme.disabledTextColor
            font.italic: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    HeartbeatActivityOverlay {
        id: folderHeartbeatOverlay
    }

    ActivityTimeline {
        id: folderActivityTimeline
    }

    Dialogs.FileDialog {
        id: docFileDialog
        title: qsTr("Select a document for the project")
        fileMode: Dialogs.FileDialog.OpenFile
        onAccepted: {
            var path = Conversations.addProjectDocument(dialog.folderId, selectedFile.toString());
            if (path && path.length > 0) {
                dialog.reloadProjectDocs();
            }
        }
    }

    AppOverlayDialog {
        id: kickoffDialog
        parent: applicationWindow().overlay
        implicitWidth: Math.min(applicationWindow().width * 0.8, Kirigami.Units.gridUnit * 34)

        property string folderId: ""
        property string folderName: ""
        property int memberCount: 0

        title: qsTr("Start chatting with your team?")
        dialogIcon: "dialog-messages"

        footer: RowLayout {
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                text: qsTr("Not Now")
                onClicked: kickoffDialog.close()
            }
            AppButton {
                text: qsTr("Start")
                highlighted: true
                enabled: groupChatCheck.checked || individualCheck.checked
                onClicked: {
                    if (individualCheck.checked) {
                        Conversations.createIndividualChatsForProject(kickoffDialog.folderId);
                    }
                    if (groupChatCheck.checked && kickoffDialog.memberCount >= 2) {
                        var id = Conversations.createGroupChatForProject(kickoffDialog.folderId);
                        if (id.length > 0)
                            ChatController.switchConversation(id);
                    }
                    kickoffDialog.close();
                }
            }
        }

        ColumnLayout {
            spacing: 14

            Controls.Label {
                text: qsTr("You saved \"%1\" with %2 member(s). Would you like to " + "start some conversations with your team right now?").arg(kickoffDialog.folderName).arg(kickoffDialog.memberCount)
                color: Kirigami.Theme.textColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            Controls.CheckBox {
                id: individualCheck
                text: qsTr("Create a 1:1 conversation with each member individually")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                checked: true
            }
            Controls.Label {
                text: qsTr("Creates one separate chat per member so you can talk to each one directly. " + "Existing 1:1 chats are reused, never duplicated.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }

            Controls.CheckBox {
                id: groupChatCheck
                text: qsTr("Start a group chat with all members")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                enabled: kickoffDialog.memberCount >= 2
                checked: kickoffDialog.memberCount >= 2
            }
            Controls.Label {
                text: kickoffDialog.memberCount >= 2 ? qsTr("Creates one team chat with everyone in it. Use @mention to address specific members.") : qsTr("Group chats need at least 2 members.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
            }

            Controls.Label {
                text: qsTr("You can also start chats with individual members later by expanding " + "the project folder in the sidebar. Each member has a dedicated chat button.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                font.italic: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.topMargin: 8
            }
        }
    }
}

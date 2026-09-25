// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: dialog
    parent: applicationWindow().overlay
    implicitWidth: Math.min(applicationWindow().width * 0.8, Kirigami.Units.gridUnit * 30)

    property string conversationId: ""

    property string folderId: ""

    property string pickedAgentId: ""
    property string pickedAgentName: ""
    property string pickedAgentIcon: ""

    property var _existingAliasesLower: ({})

    property var _projectCandidates: []

    property string pickedRosterAlias: ""

    function _toolsToJson(arr) {
        if (!arr || arr.length === undefined)
            return "[]";
        try {
            return JSON.stringify(arr);
        } catch (e) {
            return "[]";
        }
    }
    function _toolsFromJson(str) {
        if (!str || str.length === 0)
            return [];
        try {
            var v = JSON.parse(str);
            return (v && v.length !== undefined) ? v : [];
        } catch (e) {
            return [];
        }
    }

    signal memberAdded(string conversationId)

    function openFor(convId, fid) {
        dialog.conversationId = convId;
        dialog.folderId = fid || "";
        dialog.pickedAgentId = "";
        dialog.pickedAgentName = "";
        dialog.pickedAgentIcon = "";
        aliasInput.text = "";
        coordCheck.checked = false;
        mEditor.load("", "", []);
        overrideExpander.expanded = false;
        var existing = MembershipService.conversationMembersList(convId);
        var byKey = ({});
        for (var i = 0; i < existing.length; ++i) {
            var a = (existing[i].alias || "").toLowerCase();
            if (a.length > 0)
                byKey[a] = true;
        }
        dialog._existingAliasesLower = byKey;
        dialog.pickedRosterAlias = "";
        var candidates = [];
        if (dialog.folderId.length > 0) {
            var pmList = MembershipService.projectMembersList(dialog.folderId);
            for (var c = 0; c < pmList.length; ++c) {
                var pmAlias = (pmList[c].alias || "").toLowerCase();
                if (pmAlias.length === 0 || byKey[pmAlias])
                    continue;
                candidates.push(pmList[c]);
            }
        }
        dialog._projectCandidates = candidates;
        templatesListModel.clear();
        var all = AgentRegistry.agentList();
        for (var j = 0; j < all.length; ++j) {
            templatesListModel.append({
                    "id": all[j].id,
                    "name": all[j].name,
                    "description": all[j].description || "",
                    "iconName": all[j].iconName || "face-smile",
                    "isCoordinator": all[j].isCoordinator || false,
                    "modelProvider": all[j].modelProvider || "",
                    "modelName": all[j].modelName || "",
                    "allowedToolsJson": dialog._toolsToJson(all[j].allowedTools || []),
                    "defaultHeartbeatGoal": all[j].defaultHeartbeatGoal || "",
                    "defaultHeartbeatSchedule": all[j].defaultHeartbeatSchedule || "",
                    "defaultHeartbeatSurfaceCriteria": all[j].defaultHeartbeatSurfaceCriteria || ""
                });
        }
        dialog.open();
    }

    function aliasIsUnique(alias) {
        var k = (alias || "").trim().toLowerCase();
        if (k.length === 0)
            return false;
        return !dialog._existingAliasesLower[k];
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

    function uniqueAliasFor(base) {
        if (aliasIsUnique(base))
            return base;
        var i = 2;
        while (!aliasIsUnique(base + " " + i))
            ++i;
        return base + " " + i;
    }

    title: qsTr("Add Member")
    dialogIcon: "list-add-user"

    footer: RowLayout {
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Cancel")
            onClicked: dialog.close()
        }
        AppButton {
            text: qsTr("Add")
            highlighted: true
            enabled: dialog.pickedAgentId.length > 0 && aliasInput.text.trim().length > 0 && dialog.aliasIsUnique(aliasInput.text.trim()) && dialog.conversationId.length > 0 && (!hbExpander.expanded || !hbEnabledSwitch.checked || (dialog.validateSchedule(hbScheduleField.text) === "" && hbGoalField.text.trim().length > 0))
            onClicked: {
                var alias = aliasInput.text.trim();
                var ok = MembershipService.addConversationMemberMap({
                        "conversationId": dialog.conversationId,
                        "agentId": dialog.pickedAgentId,
                        "alias": alias,
                        "isCoordinator": coordCheck.checked,
                        "modelProvider": mEditor.providerId,
                        "modelName": mEditor.modelName,
                        "allowedTools": mEditor.toolWhitelist
                    });
                if (!ok) {
                    return;
                }
                if (hbExpander.expanded && hbEnabledSwitch.checked) {
                    var isGroup = Conversations.isGroup(dialog.conversationId);
                    HeartbeatConfig.upsertConfigMap({
                            "agentId": dialog.pickedAgentId,
                            "scopeType": isGroup ? "conversation_group" : "conversation_1to1",
                            "scopeId": dialog.conversationId,
                            "alias": isGroup ? alias : "",
                            "enabled": true,
                            "schedule": hbScheduleField.text.trim(),
                            "goal": hbGoalField.text.trim(),
                            "surfaceCriteria": hbCriteriaField.text.trim(),
                            "maxRunsPerDay": hbMaxRunsSpin.value,
                            "selfConfigAllowed": hbSelfConfigSwitch.checked
                        });
                }
                if (dialog.folderId.length > 0) {
                    var pm = MembershipService.projectMembersList(dialog.folderId);
                    if (pm.length > 0) {
                        var key = (dialog.pickedAgentId + "|" + alias.toLowerCase());
                        var exists = false;
                        for (var i = 0; i < pm.length; ++i) {
                            var pmKey = ((pm[i].agentId || "") + "|" + (pm[i].alias || "").toLowerCase());
                            if (pmKey === key) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            MembershipService.addProjectMemberMap({
                                    "folderId": dialog.folderId,
                                    "agentId": dialog.pickedAgentId,
                                    "alias": alias,
                                    "isCoordinator": false,
                                    "modelProvider": mEditor.providerId,
                                    "modelName": mEditor.modelName,
                                    "allowedTools": mEditor.toolWhitelist
                                });
                        }
                    }
                }
                dialog.memberAdded(dialog.conversationId);
                dialog.close();
            }
        }
    }

    ColumnLayout {
        spacing: 12

        Controls.Label {
            visible: dialog._projectCandidates.length > 0
            text: qsTr("1. Add an existing project member")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.topMargin: 4
        }

        Repeater {
            model: dialog._projectCandidates
            delegate: Rectangle {
                Layout.fillWidth: true
                implicitHeight: rosterRow.implicitHeight + 12
                radius: ThemeController.radius
                color: dialog.pickedRosterAlias === modelData.alias ? ThemeController.selectionTint : (rosterMouse.containsMouse ? Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.06) : ThemeController.surfaceCard)
                border.color: dialog.pickedRosterAlias === modelData.alias ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
                border.width: 1

                required property var modelData

                RowLayout {
                    id: rosterRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 8
                    spacing: 8

                    Kirigami.Icon {
                        source: modelData.agentIconName || "face-smile"
                        fallback: "face-smile"
                        implicitWidth: Kirigami.Units.iconSizes.smallMedium
                        implicitHeight: Kirigami.Units.iconSizes.smallMedium
                        color: Kirigami.Theme.textColor
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Controls.Label {
                            text: "@" + (modelData.alias || "").replace(/ /g, "_")
                            font.bold: true
                            font.family: ThemeController.codeFontFamily
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            color: Kirigami.Theme.highlightColor
                        }
                        Controls.Label {
                            text: modelData.agentName || ""
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            color: Kirigami.Theme.disabledTextColor
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                MouseArea {
                    id: rosterMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        dialog.pickedRosterAlias = modelData.alias;
                        dialog.pickedAgentId = modelData.agentId;
                        dialog.pickedAgentName = modelData.agentName || modelData.alias;
                        dialog.pickedAgentIcon = modelData.agentIconName || "face-smile";
                        aliasInput.text = modelData.alias;
                        mEditor.load(modelData.modelProvider || "", modelData.modelName || "", (modelData.allowedTools && modelData.allowedTools.length !== undefined) ? modelData.allowedTools : []);
                    }
                }
            }
        }

        Controls.Label {
            text: dialog._projectCandidates.length > 0 ? qsTr("…or pick an agent template (creates a new member)") : qsTr("1. Pick an agent template (Required)")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.topMargin: 4
        }

        Repeater {
            model: templatesListModel
            delegate: Rectangle {
                Layout.fillWidth: true
                implicitHeight: templateRow.implicitHeight + 12
                radius: ThemeController.radius
                color: dialog.pickedAgentId === modelData.id && dialog.pickedRosterAlias.length === 0 ? ThemeController.selectionTint : (templateMouse.containsMouse ? Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.06) : ThemeController.surfaceCard)
                border.color: dialog.pickedAgentId === modelData.id && dialog.pickedRosterAlias.length === 0 ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
                border.width: 1

                required property var modelData

                RowLayout {
                    id: templateRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 8
                    spacing: 8

                    Kirigami.Icon {
                        source: modelData.iconName
                        fallback: "face-smile"
                        implicitWidth: Kirigami.Units.iconSizes.smallMedium
                        implicitHeight: Kirigami.Units.iconSizes.smallMedium
                        color: Kirigami.Theme.textColor
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Controls.Label {
                            text: modelData.name
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            color: Kirigami.Theme.textColor
                        }
                        Controls.Label {
                            text: modelData.description
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            color: Kirigami.Theme.disabledTextColor
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                MouseArea {
                    id: templateMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        dialog.pickedRosterAlias = "";
                        dialog.pickedAgentId = modelData.id;
                        dialog.pickedAgentName = modelData.name;
                        dialog.pickedAgentIcon = modelData.iconName;
                        if (aliasInput.text.length === 0) {
                            aliasInput.text = dialog.uniqueAliasFor(modelData.name);
                        }
                        mEditor.load(modelData.modelProvider || "", modelData.modelName || "", dialog._toolsFromJson(modelData.allowedToolsJson || "[]"));
                        if (hbScheduleField.text.length === 0 && (modelData.defaultHeartbeatSchedule || "").length > 0) {
                            hbScheduleField.text = modelData.defaultHeartbeatSchedule;
                        }
                        if (hbGoalField.text.length === 0 && (modelData.defaultHeartbeatGoal || "").length > 0) {
                            hbGoalField.text = modelData.defaultHeartbeatGoal;
                        }
                        if (hbCriteriaField.text.length === 0 && (modelData.defaultHeartbeatSurfaceCriteria || "").length > 0) {
                            hbCriteriaField.text = modelData.defaultHeartbeatSurfaceCriteria;
                        }
                    }
                }
            }
        }

        Controls.Label {
            visible: templatesListModel.count === 0
            text: qsTr("No agents available. Go to the Agents page to create some first.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Controls.Label {
            text: qsTr("2. Set a unique alias (Required)")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }
        AppTextField {
            id: aliasInput
            Layout.fillWidth: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            placeholderText: qsTr("e.g. Alice, Bob, Lead_Writer")
            onTextChanged: {
                var cleaned = text.replace(/^[\s@]+/, "");
                if (cleaned !== text)
                    text = cleaned;
            }
        }
        Controls.Label {
            text: aliasInput.text.trim().length > 0 && !dialog.aliasIsUnique(aliasInput.text.trim()) ? qsTr("⚠ This alias is already in use in this chat.") : qsTr("This name will be used for @mentions in the chat.")
            color: aliasInput.text.trim().length > 0 && !dialog.aliasIsUnique(aliasInput.text.trim()) ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Controls.CheckBox {
            id: coordCheck
            text: qsTr("Mark as coordinator")
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }
        Controls.Label {
            text: qsTr("Setting coordinator clears the current one. The coordinator answers " + "when the user doesn't @mention anyone.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Controls.Label {
            visible: dialog.folderId.length > 0
            text: dialog.pickedRosterAlias.length > 0 ? qsTr("This member is already in the project's roster. They " + "join this chat under the same alias.") : qsTr("This chat lives inside a project folder. The new member will be " + "added to the chat AND to the project's roster (so future group " + "chats in this project preload them by default).")
            color: Kirigami.Theme.disabledTextColor
            font.italic: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Item {
            id: overrideExpander
            property bool expanded: false
            Layout.fillWidth: true
            Layout.topMargin: 12
            implicitHeight: overrideCol.implicitHeight

            ColumnLayout {
                id: overrideCol
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 8

                AppButton {
                    text: overrideExpander.expanded ? qsTr("▾ Model Override (optional)") : qsTr("▸ Model Override (optional)")
                    flat: true
                    Layout.fillWidth: true
                    onClicked: overrideExpander.expanded = !overrideExpander.expanded
                }

                ColumnLayout {
                    visible: overrideExpander.expanded
                    Layout.fillWidth: true
                    spacing: 8

                    Controls.Label {
                        text: qsTr("Give this member its own provider, model, and tool " + "whitelist. Leave the provider on \"(Use conversation " + "default)\" to inherit the conversation's model. Other " + "members in the chat are unaffected.")
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    ModelOverrideEditor {
                        id: mEditor
                        Layout.fillWidth: true
                    }
                }
            }
        }

        Item {
            id: hbExpander
            property bool expanded: false
            Layout.fillWidth: true
            Layout.topMargin: 12
            implicitHeight: hbColumn.implicitHeight

            ColumnLayout {
                id: hbColumn
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 8

                AppButton {
                    text: hbExpander.expanded ? qsTr("▾ Configure heartbeat for this member (advanced)") : qsTr("▸ Configure heartbeat for this member (advanced)")
                    flat: true
                    Layout.fillWidth: true
                    onClicked: hbExpander.expanded = !hbExpander.expanded
                }

                ColumnLayout {
                    visible: hbExpander.expanded
                    Layout.fillWidth: true
                    spacing: 8

                    Controls.Label {
                        text: qsTr("A heartbeat lets this member run on a schedule without user prompting (e.g. daily research, periodic monitoring). Reports always go to the heartbeat overlay first; chat posting requires the per-conversation auto-surface gate, which is OFF by default.")
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Controls.Switch {
                        id: hbEnabledSwitch
                        text: qsTr("Enable heartbeat for this member")
                        checked: false
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        visible: hbEnabledSwitch.checked
                        color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.12)
                        border.color: Kirigami.Theme.neutralTextColor
                        border.width: 1
                        radius: ThemeController.radius
                        implicitHeight: hbWarnText.implicitHeight + 16
                        Controls.Label {
                            id: hbWarnText
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 8
                            wrapMode: Text.WordWrap
                            color: Kirigami.Theme.textColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            text: qsTr("⚠ Autonomous activity. This member will run on its schedule" + " (%1) without user prompting. Each run is a billable LLM" + " call, capped at %2 runs/day. Reports go to the heartbeat" + " overlay; chat posting requires the per-conversation" + " auto-surface gate (off by default; flip it on in Chat Settings).").arg(hbScheduleField.text.trim().length > 0 ? hbScheduleField.text.trim() : qsTr("(no schedule yet)")).arg(hbMaxRunsSpin.value)
                        }
                    }

                    Controls.Label {
                        text: qsTr("Schedule")
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        Layout.topMargin: 4
                    }
                    AppTextField {
                        id: hbScheduleField
                        Layout.fillWidth: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        placeholderText: qsTr("@daily at 09:00")
                        text: ""
                    }
                    Controls.Label {
                        text: dialog.validateSchedule(hbScheduleField.text) === "" ? qsTr("Formats: @hourly | @daily at HH:MM | @weekly[ on DAY at HH:MM] | @interval N (minutes, N ≥ 5).") : qsTr("⚠ %1").arg(dialog.validateSchedule(hbScheduleField.text))
                        color: dialog.validateSchedule(hbScheduleField.text) === "" ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.negativeTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        text: qsTr("Goal (what this member should do on every run)")
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        Layout.topMargin: 4
                    }
                    AppTextArea {
                        id: hbGoalField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 80
                        wrapMode: TextEdit.Wrap
                        placeholderText: qsTr("e.g. Monitor this subreddit for new product launches")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        text: ""
                    }

                    Controls.Label {
                        text: qsTr("Surface criteria (when to share with the team)")
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        Layout.topMargin: 4
                    }
                    AppTextArea {
                        id: hbCriteriaField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 60
                        wrapMode: TextEdit.Wrap
                        placeholderText: qsTr("(optional; the goal is used if empty)")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        text: ""
                    }

                    Controls.Label {
                        text: qsTr("Max runs per day")
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        Layout.topMargin: 4
                    }
                    Controls.SpinBox {
                        id: hbMaxRunsSpin
                        Layout.fillWidth: true
                        Layout.preferredWidth: 100
                        from: 1
                        to: 1440
                        value: 24
                        editable: true
                    }

                    Controls.Switch {
                        id: hbSelfConfigSwitch
                        text: qsTr("Allow agent to manage its own heartbeat config")
                        checked: false
                        Layout.topMargin: 6
                    }
                    Controls.Label {
                        visible: hbSelfConfigSwitch.checked
                        text: qsTr("⚠ The agent can change its own goal, schedule (within cap), surface criteria, and enable/disable itself. The schedule format and the max-runs/day cap are still enforced. Every change is audited.")
                        color: Kirigami.Theme.neutralTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    ListModel {
        id: templatesListModel
    }
}

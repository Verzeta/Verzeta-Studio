// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

ColumnLayout {
    id: editor

    property var initialMembers: []

    property var allowedAgentIds: []

    property bool softDelete: false

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

    function memberList() {
        var out = [];
        for (var i = 0; i < membersModel.count; ++i) {
            var row = membersModel.get(i);
            if (row.enabled === false)
                continue;
            out.push({
                    "agentId": row.agentId,
                    "alias": row.alias,
                    "isCoordinator": row.isCoordinator,
                    "modelProvider": row.modelProvider || "",
                    "modelName": row.modelName || "",
                    "allowedTools": editor._toolsFromJson(row.allowedToolsJson)
                });
        }
        return out;
    }

    function aliasExists(alias) {
        for (var i = 0; i < membersModel.count; ++i) {
            if (membersModel.get(i).alias.toLowerCase() === alias.toLowerCase())
                return true;
        }
        return false;
    }

    function uniqueAlias(base) {
        if (!aliasExists(base))
            return base;
        var i = 2;
        while (aliasExists(base + " " + i))
            ++i;
        return base + " " + i;
    }

    function reload() {
        membersModel.clear();
        var list = editor.initialMembers || [];
        for (var i = 0; i < list.length; ++i) {
            var m = list[i];
            membersModel.append({
                    "agentId": m.agentId || "",
                    "alias": m.alias || m.agentName || "",
                    "isCoordinator": m.isCoordinator || false,
                    "agentName": m.agentName || m.name || "",
                    "iconName": m.iconName || m.agentIconName || "face-smile",
                    "wasPreloaded": true,
                    "enabled": true,
                    "modelProvider": m.modelProvider || "",
                    "modelName": m.modelName || "",
                    "allowedToolsJson": editor._toolsToJson(m.allowedTools || [])
                });
        }
    }

    spacing: 8

    Component.onCompleted: reload()

    onInitialMembersChanged: reload()

    Repeater {
        id: memberRepeater
        model: membersModel

        delegate: Rectangle {
            id: memberDelegate
            Layout.fillWidth: true
            implicitHeight: memberRow.implicitHeight + 12
            radius: ThemeController.radius
            opacity: (modelData.enabled === false) ? 0.45 : 1.0
            color: ThemeController.surfaceCard
            border.color: (modelData.isCoordinator && modelData.enabled !== false) ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
            border.width: (modelData.isCoordinator && modelData.enabled !== false) ? 2 : 1

            required property int index
            required property var modelData
            readonly property bool rowEnabled: modelData.enabled !== false
            readonly property bool isPreloaded: modelData.wasPreloaded === true

            readonly property bool hasOverride: {
                var p = modelData.modelProvider || "";
                var mdl = modelData.modelName || "";
                var toolsJson = modelData.allowedToolsJson || "[]";
                if (p.length > 0)
                    return true;
                if (mdl.length > 0)
                    return true;
                if (editor._toolsFromJson(toolsJson).length > 0)
                    return true;
                return false;
            }

            readonly property string overrideSummary: {
                var p = modelData.modelProvider || "";
                var mdl = modelData.modelName || "";
                var tools = editor._toolsFromJson(modelData.allowedToolsJson || "[]");
                var parts = [];
                if (p.length > 0) {
                    parts.push(mdl.length > 0 ? (p + " / " + mdl) : p);
                } else if (mdl.length > 0) {
                    parts.push(qsTr("model: %1").arg(mdl));
                }
                if (tools.length > 0) {
                    parts.push(qsTr("%1 tool(s)").arg(tools.length));
                }
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
                    fallback: "face-smile"
                    implicitWidth: Kirigami.Units.iconSizes.medium
                    implicitHeight: Kirigami.Units.iconSizes.medium
                    color: Kirigami.Theme.textColor
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    AppTextField {
                        Layout.fillWidth: true
                        text: modelData.alias
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        font.bold: true
                        placeholderText: qsTr("Alias (e.g. Alice, Bob)")
                        enabled: memberDelegate.rowEnabled
                        onTextChanged: {
                            var cleaned = text.replace(/^[\s@]+/, "");
                            if (cleaned !== text)
                                text = cleaned;
                        }
                        onEditingFinished: {
                            var newAlias = text.trim();
                            if (newAlias.length === 0) {
                                text = modelData.alias;
                                return;
                            }
                            var conflict = false;
                            for (var i = 0; i < membersModel.count; ++i) {
                                if (i === memberDelegate.index)
                                    continue;
                                if (membersModel.get(i).alias.toLowerCase() === newAlias.toLowerCase()) {
                                    conflict = true;
                                    break;
                                }
                            }
                            if (conflict) {
                                text = modelData.alias;
                                return;
                            }
                            membersModel.setProperty(memberDelegate.index, "alias", newAlias);
                        }
                    }

                    Controls.Label {
                        text: modelData.agentName || ""
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        color: Kirigami.Theme.disabledTextColor
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    Controls.Label {
                        text: memberDelegate.overrideSummary
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        color: memberDelegate.hasOverride ? Kirigami.Theme.highlightColor : Kirigami.Theme.disabledTextColor
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                }

                Controls.ToolButton {
                    icon.name: "configure"
                    enabled: memberDelegate.rowEnabled
                    Controls.ToolTip.text: qsTr("Configure model / tools for this member")
                    Controls.ToolTip.visible: hovered
                    onClicked: memberConfigSheet.openFor(memberDelegate.index)
                }

                Controls.ToolButton {
                    icon.name: modelData.isCoordinator ? "starred-symbolic" : "non-starred-symbolic"
                    enabled: memberDelegate.rowEnabled
                    Controls.ToolTip.text: modelData.isCoordinator ? qsTr("Unmark as coordinator") : qsTr("Mark as coordinator")
                    Controls.ToolTip.visible: hovered
                    onClicked: {
                        var wasCoord = membersModel.get(memberDelegate.index).isCoordinator;
                        if (wasCoord) {
                            membersModel.setProperty(memberDelegate.index, "isCoordinator", false);
                        } else {
                            for (var i = 0; i < membersModel.count; ++i) {
                                membersModel.setProperty(i, "isCoordinator", false);
                            }
                            membersModel.setProperty(memberDelegate.index, "isCoordinator", true);
                        }
                    }
                }

                Controls.ToolButton {
                    visible: editor.softDelete && memberDelegate.isPreloaded
                    icon.name: memberDelegate.rowEnabled ? "checkbox-checked" : "checkbox"
                    Controls.ToolTip.text: memberDelegate.rowEnabled ? qsTr("Click to exclude this member from the chat") : qsTr("Click to include this member in the chat")
                    Controls.ToolTip.visible: hovered
                    onClicked: {
                        var current = membersModel.get(memberDelegate.index).enabled;
                        membersModel.setProperty(memberDelegate.index, "enabled", current === false);
                    }
                }

                Controls.ToolButton {
                    visible: !editor.softDelete || !memberDelegate.isPreloaded
                    icon.name: "edit-delete"
                    Controls.ToolTip.text: qsTr("Remove")
                    Controls.ToolTip.visible: hovered
                    onClicked: membersModel.remove(memberDelegate.index)
                }
            }
        }
    }

    Controls.Label {
        visible: membersModel.count === 0
        text: qsTr("No members yet. Click \"Add Member\" to add an agent.")
        color: Kirigami.Theme.disabledTextColor
        font.pointSize: Kirigami.Theme.defaultFont.pointSize
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        Layout.topMargin: 8
        Layout.bottomMargin: 8
    }

    AppButton {
        text: qsTr("Add Member")
        icon.name: "list-add"
        flat: true
        Layout.fillWidth: true
        onClicked: addDialog.openFresh()
    }

    Controls.Label {
        text: qsTr("⭐ marks the coordinator: the member who answers when the user doesn't @mention anyone. " + "Each alias must be unique so the same agent template can appear multiple times. " + "Use the configure button on a member to give it its own provider, model, and tool whitelist.")
        color: Kirigami.Theme.disabledTextColor
        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    ListModel {
        id: membersModel
    }

    AppOverlayDialog {
        id: addDialog
        parent: applicationWindow().overlay
        implicitWidth: Math.min(applicationWindow().width * 0.8, Kirigami.Units.gridUnit * 30)
        implicitHeight: Math.min(applicationWindow().height * 0.8, Kirigami.Units.gridUnit * 36)

        property string pickedAgentId: ""
        property string pickedAgentName: ""
        property string pickedAgentIcon: ""
        property string pickedProvider: ""
        property string pickedModel: ""
        property string pickedToolsJson: "[]"

        function openFresh() {
            pickedAgentId = "";
            pickedAgentName = "";
            pickedAgentIcon = "";
            pickedProvider = "";
            pickedModel = "";
            pickedToolsJson = "[]";
            aliasInput.text = "";
            coordCheck.checked = false;
            templatesListModel.clear();
            var all = AgentRegistry.agentList();
            for (var i = 0; i < all.length; ++i) {
                templatesListModel.append({
                        "id": all[i].id,
                        "name": all[i].name,
                        "description": all[i].description || "",
                        "iconName": all[i].iconName || "face-smile",
                        "isCoordinator": all[i].isCoordinator || false,
                        "modelProvider": all[i].modelProvider || "",
                        "modelName": all[i].modelName || "",
                        "allowedToolsJson": editor._toolsToJson(all[i].allowedTools || [])
                    });
            }
            addDialog.open();
        }

        title: qsTr("Add Member")
        dialogIcon: "list-add-user"

        footer: RowLayout {
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                text: qsTr("Cancel")
                onClicked: addDialog.close()
            }
            AppButton {
                text: qsTr("Add")
                highlighted: true
                enabled: addDialog.pickedAgentId.length > 0 && aliasInput.text.trim().length > 0 && !editor.aliasExists(aliasInput.text.trim())
                onClicked: {
                    membersModel.append({
                            "agentId": addDialog.pickedAgentId,
                            "alias": aliasInput.text.trim(),
                            "isCoordinator": coordCheck.checked,
                            "agentName": addDialog.pickedAgentName,
                            "iconName": addDialog.pickedAgentIcon,
                            "wasPreloaded": false,
                            "enabled": true,
                            "modelProvider": addDialog.pickedProvider,
                            "modelName": addDialog.pickedModel,
                            "allowedToolsJson": addDialog.pickedToolsJson
                        });
                    if (coordCheck.checked) {
                        var newIdx = membersModel.count - 1;
                        for (var i = 0; i < newIdx; ++i) {
                            membersModel.setProperty(i, "isCoordinator", false);
                        }
                    }
                    addDialog.close();
                }
            }
        }

        ColumnLayout {
            spacing: 12

            Controls.Label {
                text: qsTr("1. Pick an agent template")
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
                    color: addDialog.pickedAgentId === modelData.id ? ThemeController.selectionTint : (templateMouse.containsMouse ? Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.06) : ThemeController.surfaceCard)
                    border.color: addDialog.pickedAgentId === modelData.id ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle
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
                            RowLayout {
                                spacing: 6
                                Controls.Label {
                                    text: modelData.name
                                    font.bold: true
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                    color: Kirigami.Theme.textColor
                                }
                                Rectangle {
                                    visible: modelData.isCoordinator
                                    radius: ThemeController.radius
                                    implicitWidth: coordBadge.implicitWidth + 8
                                    implicitHeight: coordBadge.implicitHeight + 2
                                    color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.18)
                                    Controls.Label {
                                        id: coordBadge
                                        anchors.centerIn: parent
                                        text: qsTr("Coordinator")
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                        color: Kirigami.Theme.highlightColor
                                    }
                                }
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
                            addDialog.pickedAgentId = modelData.id;
                            addDialog.pickedAgentName = modelData.name;
                            addDialog.pickedAgentIcon = modelData.iconName;
                            addDialog.pickedProvider = modelData.modelProvider || "";
                            addDialog.pickedModel = modelData.modelName || "";
                            addDialog.pickedToolsJson = modelData.allowedToolsJson || "[]";
                            if (aliasInput.text.length === 0) {
                                aliasInput.text = editor.uniqueAlias(modelData.name);
                            }
                            coordCheck.checked = modelData.isCoordinator && editor.memberList().filter(function (m) {
                                    return m.isCoordinator;
                                }).length === 0;
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
                text: qsTr("2. Set a unique alias")
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
                text: aliasInput.text.trim().length > 0 && editor.aliasExists(aliasInput.text.trim()) ? qsTr("⚠ This alias is already in use.") : qsTr("This name will be used for @mentions in the chat.")
                color: aliasInput.text.trim().length > 0 && editor.aliasExists(aliasInput.text.trim()) ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor
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
                text: qsTr("The coordinator answers when the user doesn't @mention anyone. " + "Only one member can be coordinator at a time.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Controls.Label {
                text: qsTr("The new member starts with its agent template's default model and " + "tools. Use the configure button on the member's row to give it its " + "own provider, model, or tool whitelist.")
                color: Kirigami.Theme.disabledTextColor
                font.italic: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        ListModel {
            id: templatesListModel
        }
    }

    AppOverlayDialog {
        id: memberConfigSheet
        parent: applicationWindow().overlay
        implicitWidth: Math.min(applicationWindow().width * 0.8, Kirigami.Units.gridUnit * 30)
        implicitHeight: Math.min(applicationWindow().height * 0.8, Kirigami.Units.gridUnit * 36)

        property int configIndex: -1
        property string configAlias: ""

        function openFor(index) {
            if (index < 0 || index >= membersModel.count)
                return;
            var row = membersModel.get(index);
            memberConfigSheet.configIndex = index;
            memberConfigSheet.configAlias = row.alias || "";
            mcEditor.load(row.modelProvider || "", row.modelName || "", editor._toolsFromJson(row.allowedToolsJson || "[]"));
            memberConfigSheet.open();
        }

        title: memberConfigSheet.configAlias.length > 0 ? qsTr("Configure: %1").arg(memberConfigSheet.configAlias) : qsTr("Configure Member")
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
                enabled: memberConfigSheet.configIndex >= 0
                onClicked: {
                    var idx = memberConfigSheet.configIndex;
                    if (idx < 0 || idx >= membersModel.count) {
                        memberConfigSheet.close();
                        return;
                    }
                    membersModel.setProperty(idx, "modelProvider", mcEditor.providerId);
                    membersModel.setProperty(idx, "modelName", mcEditor.modelName);
                    membersModel.setProperty(idx, "allowedToolsJson", editor._toolsToJson(mcEditor.toolWhitelist));
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
        }
    }
}

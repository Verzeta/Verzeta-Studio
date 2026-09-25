// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: toolsPage
    color: ThemeController.surfacePage

    Controls.ScrollView {
        contentWidth: applicationWindow().isCompact ? availableWidth : -1
        anchors.fill: parent
        Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
        clip: true

        ColumnLayout {
            width: Math.min(toolsPage.width - 64, 700)
            x: (toolsPage.width - width) / 2
            spacing: 4

            Item {
                Layout.preferredHeight: 32
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Kirigami.Heading {
                    text: qsTr("Tools")
                    level: 2
                    Layout.fillWidth: true
                }

                AppButton {
                    text: toolsTabBar.currentIndex === 1 ? qsTr("Add Custom Tool") : qsTr("Add MCP Server")
                    icon.name: "list-add"
                    highlighted: true
                    visible: toolsTabBar.currentIndex !== 0
                    onClicked: {
                        if (toolsTabBar.currentIndex === 1)
                            addToolDialog.open();
                        else
                            addMcpDialog.open();
                    }
                }
            }

            Controls.Label {
                text: qsTr("Tools are functions the AI assistant can call during conversations. Use the tabs below to manage built-in tools, custom user-defined tools, and MCP servers.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.bottomMargin: 8
            }

            Controls.TabBar {
                id: toolsTabBar
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 8

                Controls.TabButton {
                    text: qsTr("Built-in (%1)").arg(builtInRepeater.count)
                    width: applicationWindow().isCompact ? Math.floor(toolsTabBar.width / toolsTabBar.count) : implicitWidth
                }
                Controls.TabButton {
                    text: qsTr("Custom (%1)").arg(customRepeater.count)
                    width: applicationWindow().isCompact ? Math.floor(toolsTabBar.width / toolsTabBar.count) : implicitWidth
                }
                Controls.TabButton {
                    text: applicationWindow().isCompact ? qsTr("MCP (%1)").arg(mcpRepeater.count) : qsTr("MCP Servers (%1)").arg(mcpRepeater.count)
                    width: applicationWindow().isCompact ? Math.floor(toolsTabBar.width / toolsTabBar.count) : implicitWidth
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: toolsTabBar.currentIndex === 0
                spacing: 4

                Repeater {
                    id: builtInRepeater
                    model: {
                        var all = ToolService.registeredToolsList();
                        return all.filter(function (t) {
                                return t.kind === "builtin";
                            });
                    }
                    delegate: toolCardComponent
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: toolsTabBar.currentIndex === 1
                spacing: 4

                Controls.Label {
                    visible: customRepeater.count === 0
                    text: qsTr("No custom tools defined yet.\nClick \"Add Custom Tool\" above to create one.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 16
                }

                Repeater {
                    id: customRepeater
                    model: {
                        var all = ToolService.registeredToolsList();
                        return all.filter(function (t) {
                                return t.kind === "custom";
                            });
                    }
                    delegate: toolCardComponent
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: toolsTabBar.currentIndex === 2
                spacing: 4

                Controls.Label {
                    text: qsTr("Connect to MCP (Model Context Protocol) servers to add tools from external services, file systems, databases, APIs, web search, and more. Supports stdio (npx, python, uvx) and HTTP servers.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 12
                }

                Kirigami.Heading {
                    text: qsTr("Servers")
                    level: 4
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                }

                Controls.Label {
                    visible: mcpRepeater.count === 0
                    text: qsTr("No MCP servers configured. Click \"Add MCP Server\" above to connect one.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 12
                }

                Repeater {
                    id: mcpRepeater
                    model: McpService.serverList()
                    delegate: mcpServerCardComponent
                }

                Kirigami.Heading {
                    text: qsTr("MCP Tools (%1)").arg(mcpToolsRepeater.count)
                    level: 4
                    Layout.topMargin: 16
                    Layout.bottomMargin: 4
                }

                Controls.Label {
                    text: qsTr("Tools registered by the MCP servers above. They are exposed to the LLM as `<server>:<toolname>`. Toggle to enable/disable individual tools.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 8
                }

                Controls.Label {
                    visible: mcpToolsRepeater.count === 0
                    text: qsTr("No MCP tools registered. Connect a server above, and its tools will appear here once the server finishes its handshake.")
                    color: Kirigami.Theme.disabledTextColor
                    font.italic: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 12
                }

                Repeater {
                    id: mcpToolsRepeater
                    delegate: mcpToolCardComponent
                }
            }

            Item {
                Layout.preferredHeight: 40
            }
        }
    }

    Component {
        id: mcpServerCardComponent

        Rectangle {
            required property var modelData

            Layout.fillWidth: true
            Layout.bottomMargin: 8
            implicitHeight: serverCardContent.implicitHeight + 24
            radius: ThemeController.radius
            color: ThemeController.surfaceCard
            border.color: {
                if (modelData.status === "error")
                    return Kirigami.Theme.negativeTextColor;
                if (modelData.status === "connected")
                    return Kirigami.Theme.positiveTextColor;
                return ThemeController.borderSubtle;
            }
            border.width: 1

            ColumnLayout {
                id: serverCardContent
                anchors.fill: parent
                anchors.margins: 12
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
                            if (modelData.status === "connected")
                                return Kirigami.Theme.positiveTextColor;
                            if (modelData.status === "connecting")
                                return Kirigami.Theme.neutralTextColor;
                            if (modelData.status === "error")
                                return Kirigami.Theme.negativeTextColor;
                            return Kirigami.Theme.disabledTextColor;
                        }
                    }

                    Controls.Label {
                        text: modelData.name
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        color: Kirigami.Theme.textColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        radius: ThemeController.radius
                        color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.15)
                        Layout.preferredWidth: typeBadgeLabel.implicitWidth + 10
                        Layout.preferredHeight: typeBadgeLabel.implicitHeight + 4
                        Layout.alignment: Qt.AlignVCenter
                        Controls.Label {
                            id: typeBadgeLabel
                            anchors.centerIn: parent
                            text: modelData.type || "stdio"
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            color: Kirigami.Theme.highlightColor
                        }
                    }

                    Controls.Label {
                        text: modelData.status === "connected" ? qsTr("%1 tools").arg(modelData.toolCount || 0) : (modelData.status || "")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        color: modelData.status === "error" ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Controls.Switch {
                        checked: !modelData.disabled
                        Layout.alignment: Qt.AlignVCenter
                        onToggled: McpService.setServerEnabled(modelData.name, checked)
                    }

                    Controls.ToolButton {
                        icon.name: "document-edit"
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: openMcpEditor(modelData)
                        Controls.ToolTip.text: qsTr("Edit")
                        Controls.ToolTip.visible: hovered
                    }
                    Controls.ToolButton {
                        icon.name: "view-refresh"
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: McpService.reconnectServer(modelData.name)
                        Controls.ToolTip.text: qsTr("Reconnect")
                        Controls.ToolTip.visible: hovered
                    }
                    Controls.ToolButton {
                        icon.name: "edit-delete"
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: McpService.removeServer(modelData.name)
                        Controls.ToolTip.text: qsTr("Remove")
                        Controls.ToolTip.visible: hovered
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    text: modelData.command ? (modelData.command + " " + (modelData.args || "")) : (modelData.url || "")
                    font.family: ThemeController.codeFontFamily
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }

                Controls.Label {
                    visible: modelData.status === "error" && (modelData.error || "").length > 0
                    Layout.fillWidth: true
                    text: modelData.error || ""
                    color: Kirigami.Theme.negativeTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    Component {
        id: mcpToolCardComponent

        Rectangle {
            required property var modelData

            Layout.fillWidth: true
            Layout.bottomMargin: 6
            implicitHeight: mcpToolContent.implicitHeight + 20
            radius: ThemeController.radius
            color: ThemeController.surfaceCard
            border.color: ThemeController.borderSubtle
            border.width: 1

            ColumnLayout {
                id: mcpToolContent
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Kirigami.Icon {
                        source: "tools"
                        fallback: "configure"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        Layout.alignment: Qt.AlignVCenter
                        color: Kirigami.Theme.highlightColor
                    }

                    Controls.Label {
                        text: modelData.shortName || modelData.name
                        font.bold: true
                        font.family: ThemeController.codeFontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        color: Kirigami.Theme.highlightColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        radius: ThemeController.radius
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: mcpBadgeText.implicitWidth + 12
                        Layout.preferredHeight: mcpBadgeText.implicitHeight + 4
                        color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.18)
                        Controls.Label {
                            id: mcpBadgeText
                            anchors.centerIn: parent
                            text: qsTr("MCP · %1").arg(modelData.mcpServer || "?")
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.75
                            font.bold: true
                            color: Kirigami.Theme.neutralTextColor
                        }
                    }

                    Controls.Switch {
                        checked: modelData.enabled
                        Layout.alignment: Qt.AlignVCenter
                        onToggled: ToolService.setToolEnabled(modelData.name, checked)
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    visible: (modelData.description || "").length > 0
                    text: modelData.description || ""
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    Component {
        id: toolCardComponent

        Rectangle {
            required property var modelData

            Layout.fillWidth: true
            Layout.bottomMargin: 8
            implicitHeight: toolCardContent.implicitHeight + 24
            radius: ThemeController.radius
            color: ThemeController.surfaceCard
            border.color: ThemeController.borderSubtle
            border.width: 1

            ColumnLayout {
                id: toolCardContent
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle {
                        width: 8
                        height: 8
                        radius: height / 2
                        Layout.alignment: Qt.AlignVCenter
                        color: modelData.enabled ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
                    }

                    Controls.Label {
                        text: modelData.name
                        font.bold: true
                        font.family: ThemeController.codeFontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        color: Kirigami.Theme.textColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        radius: ThemeController.radius
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: badgeLabel.implicitWidth + 10
                        Layout.preferredHeight: badgeLabel.implicitHeight + 4
                        color: modelData.isBuiltIn ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.15) : Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.15)
                        Controls.Label {
                            id: badgeLabel
                            anchors.centerIn: parent
                            text: modelData.isBuiltIn ? qsTr("Built-in") : qsTr("Custom")
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            color: modelData.isBuiltIn ? Kirigami.Theme.highlightColor : Kirigami.Theme.positiveTextColor
                        }
                    }

                    Controls.Switch {
                        checked: modelData.enabled
                        Layout.alignment: Qt.AlignVCenter
                        onToggled: ToolService.setToolEnabled(modelData.name, checked)
                    }

                    Controls.ToolButton {
                        icon.name: "edit-delete"
                        visible: !modelData.isBuiltIn
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: ToolService.removeCustomTool(modelData.name)
                        Controls.ToolTip.text: qsTr("Remove tool")
                        Controls.ToolTip.visible: hovered
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    visible: (modelData.description || "").length > 0
                    text: modelData.description || ""
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                }

                Repeater {
                    model: modelData.parameters || []
                    delegate: RowLayout {
                        required property var modelData

                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        spacing: 6

                        Controls.Label {
                            text: modelData.name
                            font.family: ThemeController.codeFontFamily
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            color: Kirigami.Theme.highlightColor
                        }
                        Controls.Label {
                            text: "(" + modelData.type + ")"
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                            color: Kirigami.Theme.disabledTextColor
                        }
                        Controls.Label {
                            text: modelData.required ? qsTr("required") : qsTr("optional")
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                            font.italic: true
                            color: modelData.required ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor
                        }
                        Controls.Label {
                            text: "- " + (modelData.description || "")
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                            color: Kirigami.Theme.disabledTextColor
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }

    function openMcpEditor(server) {
        addMcpDialog.editingName = server.name || "";
        mcpNameField.text = server.name || "";
        var t = server.type || "stdio";
        for (var i = 0; i < mcpTypeCombo.model.length; ++i) {
            if (mcpTypeCombo.model[i].value === t) {
                mcpTypeCombo.currentIndex = i;
                break;
            }
        }
        mcpCmdField.text = server.command || "";
        if (Array.isArray(server.args)) {
            mcpArgsField.text = server.args.join(" ");
        } else {
            mcpArgsField.text = server.args || "";
        }
        var envLines = [];
        if (server.env && typeof server.env === "object") {
            for (var k in server.env) {
                if (server.env.hasOwnProperty(k)) {
                    envLines.push(k + "=" + server.env[k]);
                }
            }
        }
        mcpEnvField.text = envLines.join("\n");
        mcpUrlField.text = server.url || "";
        mcpAuthField.text = "";
        addMcpDialog.open();
    }

    function refreshTools() {
        var all = ToolService.registeredToolsList();
        builtInRepeater.model = all.filter(function (t) {
                return t.kind === "builtin";
            });
        customRepeater.model = all.filter(function (t) {
                return t.kind === "custom";
            });
        mcpToolsRepeater.model = all.filter(function (t) {
                return t.kind === "mcp";
            });
    }

    function refreshMcp() {
        mcpRepeater.model = McpService.serverList();
    }

    Connections {
        target: ToolService
        function onToolsChanged() {
            toolsPage.refreshTools();
        }
    }

    Connections {
        target: McpService
        function onServersChanged() {
            toolsPage.refreshMcp();
        }
        function onMcpToolsChanged() {
            toolsPage.refreshTools();
        }
    }

    Component.onCompleted: {
        refreshTools();
        refreshMcp();
    }

    Kirigami.Dialog {
        id: addToolDialog
        title: qsTr("Add Custom Tool")
        preferredWidth: Kirigami.Units.gridUnit * 38
        preferredHeight: Kirigami.Units.gridUnit * 36
        standardButtons: Kirigami.Dialog.NoButton
        padding: 24

        ColumnLayout {
            spacing: 16

            Controls.Label {
                text: qsTr("Tool Name")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.topMargin: 4
            }
            AppTextField {
                id: toolNameField
                Layout.fillWidth: true
                placeholderText: "my_tool"
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                validator: RegularExpressionValidator {
                    regularExpression: /[a-z][a-z0-9_]*/
                }
            }

            Controls.Label {
                text: qsTr("Description")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            AppTextArea {
                id: toolDescField
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                placeholderText: qsTr("What does this tool do? The LLM reads this to decide when to use it.")
                wrapMode: TextEdit.Wrap
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                text: qsTr("Command Template")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            Controls.Label {
                text: qsTr("Shell command to execute. Use {{param_name}} for parameter substitution.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            AppTextField {
                id: toolCmdField
                Layout.fillWidth: true
                placeholderText: "curl -s https://api.example.com/{{query}}"
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                text: qsTr("Parameters")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.topMargin: 4
            }

            Repeater {
                id: paramRepeater
                model: paramModel

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    required property int index

                    AppTextField {
                        Layout.preferredWidth: 120
                        placeholderText: "name"
                        font.family: ThemeController.codeFontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        text: paramModel.get(index).pName
                        onTextChanged: paramModel.setProperty(index, "pName", text)
                    }
                    Controls.ComboBox {
                        Layout.preferredWidth: 110
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        model: ["string", "integer", "boolean", "number"]
                        currentIndex: {
                            var t = paramModel.get(index).pType;
                            return Math.max(0, ["string", "integer", "boolean", "number"].indexOf(t));
                        }
                        onActivated: paramModel.setProperty(index, "pType", currentText)
                    }
                    AppTextField {
                        Layout.fillWidth: true
                        placeholderText: "description"
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        text: paramModel.get(index).pDesc
                        onTextChanged: paramModel.setProperty(index, "pDesc", text)
                    }
                    Controls.CheckBox {
                        text: qsTr("Req")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        checked: paramModel.get(index).pRequired
                        onToggled: paramModel.setProperty(index, "pRequired", checked)
                    }
                    Controls.ToolButton {
                        icon.name: "edit-delete"
                        onClicked: paramModel.remove(index)
                    }
                }
            }

            AppButton {
                text: qsTr("Add Parameter")
                icon.name: "list-add"
                flat: true
                onClicked: paramModel.append({
                        "pName": "",
                        "pType": "string",
                        "pDesc": "",
                        "pRequired": false
                    })
            }

            Item {
                Layout.fillHeight: true
                Layout.preferredHeight: 8
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 12

                Item {
                    Layout.fillWidth: true
                }

                AppButton {
                    text: qsTr("Cancel")
                    onClicked: addToolDialog.close()
                }

                AppButton {
                    text: qsTr("Create Tool")
                    highlighted: true
                    enabled: toolNameField.text.length > 0 && toolDescField.text.length > 0
                    onClicked: {
                        var params = [];
                        for (var i = 0; i < paramModel.count; ++i) {
                            var p = paramModel.get(i);
                            if (p.pName.length > 0) {
                                params.push({
                                        "name": p.pName,
                                        "type": p.pType,
                                        "description": p.pDesc,
                                        "required": p.pRequired
                                    });
                            }
                        }
                        var ok = ToolService.addCustomTool({
                                "name": toolNameField.text,
                                "description": toolDescField.text,
                                "commandTemplate": toolCmdField.text,
                                "parameters": params
                            });
                        if (ok) {
                            toolNameField.text = "";
                            toolDescField.text = "";
                            toolCmdField.text = "";
                            paramModel.clear();
                            addToolDialog.close();
                        }
                    }
                }
            }
        }

        ListModel {
            id: paramModel
        }

        onOpened: paramModel.clear()
    }

    Kirigami.Dialog {
        id: addMcpDialog
        title: editingName.length > 0 ? qsTr("Edit MCP Server") : qsTr("Add MCP Server")
        preferredWidth: Kirigami.Units.gridUnit * 38
        preferredHeight: Kirigami.Units.gridUnit * 32
        standardButtons: Kirigami.Dialog.NoButton
        padding: 24

        property string editingName: ""

        onClosed: editingName = ""

        ColumnLayout {
            spacing: 16

            Controls.Label {
                text: qsTr("Server Name")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.topMargin: 4
            }
            AppTextField {
                id: mcpNameField
                Layout.fillWidth: true
                placeholderText: "my-mcp-server"
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                text: qsTr("Transport Type")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            Controls.ComboBox {
                id: mcpTypeCombo
                Layout.fillWidth: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                model: [{
                        "text": "stdio (npx, python, uvx)",
                        "value": "stdio"
                    }, {
                        "text": "SSE (HTTP Server-Sent Events)",
                        "value": "sse"
                    }, {
                        "text": "Streamable HTTP",
                        "value": "streamable_http"
                    }]
                textRole: "text"
                valueRole: "value"
                currentIndex: 0
            }

            Controls.Label {
                visible: mcpTypeCombo.currentValue === "stdio"
                text: qsTr("Command")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            AppTextField {
                id: mcpCmdField
                visible: mcpTypeCombo.currentValue === "stdio"
                Layout.fillWidth: true
                placeholderText: "npx"
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                visible: mcpTypeCombo.currentValue === "stdio"
                text: qsTr("Arguments (space-separated)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            AppTextField {
                id: mcpArgsField
                visible: mcpTypeCombo.currentValue === "stdio"
                Layout.fillWidth: true
                placeholderText: "-y @modelcontextprotocol/server-filesystem"
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                visible: mcpTypeCombo.currentValue !== "stdio"
                text: qsTr("Server URL")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            AppTextField {
                id: mcpUrlField
                visible: mcpTypeCombo.currentValue !== "stdio"
                Layout.fillWidth: true
                placeholderText: "http://localhost:8000/sse"
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }

            Controls.Label {
                visible: mcpTypeCombo.currentValue !== "stdio"
                text: qsTr("Authorization Header (optional)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            AppTextField {
                id: mcpAuthField
                visible: mcpTypeCombo.currentValue !== "stdio"
                Layout.fillWidth: true
                placeholderText: "Bearer your-token-here"
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                echoMode: TextInput.Password
            }

            Controls.Label {
                visible: mcpTypeCombo.currentValue === "stdio"
                text: qsTr("Environment Variables (optional)")
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            Controls.Label {
                visible: mcpTypeCombo.currentValue === "stdio"
                text: qsTr("One KEY=VALUE per line")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
            }
            AppTextArea {
                id: mcpEnvField
                visible: mcpTypeCombo.currentValue === "stdio"
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                placeholderText: "API_KEY=your-key\nDEBUG=true"
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: TextEdit.Wrap
            }

            Item {
                Layout.fillHeight: true
                Layout.preferredHeight: 8
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 12

                Item {
                    Layout.fillWidth: true
                }

                AppButton {
                    text: qsTr("Cancel")
                    onClicked: addMcpDialog.close()
                }

                AppButton {
                    text: addMcpDialog.editingName.length > 0 ? qsTr("Save Changes") : qsTr("Add Server")
                    highlighted: true
                    enabled: mcpNameField.text.length > 0 && (mcpTypeCombo.currentValue === "stdio" ? mcpCmdField.text.length > 0 : mcpUrlField.text.length > 0)
                    onClicked: {
                        var config = {
                            "name": mcpNameField.text
                        };
                        if (mcpTypeCombo.currentValue === "stdio") {
                            config.type = "stdio";
                            config.command = mcpCmdField.text;
                            config.args = mcpArgsField.text.split(" ").filter(function (a) {
                                    return a.length > 0;
                                });
                            var envLines = mcpEnvField.text.split("\n");
                            var env = {};
                            for (var i = 0; i < envLines.length; ++i) {
                                var line = envLines[i].trim();
                                var eqIdx = line.indexOf("=");
                                if (eqIdx > 0) {
                                    env[line.substring(0, eqIdx)] = line.substring(eqIdx + 1);
                                }
                            }
                            config.env = env;
                        } else {
                            config.type = mcpTypeCombo.currentValue;
                            config.url = mcpUrlField.text;
                            if (mcpAuthField.text.length > 0) {
                                config.headers = {
                                    "Authorization": mcpAuthField.text
                                };
                            }
                        }
                        if (addMcpDialog.editingName.length > 0 && addMcpDialog.editingName !== mcpNameField.text) {
                            McpService.removeServer(addMcpDialog.editingName);
                        } else if (addMcpDialog.editingName.length > 0) {
                            McpService.removeServer(addMcpDialog.editingName);
                        }
                        McpService.addServer(config);
                        mcpNameField.text = "";
                        mcpCmdField.text = "";
                        mcpArgsField.text = "";
                        mcpUrlField.text = "";
                        mcpAuthField.text = "";
                        mcpEnvField.text = "";
                        addMcpDialog.editingName = "";
                        addMcpDialog.close();
                    }
                }
            }
        }
    }
}

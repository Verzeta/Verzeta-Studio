// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

ColumnLayout {
    id: editor
    spacing: 12

    readonly property string providerId: providerCombo.currentValue || ""

    readonly property string modelName: modelPicker.currentValue.trim()

    readonly property var toolWhitelist: editor._toolWhitelist

    function load(provider, model, tools) {
        providerCombo.currentIndex = Math.max(0, providerCombo.indexOfValue(provider || ""));
        modelPicker.currentValue = model || "";
        editor._toolWhitelist = (tools && tools.length !== undefined) ? tools.slice() : [];
        toolWhitelistExpander.expanded = editor._toolWhitelist.length > 0;
    }

    property var _toolWhitelist: []

    readonly property var _providerOptions: {
        var opts = [{
                "providerId": "",
                "displayName": qsTr("(Use conversation default)")
            }];
        var avail = AgentSettings.availableProviders;
        for (var i = 0; i < avail.length; ++i)
            opts.push(avail[i]);
        return opts;
    }

    property int _modelsRev: 0

    Connections {
        target: AgentSettings
        function onModelsRefreshed(refreshedProviderId, models) {
            if (refreshedProviderId === providerCombo.currentValue)
                editor._modelsRev++;
        }
    }

    Controls.Label {
        text: qsTr("Provider")
        font.bold: true
        font.pointSize: Kirigami.Theme.defaultFont.pointSize
    }
    Controls.ComboBox {
        id: providerCombo
        Layout.fillWidth: true
        Layout.preferredWidth: 100
        font.pointSize: Kirigami.Theme.defaultFont.pointSize
        model: editor._providerOptions
        textRole: "displayName"
        valueRole: "providerId"
    }

    Controls.Label {
        text: qsTr("Model")
        font.bold: true
        font.pointSize: Kirigami.Theme.defaultFont.pointSize
    }
    SearchableModelComboBox {
        id: modelPicker
        Layout.fillWidth: true
        Layout.preferredWidth: 100
        placeholderText: (providerCombo.currentValue && providerCombo.currentValue.length > 0) ? qsTr("Model name for the selected provider (e.g. gemma4:e4b)") : qsTr("Model name (optional)")
        models: {
            var rev = editor._modelsRev;
            var pid = providerCombo.currentValue || "";
            return pid.length > 0 ? AgentSettings.modelsForProvider(pid) : [];
        }
    }
    Controls.Label {
        text: (providerCombo.currentValue && providerCombo.currentValue.length > 0) ? qsTr("Pick from %1's fetched models, or type a model name.").arg(providerCombo.currentText) : qsTr("Optional: type a model name to pin it on the conversation's own provider.")
        color: Kirigami.Theme.disabledTextColor
        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    Item {
        id: toolWhitelistExpander
        property bool expanded: false
        Layout.fillWidth: true
        Layout.topMargin: 4
        implicitHeight: toolWhitelistCol.implicitHeight

        ColumnLayout {
            id: toolWhitelistCol
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 8

            AppButton {
                text: {
                    var n = editor._toolWhitelist.length;
                    var label = n > 0 ? qsTr("Tool Whitelist (%1 selected)").arg(n) : qsTr("Tool Whitelist (optional; all tools allowed)");
                    return (toolWhitelistExpander.expanded ? "▾ " : "▸ ") + label;
                }
                flat: true
                Layout.fillWidth: true
                onClicked: toolWhitelistExpander.expanded = !toolWhitelistExpander.expanded
            }

            ColumnLayout {
                visible: toolWhitelistExpander.expanded
                Layout.fillWidth: true
                spacing: 6

                Rectangle {
                    id: whitelistStatus
                    readonly property bool allAllowed: editor._toolWhitelist.length === 0
                    Layout.fillWidth: true
                    radius: ThemeController.radius
                    border.width: 1
                    color: Qt.rgba((allAllowed ? Kirigami.Theme.positiveTextColor.r : Kirigami.Theme.highlightColor.r), (allAllowed ? Kirigami.Theme.positiveTextColor.g : Kirigami.Theme.highlightColor.g), (allAllowed ? Kirigami.Theme.positiveTextColor.b : Kirigami.Theme.highlightColor.b), 0.12)
                    border.color: allAllowed ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.highlightColor
                    implicitHeight: whitelistStatusLabel.implicitHeight + 12
                    Controls.Label {
                        id: whitelistStatusLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 8
                        wrapMode: Text.WordWrap
                        font.bold: true
                        color: Kirigami.Theme.textColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        text: whitelistStatus.allAllowed ? qsTr("✓ All tools are allowed. Check tools below to restrict to a specific set.") : qsTr("Restricted to %1 tool(s). Uncheck all to allow every tool again.").arg(editor._toolWhitelist.length)
                    }
                }

                Controls.Label {
                    text: qsTr("Checking tools narrows the turn to only those " + "(intersected with task-tool gating). It never " + "grants a tool the turn wouldn't otherwise have.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Repeater {
                    model: ToolService.registeredToolsList()
                    delegate: Controls.CheckBox {
                        required property var modelData
                        Layout.fillWidth: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                        text: modelData.name + (modelData.description ? "  -  " + modelData.description : "")
                        checked: editor._toolWhitelist.indexOf(modelData.name) >= 0
                        onToggled: {
                            var list = editor._toolWhitelist.slice();
                            var idx = list.indexOf(modelData.name);
                            if (checked && idx < 0) {
                                list.push(modelData.name);
                            } else if (!checked && idx >= 0) {
                                list.splice(idx, 1);
                            }
                            editor._toolWhitelist = list;
                        }
                    }
                }
            }
        }
    }
}

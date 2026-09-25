// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: bar
    implicitHeight: 48
    color: ThemeController.surfaceCard

    property bool showMenuButton: false
    property bool settingsActive: false

    property bool canvasAvailable: false
    property bool canvasActive: false
    property bool canvasButtonHidden: false

    readonly property bool compact: bar.width < 760

    signal menuClicked
    signal toggleSettings
    signal toggleCanvas
    signal openArtifacts
    signal openTerminal
    signal openPlans
    signal startCall

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 6

        AppButton {
            icon.name: "application-menu"
            flat: true
            iconOnly: true
            visible: bar.showMenuButton
            onClicked: bar.menuClicked()
            Controls.ToolTip.text: qsTr("Open sidebar")
            Controls.ToolTip.visible: hovered
            Controls.ToolTip.delay: 600
        }

        ColumnLayout {
            id: titleBlock
            spacing: 0
            Layout.maximumWidth: bar.compact ? 140 : 260

            property string scopeLabel: ""
            function refreshScope() {
                titleBlock.scopeLabel = ChatController.activeConversationScopeLabel();
            }
            Component.onCompleted: titleBlock.refreshScope()

            Connections {
                target: ChatController
                function onActiveConversationChanged() {
                    titleBlock.refreshScope();
                }
            }
            Connections {
                target: ConversationService
                function onConversationUpdated(id) {
                    if (id === ChatController.activeConversationId) {
                        titleBlock.refreshScope();
                    }
                }
                function onFolderUpdated(id) {
                    titleBlock.refreshScope();
                }
            }

            Controls.Label {
                text: ChatController.activeConversationTitle.length > 0 ? ChatController.activeConversationTitle : qsTr("New Chat")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                font.bold: true
                color: Kirigami.Theme.textColor
                elide: Text.ElideRight
                maximumLineCount: 1
                Layout.fillWidth: true
            }
            Controls.Label {
                visible: titleBlock.scopeLabel.length > 0
                text: titleBlock.scopeLabel
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                color: Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
                maximumLineCount: 1
                Layout.fillWidth: true
            }
        }

        Rectangle {
            width: 1
            Layout.preferredHeight: 24
            Layout.alignment: Qt.AlignVCenter
            color: ThemeController.borderSubtle
        }

        ModelSelector {
            id: modelSelector
            visible: AgentSettings.activeModel.length > 0 && bar.width >= 560
        }

        Item {
            Layout.fillWidth: true
        }

        AppButton {
            id: overflowButton
            visible: bar.compact
            icon.name: "view-more-symbolic"
            flat: true
            iconOnly: true
            text: qsTr("More")
            Controls.ToolTip.text: qsTr("More actions")
            Controls.ToolTip.visible: hovered
            Controls.ToolTip.delay: 600
            onClicked: overflowMenu.popup()

            Controls.Menu {
                id: overflowMenu
                Controls.MenuItem {
                    text: ArtifactsModel.count > 0 ? qsTr("Artifacts (%1)").arg(ArtifactsModel.count) : qsTr("Artifacts")
                    icon.name: "folder-documents"
                    onTriggered: bar.openArtifacts()
                }
                Controls.MenuItem {
                    text: PlansModel.count > 0 ? qsTr("Plans (%1)").arg(PlansModel.count) : qsTr("Plans")
                    icon.name: "view-task"
                    onTriggered: bar.openPlans()
                }
                Controls.MenuItem {
                    text: VoiceCallService.callActive && VoiceCallService.callConversationId === ChatController.activeConversationId ? qsTr("Return to the call") : qsTr("Voice call")
                    icon.name: VoiceCallService.callActive ? "call-stop" : "call-start"
                    visible: VoiceCallService.running
                    height: visible ? implicitHeight : 0
                    enabled: (VoiceCallService.callActive && VoiceCallService.callConversationId === ChatController.activeConversationId) || (!VoiceCallService.callActive && ChatController.activeConversationId.length > 0)
                    onTriggered: bar.startCall()
                }
                Controls.MenuItem {
                    text: ToolCallLogModel.count > 0 ? qsTr("Tool Log (%1)").arg(ToolCallLogModel.count) : qsTr("Tool Log")
                    icon.name: "utilities-terminal"
                    onTriggered: bar.openTerminal()
                }
            }
        }

        Item {
            visible: !bar.compact && VoiceCallService.running
            Layout.preferredWidth: visible ? callBtn.implicitWidth : 0
            Layout.preferredHeight: callBtn.implicitHeight

            AppButton {
                id: callBtn
                anchors.left: parent.left
                readonly property bool callHere: VoiceCallService.callActive && VoiceCallService.callConversationId === ChatController.activeConversationId
                enabled: callHere || (!VoiceCallService.callActive && ChatController.activeConversationId.length > 0)
                icon.name: VoiceCallService.callActive ? "call-stop" : "call-start"
                highlighted: true
                iconOnly: true
                text: callHere ? qsTr("Return to the call") : qsTr("Voice call")
                onClicked: bar.startCall()
                Controls.ToolTip.text: {
                    if (callBtn.callHere)
                        return qsTr("Call in progress: click to return " + "to it");
                    if (VoiceCallService.callActive)
                        return qsTr("End the call in the other " + "conversation first");
                    if (ChatController.activeConversationId.length === 0)
                        return qsTr("Open a conversation to start a call");
                    return qsTr("Start a voice call");
                }
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }

            Rectangle {
                visible: VoiceCallService.callActive
                anchors.left: callBtn.right
                anchors.leftMargin: -6
                anchors.verticalCenter: callBtn.top
                anchors.verticalCenterOffset: 6
                width: 10
                height: 10
                radius: height / 2
                color: Kirigami.Theme.positiveTextColor
                border.width: 1
                border.color: Kirigami.Theme.backgroundColor
            }
        }

        Item {
            visible: !bar.compact
            Layout.preferredWidth: visible ? (artifactsBtn.implicitWidth + (artifactsBadge.visible ? 22 : 0)) : 0
            Layout.preferredHeight: artifactsBtn.implicitHeight

            AppButton {
                id: artifactsBtn
                anchors.left: parent.left
                icon.name: "folder-documents"
                flat: true
                iconOnly: true
                text: qsTr("Artifacts")
                onClicked: bar.openArtifacts()
                Controls.ToolTip.text: qsTr("Generated Files: %1 file(s)").arg(ArtifactsModel.count)
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }

            Rectangle {
                id: artifactsBadge
                anchors.left: artifactsBtn.right
                anchors.leftMargin: -4
                anchors.verticalCenter: artifactsBtn.top
                anchors.verticalCenterOffset: 6
                visible: ArtifactsModel.count > 0
                radius: height / 2
                implicitWidth: Math.max(16, badgeText.implicitWidth + 8)
                implicitHeight: 16
                color: Kirigami.Theme.positiveTextColor
                Controls.Label {
                    id: badgeText
                    anchors.centerIn: parent
                    text: ArtifactsModel.count
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.7
                    font.bold: true
                    color: Kirigami.Theme.backgroundColor
                }
            }
        }

        Item {
            id: plansButton
            visible: !bar.compact
            Layout.preferredWidth: visible ? (plansBtn.implicitWidth + (plansBadge.visible ? 22 : 0)) : 0
            Layout.preferredHeight: plansBtn.implicitHeight

            AppButton {
                id: plansBtn
                anchors.left: parent.left
                icon.name: "view-task"
                flat: true
                iconOnly: true
                text: qsTr("Plans")
                onClicked: bar.openPlans()
                Controls.ToolTip.text: qsTr("Plans: %1 in this chat").arg(PlansModel.count)
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }

            Rectangle {
                id: plansBadge
                anchors.left: plansBtn.right
                anchors.leftMargin: -4
                anchors.verticalCenter: plansBtn.top
                anchors.verticalCenterOffset: 6
                visible: PlansModel.count > 0
                radius: height / 2
                implicitWidth: Math.max(16, plansBadgeText.implicitWidth + 8)
                implicitHeight: 16
                color: Kirigami.Theme.neutralTextColor
                Controls.Label {
                    id: plansBadgeText
                    anchors.centerIn: parent
                    text: PlansModel.count
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.7
                    font.bold: true
                    color: Kirigami.Theme.backgroundColor
                }
            }
        }

        Item {
            visible: !bar.compact
            Layout.preferredWidth: visible ? (toolLogBtn.implicitWidth + (toolLogBadge.visible ? 22 : 0)) : 0
            Layout.preferredHeight: toolLogBtn.implicitHeight

            AppButton {
                id: toolLogBtn
                anchors.left: parent.left
                icon.name: "utilities-terminal"
                flat: true
                iconOnly: true
                text: qsTr("Tool Log")
                onClicked: bar.openTerminal()
                Controls.ToolTip.text: qsTr("Tool Activity Log: %1 call(s)").arg(ToolCallLogModel.count)
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600
            }

            Rectangle {
                id: toolLogBadge
                anchors.left: toolLogBtn.right
                anchors.leftMargin: -4
                anchors.verticalCenter: toolLogBtn.top
                anchors.verticalCenterOffset: 6
                visible: ToolCallLogModel.count > 0
                radius: height / 2
                implicitWidth: Math.max(16, toolLogBadgeText.implicitWidth + 8)
                implicitHeight: 16
                color: Kirigami.Theme.highlightColor
                Controls.Label {
                    id: toolLogBadgeText
                    anchors.centerIn: parent
                    text: ToolCallLogModel.count
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.7
                    font.bold: true
                    color: Kirigami.Theme.backgroundColor
                }
            }
        }

        Rectangle {
            width: 1
            Layout.preferredHeight: 24
            Layout.alignment: Qt.AlignVCenter
            color: ThemeController.borderSubtle
        }

        AppButton {
            icon.name: "configure"
            flat: true
            iconOnly: true
            text: qsTr("Chat Settings")
            checkable: true
            checked: bar.settingsActive
            onClicked: bar.toggleSettings()
        }

        AppButton {
            icon.name: "view-pim-notes"
            flat: true
            iconOnly: true
            text: qsTr("Canvas")
            checkable: true
            checked: bar.canvasActive
            visible: bar.canvasAvailable && !bar.canvasButtonHidden
            onClicked: bar.toggleCanvas()
            Controls.ToolTip.text: bar.canvasActive ? qsTr("Hide canvas") : qsTr("Show canvas")
            Controls.ToolTip.visible: hovered
        }
    }
}

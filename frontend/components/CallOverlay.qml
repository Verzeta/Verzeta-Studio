// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Item {
    id: root
    anchors.fill: parent
    visible: VoiceCallService.callActive && VoiceCallService.callConversationId === root.conversationId
    z: 100

    property string conversationId: ""

    property bool minimized: false

    onVisibleChanged: if (!visible)
        minimized = false

    readonly property var _members: conversationId.length > 0 ? Conversations.groupMembers(conversationId) : []

    function startCall() {
        if (conversationId.length === 0)
            return;
        if (VoiceCallService.callActive) {
            if (VoiceCallService.callConversationId === conversationId)
                minimized = false;
            return;
        }
        var map = {};
        var voices = VoiceCallService.voices;
        for (var i = 0; i < _members.length; ++i) {
            var alias = _members[i].alias || "";
            if (alias.length > 0 && voices.length > 0)
                map[alias] = voices[i % voices.length];
        }
        VoiceCallService.startCall(conversationId, map, _members.length > 0);
    }

    Rectangle {
        anchors.fill: parent
        visible: !root.minimized
        color: ThemeController.surfacePage
        opacity: 0.97

        MouseArea {
            anchors.fill: parent
        }
    }

    ColumnLayout {
        anchors.fill: parent
        visible: !root.minimized
        anchors.margins: Kirigami.Units.largeSpacing * 2
        spacing: Kirigami.Units.largeSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                source: "call-start"
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
                color: VoiceCallService.callLive ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
            }

            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true

                Kirigami.Heading {
                    text: qsTr("Voice call")
                    level: 2
                }
                Controls.Label {
                    text: VoiceCallService.lastError.length > 0 ? VoiceCallService.lastError : (VoiceCallService.callLive ? (VoiceCallService.speakingAlias.length > 0 ? qsTr("%1 is speaking").arg(VoiceCallService.speakingAlias) : (VoiceCallService.talking ? qsTr("Listening to you") : qsTr("Connected. Hold the talk button to speak."))) : qsTr("Connecting to the voice service…"))
                    color: VoiceCallService.lastError.length > 0 ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            AppButton {
                icon.name: "window-minimize"
                flat: true
                iconOnly: true
                text: qsTr("Minimize the call")
                onClicked: root.minimized = true
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        GridView {
            id: tiles
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            readonly property int columns: Math.max(1, Math.min(3, Math.floor(width / 260)))
            cellWidth: width / columns
            cellHeight: Math.max(160, Math.min(240, cellWidth * 0.72))

            model: root._members.length > 0 ? root._members.length + 1 : 2

            delegate: Item {
                required property int index
                width: tiles.cellWidth
                height: tiles.cellHeight

                readonly property bool isUser: index === 0
                readonly property string participant: isUser ? qsTr("You") : (root._members.length > 0 ? (root._members[index - 1].alias || "") : qsTr("Assistant"))
                readonly property bool active: isUser ? VoiceCallService.talking : (VoiceCallService.speakingAlias.length > 0 && VoiceCallService.speakingAlias === participant)

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.width: parent.active ? 2 : 1
                    border.color: parent.active ? Kirigami.Theme.highlightColor : ThemeController.borderSubtle

                    Behavior on border.color  {
                        ColorAnimation {
                            duration: 160
                        }
                    }

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: Kirigami.Units.smallSpacing

                        AgentAvatar {
                            agentName: participant
                            diameter: Kirigami.Units.gridUnit * 3
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Controls.Label {
                            text: participant
                            font.bold: true
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            Layout.maximumWidth: tiles.cellWidth - 32
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Controls.Label {
                            text: active ? qsTr("Speaking…") : " "
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.smallFont.pointSize
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                }
            }
        }

        Rectangle {
            visible: VoiceCallService.caption.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: captionLabel.implicitHeight + Kirigami.Units.largeSpacing
            radius: ThemeController.radius
            color: ThemeController.surfaceCard

            Controls.Label {
                id: captionLabel
                anchors.fill: parent
                anchors.margins: Kirigami.Units.smallSpacing * 2
                text: VoiceCallService.caption
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: controlRow.implicitHeight + Kirigami.Units.largeSpacing * 2
            radius: ThemeController.radius
            color: ThemeController.surfaceCard
            border.width: 1
            border.color: ThemeController.borderSubtle

            RowLayout {
                id: controlRow
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.largeSpacing * 2
                spacing: Kirigami.Units.largeSpacing

                Controls.ComboBox {
                    visible: root._members.length > 0
                    Layout.preferredWidth: 180
                    model: {
                        var names = [qsTr("Everyone")];
                        for (var i = 0; i < root._members.length; ++i)
                            names.push(root._members[i].alias || "");
                        return names;
                    }
                    currentIndex: 0
                    onActivated: VoiceCallService.targetAlias = (currentIndex === 0 ? "" : currentText)
                }

                Item {
                    Layout.fillWidth: true
                }

                AppButton {
                    id: talkButton
                    enabled: VoiceCallService.callLive
                    icon.name: "audio-input-microphone"
                    text: VoiceCallService.talking ? qsTr("Listening… release to send") : (VoiceCallService.targetAlias.length > 0 ? qsTr("Hold to talk to %1").arg(VoiceCallService.targetAlias) : qsTr("Hold to talk"))
                    highlighted: VoiceCallService.talking
                    Layout.preferredWidth: 280
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2.5
                    onPressed: VoiceCallService.setTalking(true)
                    onReleased: VoiceCallService.setTalking(false)
                    onCanceled: VoiceCallService.setTalking(false)
                }

                Item {
                    Layout.fillWidth: true
                }

                AppButton {
                    enabled: VoiceCallService.speakingAlias.length > 0
                    icon.name: "media-playback-stop"
                    text: qsTr("Stop speaking")
                    onClicked: VoiceCallService.stopSpeaking()
                }

                AppButton {
                    icon.name: "call-stop"
                    text: qsTr("End call")
                    onClicked: VoiceCallService.endCall()
                }
            }
        }
    }

    Rectangle {
        id: compactBar
        visible: root.minimized
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Kirigami.Units.largeSpacing
        height: compactTalk.height + Kirigami.Units.largeSpacing * 2
        radius: ThemeController.radius
        color: ThemeController.surfaceCard
        border.width: 1
        border.color: ThemeController.borderSubtle

        RowLayout {
            id: compactStatus
            anchors.left: parent.left
            anchors.leftMargin: Kirigami.Units.largeSpacing
            anchors.verticalCenter: parent.verticalCenter
            spacing: Kirigami.Units.smallSpacing
            width: Math.max(0, (compactBar.width - compactTalk.width) / 2 - Kirigami.Units.largeSpacing * 2)

            Kirigami.Icon {
                source: "call-start"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
                color: VoiceCallService.callLive ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
            }

            Controls.Label {
                text: VoiceCallService.speakingAlias.length > 0 ? qsTr("%1 is speaking").arg(VoiceCallService.speakingAlias) : (VoiceCallService.talking ? qsTr("Listening to you") : qsTr("On a call"))
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        AppButton {
            id: compactTalk
            anchors.centerIn: parent
            width: Math.min(280, compactBar.width * 0.4)
            height: Kirigami.Units.gridUnit * 2.5
            enabled: VoiceCallService.callLive
            icon.name: "audio-input-microphone"
            text: VoiceCallService.talking ? qsTr("Listening… release to send") : qsTr("Hold to talk")
            highlighted: true
            onPressed: VoiceCallService.setTalking(true)
            onReleased: VoiceCallService.setTalking(false)
            onCanceled: VoiceCallService.setTalking(false)
        }

        RowLayout {
            anchors.right: parent.right
            anchors.rightMargin: Kirigami.Units.largeSpacing
            anchors.verticalCenter: parent.verticalCenter
            spacing: Kirigami.Units.smallSpacing

            AppButton {
                enabled: VoiceCallService.speakingAlias.length > 0
                icon.name: "media-playback-stop"
                flat: true
                iconOnly: true
                text: qsTr("Stop speaking")
                onClicked: VoiceCallService.stopSpeaking()
            }

            AppButton {
                icon.name: "window-restore"
                flat: true
                iconOnly: true
                text: qsTr("Show the call")
                onClicked: root.minimized = false
            }

            AppButton {
                icon.name: "call-stop"
                flat: true
                iconOnly: true
                text: qsTr("End call")
                onClicked: VoiceCallService.endCall()
            }
        }
    }
}

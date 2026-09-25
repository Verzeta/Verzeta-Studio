// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: kickoff
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 42)
    implicitHeight: Math.min(applicationWindow().height * 0.85, Kirigami.Units.gridUnit * 32)

    readonly property color _cPanel: Qt.darker(Kirigami.Theme.backgroundColor, 0.6)

    property string _folderId: ""
    property string _folderName: ""
    property int _memberCount: 0
    property var _memberNames: []

    signal finished(string openConversationId)

    function openForProject(folderId, folderName, memberCount) {
        kickoff._folderId = folderId;
        kickoff._folderName = folderName;
        kickoff._memberCount = memberCount;
        var members = MembershipService.projectMembersList(folderId);
        var names = [];
        for (var i = 0; i < members.length; ++i) {
            names.push(members[i].alias || "");
        }
        kickoff._memberNames = names;
        individualCheck.checked = true;
        groupCheck.checked = memberCount >= 2;
        kickoff.open();
    }

    function _start() {
        if (individualCheck.checked) {
            Conversations.createIndividualChatsForProject(kickoff._folderId);
        }
        var openId = "";
        if (groupCheck.checked && kickoff._memberCount >= 2) {
            var id = Conversations.createGroupChatForProject(kickoff._folderId);
            if (id.length > 0)
                openId = id;
        }
        kickoff.close();
        kickoff.finished(openId);
    }

    title: qsTr("Start chatting with your team")
    subtitle: kickoff._folderName
    dialogIcon: "dialog-messages"

    footer: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Not now")
            onClicked: {
                kickoff.close();
                kickoff.finished("");
            }
        }
        AppButton {
            text: qsTr("Start  →")
            highlighted: true
            enabled: individualCheck.checked || groupCheck.checked
            onClicked: kickoff._start()
        }
    }

    Item {
        id: bodyRoot
        implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 40)
        implicitHeight: panel.implicitHeight

        Rectangle {
            id: panel
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            implicitHeight: panelCol.implicitHeight + Kirigami.Units.gridUnit * 2
            radius: ThemeController.radius
            color: kickoff._cPanel
            antialiasing: true

            ColumnLayout {
                id: panelCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.largeSpacing

                RowLayout {
                    Layout.fillWidth: true
                    spacing: -(Kirigami.Units.gridUnit * 1.6 * 0.34)

                    Repeater {
                        model: kickoff._memberNames
                        delegate: AgentAvatar {
                            agentName: modelData
                            diameter: Kirigami.Units.gridUnit * 1.9
                        }
                    }
                    Item {
                        Layout.preferredWidth: Kirigami.Units.largeSpacing
                    }
                    Controls.Label {
                        text: kickoff._memberCount + qsTr(" teammates ready")
                        color: Kirigami.Theme.textColor
                        font.family: ThemeController.fontFamily
                        font.bold: true
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    text: qsTr("Your project room is ready. Pick how you " + "want to start talking to the team. You can " + "always do the other later from the sidebar.")
                    color: Kirigami.Theme.disabledTextColor
                    font.family: ThemeController.fontFamily
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    wrapMode: Text.WordWrap
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Controls.CheckBox {
                        id: individualCheck
                        text: qsTr("Create a 1:1 chat with each member")
                        font.family: ThemeController.fontFamily
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.gridUnit * 1.6
                        text: qsTr("One private chat per teammate. Talk to " + "each agent directly. Existing 1:1 chats are " + "reused, never duplicated.")
                        color: Kirigami.Theme.disabledTextColor
                        font.family: ThemeController.fontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Controls.CheckBox {
                        id: groupCheck
                        text: qsTr("Start a group chat with everyone")
                        font.family: ThemeController.fontFamily
                        enabled: kickoff._memberCount >= 2
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.gridUnit * 1.6
                        text: kickoff._memberCount >= 2 ? qsTr("One team chat with everyone in it. " + "@mention a member to address them directly. " + "You'll land straight in this chat.") : qsTr("A group chat needs at least 2 members.")
                        color: Kirigami.Theme.disabledTextColor
                        font.family: ThemeController.fontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}

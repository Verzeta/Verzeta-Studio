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
    implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 40)

    property string folderId: ""

    property var allowedAgentIds: []

    property var preloadedMembers: []

    property string folderName: ""

    onOpened: {
        titleField.text = "";
        if (dialog.folderId.length > 0) {
            var info = Conversations.folderInfo(dialog.folderId);
            dialog.folderName = info && info.name ? info.name : "";
        } else {
            dialog.folderName = "";
        }
        editor.initialMembers = dialog.preloadedMembers || [];
        editor.allowedAgentIds = dialog.allowedAgentIds;
        editor.softDelete = (dialog.folderId.length > 0);
        editor.reload();
    }

    title: qsTr("New Group Chat")
    dialogIcon: "resource-group-new"

    footer: RowLayout {
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Cancel")
            onClicked: dialog.close()
        }
        AppButton {
            text: qsTr("Create")
            highlighted: true
            enabled: titleField.text.trim().length > 0 && editor.memberList().length >= 2
            onClicked: {
                var members = editor.memberList();
                var hasCoord = false;
                for (var i = 0; i < members.length; ++i) {
                    if (members[i].isCoordinator) {
                        hasCoord = true;
                        break;
                    }
                }
                if (!hasCoord && members.length > 0) {
                    members[0].isCoordinator = true;
                }
                var id = Conversations.newGroupConversation(titleField.text.trim(), members, dialog.folderId);
                if (id.length > 0) {
                    if (dialog.folderId.length > 0) {
                        var preloadedSet = {};
                        var pmList = dialog.preloadedMembers || [];
                        for (var j = 0; j < pmList.length; ++j) {
                            var pm = pmList[j];
                            var pmKey = (pm.agentId || "") + "|" + (pm.alias || "").toLowerCase();
                            preloadedSet[pmKey] = true;
                        }
                        for (var k = 0; k < members.length; ++k) {
                            var m = members[k];
                            var key = (m.agentId || "") + "|" + (m.alias || "").toLowerCase();
                            if (!preloadedSet[key]) {
                                MembershipService.addProjectMemberMap({
                                        "folderId": dialog.folderId,
                                        "agentId": m.agentId,
                                        "alias": m.alias,
                                        "isCoordinator": false,
                                        "modelProvider": m.modelProvider || "",
                                        "modelName": m.modelName || "",
                                        "allowedTools": m.allowedTools || []
                                    });
                            }
                        }
                    }
                    ChatController.switchConversation(id);
                    dialog.close();
                }
            }
        }
    }

    ColumnLayout {
        spacing: 14

        Rectangle {
            visible: dialog.folderName.length > 0
            Layout.fillWidth: true
            Layout.topMargin: 4
            implicitHeight: scopeRow.implicitHeight + 12
            radius: ThemeController.radius
            color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.10)
            border.color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.35)
            border.width: 1

            RowLayout {
                id: scopeRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8

                Kirigami.Icon {
                    source: "view-calendar-tasks"
                    fallback: "folder-tasks"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                    color: Kirigami.Theme.highlightColor
                }
                Controls.Label {
                    text: qsTr("Scoped to project \"%1\": you can only add members from this project's roster.").arg(dialog.folderName)
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        Controls.Label {
            text: qsTr("Title (Required)")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.topMargin: 4
        }
        AppTextField {
            id: titleField
            Layout.fillWidth: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            placeholderText: qsTr("e.g. Q2 Campaign Planning")
        }

        Controls.Label {
            text: qsTr("Members (Required: at least 2)")
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }
        Controls.Label {
            text: qsTr("Add at least two members. The same agent template can be added multiple " + "times with different aliases (e.g. two Engineers named \"Alice\" and \"Bob\"). " + "In the chat, @mention a member to address them specifically.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        MembershipEditor {
            id: editor
            Layout.fillWidth: true
        }
    }
}

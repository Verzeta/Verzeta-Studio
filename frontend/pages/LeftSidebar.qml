// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: sidebar
    color: ThemeController.surfaceCard

    signal newChat
    signal conversationActivated(string convId)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 52
            color: "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6

                AppButton {
                    text: qsTr("New Chat")
                    icon.name: "list-add"
                    onClicked: {
                        var id = Conversations.newConversation();
                        if (id.length > 0)
                            ChatController.switchConversation(id);
                        sidebar.newChat();
                    }
                    Layout.fillWidth: true
                }

                AppButton {
                    icon.name: "user-group-new"
                    flat: true
                    iconOnly: true
                    text: qsTr("New Group Chat")
                    onClicked: {
                        groupChatDialog.folderId = "";
                        groupChatDialog.allowedAgentIds = [];
                        groupChatDialog.preloadedMembers = [];
                        groupChatDialog.open();
                    }
                    Controls.ToolTip.text: qsTr("New Group Chat")
                    Controls.ToolTip.visible: hovered
                }

                AppButton {
                    icon.name: "folder-new"
                    flat: true
                    iconOnly: true
                    text: qsTr("New Folder")
                    onClicked: newFolderDialog.open()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Layout.bottomMargin: 6
            implicitHeight: Math.max(44, Kirigami.Units.gridUnit * 2.2)
            radius: ThemeController.radius
            color: ThemeController.surfaceSunken

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 4
                spacing: 4

                Kirigami.Icon {
                    source: "search"
                    fallback: "system-search"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                    color: Kirigami.Theme.disabledTextColor
                    Layout.alignment: Qt.AlignVCenter
                }

                Controls.TextField {
                    id: searchField
                    placeholderText: qsTr("Search…")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    background: null
                    color: Kirigami.Theme.textColor
                    placeholderTextColor: Kirigami.Theme.disabledTextColor
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    focus: false
                    activeFocusOnTab: true
                    onTextChanged: SidebarModel.filterText = text

                    Keys.onEscapePressed: {
                        text = "";
                        focus = false;
                    }
                }

                AppButton {
                    icon.name: "edit-clear"
                    flat: true
                    iconOnly: true
                    visible: searchField.text.length > 0
                    implicitWidth: Math.max(44, Kirigami.Units.gridUnit * 2.2)
                    implicitHeight: Math.max(44, Kirigami.Units.gridUnit * 2.2)
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: {
                        searchField.text = "";
                        searchField.focus = false;
                    }
                    Controls.ToolTip.text: qsTr("Clear search")
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        ListView {
            id: convList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: SidebarModel

            delegate: Loader {
                id: rowLoader
                width: convList.width
                required property string itemType
                required property string itemId
                required property string itemTitle
                required property string itemSubtitle
                required property int itemChildCount
                required property string folderKind
                required property bool folderExpanded
                required property string memberAlias
                required property string memberAgentId
                required property string memberAgentName
                required property bool memberIsCoordinator
                required property string memberIconName
                required property string parentFolderId
                required property bool isActive
                required property bool isPinned
                required property string sectionId
                required property bool sectionCollapsible
                required property bool sectionCollapsed
                required property int depth
                required property int index

                sourceComponent: {
                    if (itemType === "folder")
                        return folderComp;
                    if (itemType === "section_header")
                        return sectionHeaderComp;
                    if (itemType === "subsection_header")
                        return sectionHeaderComp;
                    if (itemType === "membersHeader")
                        return sectionHeaderComp;
                    if (itemType === "convsHeader")
                        return sectionHeaderComp;
                    if (itemType === "member")
                        return memberComp;
                    if (itemType === "groupChatAction")
                        return groupChatActionComp;
                    return convComp;
                }

                onLoaded: {
                    if (itemType === "folder") {
                        item.folderId = rowLoader.itemId;
                        item.title = rowLoader.itemTitle;
                        item.expanded = Qt.binding(function () {
                                return rowLoader.folderExpanded;
                            });
                        item.childCount = rowLoader.itemChildCount;
                        item.folderType = rowLoader.folderKind.length > 0 ? rowLoader.folderKind : "regular";
                    } else if (itemType === "section_header" || itemType === "subsection_header") {
                        item.title = rowLoader.itemTitle;
                        item.sectionId = rowLoader.sectionId;
                        item.count = rowLoader.itemChildCount;
                        item.collapsible = rowLoader.sectionCollapsible;
                        item.depth = rowLoader.depth;
                        item.collapsed = Qt.binding(function () {
                                return rowLoader.sectionCollapsed;
                            });
                    } else if (itemType === "membersHeader" || itemType === "convsHeader") {
                        item.title = rowLoader.itemTitle;
                        item.sectionId = "";
                        item.count = 0;
                        item.collapsible = false;
                        item.depth = rowLoader.depth;
                    } else if (itemType === "member") {
                        item.folderId = rowLoader.parentFolderId;
                        item.alias = rowLoader.memberAlias;
                        item.agentId = rowLoader.memberAgentId;
                        item.isCoordinator = rowLoader.memberIsCoordinator;
                        item.iconName = rowLoader.memberIconName;
                        item.agentName = rowLoader.memberAgentName;
                    } else if (itemType === "groupChatAction") {
                        item.folderId = rowLoader.parentFolderId;
                    } else {
                        item.convId = rowLoader.itemId;
                        item.title = rowLoader.itemTitle;
                        item.updatedAt = rowLoader.itemSubtitle;
                        item.isActive = Qt.binding(function () {
                                return rowLoader.isActive;
                            });
                        item.isPinned = Qt.binding(function () {
                                return rowLoader.isPinned;
                            });
                    }
                }
            }

            Controls.Label {
                anchors.centerIn: parent
                visible: convList.count === 0
                text: searchField.text.length > 0 ? qsTr("No matching conversations") : qsTr("No conversations yet")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                width: parent.width - 40
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    Component {
        id: folderComp
        FolderSectionHeader {
            folderId: ""
            title: ""
            width: convList.width
            onFolderToggled: function (fid) {
                SidebarModel.toggleFolder(fid);
            }
            onConfigureFolderRequested: function (fid) {
                folderSettingsDialog.openFor(fid);
            }
        }
    }

    Component {
        id: sectionHeaderComp
        SidebarSectionHeader {
            width: convList.width
        }
    }

    Component {
        id: memberComp
        Controls.ItemDelegate {
            id: memberRoot
            property string folderId: ""
            property string alias: ""
            property string agentId: ""
            property bool isCoordinator: false
            property string iconName: ""
            property string agentName: ""

            width: convList.width
            height: 44
            leftPadding: 28
            rightPadding: 8

            onClicked: {
                var id = Conversations.openDirectChatWithMember(folderId, agentId, alias);
                if (id.length > 0)
                    ChatController.switchConversation(id);
                sidebar.conversationActivated(ChatController.activeConversationId);
            }

            background: Rectangle {
                color: memberRoot.hovered === true ? ThemeController.hoverTint : "transparent"
                Behavior on color  {
                    ColorAnimation {
                        duration: 80
                    }
                }
            }

            contentItem: RowLayout {
                spacing: 8

                Kirigami.Icon {
                    source: memberRoot.iconName
                    fallback: "user"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                    Layout.alignment: Qt.AlignVCenter
                    color: Kirigami.Theme.textColor
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    RowLayout {
                        spacing: 4

                        Controls.Label {
                            text: "@" + memberRoot.alias.replace(/ /g, "_")
                            font.bold: true
                            font.family: ThemeController.codeFontFamily
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            color: Kirigami.Theme.textColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            visible: memberRoot.isCoordinator
                            text: "⭐"
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            color: Kirigami.Theme.highlightColor
                        }
                    }

                    Controls.Label {
                        text: memberRoot.agentName
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                Controls.ToolButton {
                    icon.name: "dialog-messages"
                    Controls.ToolTip.text: qsTr("Chat with @%1").arg(memberRoot.alias.replace(/ /g, "_"))
                    Controls.ToolTip.visible: hovered
                    onClicked: {
                        var id = Conversations.openDirectChatWithMember(memberRoot.folderId, memberRoot.agentId, memberRoot.alias);
                        if (id.length > 0)
                            ChatController.switchConversation(id);
                        sidebar.conversationActivated(ChatController.activeConversationId);
                    }
                }
            }
        }
    }

    Component {
        id: groupChatActionComp
        Controls.ItemDelegate {
            id: actionRoot
            property string folderId: ""

            width: convList.width
            height: 36
            leftPadding: 28
            rightPadding: 8

            onClicked: {
                var members = MembershipService.projectMembersList(folderId);
                var ids = [];
                for (var i = 0; i < members.length; ++i) {
                    if (ids.indexOf(members[i].agentId) < 0) {
                        ids.push(members[i].agentId);
                    }
                }
                groupChatDialog.folderId = folderId;
                groupChatDialog.allowedAgentIds = ids;
                groupChatDialog.preloadedMembers = members;
                groupChatDialog.open();
            }

            background: Rectangle {
                color: actionRoot.hovered === true ? ThemeController.hoverTint : "transparent"
                Behavior on color  {
                    ColorAnimation {
                        duration: 80
                    }
                }
            }

            contentItem: RowLayout {
                spacing: 8

                Kirigami.Icon {
                    source: "user-group-new"
                    fallback: "system-users"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                    Layout.alignment: Qt.AlignVCenter
                    color: Kirigami.Theme.highlightColor
                }

                Controls.Label {
                    text: qsTr("Start Group Chat…")
                    font.italic: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    color: Kirigami.Theme.highlightColor
                    Layout.fillWidth: true
                }
            }
        }
    }

    FolderSettingsDialog {
        id: folderSettingsDialog
    }

    NewGroupChatDialog {
        id: groupChatDialog
    }

    Component {
        id: convComp
        SidebarConvItem {
            convId: ""
            title: ""
            width: convList.width
            onActivated: function (cid) {
                ChatController.switchConversation(cid);
                sidebar.conversationActivated(cid);
            }
            onRenameRequested: function (cid, currentTitle) {
                renameDialog.conversationId = cid;
                renameDialog.currentTitle = currentTitle;
                renameDialog.open();
            }
            onDeleteRequested: function (cid) {
                Conversations.deleteConversation(cid);
            }
            onExportRequested: function (cid) {
                ChatController.switchConversation(cid);
                sidebar.exportRequested(cid);
            }
            onMoveToFolderRequested: function (cid) {
                moveToFolderDialog.targetConvId = cid;
                moveToFolderDialog.open();
            }
            onPinRequested: function (cid, pinned) {
                ConversationService.setConversationPinned(cid, pinned);
            }
        }
    }

    signal exportRequested(string convId)

    Kirigami.PromptDialog {
        id: renameDialog
        property string conversationId: ""
        property string currentTitle: ""

        title: qsTr("Rename Conversation")
        subtitle: qsTr("Enter a new title:")

        AppTextField {
            id: renameField
            text: renameDialog.currentTitle
            placeholderText: qsTr("Conversation title")
            Layout.fillWidth: true
            onAccepted: renameDialog.accept()
            Component.onCompleted: selectAll()
        }

        onOpened: {
            renameField.text = currentTitle;
            renameField.selectAll();
            renameField.forceActiveFocus();
        }

        onAccepted: {
            var newTitle = renameField.text.trim();
            if (newTitle.length > 0 && newTitle !== currentTitle) {
                Conversations.renameConversation(conversationId, newTitle);
            }
        }
    }

    Kirigami.PromptDialog {
        id: newFolderDialog
        title: qsTr("New Folder")
        subtitle: qsTr("Enter a name for the folder:")

        AppTextField {
            id: folderNameField
            placeholderText: qsTr("Folder name")
            Layout.fillWidth: true
            onAccepted: newFolderDialog.accept()
        }

        onOpened: {
            folderNameField.text = "";
            folderNameField.forceActiveFocus();
        }

        onAccepted: {
            var name = folderNameField.text.trim();
            if (name.length > 0) {
                Conversations.createFolder(name);
            }
        }
    }

    Kirigami.Dialog {
        id: moveToFolderDialog
        title: qsTr("Move to Folder")
        preferredWidth: Kirigami.Units.gridUnit * 20
        standardButtons: Kirigami.Dialog.Cancel

        property string targetConvId: ""

        ColumnLayout {
            spacing: 4
            width: parent ? parent.width : 200

            Controls.ItemDelegate {
                text: qsTr("Root (no folder)")
                icon.name: "folder"
                Layout.fillWidth: true
                onClicked: {
                    Conversations.moveToFolder(moveToFolderDialog.targetConvId, "");
                    moveToFolderDialog.close();
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            Repeater {
                model: folderListModel

                Controls.ItemDelegate {
                    text: model.folderName
                    icon.name: "folder"
                    Layout.fillWidth: true
                    onClicked: {
                        Conversations.moveToFolder(moveToFolderDialog.targetConvId, model.folderId);
                        moveToFolderDialog.close();
                    }
                }
            }
        }

        onOpened: rebuildFolderList()
    }

    ListModel {
        id: folderListModel
    }

    function rebuildFolderList() {
        folderListModel.clear();
        var mdl = ConversationListModel;
        var rootCount = mdl.rowCount();
        for (var i = 0; i < rootCount; ++i) {
            var idx = mdl.index(i, 0);
            if (mdl.data(idx, 259) === "folder") {
                folderListModel.append({
                        "folderId": mdl.data(idx, 257),
                        "folderName": mdl.data(idx, 258)
                    });
            }
        }
    }
}

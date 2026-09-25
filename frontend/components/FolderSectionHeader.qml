// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.ItemDelegate {
    id: root

    required property string folderId
    required property string title
    property string folderType: "regular"
    property bool expanded: true
    property int childCount: 0

    signal folderToggled(string folderId)
    signal configureFolderRequested(string folderId)

    property bool _animationsArmed: false
    Component.onCompleted: Qt.callLater(function () {
            if (root)
                root._animationsArmed = true;
        })

    height: 40
    leftPadding: 8
    rightPadding: 8

    onClicked: root.folderToggled(root.folderId)

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: function (mouse) {
            folderMenu.popup();
        }
    }

    background: Rectangle {
        color: root.hovered === true ? ThemeController.hoverTint : "transparent"
        Behavior on color  {
            enabled: root._animationsArmed
            ColorAnimation {
                duration: 80
            }
        }
    }

    contentItem: RowLayout {
        spacing: 6

        Kirigami.Icon {
            source: "arrow-right"
            fallback: "arrow-down"
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            color: Kirigami.Theme.disabledTextColor
            Layout.alignment: Qt.AlignVCenter
            rotation: root.expanded ? 90 : 0
        }

        Kirigami.Icon {
            source: {
                if (root.folderType === "organization")
                    return "user-group-new";
                if (root.folderType === "project")
                    return "view-calendar-tasks";
                return root.expanded ? "folder-open" : "folder";
            }
            fallback: "folder"
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            color: root.folderType === "regular" ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.highlightColor
            Layout.alignment: Qt.AlignVCenter
        }

        Controls.Label {
            text: root.title
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            font.bold: true
            font.capitalization: Font.AllUppercase
            color: root.folderType === "regular" ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.textColor
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        Controls.Label {
            text: root.childCount > 0 ? String(root.childCount) : ""
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            color: Kirigami.Theme.disabledTextColor
            visible: root.childCount > 0
            Layout.alignment: Qt.AlignVCenter
        }

        Controls.ToolButton {
            icon.name: "application-menu"
            visible: root.hovered
            Layout.preferredWidth: 24
            Layout.preferredHeight: 24
            onClicked: folderMenu.popup()
        }
    }

    Controls.Menu {
        id: folderMenu

        Controls.MenuItem {
            text: qsTr("Configure…")
            icon.name: "configure"
            onTriggered: root.configureFolderRequested(root.folderId)
        }
        Controls.MenuSeparator {
        }
        Controls.MenuItem {
            text: qsTr("Rename…")
            icon.name: "edit-rename"
            onTriggered: {
                renameFolderDialog.folderId = root.folderId;
                renameFolderDialog.currentName = root.title;
                renameFolderDialog.open();
            }
        }
        Controls.MenuItem {
            text: qsTr("Delete Folder")
            icon.name: "edit-delete"
            onTriggered: {
                deleteFolderDialog.folderId = root.folderId;
                deleteFolderDialog.folderName = root.title;
                deleteFolderDialog.open();
            }
        }
    }

    Kirigami.PromptDialog {
        id: renameFolderDialog
        property string folderId: ""
        property string currentName: ""
        title: qsTr("Rename Folder")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        AppTextField {
            id: renameFolderField
            Layout.fillWidth: true
            text: renameFolderDialog.currentName
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }

        onOpened: renameFolderField.selectAll()
        onAccepted: {
            if (renameFolderField.text.trim().length > 0) {
                Conversations.renameFolder(renameFolderDialog.folderId, renameFolderField.text.trim());
            }
        }
    }

    Kirigami.PromptDialog {
        id: deleteFolderDialog
        property string folderId: ""
        property string folderName: ""
        title: qsTr("Delete Folder")
        subtitle: qsTr("Delete folder \"%1\"? Conversations inside will be moved to root.").arg(folderName)
        standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.No
        onAccepted: Conversations.deleteFolder(deleteFolderDialog.folderId)
    }
}

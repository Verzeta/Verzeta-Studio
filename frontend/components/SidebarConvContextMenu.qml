// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls

Controls.Menu {
    id: root

    property string convId: ""

    property string convTitle: ""

    property bool isPinned: false

    signal pinRequested(string conversationId, bool pinned)
    signal renameRequested(string conversationId, string currentTitle)
    signal exportRequested(string conversationId)
    signal moveToFolderRequested(string conversationId)
    signal deleteRequested(string conversationId)

    function openFor(id, title, pinned) {
        root.convId = id;
        root.convTitle = title;
        root.isPinned = pinned;
        root.popup();
    }

    Controls.MenuItem {
        text: root.isPinned ? qsTr("Unpin") : qsTr("Pin to top")
        icon.name: root.isPinned ? "bookmark-remove" : "bookmark-new-symbolic"
        onTriggered: root.pinRequested(root.convId, !root.isPinned)
    }

    Controls.MenuSeparator {
    }

    Controls.MenuItem {
        text: qsTr("Rename…")
        icon.name: "edit-rename"
        onTriggered: root.renameRequested(root.convId, root.convTitle)
    }
    Controls.MenuItem {
        text: qsTr("Export…")
        icon.name: "document-export"
        onTriggered: root.exportRequested(root.convId)
    }
    Controls.MenuItem {
        text: qsTr("Move to Folder…")
        icon.name: "folder-move"
        onTriggered: root.moveToFolderRequested(root.convId)
    }
    Controls.MenuSeparator {
    }
    Controls.MenuItem {
        text: qsTr("Delete")
        icon.name: "edit-delete"
        onTriggered: root.deleteRequested(root.convId)
    }
}

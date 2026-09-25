// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.ItemDelegate {
    id: root

    required property string convId
    required property string title
    property string updatedAt: ""
    property bool isActive: false
    property bool isPinned: false

    property bool isGenerating: false

    function _refreshGenerating() {
        root.isGenerating = root.convId.length > 0 && ChatController.isConversationGenerating(root.convId);
    }

    Connections {
        target: ChatController
        function onGeneratingConversationsChanged() {
            root._refreshGenerating();
        }
    }

    onConvIdChanged: root._refreshGenerating()

    signal activated(string conversationId)
    signal renameRequested(string conversationId, string currentTitle)
    signal deleteRequested(string conversationId)
    signal exportRequested(string conversationId)
    signal moveToFolderRequested(string conversationId)
    signal pinRequested(string conversationId, bool pinned)

    property bool _animationsArmed: false
    Component.onCompleted: {
        root._refreshGenerating();
        Qt.callLater(function () {
                if (root)
                    root._animationsArmed = true;
            });
    }

    height: 44
    leftPadding: 12
    rightPadding: 8

    highlighted: isActive

    onClicked: root.activated(root.convId)

    background: Rectangle {
        color: {
            if (root.isActive === true)
                return ThemeController.selectionTint;
            if (root.hovered === true)
                return ThemeController.hoverTint;
            return "transparent";
        }
        radius: ThemeController.radius
        Behavior on color  {
            enabled: root._animationsArmed
            ColorAnimation {
                duration: 80
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 1

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Controls.Label {
                visible: root.isPinned
                text: "📌"
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                color: Kirigami.Theme.textColor
                Layout.alignment: Qt.AlignVCenter
            }

            Controls.Label {
                text: root.title.length > 0 ? root.title : qsTr("(untitled)")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                font.bold: root.isActive
                color: Kirigami.Theme.textColor
                elide: Text.ElideRight
                maximumLineCount: 1
                Layout.fillWidth: true
            }

            Controls.BusyIndicator {
                id: generatingSpinner
                visible: root.isGenerating
                running: visible
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: implicitHeight
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: qsTr("Generating")
            }
        }

        Controls.Label {
            text: {
                if (root.updatedAt.length === 0)
                    return "";
                var d = new Date(root.updatedAt);
                if (isNaN(d.getTime()))
                    return root.updatedAt;
                var now = new Date();
                var diffMs = now - d;
                var diffMins = Math.floor(diffMs / 60000);
                if (diffMins < 1)
                    return qsTr("just now");
                if (diffMins < 60)
                    return qsTr("%1m ago").arg(diffMins);
                var diffHrs = Math.floor(diffMins / 60);
                if (diffHrs < 24)
                    return qsTr("%1h ago").arg(diffHrs);
                var diffDays = Math.floor(diffHrs / 24);
                if (diffDays < 7)
                    return qsTr("%1d ago").arg(diffDays);
                return d.toLocaleDateString(Qt.locale(), Locale.ShortFormat);
            }
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            color: Kirigami.Theme.disabledTextColor
            elide: Text.ElideRight
            maximumLineCount: 1
            Layout.fillWidth: true
            visible: root.updatedAt.length > 0
        }
    }

    SidebarConvContextMenu {
        id: contextMenu
        onPinRequested: (cid, pin) => root.pinRequested(cid, pin);
        onRenameRequested: (cid, title) => root.renameRequested(cid, title);
        onExportRequested: cid => root.exportRequested(cid);
        onMoveToFolderRequested: cid => root.moveToFolderRequested(cid);
        onDeleteRequested: cid => root.deleteRequested(cid);
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        longPressThreshold: 600
        onTapped: function (eventPoint, button) {
            if (button === Qt.RightButton) {
                contextMenu.openFor(root.convId, root.title, root.isPinned);
            }
        }
        onLongPressed: {
            contextMenu.openFor(root.convId, root.title, root.isPinned);
        }
    }
}

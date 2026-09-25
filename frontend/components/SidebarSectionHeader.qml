// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.ItemDelegate {
    id: root

    property string title: ""

    property string sectionId: ""

    property int count: 0

    property bool collapsible: false

    property bool collapsed: false

    property int depth: 0

    property bool _animationsArmed: false
    Component.onCompleted: Qt.callLater(function () {
            if (root)
                root._animationsArmed = true;
        })

    width: parent ? parent.width : 0

    implicitHeight: collapsible ? Math.max(44, Kirigami.Units.gridUnit * 2.2) : 24
    leftPadding: 8 + root.depth * 8
    rightPadding: 8

    hoverEnabled: collapsible
    focusPolicy: collapsible ? Qt.StrongFocus : Qt.NoFocus

    onClicked: {
        if (root.collapsible && root.sectionId.length > 0) {
            SidebarModel.toggleSection(root.sectionId);
        }
    }

    background: Rectangle {
        color: (root.collapsible && root.hovered) === true ? ThemeController.hoverTint : "transparent"
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
            visible: root.collapsible
            source: root.collapsed ? "arrow-right" : "arrow-down"
            fallback: "arrow-down"
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            Layout.alignment: Qt.AlignVCenter
            color: Kirigami.Theme.disabledTextColor
        }

        Controls.Label {
            text: root.title
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * (root.collapsible ? 1.0 : 0.8)
            font.bold: root.collapsible
            font.capitalization: root.collapsible ? Font.MixedCase : Font.AllUppercase
            color: root.collapsible ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
        }

        Controls.Label {
            visible: root.count > 0
            text: root.count
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
            color: Kirigami.Theme.disabledTextColor
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
        }
    }
}

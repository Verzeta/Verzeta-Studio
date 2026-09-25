// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: navBar
    color: ThemeController.surfaceCard
    implicitWidth: 48

    property string activeSection: "home"

    signal sectionChanged(string section)

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 4

        IconNavButton {
            icon.name: "go-home"
            text: qsTr("Home")
            active: navBar.activeSection === "home"
            onClicked: {
                navBar.activeSection = "home";
                navBar.sectionChanged("home");
            }
        }

        IconNavButton {
            icon.name: "dialog-messages"
            text: qsTr("Chat")
            active: navBar.activeSection === "conversations"
            onClicked: {
                navBar.activeSection = "conversations";
                navBar.sectionChanged("conversations");
            }
        }

        IconNavButton {
            icon.name: "user-group-new"
            text: qsTr("Agents")
            active: navBar.activeSection === "agents"
            onClicked: {
                navBar.activeSection = "agents";
                navBar.sectionChanged("agents");
            }
        }

        IconNavButton {
            icon.name: "preferences-plugin"
            text: qsTr("Tools")
            active: navBar.activeSection === "tools"
            onClicked: {
                navBar.activeSection = "tools";
                navBar.sectionChanged("tools");
            }
        }

        IconNavButton {
            icon.name: "applications-education"
            text: qsTr("Skills")
            active: navBar.activeSection === "skills"
            onClicked: {
                navBar.activeSection = "skills";
                navBar.sectionChanged("skills");
            }
        }

        Item {
            Layout.fillHeight: true
        }

        IconNavButton {
            icon.name: "settings-configure"
            text: qsTr("Settings")
            active: navBar.activeSection === "settings"
            onClicked: {
                navBar.activeSection = "settings";
                navBar.sectionChanged("settings");
            }
        }
    }

    component IconNavButton: Controls.AbstractButton {
        id: navBtn
        property bool active: false

        Layout.fillWidth: true
        Layout.preferredHeight: 40
        hoverEnabled: true

        background: Rectangle {
            radius: ThemeController.radius
            anchors.margins: 4
            color: {
                if (navBtn.active)
                    return ThemeController.selectionTint;
                if (navBtn.hovered)
                    return Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.08);
                return "transparent";
            }
            Behavior on color  {
                ColorAnimation {
                    duration: 80
                }
            }
        }

        contentItem: Kirigami.Icon {
            source: navBtn.icon.name
            fallback: "application-x-executable"
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: Kirigami.Units.iconSizes.smallMedium
            color: navBtn.active ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
        }

        Controls.ToolTip.text: navBtn.text
        Controls.ToolTip.visible: navBtn.hovered
        Controls.ToolTip.delay: 400
    }
}

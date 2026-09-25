// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.ScrollablePage {
    id: modelSettingsPage

    title: qsTr("Model Settings")

    Kirigami.FormLayout {

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: qsTr("Active Model")
        }

        Controls.Label {
            Kirigami.FormData.label: qsTr("Provider:")
            text: AgentSettings.activeProvider.length > 0 ? AgentSettings.activeProvider : qsTr("(none)")
        }

        Controls.Label {
            Kirigami.FormData.label: qsTr("Model:")
            text: AgentSettings.activeModel.length > 0 ? AgentSettings.activeModel : qsTr("(none)")
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: qsTr("Change Model")
        }

        ModelSelector {
            Kirigami.FormData.label: qsTr("Select:")
        }

        Controls.Button {
            Kirigami.FormData.label: qsTr("Model List:")
            text: qsTr("Refresh")
            icon.name: "view-refresh"
            onClicked: {
                refreshStatusLabel.text = qsTr("Refreshing…");
                refreshStatusLabel.visible = true;
            }
        }

        Controls.Label {
            id: refreshStatusLabel
            visible: false
            Kirigami.FormData.label: ""
            color: Kirigami.Theme.positiveTextColor

            Timer {
                interval: 3000
                running: refreshStatusLabel.visible
                onTriggered: refreshStatusLabel.visible = false
            }
        }
    }
}

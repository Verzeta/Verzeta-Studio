// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Item {
    id: tab

    property var auditRows: []

    function reload() {
        auditRows = HeartbeatConfig.recentConfigChanges(100);
    }

    Connections {
        target: HeartbeatConfig
        function onConfigChanged(configId) {
            tab.reload();
        }
        function onConfigRemoved(configId) {
            tab.reload();
        }
    }

    Timer {
        interval: 5000
        running: tab.visible
        repeat: true
        onTriggered: tab.reload()
    }

    Component.onCompleted: tab.reload()

    Controls.ScrollView {
        id: scroll
        anchors.fill: parent
        Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
        clip: true

        Item {
            width: scroll.availableWidth
            implicitHeight: contentCol.implicitHeight + 32

            ColumnLayout {
                id: contentCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.topMargin: 16
                spacing: 12

                Controls.Label {
                    text: qsTr("Every change to a heartbeat config, by an " + "agent through the self-config tools, or " + "by you through chat / folder settings, " + "is recorded here. Tag 'agent' means the " + "change was made by the agent itself; " + "'user' means it was made through the UI.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Kirigami.Heading {
                    text: qsTr("Last 100 changes (newest first)")
                    level: 4
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 420
                    color: ThemeController.surfaceCard
                    radius: ThemeController.radius
                    border.color: ThemeController.borderSubtle
                    border.width: 1

                    ListView {
                        id: auditList
                        anchors.fill: parent
                        anchors.margins: 4
                        clip: true
                        model: tab.auditRows
                        spacing: 4

                        delegate: Rectangle {
                            id: auditCardItem
                            width: auditList.width
                            radius: ThemeController.radius
                            color: Kirigami.Theme.backgroundColor
                            implicitHeight: auditCard.implicitHeight + 12
                            border.color: ThemeController.borderSubtle
                            border.width: 1

                            required property var modelData

                            ColumnLayout {
                                id: auditCard
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 4

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Rectangle {
                                        radius: ThemeController.radius
                                        implicitWidth: srcLabel.implicitWidth + 12
                                        implicitHeight: srcLabel.implicitHeight + 4
                                        color: auditCardItem.modelData.source === "agent" ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.15) : Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.15)
                                        Controls.Label {
                                            id: srcLabel
                                            anchors.centerIn: parent
                                            text: auditCardItem.modelData.source
                                            color: auditCardItem.modelData.source === "agent" ? Kirigami.Theme.highlightColor : Kirigami.Theme.neutralTextColor
                                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                            font.bold: true
                                        }
                                    }

                                    Item {
                                        Layout.fillWidth: true
                                    }

                                    Controls.Label {
                                        text: {
                                            var ms = auditCardItem.modelData.changedAtMs;
                                            if (!ms || ms < 0)
                                                return "-";
                                            var d = new Date(ms);
                                            return d.toLocaleString(Qt.locale(), "yyyy-MM-dd HH:mm:ss");
                                        }
                                        color: Kirigami.Theme.disabledTextColor
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                        elide: Text.ElideRight
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Controls.Label {
                                        text: auditCardItem.modelData.alias && auditCardItem.modelData.alias.length > 0 ? "@" + auditCardItem.modelData.alias.replace(/ /g, "_") : qsTr("(direct)")
                                        font.family: ThemeController.codeFontFamily
                                        color: Kirigami.Theme.highlightColor
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        text: auditCardItem.modelData.field
                                        font.bold: true
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                                        elide: Text.ElideRight
                                    }
                                }

                                Controls.Label {
                                    text: {
                                        var oldV = auditCardItem.modelData.oldValue || "";
                                        var newV = auditCardItem.modelData.newValue || "";
                                        if (oldV.length > 80)
                                            oldV = oldV.substring(0, 80) + "…";
                                        if (newV.length > 80)
                                            newV = newV.substring(0, 80) + "…";
                                        return oldV + "  →  " + newV;
                                    }
                                    color: Kirigami.Theme.textColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                    font.family: ThemeController.codeFontFamily
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }
                }
                Controls.Label {
                    visible: tab.auditRows.length === 0
                    text: qsTr("No self-config changes recorded yet. " + "Heartbeat configs created or edited from " + "chat / folder settings (or by an agent " + "through self-config tools) will appear " + "here.")
                    color: Kirigami.Theme.disabledTextColor
                    font.italic: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Item {
                    Layout.preferredHeight: 16
                }
            }
        }
    }
}

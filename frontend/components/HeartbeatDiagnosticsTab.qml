// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Item {
    id: tab

    property var hbScheduleFires: []
    property var hbRecentRuns: []

    function reload() {
        hbScheduleFires = HeartbeatSubagent.nextFiresPreview(10);
        hbRecentRuns = HeartbeatSubagent.recentRunsList(50);
    }

    Connections {
        target: HeartbeatSubagent
        function onRunCompleted(runId, outcome) {
            tab.reload();
        }
        function onRunFailed(runId, error) {
            tab.reload();
        }
        function onConfigsChanged() {
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
            id: contentRoot
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
                    text: qsTr("Heartbeats are background routines that " + "run agents on a schedule. This panel " + "shows what the scheduler is doing right " + "now. Configure individual heartbeats " + "from chat settings (per-conversation " + "members) or folder settings (project " + "members).")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: ThemeController.radius
                    color: ThemeController.surfaceCard
                    border.color: ThemeController.borderSubtle
                    border.width: 1
                    implicitHeight: pauseCard.implicitHeight + 24

                    ColumnLayout {
                        id: pauseCard
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 6

                        Controls.Switch {
                            id: hbGlobalPauseSwitch
                            text: qsTr("Pause All Heartbeats")
                            font.bold: true
                            checked: HeartbeatSubagent.globallyPaused
                            onToggled: HeartbeatSubagent.globallyPaused = checked
                        }
                        Controls.Label {
                            text: qsTr("Master kill switch. When on, no " + "heartbeat fires anywhere in the " + "app. In-flight runs continue to " + "completion; queued runs are " + "NOT dispatched.")
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                        }
                        Controls.Label {
                            text: qsTr("Total enabled heartbeat configs: %1").arg(HeartbeatSubagent.totalEnabledConfigs)
                            color: Kirigami.Theme.textColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            Layout.fillWidth: true
                        }
                    }
                }

                Kirigami.Heading {
                    text: qsTr("Next 10 scheduled runs")
                    level: 4
                    Layout.topMargin: 8
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Repeater {
                    model: tab.hbScheduleFires
                    Rectangle {
                        Layout.fillWidth: true
                        radius: ThemeController.radius
                        color: ThemeController.surfaceCard
                        implicitHeight: scheduleCard.implicitHeight + 16

                        required property var modelData

                        ColumnLayout {
                            id: scheduleCard
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 4

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Controls.Label {
                                    text: modelData.alias.length > 0 ? "@" + modelData.alias.replace(/ /g, "_") : qsTr("(direct)")
                                    font.bold: true
                                    font.family: ThemeController.codeFontFamily
                                    color: Kirigami.Theme.highlightColor
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Controls.Label {
                                    text: modelData.schedule
                                    color: Kirigami.Theme.textColor
                                    opacity: 0.85
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                                    elide: Text.ElideRight
                                }
                            }
                            Controls.Label {
                                text: {
                                    var d = new Date(modelData.nextFireMs);
                                    return d.toLocaleString(Qt.locale(), "yyyy-MM-dd HH:mm");
                                }
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
                Controls.Label {
                    visible: tab.hbScheduleFires.length === 0
                    text: qsTr("No scheduled runs.")
                    color: Kirigami.Theme.disabledTextColor
                    font.italic: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    Layout.fillWidth: true
                }

                Kirigami.Heading {
                    text: qsTr("Recent runs (last 50)")
                    level: 4
                    Layout.topMargin: 8
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 260
                    color: ThemeController.surfaceCard
                    radius: ThemeController.radius
                    border.color: ThemeController.borderSubtle
                    border.width: 1

                    ListView {
                        id: hbRunsList
                        anchors.fill: parent
                        anchors.margins: 4
                        clip: true
                        model: tab.hbRecentRuns
                        spacing: 4

                        delegate: Rectangle {
                            id: runCardItem
                            width: hbRunsList.width
                            radius: ThemeController.radius
                            color: "transparent"
                            implicitHeight: runCard.implicitHeight + 8

                            required property var modelData

                            readonly property color _accent: {
                                if (modelData.outcome === "success")
                                    return Kirigami.Theme.positiveTextColor;
                                if (modelData.outcome === "error" || modelData.outcome === "timeout")
                                    return Kirigami.Theme.negativeTextColor;
                                if (modelData.outcome === "rate_limited" || modelData.outcome === "queue_overflow" || modelData.outcome === "cancelled" || modelData.outcome === "skipped_busy")
                                    return Kirigami.Theme.neutralTextColor;
                                return Kirigami.Theme.disabledTextColor;
                            }

                            ColumnLayout {
                                id: runCard
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    Kirigami.Icon {
                                        source: {
                                            if (modelData.outcome === "success")
                                                return "emblem-success";
                                            if (modelData.outcome === "error" || modelData.outcome === "timeout")
                                                return "emblem-error";
                                            if (modelData.outcome === "rate_limited" || modelData.outcome === "queue_overflow" || modelData.outcome === "cancelled" || modelData.outcome === "skipped_busy")
                                                return "emblem-warning";
                                            return "task-due";
                                        }
                                        fallback: "task-due"
                                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                        color: runCardItem._accent
                                    }
                                    Controls.Label {
                                        text: modelData.alias.length > 0 ? "@" + modelData.alias.replace(/ /g, "_") : (modelData.agentName || qsTr("(unknown)"))
                                        font.bold: true
                                        font.family: ThemeController.codeFontFamily
                                        color: Kirigami.Theme.highlightColor
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        text: modelData.outcome
                                        color: runCardItem._accent
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                    }
                                }
                                Controls.Label {
                                    text: {
                                        var d = new Date(modelData.startedAtMs);
                                        var t = d.toLocaleTimeString(Qt.locale(), "HH:mm:ss");
                                        return modelData.title && modelData.title.length > 0 ? t + "  -  " + modelData.title : t;
                                    }
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }
                Controls.Label {
                    visible: tab.hbRecentRuns.length === 0
                    text: qsTr("No runs yet.")
                    color: Kirigami.Theme.disabledTextColor
                    font.italic: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    Layout.fillWidth: true
                }

                Kirigami.Heading {
                    text: qsTr("Service log (last %1)").arg(HeartbeatSubagent.recentLogLines.length)
                    level: 4
                    Layout.topMargin: 8
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 220
                    color: ThemeController.surfaceCard
                    radius: ThemeController.radius
                    border.color: ThemeController.borderSubtle
                    border.width: 1

                    Controls.ScrollView {
                        anchors.fill: parent
                        anchors.margins: 6
                        clip: true

                        Controls.TextArea {
                            readOnly: true
                            wrapMode: TextEdit.NoWrap
                            font.family: ThemeController.codeFontFamily
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                            text: HeartbeatSubagent.recentLogLines.join("\n")
                            background: null
                        }
                    }
                }
                Controls.Label {
                    visible: HeartbeatSubagent.recentLogLines.length === 0
                    text: qsTr("No service log entries.")
                    color: Kirigami.Theme.disabledTextColor
                    font.italic: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    Layout.fillWidth: true
                }

                Item {
                    Layout.preferredHeight: 16
                }
            }
        }
    }
}

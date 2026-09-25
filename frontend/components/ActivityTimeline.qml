// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: timeline
    parent: applicationWindow().overlay

    implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 46)
    implicitHeight: Math.min(applicationWindow().height * 0.85, Kirigami.Units.gridUnit * 38)

    property string _scopeKind: ""
    property string _scopeFolderId: ""
    property string _scopeConvId: ""
    property string _scopeLabel: ""

    property string _filterActorKind: ""
    property string _filterEventType: ""

    property var _rows: []
    property int _expandedRow: -1

    function openForProject(folderId, label) {
        if (!folderId || folderId.length === 0)
            return;
        timeline._scopeKind = "project";
        timeline._scopeFolderId = folderId;
        timeline._scopeConvId = "";
        timeline._scopeLabel = label || qsTr("project");
        timeline._expandedRow = -1;
        timeline.reload();
        timeline.open();
    }

    function openForConversation(convId, label) {
        if (!convId || convId.length === 0)
            return;
        timeline._scopeKind = "conversation";
        timeline._scopeFolderId = "";
        timeline._scopeConvId = convId;
        timeline._scopeLabel = label || qsTr("conversation");
        timeline._expandedRow = -1;
        timeline.reload();
        timeline.open();
    }

    function reload() {
        if (timeline._scopeKind === "project") {
            timeline._rows = Activity.recentActivityForProject(timeline._scopeFolderId, 200);
        } else if (timeline._scopeKind === "conversation") {
            timeline._rows = Activity.recentActivityForConversation(timeline._scopeConvId, 200);
        } else {
            timeline._rows = [];
        }
    }

    Connections {
        target: Activity
        function onActivityLogged(folderId, convId) {
            if (timeline._scopeKind === "project" && folderId === timeline._scopeFolderId) {
                timeline.reload();
            } else if (timeline._scopeKind === "conversation" && convId === timeline._scopeConvId) {
                timeline.reload();
            }
        }
    }

    Timer {
        interval: 5000
        running: timeline.opened && timeline._scopeKind.length > 0
        repeat: true
        onTriggered: timeline.reload()
    }

    readonly property var _visibleRows: {
        const kind = timeline._filterActorKind;
        const type = timeline._filterEventType;
        if (kind.length === 0 && type.length === 0) {
            return timeline._rows;
        }
        var out = [];
        for (var i = 0; i < timeline._rows.length; ++i) {
            const r = timeline._rows[i];
            if (kind.length > 0 && r.actorKind !== kind)
                continue;
            if (type.length > 0 && r.eventType !== type)
                continue;
            out.push(r);
        }
        return out;
    }

    function formatTime(dt) {
        if (!dt)
            return "-";
        const d = (dt instanceof Date) ? dt : new Date(dt);
        if (isNaN(d.getTime()))
            return "-";
        return d.toLocaleString(Qt.locale(), "yyyy-MM-dd HH:mm:ss");
    }

    function actorKindColor(kind) {
        if (kind === "agent")
            return Kirigami.Theme.highlightColor;
        if (kind === "user")
            return Kirigami.Theme.positiveTextColor;
        if (kind === "system")
            return Kirigami.Theme.neutralTextColor;
        if (kind === "client")
            return Kirigami.Theme.linkColor;
        return Kirigami.Theme.disabledTextColor;
    }

    function eventTypeLabel(et) {
        if (et === "agent_turn")
            return qsTr("Agent turn");
        if (et === "image_generated")
            return qsTr("Image generated");
        if (et === "tool_invoked")
            return qsTr("Tool invoked");
        if (et === "poll_created")
            return qsTr("Poll created");
        if (et === "poll_vote")
            return qsTr("Poll vote");
        if (et === "poll_closed")
            return qsTr("Poll closed");
        if (et === "member_added")
            return qsTr("Member added");
        if (et === "member_removed")
            return qsTr("Member removed");
        if (et === "canvas_edited")
            return qsTr("Canvas edited");
        if (et === "file_written")
            return qsTr("File written");
        if (et === "permission_denied")
            return qsTr("Permission denied");
        if (et === "deliverable_produced")
            return qsTr("Deliverable");
        if (et === "workspace.mount.registered")
            return qsTr("Workspace mount registered");
        if (et === "workspace.mount.unregistered")
            return qsTr("Workspace mount unregistered");
        if (et === "workspace.mount.replaced")
            return qsTr("Workspace mount replaced");
        if (et === "workspace.mount.tree_updated")
            return qsTr("Workspace tree refreshed");
        if (et === "workspace.mount.tier_changed")
            return qsTr("Workspace tier changed");
        if (et === "workspace.mount.stale")
            return qsTr("Workspace mount stale");
        return et;
    }

    function prettyDetail(detail) {
        if (!detail)
            return "";
        if (typeof detail !== "object")
            return String(detail);
        const keys = Object.keys(detail);
        if (keys.length === 0)
            return "";
        var lines = [];
        for (var i = 0; i < keys.length; ++i) {
            const k = keys[i];
            var v = detail[k];
            if (v === null || v === undefined)
                v = "";
            else if (typeof v === "object")
                v = JSON.stringify(v);
            else
                v = String(v);
            lines.push(k + ": " + v);
        }
        return lines.join("\n");
    }

    title: qsTr("Activity (%1)").arg(timeline._visibleRows.length)
    subtitle: timeline._scopeKind === "project" ? qsTr("Project scope: %1").arg(timeline._scopeLabel) : (timeline._scopeKind === "conversation" ? qsTr("Conversation scope: %1").arg(timeline._scopeLabel) : "")
    dialogIcon: "view-history"

    headerTrailing: AppButton {
        text: qsTr("Refresh")
        icon.name: "view-refresh"
        flat: true
        onClicked: timeline.reload()
        Controls.ToolTip.text: qsTr("Re-fetch the latest rows from the audit log.")
        Controls.ToolTip.visible: hovered
        Controls.ToolTip.delay: 500
    }

    subHeader: Flow {
        spacing: 8

        RowLayout {
            spacing: 4
            Controls.Label {
                text: qsTr("Actor")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            }
            Controls.ComboBox {
                id: actorFilter
                implicitWidth: Math.max(140, Kirigami.Units.gridUnit * 7)
                model: [{
                        "label": qsTr("All actors"),
                        "value": ""
                    }, {
                        "label": qsTr("Users"),
                        "value": "user"
                    }, {
                        "label": qsTr("Agents"),
                        "value": "agent"
                    }, {
                        "label": qsTr("System"),
                        "value": "system"
                    }, {
                        "label": qsTr("Clients"),
                        "value": "client"
                    }]
                textRole: "label"
                valueRole: "value"
                currentIndex: 0
                onActivated: timeline._filterActorKind = currentValue
            }
        }

        RowLayout {
            spacing: 4
            Controls.Label {
                text: qsTr("Event")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            }
            Controls.ComboBox {
                id: eventFilter
                implicitWidth: Math.max(180, Kirigami.Units.gridUnit * 10)
                model: [{
                        "label": qsTr("All events"),
                        "value": ""
                    }, {
                        "label": qsTr("Agent turns"),
                        "value": "agent_turn"
                    }, {
                        "label": qsTr("Tools invoked"),
                        "value": "tool_invoked"
                    }, {
                        "label": qsTr("Polls (created)"),
                        "value": "poll_created"
                    }, {
                        "label": qsTr("Polls (votes)"),
                        "value": "poll_vote"
                    }, {
                        "label": qsTr("Polls (closed)"),
                        "value": "poll_closed"
                    }, {
                        "label": qsTr("Members added"),
                        "value": "member_added"
                    }, {
                        "label": qsTr("Members removed"),
                        "value": "member_removed"
                    }, {
                        "label": qsTr("Files written"),
                        "value": "file_written"
                    }, {
                        "label": qsTr("Canvas edits"),
                        "value": "canvas_edited"
                    }, {
                        "label": qsTr("Images generated"),
                        "value": "image_generated"
                    }, {
                        "label": qsTr("Permission denials"),
                        "value": "permission_denied"
                    }, {
                        "label": qsTr("Deliverables"),
                        "value": "deliverable_produced"
                    }]
                textRole: "label"
                valueRole: "value"
                currentIndex: 0
                onActivated: timeline._filterEventType = currentValue
            }
        }
    }

    ListView {
        id: rowsList
        model: timeline._visibleRows
        spacing: 8
        clip: true

        Controls.Label {
            anchors.centerIn: parent
            visible: rowsList.count === 0
            text: timeline._rows.length === 0 ? qsTr("No activity recorded yet in this scope. Agent turns, " + "tool invocations, polls, member changes, and file / " + "canvas writes will appear here as they happen.") : qsTr("No rows match the current filter. Choose a different " + "actor or event type to widen the view.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            width: parent.width - 40
            horizontalAlignment: Text.AlignHCenter
        }

        delegate: Rectangle {
            id: rowCard
            required property var modelData
            required property int index

            width: rowsList.width
            height: cardCol.implicitHeight + 16
            radius: ThemeController.radius
            color: ThemeController.surfaceCard
            border.color: ThemeController.borderSubtle
            border.width: 1

            TapHandler {
                onTapped: {
                    timeline._expandedRow = (timeline._expandedRow === rowCard.index) ? -1 : rowCard.index;
                }
            }

            ColumnLayout {
                id: cardCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle {
                        radius: ThemeController.radius
                        implicitWidth: kindPillLabel.implicitWidth + 12
                        implicitHeight: kindPillLabel.implicitHeight + 4
                        color: {
                            const c = timeline.actorKindColor(rowCard.modelData.actorKind);
                            return Qt.rgba(c.r, c.g, c.b, 0.15);
                        }
                        border.color: timeline.actorKindColor(rowCard.modelData.actorKind)
                        border.width: 1
                        Controls.Label {
                            id: kindPillLabel
                            anchors.centerIn: parent
                            text: rowCard.modelData.actorKind || ""
                            color: timeline.actorKindColor(rowCard.modelData.actorKind)
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            font.bold: true
                        }
                    }

                    Controls.Label {
                        text: {
                            const a = rowCard.modelData.actorAlias || "";
                            if (a.length === 0)
                                return qsTr("(no alias)");
                            return "@" + a.replace(/ /g, "_");
                        }
                        font.family: ThemeController.codeFontFamily
                        color: Kirigami.Theme.highlightColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        text: timeline.formatTime(rowCard.modelData.createdAt)
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        elide: Text.ElideRight
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Controls.Label {
                        text: timeline.eventTypeLabel(rowCard.modelData.eventType)
                        color: Kirigami.Theme.textColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        font.bold: true
                    }

                    Rectangle {
                        visible: rowCard.modelData.toolName && rowCard.modelData.toolName.length > 0
                        radius: ThemeController.radius
                        implicitWidth: toolPillLabel.implicitWidth + 12
                        implicitHeight: toolPillLabel.implicitHeight + 4
                        color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.12)
                        border.color: Kirigami.Theme.neutralTextColor
                        border.width: 1
                        Controls.Label {
                            id: toolPillLabel
                            anchors.centerIn: parent
                            text: rowCard.modelData.toolName || ""
                            color: Kirigami.Theme.neutralTextColor
                            font.family: ThemeController.codeFontFamily
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    Kirigami.Icon {
                        source: (timeline._expandedRow === rowCard.index) ? "go-up" : "go-down"
                        fallback: "arrow-down"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        color: Kirigami.Theme.disabledTextColor
                    }
                }

                Controls.Label {
                    text: rowCard.modelData.eventSummary || ""
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Rectangle {
                    visible: (timeline._expandedRow === rowCard.index) && detailText.text.length > 0
                    Layout.fillWidth: true
                    color: ThemeController.surfaceSunken
                    border.color: ThemeController.borderSubtle
                    border.width: 1
                    radius: ThemeController.radius
                    implicitHeight: detailText.implicitHeight + 16

                    Controls.Label {
                        id: detailText
                        anchors.fill: parent
                        anchors.margins: 8
                        text: timeline.prettyDetail(rowCard.modelData.eventDetail)
                        color: Kirigami.Theme.textColor
                        font.family: ThemeController.codeFontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                    }
                }
            }
        }
    }
}

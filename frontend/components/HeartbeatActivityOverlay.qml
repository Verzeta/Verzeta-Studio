// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: overlay
    parent: applicationWindow().overlay
    implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 44)
    implicitHeight: Math.min(applicationWindow().height * 0.85, Kirigami.Units.gridUnit * 36)

    function openForFolder(folderId) {
        if (folderId.length === 0)
            return;
        HeartbeatReportsModel.setActiveFolder(folderId);
        overlay.open();
    }

    onClosed: {
        if (HeartbeatReportsModel.activeFolderId.length > 0) {
            HeartbeatReportsModel.setActiveConversation(ChatController.activeConversationId);
        }
    }

    function formatTime(epochMs) {
        if (!epochMs || epochMs < 0)
            return "-";
        var d = new Date(epochMs);
        return d.toLocaleTimeString(Qt.locale(), "HH:mm:ss");
    }

    function formatDuration(durMs) {
        if (durMs === undefined || durMs < 0)
            return "-";
        if (durMs < 1000)
            return durMs + " ms";
        var s = (durMs / 1000).toFixed(durMs < 10000 ? 1 : 0);
        return s + " s";
    }

    function outcomeIcon(outcome) {
        if (outcome === "success")
            return "emblem-success";
        if (outcome === "error")
            return "emblem-error";
        if (outcome === "timeout")
            return "media-playback-stop";
        if (outcome === "rate_limited")
            return "emblem-warning";
        if (outcome === "queue_overflow")
            return "emblem-warning";
        if (outcome === "cancelled")
            return "edit-undo";
        if (outcome === "skipped_busy")
            return "task-due";
        return "task-due";
    }

    function outcomeColor(outcome) {
        if (outcome === "success")
            return Kirigami.Theme.positiveTextColor;
        if (outcome === "error" || outcome === "timeout")
            return Kirigami.Theme.negativeTextColor;
        if (outcome === "rate_limited" || outcome === "queue_overflow" || outcome === "cancelled" || outcome === "skipped_busy")
            return Kirigami.Theme.neutralTextColor;
        return Kirigami.Theme.disabledTextColor;
    }

    function surfaceBadgeText(status) {
        if (status === "posted_auto")
            return qsTr("Posted (auto)");
        if (status === "posted_manual")
            return qsTr("Posted (manual)");
        if (status === "skipped_by_agent")
            return qsTr("Agent skipped");
        if (status === "skipped_by_gate")
            return qsTr("Gate off");
        if (status === "skipped_by_rate_limit")
            return qsTr("Rate-limited");
        if (status === "dismissed_by_user")
            return qsTr("Dismissed");
        return qsTr("Pending review");
    }

    function surfaceBadgeColor(status) {
        if (status === "posted_auto" || status === "posted_manual")
            return Kirigami.Theme.positiveTextColor;
        if (status === "dismissed_by_user")
            return Kirigami.Theme.disabledTextColor;
        if (status === "skipped_by_agent" || status === "skipped_by_gate" || status === "skipped_by_rate_limit")
            return Kirigami.Theme.neutralTextColor;
        return Kirigami.Theme.highlightColor;
    }

    readonly property bool _manualOverrideEnabled: true

    title: qsTr("Heartbeat Activity (%1)").arg(HeartbeatReportsModel.count)
    dialogIcon: "chronometer"
    subtitle: HeartbeatReportsModel.pendingReviewCount > 0 ? qsTr("%1 report(s) need review.").arg(HeartbeatReportsModel.pendingReviewCount) : ""
    headerTrailing: Controls.Switch {
        text: qsTr("Pause All")
        checked: HeartbeatSubagent.globallyPaused
        onCheckedChanged: HeartbeatSubagent.globallyPaused = checked
        Controls.ToolTip.text: qsTr("Master kill switch: when on, no heartbeat fires anywhere in the app. Existing in-flight runs continue to completion.")
        Controls.ToolTip.visible: hovered
        Controls.ToolTip.delay: 500
    }

    ListView {
        id: reportsList
        model: HeartbeatReportsModel
        spacing: 10
        clip: true

        Controls.Label {
            anchors.centerIn: parent
            visible: reportsList.count === 0
            text: qsTr("No heartbeat reports for this conversation. Configure a heartbeat for an agent member from Chat Settings or run one manually from there.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            width: parent.width - 40
            horizontalAlignment: Text.AlignHCenter
        }

        delegate: Rectangle {
            id: reportCard
            required property string id
            required property string configId
            required property string agentId
            required property string agentName
            required property string agentIconName
            required property string alias
            required property var startedAtMs
            required property var completedAtMs
            required property int durationMs
            required property string outcome
            required property string title
            required property string body
            required property string summary
            required property string parentReviewStatus
            required property string surfaceStatus
            required property string surfacedMessageId
            required property string error

            property bool expanded: false

            width: reportsList.width
            height: cardCol.implicitHeight + 20
            color: ThemeController.surfaceCard
            radius: ThemeController.radius
            border.color: ThemeController.borderSubtle
            border.width: 1

            Behavior on height  {
                NumberAnimation {
                    duration: 80
                }
            }

            ColumnLayout {
                id: cardCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Kirigami.Icon {
                        source: reportCard.agentIconName.length > 0 ? reportCard.agentIconName : "face-smile"
                        fallback: "user-identity"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                        color: Kirigami.Theme.textColor
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Controls.Label {
                            text: {
                                var a = reportCard.alias.length > 0 ? "@" + reportCard.alias.replace(/ /g, "_") : reportCard.agentName;
                                return a;
                            }
                            font.bold: true
                            font.family: ThemeController.codeFontFamily
                            color: Kirigami.Theme.highlightColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        }
                        Controls.Label {
                            text: reportCard.title.length > 0 ? reportCard.title : (reportCard.outcome === "error" ? qsTr("Run failed") : qsTr("(no title)"))
                            color: Kirigami.Theme.textColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            wrapMode: Text.WordWrap
                            elide: Text.ElideRight
                            maximumLineCount: 2
                            Layout.fillWidth: true
                        }
                    }

                    Kirigami.Icon {
                        source: overlay.outcomeIcon(reportCard.outcome)
                        fallback: "task-due"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        color: overlay.outcomeColor(reportCard.outcome)
                    }
                    Controls.Label {
                        text: reportCard.outcome
                        color: overlay.outcomeColor(reportCard.outcome)
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    Controls.Label {
                        text: qsTr("▶ started %1").arg(overlay.formatTime(reportCard.startedAtMs))
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    }
                    Controls.Label {
                        text: qsTr("⏱ %1").arg(overlay.formatDuration(reportCard.durationMs))
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        radius: ThemeController.radius
                        implicitWidth: badgeLabel.implicitWidth + 12
                        implicitHeight: badgeLabel.implicitHeight + 6
                        color: Qt.rgba(overlay.surfaceBadgeColor(reportCard.surfaceStatus).r, overlay.surfaceBadgeColor(reportCard.surfaceStatus).g, overlay.surfaceBadgeColor(reportCard.surfaceStatus).b, 0.15)
                        border.color: overlay.surfaceBadgeColor(reportCard.surfaceStatus)
                        border.width: 1
                        Controls.Label {
                            id: badgeLabel
                            anchors.centerIn: parent
                            text: overlay.surfaceBadgeText(reportCard.surfaceStatus)
                            color: overlay.surfaceBadgeColor(reportCard.surfaceStatus)
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        }
                    }
                }

                Controls.Label {
                    visible: reportCard.summary.length > 0
                    text: reportCard.summary
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.leftMargin: 30
                    maximumLineCount: reportCard.expanded ? -1 : 3
                    elide: reportCard.expanded ? Text.ElideNone : Text.ElideRight
                }

                ColumnLayout {
                    visible: reportCard.expanded
                    Layout.fillWidth: true
                    Layout.leftMargin: 30
                    spacing: 6

                    Controls.Label {
                        visible: reportCard.body.length > 0
                        text: qsTr("Results:")
                        font.bold: true
                        color: Kirigami.Theme.textColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    }
                    Controls.Label {
                        visible: reportCard.body.length > 0
                        text: reportCard.body
                        color: Kirigami.Theme.textColor
                        font.family: ThemeController.codeFontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Controls.Label {
                        visible: reportCard.error.length > 0
                        text: qsTr("Error: %1").arg(reportCard.error)
                        color: Kirigami.Theme.negativeTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 30
                    spacing: 6

                    AppButton {
                        text: reportCard.expanded ? qsTr("Show less") : qsTr("Show details")
                        flat: true
                        onClicked: reportCard.expanded = !reportCard.expanded
                    }

                    AppButton {
                        text: qsTr("Run now")
                        icon.name: "media-playback-start"
                        flat: true
                        visible: reportCard.configId.length > 0
                        onClicked: HeartbeatSubagent.runNow(reportCard.configId)
                        Controls.ToolTip.text: qsTr("Trigger another run for this heartbeat now, ignoring its schedule.")
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: 500
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    AppButton {
                        text: qsTr("Post anyway")
                        icon.name: "document-send"
                        flat: true
                        enabled: overlay._manualOverrideEnabled
                        visible: reportCard.surfaceStatus === "skipped_by_agent" || reportCard.surfaceStatus === "skipped_by_gate" || reportCard.surfaceStatus === "skipped_by_rate_limit" || reportCard.surfaceStatus === "pending"
                        onClicked: {
                            postAnywayDialog.reportId = reportCard.id;
                            postAnywayDialog.editText = reportCard.summary.length > 0 ? reportCard.summary : reportCard.title;
                            postAnywayDialog.open();
                        }
                        Controls.ToolTip.text: qsTr("Open this report's summary in an editor and post to the chat.")
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: 500
                    }

                    AppButton {
                        text: qsTr("Dismiss")
                        icon.name: "edit-delete"
                        flat: true
                        enabled: overlay._manualOverrideEnabled
                        visible: reportCard.surfaceStatus !== "posted_auto" && reportCard.surfaceStatus !== "posted_manual" && reportCard.surfaceStatus !== "dismissed_by_user"
                        onClicked: HeartbeatSubagent.manualDismiss(reportCard.id)
                        Controls.ToolTip.text: qsTr("Mark this report as dismissed. The audit trail keeps the row.")
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: 500
                    }
                }
            }
        }
    }

    Kirigami.PromptDialog {
        id: postAnywayDialog
        title: qsTr("Post heartbeat report to chat")
        subtitle: qsTr("Edit the message body if needed. The post will be attributed to the heartbeat-enabled member and may @mention teammates.")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        property string reportId: ""
        property string editText: ""

        AppTextArea {
            id: postAnywayBody
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            wrapMode: TextEdit.Wrap
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            text: postAnywayDialog.editText
        }

        onAccepted: {
            var body = postAnywayBody.text.trim();
            if (body.length > 0) {
                HeartbeatSubagent.manualPost(postAnywayDialog.reportId, body);
            }
            postAnywayBody.text = "";
        }
        onRejected: postAnywayBody.text = ""
    }
}

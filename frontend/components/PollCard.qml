// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: root
    property string pollId: ""

    property int _refreshTick: 0

    readonly property var poll: {
        var _ = root._refreshTick;
        if (root.pollId.length === 0)
            return null;
        return PollService.pollResults(root.pollId);
    }

    readonly property bool isOpen: root.poll && root.poll.status === "open"
    readonly property bool hasPoll: root.poll !== null && root.poll.id !== undefined && root.poll.id.length > 0

    function voteShare(votes) {
        if (!root.poll)
            return 0;
        var total = root.poll.total_votes || 0;
        if (total <= 0 || votes <= 0)
            return 0;
        return votes / total;
    }

    property string closesInLabel: ""

    function recomputeCountdown() {
        if (!root.poll || !root.isOpen) {
            root.closesInLabel = "";
            return;
        }
        var closesIso = root.poll.closes_at || "";
        if (closesIso.length === 0) {
            root.closesInLabel = "";
            return;
        }
        var closesMs = Date.parse(closesIso);
        if (isNaN(closesMs)) {
            root.closesInLabel = "";
            return;
        }
        var remainMs = closesMs - Date.now();
        if (remainMs <= 0) {
            root.closesInLabel = qsTr("closing…");
            root._refreshTick++;
            return;
        }
        var totalSec = Math.floor(remainMs / 1000);
        if (totalSec < 60) {
            root.closesInLabel = qsTr("closes in %1s").arg(totalSec);
            return;
        }
        var minutes = Math.floor(totalSec / 60);
        var seconds = totalSec % 60;
        if (minutes < 60) {
            root.closesInLabel = qsTr("closes in %1m %2s").arg(minutes).arg(seconds);
            return;
        }
        var hours = Math.floor(minutes / 60);
        var mm = minutes % 60;
        root.closesInLabel = qsTr("closes in %1h %2m").arg(hours).arg(mm);
    }

    Timer {
        id: countdownTimer
        interval: 1000
        running: root.isOpen && root.poll && (root.poll.closes_at || "").length > 0
        repeat: true
        onTriggered: root.recomputeCountdown()
    }

    Component.onCompleted: {
        root._refreshTick++;
        root.recomputeCountdown();
    }
    onPollIdChanged: {
        root._refreshTick++;
        root.recomputeCountdown();
    }

    Connections {
        target: PollService
        function onPollUpdated(convId, pid) {
            if (pid === root.pollId) {
                root._refreshTick++;
                root.recomputeCountdown();
            }
        }
        function onPollClosed(convId, pid) {
            if (pid === root.pollId) {
                root._refreshTick++;
                root.recomputeCountdown();
            }
        }
        function onVoteCast(convId, pid, voter, optId) {
            if (pid === root.pollId) {
                root._refreshTick++;
            }
        }
    }

    visible: hasPoll
    implicitHeight: visible ? bodyCol.implicitHeight + Kirigami.Units.largeSpacing * 2 : 0
    Layout.fillWidth: true
    radius: ThemeController.radius
    color: isOpen ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.05) : ThemeController.surfaceCard
    border.color: isOpen ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.4) : ThemeController.borderSubtle
    border.width: 1

    ColumnLayout {
        id: bodyCol
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Icon {
                source: root.isOpen ? "view-list-symbolic" : "checkmark"
                fallback: "view-list"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                color: root.isOpen ? Kirigami.Theme.highlightColor : Kirigami.Theme.positiveTextColor
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Kirigami.Heading {
                    text: root.poll ? root.poll.question : ""
                    level: 5
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    color: Kirigami.Theme.textColor
                }
                Controls.Label {
                    text: {
                        if (!root.poll)
                            return "";
                        var bits = [];
                        if (root.poll.creator_alias) {
                            bits.push(qsTr("by %1").arg(root.poll.creator_alias));
                        }
                        bits.push(qsTr("%1 mode").arg(root.poll.mode || "single"));
                        var total = root.poll.total_votes || 0;
                        var voters = root.poll.voter_count || 0;
                        bits.push(qsTr("%1 vote%2 / %3 voter%4").arg(total).arg(total === 1 ? "" : "s").arg(voters).arg(voters === 1 ? "" : "s"));
                        return bits.join("  •  ");
                    }
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                }
            }

            Rectangle {
                Layout.alignment: Qt.AlignTop
                radius: ThemeController.radius
                implicitHeight: statusLabel.implicitHeight + Kirigami.Units.smallSpacing
                implicitWidth: statusLabel.implicitWidth + Kirigami.Units.largeSpacing
                color: root.isOpen ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.20) : Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.20)
                Controls.Label {
                    id: statusLabel
                    anchors.centerIn: parent
                    text: root.isOpen ? qsTr("OPEN") : qsTr("CLOSED")
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    font.bold: true
                    color: root.isOpen ? Kirigami.Theme.highlightColor : Kirigami.Theme.positiveTextColor
                }
            }
        }

        RowLayout {
            visible: root.closesInLabel.length > 0
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                source: "clock"
                fallback: "chronometer"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                color: Kirigami.Theme.disabledTextColor
            }
            Controls.Label {
                text: root.closesInLabel
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing / 2
        }

        Repeater {
            model: root.poll && root.poll.options ? root.poll.options : []
            delegate: ColumnLayout {
                id: optionDelegate
                required property var modelData
                Layout.fillWidth: true
                spacing: 2

                readonly property bool isWinner: {
                    if (!modelData)
                        return false;
                    if (!root.poll)
                        return false;
                    var winners = root.poll.winning_option_ids;
                    if (!winners || winners.length === 0)
                        return false;
                    if (winners.indexOf(modelData.id) < 0)
                        return false;
                    return (modelData.votes || 0) > 0;
                }
                readonly property real share: {
                    if (!modelData || !root.poll)
                        return 0;
                    var total = root.poll.total_votes || 0;
                    var votes = modelData.votes || 0;
                    if (total <= 0 || votes <= 0)
                        return 0;
                    return votes / total;
                }

                Rectangle {
                    id: optionRow
                    property bool _hovered: false

                    Layout.fillWidth: true
                    Layout.preferredHeight: optionContent.implicitHeight + Kirigami.Units.largeSpacing
                    radius: Kirigami.Units.smallSpacing
                    opacity: root.isOpen === true ? 1.0 : 0.85

                    color: {
                        var hovered = (optionRow._hovered === true) && (root.isOpen === true);
                        if (hovered) {
                            return Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.10);
                        }
                        return Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.6);
                    }
                    border.width: 1
                    border.color: {
                        var winner = (optionDelegate.isWinner === true);
                        if (winner)
                            return Kirigami.Theme.positiveTextColor;
                        var hovered = (optionRow._hovered === true) && (root.isOpen === true);
                        if (hovered)
                            return Kirigami.Theme.highlightColor;
                        return Kirigami.Theme.disabledTextColor;
                    }

                    Behavior on color  {
                        ColorAnimation {
                            duration: 90
                        }
                    }

                    RowLayout {
                        id: optionContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Kirigami.Units.largeSpacing
                        anchors.rightMargin: Kirigami.Units.largeSpacing
                        spacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: (optionDelegate.modelData && optionDelegate.modelData.text) ? optionDelegate.modelData.text : ""
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Kirigami.Theme.textColor
                            elide: Text.ElideRight
                            maximumLineCount: 2
                            verticalAlignment: Text.AlignVCenter
                        }

                        Rectangle {
                            id: voteBadge
                            radius: ThemeController.radius
                            implicitWidth: voteCount.implicitWidth + Kirigami.Units.largeSpacing
                            implicitHeight: voteCount.implicitHeight + Kirigami.Units.smallSpacing
                            color: {
                                var winner = (optionDelegate.isWinner === true);
                                if (winner) {
                                    return Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.30);
                                }
                                return Qt.rgba(Kirigami.Theme.disabledTextColor.r, Kirigami.Theme.disabledTextColor.g, Kirigami.Theme.disabledTextColor.b, 0.15);
                            }
                            Controls.Label {
                                id: voteCount
                                anchors.centerIn: parent
                                text: (optionDelegate.modelData) ? String(optionDelegate.modelData.votes || 0) : "0"
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                font.bold: true
                                color: (optionDelegate.isWinner === true) ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.textColor
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: root.isOpen ? Qt.PointingHandCursor : Qt.ArrowCursor
                        enabled: root.isOpen
                        onEntered: optionRow._hovered = true
                        onExited: optionRow._hovered = false
                        onClicked: {
                            if (optionDelegate.modelData && optionDelegate.modelData.id) {
                                PollService.castVoteByOptionId(root.pollId, "user", "user", optionDelegate.modelData.id);
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 4
                    radius: 2
                    color: Qt.rgba(Kirigami.Theme.disabledTextColor.r, Kirigami.Theme.disabledTextColor.g, Kirigami.Theme.disabledTextColor.b, 0.10)
                    Rectangle {
                        height: parent.height
                        width: parent.width * (optionDelegate.share || 0)
                        radius: 2
                        color: (optionDelegate.isWinner === true) ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.highlightColor
                        Behavior on width  {
                            NumberAnimation {
                                duration: 220
                                easing.type: Easing.OutCubic
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing

            Controls.Label {
                visible: !root.isOpen
                text: {
                    if (!root.poll)
                        return "";
                    var winners = root.poll.winning_option_ids || [];
                    if (winners.length === 0)
                        return qsTr("Closed with no votes.");
                    if (winners.length > 1)
                        return qsTr("Closed: tied.");
                    var winId = winners[0];
                    var opts = root.poll.options || [];
                    for (var i = 0; i < opts.length; ++i) {
                        if (opts[i].id === winId) {
                            return qsTr("Winner: %1").arg(opts[i].text);
                        }
                    }
                    return qsTr("Closed.");
                }
                color: Kirigami.Theme.positiveTextColor
                font.italic: true
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            Item {
                visible: root.isOpen
                Layout.fillWidth: true
            }

            Controls.Button {
                visible: root.isOpen
                text: qsTr("Close poll")
                icon.name: "dialog-close"
                flat: true
                onClicked: PollService.closePoll(root.pollId)
            }
        }
    }
}

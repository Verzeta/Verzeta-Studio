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
    implicitHeight: Math.min(applicationWindow().height * 0.85, Kirigami.Units.gridUnit * 34)

    function formatTime(epochMs) {
        if (!epochMs)
            return "-";
        var d = new Date(epochMs);
        return d.toLocaleTimeString(Qt.locale(), "HH:mm:ss");
    }

    function stepIcon(status) {
        if (status === "done")
            return "emblem-success";
        if (status === "blocked")
            return "emblem-warning";
        if (status === "needs_rework")
            return "view-refresh";
        if (status === "in_progress")
            return "media-playback-start";
        if (status === "submitted")
            return "document-send";
        return "checkbox-symbolic";
    }

    function planIcon(status) {
        if (status === "completed")
            return "emblem-success";
        if (status === "failed")
            return "emblem-error";
        if (status === "blocked")
            return "emblem-warning";
        if (status === "critiquing")
            return "view-refresh";
        return "view-task";
    }

    title: qsTr("Plans (%1)").arg(PlansModel.count)
    dialogIcon: "view-task"

    ListView {
        id: planList
        model: PlansModel
        spacing: 12
        clip: true

        Controls.Label {
            anchors.centerIn: parent
            text: qsTr("No plans in this conversation. Click \"Start a Task\" in the chat input bar to begin one.")
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            wrapMode: Text.WordWrap
            width: parent.width - 40
            horizontalAlignment: Text.AlignHCenter
            visible: planList.count === 0
        }

        delegate: Rectangle {
            id: planCard
            required property string id
            required property string goal
            required property string status
            required property string startedBy
            required property var createdAtMs
            required property var updatedAtMs
            required property int totalSteps
            required property int doneSteps
            required property var steps

            width: planList.width
            height: planColumn.implicitHeight + 20
            color: ThemeController.surfaceCard
            radius: ThemeController.radius
            border.color: ThemeController.borderSubtle
            border.width: 1

            ColumnLayout {
                id: planColumn
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Kirigami.Icon {
                        source: overlay.planIcon(planCard.status)
                        fallback: "view-task"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                        color: {
                            if (planCard.status === "completed")
                                return Kirigami.Theme.positiveTextColor;
                            if (planCard.status === "failed")
                                return Kirigami.Theme.negativeTextColor;
                            if (planCard.status === "blocked")
                                return Kirigami.Theme.neutralTextColor;
                            return Kirigami.Theme.highlightColor;
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Controls.Label {
                            text: planCard.goal
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            maximumLineCount: 2
                        }
                        Controls.Label {
                            text: qsTr("Status: %1  •  Steps %2/%3").arg(planCard.status).arg(planCard.doneSteps).arg(planCard.totalSteps)
                            color: Kirigami.Theme.disabledTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                        }
                    }

                    AppButton {
                        icon.name: "media-playback-stop"
                        flat: true
                        iconOnly: true
                        text: qsTr("Stop plan")
                        visible: planCard.status !== "completed" && planCard.status !== "failed"
                        onClicked: Tasks.stopPlan(planCard.id, qsTr("Stopped from plans overlay"))
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 2
                    spacing: 14

                    Controls.Label {
                        text: qsTr("▶ started %1").arg(overlay.formatTime(planCard.createdAtMs))
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    }
                    Controls.Label {
                        text: qsTr("⟳ updated %1").arg(overlay.formatTime(planCard.updatedAtMs))
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                }

                Controls.ProgressBar {
                    Layout.fillWidth: true
                    Layout.topMargin: 2
                    from: 0
                    to: Math.max(1, planCard.totalSteps)
                    value: planCard.doneSteps
                }

                Repeater {
                    model: planCard.steps
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
                        Layout.topMargin: 4
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Kirigami.Icon {
                                source: overlay.stepIcon(modelData.status)
                                fallback: "checkbox-symbolic"
                                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                color: {
                                    if (modelData.status === "done")
                                        return Kirigami.Theme.positiveTextColor;
                                    if (modelData.status === "blocked")
                                        return Kirigami.Theme.negativeTextColor;
                                    if (modelData.status === "needs_rework")
                                        return Kirigami.Theme.neutralTextColor;
                                    return Kirigami.Theme.textColor;
                                }
                            }

                            Controls.Label {
                                text: modelData.title
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                color: modelData.status === "done" ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.textColor
                                font.strikeout: modelData.status === "done"
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Controls.Label {
                                text: qsTr("@%1").arg(modelData.ownerAlias)
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 22
                            spacing: 12
                            visible: modelData.rejectionCount > 0 || modelData.toolRetryCount > 0 || modelData.executorTurnsUsed > 0

                            Controls.Label {
                                visible: modelData.rejectionCount > 0
                                text: qsTr("rework %1/3").arg(modelData.rejectionCount)
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            }
                            Controls.Label {
                                visible: modelData.toolRetryCount > 0
                                text: qsTr("text retry %1/3").arg(modelData.toolRetryCount)
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            }
                            Controls.Label {
                                visible: modelData.executorTurnsUsed > 0
                                text: qsTr("%1/20 turns").arg(modelData.executorTurnsUsed)
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                        }

                        Controls.Label {
                            visible: modelData.lastRejectionReason !== undefined && modelData.lastRejectionReason.length > 0
                            Layout.fillWidth: true
                            Layout.leftMargin: 22
                            text: qsTr("note: %1").arg(modelData.lastRejectionReason)
                            color: Kirigami.Theme.disabledTextColor
                            font.italic: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            wrapMode: Text.WordWrap
                            elide: Text.ElideRight
                            maximumLineCount: 2
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 22
                            Layout.topMargin: 2
                            spacing: 4
                            visible: modelData.canIntervene === true

                            AppButton {
                                text: qsTr("Retry")
                                icon.name: "view-refresh"
                                flat: true
                                onClicked: {
                                    retryDialog.stepId = modelData.id;
                                    retryDialog.stepTitle = modelData.title;
                                    retryDialog.open();
                                }
                                Controls.ToolTip.text: qsTr("Reset this step and ask the agent to try again with your notes as guidance")
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.delay: 500
                            }
                            AppButton {
                                text: qsTr("Mark Done")
                                icon.name: "emblem-success"
                                flat: true
                                onClicked: Tasks.overrideStepAsDone(modelData.id)
                                Controls.ToolTip.text: qsTr("Mark this step as completed. Use when the work is already good or was done elsewhere.")
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.delay: 500
                            }
                            AppButton {
                                text: qsTr("Skip")
                                icon.name: "go-next"
                                flat: true
                                onClicked: Tasks.skipStep(modelData.id, qsTr("Not needed"))
                                Controls.ToolTip.text: qsTr("Skip this step. The plan continues with the remaining steps.")
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.delay: 500
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }
    }

    Kirigami.PromptDialog {
        id: retryDialog
        title: qsTr("Retry step")
        subtitle: qsTr("Give the agent specific guidance on what to change. " + "The note is added to the agent's next attempt " + "at this step.")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        property string stepId: ""
        property string stepTitle: ""

        Controls.Label {
            Layout.fillWidth: true
            text: qsTr("Step: %1").arg(retryDialog.stepTitle)
            font.bold: true
            color: Kirigami.Theme.textColor
            wrapMode: Text.WordWrap
        }

        AppTextArea {
            id: retryNotes
            Layout.fillWidth: true
            Layout.preferredHeight: 100
            wrapMode: TextEdit.Wrap
            placeholderText: qsTr("e.g. Focus on a playful tone, include a clear CTA, keep it under 150 words")
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
        }

        onAccepted: {
            Tasks.retryStep(retryDialog.stepId, retryNotes.text.trim());
            retryNotes.text = "";
        }
        onRejected: retryNotes.text = ""
    }
}

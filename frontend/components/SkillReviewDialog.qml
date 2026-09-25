// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.Dialog {
    id: dialog

    readonly property string skillLabel: currentSkill.displayName || currentSkill.id || ""

    readonly property bool isApproved: (currentSkill.reviewState || "") === "approved"

    title: (dialog.isApproved ? qsTr("Skill: %1") : qsTr("Review Skill: %1")).arg(dialog.skillLabel)
    preferredWidth: Kirigami.Units.gridUnit * 38
    preferredHeight: Kirigami.Units.gridUnit * 32

    property var currentSkill: ({})

    function openFor(skillId) {
        currentSkill = Skills.skillDetails(skillId);
        ackCheckbox.checked = false;
        tabBar.currentIndex = 0;
        instructionsArea.text = Skills.readSkillFile(skillId, "SKILL.md");
        open();
    }

    standardButtons: Kirigami.Dialog.NoButton

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("Block")
            icon.name: "edit-delete"
            onTriggered: {
                Skills.blockSkill(currentSkill.id);
                dialog.close();
            }
        },
        Kirigami.Action {
            text: dialog.isApproved ? qsTr("Close") : qsTr("Cancel")
            icon.name: "dialog-cancel"
            onTriggered: dialog.close()
        },
        Kirigami.Action {
            text: qsTr("Approve")
            icon.name: "dialog-ok"
            visible: !dialog.isApproved
            enabled: ackCheckbox.checked
            onTriggered: {
                Skills.approveSkill(currentSkill.id);
                dialog.close();
            }
        }
    ]

    contentItem: ColumnLayout {
        spacing: 12

        Rectangle {
            Layout.fillWidth: true
            visible: !dialog.isApproved
            radius: ThemeController.radius
            color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.12)
            border.color: Kirigami.Theme.neutralTextColor
            border.width: 1
            implicitHeight: visible ? bannerCol.implicitHeight + 16 : 0

            ColumnLayout {
                id: bannerCol
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6
                Controls.Label {
                    text: qsTr("⚠ User Responsibility")
                    font.bold: true
                    color: Kirigami.Theme.neutralTextColor
                }
                Controls.Label {
                    text: qsTr("Installed skills are untrusted. Their " + "instructions can ask agents to run " + "commands, read files, or exfiltrate " + "data. We scan for suspicious patterns " + "but cannot guarantee safety. By " + "approving a skill you accept " + "responsibility for whatever its " + "instructions cause an agent to do on " + "your system.")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Controls.CheckBox {
                    id: ackCheckbox
                    text: qsTr("I understand that I am responsible for how this skill is used.")
                }
            }
        }

        ColumnLayout {
            spacing: 2
            Layout.fillWidth: true
            Controls.Label {
                text: dialog.skillLabel
                font.bold: true
            }
            Controls.Label {
                visible: (currentSkill.displayName || "") !== "" && currentSkill.displayName !== currentSkill.id
                text: currentSkill.id || ""
                font.family: ThemeController.codeFontFamily
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                color: Kirigami.Theme.disabledTextColor
            }
            Controls.Label {
                text: currentSkill.description || ""
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Controls.Label {
                text: qsTr("version %1 • hash %2 • source %3").arg(currentSkill.version || "-").arg(currentSkill.contentHashShort || "-").arg(currentSkill.source || "manual")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            }
        }

        Controls.TabBar {
            id: tabBar
            Layout.fillWidth: true
            Controls.TabButton {
                text: qsTr("Instructions")
                Layout.fillWidth: true
            }
            Controls.TabButton {
                text: qsTr("Warnings (%1)").arg(currentSkill.warnings ? currentSkill.warnings.length : 0)
                Layout.fillWidth: true
            }
            Controls.TabButton {
                text: qsTr("Frontmatter")
                Layout.fillWidth: true
            }
        }

        Controls.SwipeView {
            id: tabSwipe
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 18
            currentIndex: tabBar.currentIndex
            interactive: false
            clip: true

            Item {
                Controls.ScrollView {
                    anchors.fill: parent
                    clip: true
                    Controls.TextArea {
                        id: instructionsArea
                        readOnly: true
                        wrapMode: TextEdit.WordWrap
                        font.family: ThemeController.codeFontFamily
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    }
                }
            }

            Item {
                Controls.ScrollView {
                    anchors.fill: parent
                    clip: true
                    ColumnLayout {
                        width: parent.parent.width
                        spacing: 6
                        Controls.Label {
                            visible: !currentSkill.warnings || currentSkill.warnings.length === 0
                            text: qsTr("No warnings, but that is not a guarantee of safety. Read the instructions yourself.")
                            wrapMode: Text.WordWrap
                            color: Kirigami.Theme.disabledTextColor
                            Layout.fillWidth: true
                            Layout.margins: 12
                        }
                        Repeater {
                            model: currentSkill.warnings
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.leftMargin: 8
                                Layout.rightMargin: 8
                                radius: ThemeController.radius
                                color: Qt.rgba(Kirigami.Theme.neutralTextColor.r, Kirigami.Theme.neutralTextColor.g, Kirigami.Theme.neutralTextColor.b, 0.10)
                                border.color: Kirigami.Theme.neutralTextColor
                                border.width: 1
                                implicitHeight: warnCol.implicitHeight + 12
                                ColumnLayout {
                                    id: warnCol
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    spacing: 2
                                    required property var modelData
                                    Controls.Label {
                                        text: warnCol.modelData.regexName + " · " + warnCol.modelData.fileRelativePath + ":" + warnCol.modelData.lineNumber
                                        font.bold: true
                                        color: Kirigami.Theme.neutralTextColor
                                    }
                                    Controls.Label {
                                        text: warnCol.modelData.matchedExcerpt
                                        font.family: ThemeController.codeFontFamily
                                        wrapMode: Text.Wrap
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item {
                Controls.ScrollView {
                    anchors.fill: parent
                    clip: true
                    ColumnLayout {
                        width: parent.parent.width
                        spacing: 4
                        Controls.Label {
                            Layout.margins: 12
                            text: qsTr("Name: %1").arg(currentSkill.displayName || currentSkill.id || "-")
                        }
                        Controls.Label {
                            Layout.margins: 12
                            text: qsTr("Tags: %1").arg((currentSkill.tags || []).join(", ") || "-")
                        }
                        Controls.Label {
                            Layout.margins: 12
                            text: qsTr("Declared tools (advisory): %1").arg((currentSkill.declaredTools || []).join(", ") || "-")
                        }
                        Controls.Label {
                            Layout.margins: 12
                            text: qsTr("Hash: %1").arg(currentSkill.contentHashSha256 || "")
                            font.family: ThemeController.codeFontFamily
                            wrapMode: Text.WrapAnywhere
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }
}

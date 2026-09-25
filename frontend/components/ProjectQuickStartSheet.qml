// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: sheet
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: applicationWindow().width * 0.95
    implicitHeight: applicationWindow().height * 0.92

    readonly property bool _isMobile: applicationWindow().width < Kirigami.Units.gridUnit * 36
    readonly property color _cPanel: Qt.darker(Kirigami.Theme.backgroundColor, 0.6)

    property var _template: ({})
    property string _templateId: ""

    property string _savedHint: ""

    readonly property var _designs: [{
            "geometryKind": "circles",
            "baseHue": 210
        }, {
            "geometryKind": "wave",
            "baseHue": 180
        }, {
            "geometryKind": "grid",
            "baseHue": 130
        }, {
            "geometryKind": "bars",
            "baseHue": 240
        }, {
            "geometryKind": "triangle",
            "baseHue": 30
        }, {
            "geometryKind": "arrows",
            "baseHue": 300
        }, {
            "geometryKind": "spiral",
            "baseHue": 45
        }, {
            "geometryKind": "dots",
            "baseHue": 270
        }]
    property int _designIndex: 0
    readonly property var _design: _designs[_designIndex]

    signal projectCreated(string folderId, string folderName, int memberCount)

    function _indexForDesign(geometryKind) {
        for (var i = 0; i < sheet._designs.length; ++i) {
            if (sheet._designs[i].geometryKind === geometryKind)
                return i;
        }
        return 0;
    }

    function _cycleDesign(delta) {
        var n = sheet._designs.length;
        sheet._designIndex = (sheet._designIndex + delta + n) % n;
    }

    function openForTemplate(t) {
        sheet._template = t || ({});
        sheet._templateId = (t && t.id) ? t.id : "";
        sheet._savedHint = "";
        sheet._designIndex = sheet._indexForDesign((t && t.geometryKind) ? t.geometryKind : "");
        nameField.text = (t && t.name) ? t.name : "";
        scenarioField.text = (t && t.scenario) ? t.scenario : "";
        goalArea.text = (t && t.goal) ? t.goal : "";
        descArea.text = (t && t.description) ? t.description : "";
        teammates.initialMembers = ProjectTemplates.templateRoster(sheet._templateId);
        sheet.open();
    }

    function _spinUp() {
        const projectName = nameField.text.trim();
        if (projectName.length === 0)
            return;
        const members = teammates.memberList();
        const folderId = ProjectTemplates.createProjectFromTemplate(sheet._templateId, {
                "name": projectName,
                "scenario": scenarioField.text.trim(),
                "goal": goalArea.text.trim(),
                "description": descArea.text.trim(),
                "members": members
            });
        if (folderId.length === 0)
            return;
        sheet.close();
        sheet.projectCreated(folderId, projectName, members.length);
    }

    function _saveAsTemplate() {
        const projectName = nameField.text.trim();
        if (projectName.length === 0)
            return;
        const newId = ProjectTemplates.saveAsNewTemplate(sheet._templateId, {
                "name": projectName,
                "scenario": scenarioField.text.trim(),
                "goal": goalArea.text.trim(),
                "description": descArea.text.trim(),
                "members": teammates.memberList(),
                "geometryKind": sheet._design.geometryKind,
                "baseHue": sheet._design.baseHue
            });
        sheet._savedHint = newId.length > 0 ? qsTr("✓  Saved to your Template Library") : qsTr("Couldn't save the template: check the name.");
        savedHintTimer.restart();
    }

    component FieldLabel: Controls.Label {
        font.family: ThemeController.fontFamily
        font.bold: true
        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.76
        font.letterSpacing: 0.8
        color: Kirigami.Theme.disabledTextColor
    }

    title: qsTr("Quick Start")
    subtitle: {
        var tag = (sheet._template && sheet._template.tagLabel) ? sheet._template.tagLabel : "";
        return tag.length > 0 ? qsTr("HOME · NEW PROJECT · %1").arg(tag) : qsTr("HOME · NEW PROJECT");
    }
    dialogIcon: "folder-projects"

    footer: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Controls.Label {
            text: sheet._savedHint.length > 0 ? sheet._savedHint : qsTr("All settings are editable inside the room later.")
            color: {
                if (sheet._savedHint.length > 0)
                    return Kirigami.Theme.positiveTextColor;
                return Kirigami.Theme.disabledTextColor;
            }
            font.family: ThemeController.fontFamily
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            visible: !sheet._isMobile
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        Item {
            visible: sheet._isMobile
            Layout.fillWidth: true
        }

        AppButton {
            text: qsTr("Save as template")
            flat: true
            enabled: nameField.text.trim().length > 0
            onClicked: sheet._saveAsTemplate()
        }
        AppButton {
            text: qsTr("Cancel")
            onClicked: sheet.close()
        }
        AppButton {
            text: qsTr("✓  Create room")
            highlighted: true
            enabled: nameField.text.trim().length > 0
            onClicked: sheet._spinUp()
        }
    }

    Item {
        id: bodyRoot
        implicitWidth: applicationWindow().width * 0.92
        implicitHeight: applicationWindow().height * 0.74

        Timer {
            id: savedHintTimer
            interval: 3500
            onTriggered: sheet._savedHint = ""
        }

        Controls.ScrollView {
            id: bodyScroll
            anchors.fill: parent
            clip: true
            contentWidth: availableWidth
            Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
            Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded

            ColumnLayout {
                width: bodyScroll.availableWidth
                spacing: 0

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.gridUnit
                    spacing: Kirigami.Units.gridUnit

                    Rectangle {
                        Layout.fillWidth: true
                        radius: ThemeController.radius
                        color: sheet._cPanel
                        antialiasing: true
                        implicitHeight: panel1.implicitHeight + Kirigami.Units.gridUnit * 2

                        RowLayout {
                            id: panel1
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: parent.top
                                margins: Kirigami.Units.gridUnit
                            }
                            spacing: Kirigami.Units.gridUnit

                            ColumnLayout {
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 9
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 9
                                visible: !sheet._isMobile
                                spacing: Kirigami.Units.smallSpacing

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Kirigami.Units.gridUnit * 9
                                    radius: ThemeController.radius
                                    clip: true
                                    color: "transparent"

                                    ProjectTemplateBanner {
                                        anchors.fill: parent
                                        cornerRadius: 10
                                        geometryKind: sheet._design.geometryKind
                                        baseHue: sheet._design.baseHue
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Controls.ToolButton {
                                        icon.name: "go-previous"
                                        onClicked: sheet._cycleDesign(-1)
                                        Controls.ToolTip.text: qsTr("Previous design")
                                        Controls.ToolTip.visible: hovered
                                        Controls.ToolTip.delay: 500
                                    }
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignHCenter
                                        text: qsTr("Design %1 / %2").arg(sheet._designIndex + 1).arg(sheet._designs.length)
                                        elide: Text.ElideRight
                                        color: Kirigami.Theme.disabledTextColor
                                        font.family: ThemeController.fontFamily
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                    }
                                    Controls.ToolButton {
                                        icon.name: "go-next"
                                        onClicked: sheet._cycleDesign(1)
                                        Controls.ToolTip.text: qsTr("Next design")
                                        Controls.ToolTip.visible: hovered
                                        Controls.ToolTip.delay: 500
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing

                                FieldLabel {
                                    text: qsTr("PROJECT NAME (REQUIRED)")
                                }
                                AppTextField {
                                    id: nameField
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("Name your project room")
                                }

                                Item {
                                    Layout.preferredHeight: Kirigami.Units.smallSpacing
                                }

                                FieldLabel {
                                    text: qsTr("SCENARIO")
                                }
                                AppTextField {
                                    id: scenarioField
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("Shown in the " + "room header: e.g. \"Launch · " + "multi-channel\"")
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        radius: ThemeController.radius
                        color: sheet._cPanel
                        antialiasing: true
                        implicitHeight: panel2.implicitHeight + Kirigami.Units.gridUnit * 2

                        ColumnLayout {
                            id: panel2
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: parent.top
                                margins: Kirigami.Units.gridUnit
                            }
                            spacing: Kirigami.Units.smallSpacing

                            FieldLabel {
                                text: qsTr("GOAL: WHAT DOES SUCCESS LOOK LIKE?")
                            }
                            AppTextArea {
                                id: goalArea
                                Layout.fillWidth: true
                                Layout.preferredHeight: Kirigami.Units.gridUnit * 5
                                wrapMode: TextEdit.Wrap
                                placeholderText: qsTr("Describe the " + "outcome the team should deliver.")
                            }

                            Item {
                                Layout.preferredHeight: Kirigami.Units.smallSpacing
                            }

                            FieldLabel {
                                text: qsTr("PROJECT DESCRIPTION / CONTEXT")
                            }
                            AppTextArea {
                                id: descArea
                                Layout.fillWidth: true
                                Layout.preferredHeight: Kirigami.Units.gridUnit * 5
                                wrapMode: TextEdit.Wrap
                                placeholderText: qsTr("Paste a brief, link " + "a doc, or describe the situation.")
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        radius: ThemeController.radius
                        color: sheet._cPanel
                        antialiasing: true
                        implicitHeight: panel3.implicitHeight + Kirigami.Units.gridUnit * 2

                        ColumnLayout {
                            id: panel3
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: parent.top
                                margins: Kirigami.Units.gridUnit
                            }
                            spacing: Kirigami.Units.largeSpacing

                            RowLayout {
                                Layout.fillWidth: true
                                Controls.Label {
                                    text: qsTr("Teammates")
                                    color: Kirigami.Theme.textColor
                                    font.family: ThemeController.fontFamily
                                    font.bold: true
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.1
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                            }

                            Controls.Label {
                                Layout.fillWidth: true
                                text: qsTr("Add, remove, or rename agents " + "and tune each one's provider, model " + "and tools. Everything is editable " + "again once the room exists.")
                                color: Kirigami.Theme.disabledTextColor
                                font.family: ThemeController.fontFamily
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.88
                                wrapMode: Text.WordWrap
                            }

                            MembershipEditor {
                                id: teammates
                                Layout.fillWidth: true
                                softDelete: false
                            }
                        }
                    }
                }
            }
        }
    }
}

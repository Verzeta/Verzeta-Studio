// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Kirigami.Dialog {
    id: dialog
    title: qsTr("Preferred Skills")
    preferredWidth: Kirigami.Units.gridUnit * 38
    preferredHeight: Kirigami.Units.gridUnit * 28

    property string scopeType: ""
    property string scopeId: ""

    function openFor(typ, id) {
        scopeType = typ;
        scopeId = id;
        _reload();
        open();
    }

    standardButtons: Kirigami.Dialog.NoButton

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("Cancel")
            icon.name: "dialog-cancel"
            onTriggered: dialog.close()
        },
        Kirigami.Action {
            text: qsTr("Save")
            icon.name: "document-save"
            onTriggered: {
                Skills.setPreferredSkills(dialog.scopeType, dialog.scopeId, preferredList.skillIdList());
                dialog.close();
            }
        }
    ]

    function _reload() {
        availableList.refresh();
        preferredList.skillIds = Skills.preferredSkillsFor(scopeType, scopeId);
    }

    contentItem: ColumnLayout {
        spacing: 8

        Controls.Label {
            text: qsTr("Move skills between Available (left) and " + "Preferred (right). Preferred skills are " + "shown to agents first; ordering matters.")
            wrapMode: Text.WordWrap
            color: Kirigami.Theme.disabledTextColor
            Layout.fillWidth: true
        }

        AppTextField {
            id: searchField
            placeholderText: qsTr("Search available skills…")
            Layout.fillWidth: true
            onTextChanged: availableList.applyFilter(text)
        }

        readonly property bool wide: dialog.width > Kirigami.Units.gridUnit * 30

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: parent.wide ? 2 : 1
            rowSpacing: 8
            columnSpacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 4
                Controls.Label {
                    text: qsTr("All Available Skills")
                    font.bold: true
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: ThemeController.surfaceCard
                    radius: ThemeController.radius
                    border.color: ThemeController.borderSubtle
                    border.width: 1
                    ListView {
                        id: availableList
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true
                        spacing: 2

                        property var allRows: []
                        property string filterText: ""

                        function refresh() {
                            const all = Skills.installedSkills();
                            const filtered = [];
                            for (var i = 0; i < all.length; ++i) {
                                if (all[i].reviewState === "blocked")
                                    continue;
                                filtered.push(all[i]);
                            }
                            allRows = filtered;
                            applyFilter(filterText);
                        }
                        function applyFilter(t) {
                            filterText = t.toLowerCase();
                            var preferred = preferredList.skillIds;
                            var rows = [];
                            for (var i = 0; i < allRows.length; ++i) {
                                var r = allRows[i];
                                if (preferred.indexOf(r.id) >= 0)
                                    continue;
                                if (filterText.length > 0) {
                                    var hay = (r.id + " " + r.description + " " + (r.tags || []).join(" ")).toLowerCase();
                                    if (hay.indexOf(filterText) < 0)
                                        continue;
                                }
                                rows.push(r);
                            }
                            model = rows;
                        }

                        Connections {
                            target: Skills
                            function onSkillsChanged() {
                                availableList.refresh();
                            }
                        }

                        delegate: Rectangle {
                            width: ListView.view.width
                            radius: ThemeController.radius
                            color: Kirigami.Theme.backgroundColor
                            implicitHeight: row.implicitHeight + 8
                            border.color: ThemeController.borderSubtle
                            border.width: 1

                            required property var modelData

                            RowLayout {
                                id: row
                                anchors.fill: parent
                                anchors.margins: 4
                                spacing: 4
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Controls.Label {
                                        text: modelData.id
                                        font.bold: true
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        text: modelData.reviewState === "approved" ? qsTr("Approved") : qsTr("Unreviewed: review before agents can use it")
                                        color: modelData.reviewState === "approved" ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }
                                }
                                Controls.ToolButton {
                                    text: ">>"
                                    onClicked: {
                                        preferredList.add(modelData.id);
                                        availableList.applyFilter(availableList.filterText);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 4
                Controls.Label {
                    text: qsTr("Preferred Skills (in order)")
                    font.bold: true
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: ThemeController.surfaceCard
                    radius: ThemeController.radius
                    border.color: ThemeController.borderSubtle
                    border.width: 1
                    ListView {
                        id: preferredList
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true
                        spacing: 2

                        property var skillIds: []

                        function skillIdList() {
                            return skillIds.slice();
                        }
                        function add(id) {
                            var n = skillIds.slice();
                            if (n.indexOf(id) < 0)
                                n.push(id);
                            skillIds = n;
                        }
                        function remove(idx) {
                            var n = skillIds.slice();
                            n.splice(idx, 1);
                            skillIds = n;
                            availableList.applyFilter(availableList.filterText);
                        }
                        function moveUp(idx) {
                            if (idx <= 0)
                                return;
                            var n = skillIds.slice();
                            var t = n[idx];
                            n[idx] = n[idx - 1];
                            n[idx - 1] = t;
                            skillIds = n;
                        }
                        function moveDown(idx) {
                            if (idx >= skillIds.length - 1)
                                return;
                            var n = skillIds.slice();
                            var t = n[idx];
                            n[idx] = n[idx + 1];
                            n[idx + 1] = t;
                            skillIds = n;
                        }

                        model: skillIds
                        delegate: Rectangle {
                            width: ListView.view.width
                            radius: ThemeController.radius
                            color: Kirigami.Theme.backgroundColor
                            implicitHeight: row2.implicitHeight + 8
                            border.color: ThemeController.borderSubtle
                            border.width: 1
                            required property string modelData
                            required property int index

                            RowLayout {
                                id: row2
                                anchors.fill: parent
                                anchors.margins: 4
                                spacing: 4
                                Controls.Label {
                                    text: modelData
                                    font.bold: true
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Controls.ToolButton {
                                    text: "↑"
                                    onClicked: preferredList.moveUp(index)
                                }
                                Controls.ToolButton {
                                    text: "↓"
                                    onClicked: preferredList.moveDown(index)
                                }
                                Controls.ToolButton {
                                    text: "<<"
                                    onClicked: preferredList.remove(index)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

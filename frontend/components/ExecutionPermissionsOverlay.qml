// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: sheet
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: Math.min(applicationWindow().width * 0.9, Kirigami.Units.gridUnit * 40)

    readonly property color _cPanel: Qt.darker(Kirigami.Theme.backgroundColor, 0.6)
    readonly property color _cWell: Qt.darker(Kirigami.Theme.backgroundColor, 1.15)

    title: qsTr("Execution & Permissions")
    subtitle: qsTr("SHELL COMMANDS AGENTS MAY RUN")
    dialogIcon: "security-medium"

    function _addProgram() {
        var v = ("" + addField.text).trim();
        if (v.length === 0)
            return;
        var list = SettingsService.shellAllowList.slice();
        if (list.indexOf(v) < 0) {
            list.push(v);
            SettingsService.shellAllowList = list;
        }
        addField.text = "";
    }

    function _removeProgram(name) {
        var list = SettingsService.shellAllowList.slice();
        var i = list.indexOf(name);
        if (i >= 0) {
            list.splice(i, 1);
            SettingsService.shellAllowList = list;
        }
    }

    function _removeFolder(path) {
        var list = SettingsService.shellWritableFolders.slice();
        var i = list.indexOf(path);
        if (i >= 0) {
            list.splice(i, 1);
            SettingsService.shellWritableFolders = list;
        }
    }

    Dialogs.FolderDialog {
        id: writableFolderPicker
        title: qsTr("Allow agents to write in a folder")
        onAccepted: {
            var path = PathUtils.toLocalFile(selectedFolder);
            if (path.length === 0)
                return;
            var list = SettingsService.shellWritableFolders.slice();
            if (list.indexOf(path) < 0) {
                list.push(path);
                SettingsService.shellWritableFolders = list;
            }
        }
    }

    footer: RowLayout {
        AppButton {
            text: qsTr("Restore to Default")
            icon.name: "edit-undo"
            onClicked: SettingsService.resetShellAllowListToDefault()
        }
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Close")
            highlighted: true
            onClicked: sheet.close()
        }
    }

    ColumnLayout {
        Layout.margins: Kirigami.Units.gridUnit
        spacing: Kirigami.Units.gridUnit

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: sheet._cPanel
            antialiasing: true
            implicitHeight: explainerRow.implicitHeight + Kirigami.Units.gridUnit * 2

            RowLayout {
                id: explainerRow
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing * 2

                Kirigami.Icon {
                    source: "data-warning"
                    fallback: "dialog-warning"
                    color: Kirigami.Theme.neutralTextColor
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    Layout.alignment: Qt.AlignTop
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.family: ThemeController.fontFamily
                    color: Kirigami.Theme.disabledTextColor
                    text: qsTr("Programs listed here can be run by agents on " + "your machine through the shell tool. You are " + "responsible for what you allow. Dangerous " + "command patterns (such as piping a download " + "straight into a shell) are always blocked, " + "even for an allowed program.")
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: sheet._cPanel
            antialiasing: true
            implicitHeight: addCol.implicitHeight + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: addCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Allow a program")
                    font.family: ThemeController.fontFamily
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    AppTextField {
                        id: addField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Program name (e.g. rsync)")
                        onAccepted: sheet._addProgram()
                    }
                    AppButton {
                        text: qsTr("Add")
                        icon.name: "list-add"
                        enabled: ("" + addField.text).trim().length > 0
                        onClicked: sheet._addProgram()
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: sheet._cPanel
            antialiasing: true
            implicitHeight: listCol.implicitHeight + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: listCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Allowed programs (%1)").arg(SettingsService.shellAllowList.length)
                    font.family: ThemeController.fontFamily
                }
                Controls.Label {
                    Layout.fillWidth: true
                    visible: SettingsService.shellAllowList.length === 0
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.negativeTextColor
                    font.family: ThemeController.fontFamily
                    text: qsTr("No programs allowed. Agents cannot run any " + "shell command until you add one or Restore to " + "Default.")
                }

                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing
                    visible: SettingsService.shellAllowList.length > 0

                    Repeater {
                        model: SettingsService.shellAllowList

                        delegate: Rectangle {
                            id: pill
                            required property string modelData

                            implicitWidth: pillRow.implicitWidth + 16
                            implicitHeight: 28
                            radius: height / 2
                            color: sheet._cWell
                            border.width: 1
                            border.color: ThemeController.borderSubtle
                            antialiasing: true

                            RowLayout {
                                id: pillRow
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 4
                                spacing: 4

                                Controls.Label {
                                    text: pill.modelData
                                    font.family: ThemeController.fontFamily
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                Controls.ToolButton {
                                    icon.name: "edit-delete-remove"
                                    icon.width: 12
                                    icon.height: 12
                                    display: Controls.AbstractButton.IconOnly
                                    padding: 2
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    Layout.alignment: Qt.AlignVCenter
                                    Controls.ToolTip.text: qsTr("Remove %1").arg(pill.modelData)
                                    Controls.ToolTip.visible: hovered
                                    Controls.ToolTip.delay: 500
                                    onClicked: sheet._removeProgram(pill.modelData)
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: sheet._cPanel
            antialiasing: true
            implicitHeight: writeCol.implicitHeight + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: writeCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Write protection")
                    font.family: ThemeController.fontFamily
                }
                Controls.Switch {
                    Layout.fillWidth: true
                    text: qsTr("Only let agent commands write inside the project folder")
                    enabled: SettingsService.shellWriteRestrictionAvailable
                    checked: SettingsService.shellWriteRestriction
                    onToggled: SettingsService.shellWriteRestriction = checked
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.family: ThemeController.fontFamily
                    color: Kirigami.Theme.disabledTextColor
                    text: SettingsService.shellWriteRestrictionAvailable ? qsTr("Off by default. When on, commands agents run can still " + "read files and run programs anywhere, but can only " + "create, change or delete files in the project folder, " + "temporary folders and the folders below.") : qsTr("Not available on this system. Write protection needs " + "Linux with the Landlock security module.")
                }

                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing
                    visible: SettingsService.shellWriteRestrictionAvailable && SettingsService.shellWritableFolders.length > 0

                    Repeater {
                        model: SettingsService.shellWritableFolders

                        delegate: Rectangle {
                            id: folderPill
                            required property string modelData

                            implicitWidth: Math.min(folderRow.implicitWidth + 16, writeCol.width)
                            implicitHeight: 28
                            radius: height / 2
                            color: sheet._cWell
                            border.width: 1
                            border.color: ThemeController.borderSubtle
                            antialiasing: true

                            RowLayout {
                                id: folderRow
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 4
                                spacing: 4

                                Controls.Label {
                                    text: folderPill.modelData
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: implicitWidth
                                    font.family: ThemeController.fontFamily
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                Controls.ToolButton {
                                    icon.name: "edit-delete-remove"
                                    icon.width: 12
                                    icon.height: 12
                                    display: Controls.AbstractButton.IconOnly
                                    padding: 2
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    Layout.alignment: Qt.AlignVCenter
                                    Controls.ToolTip.text: qsTr("Remove %1").arg(folderPill.modelData)
                                    Controls.ToolTip.visible: hovered
                                    Controls.ToolTip.delay: 500
                                    onClicked: sheet._removeFolder(folderPill.modelData)
                                }
                            }
                        }
                    }
                }

                AppButton {
                    visible: SettingsService.shellWriteRestrictionAvailable
                    text: qsTr("Add folder")
                    icon.name: "folder-new"
                    onClicked: writableFolderPicker.open()
                }
            }
        }
    }
}

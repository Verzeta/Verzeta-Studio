// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: bar

    property string conversationId: ""

    property bool hasActiveCanvas: false

    property string canvasLanguage: ""

    signal toggleConsoleRequested

    property bool consoleVisible: false

    Layout.fillWidth: true
    implicitHeight: 60
    color: ThemeController.surfaceCard

    readonly property int _chipWideWidth: 140
    readonly property int _chipCompactWidth: 40
    readonly property int _chipHeight: 40
    readonly property int _moreWideWidth: 105
    readonly property int _moreCompactWidth: 40

    readonly property int _compactThreshold: 770
    readonly property int _iconOnlyThreshold: 360

    readonly property bool _isWide: bar.width >= _compactThreshold
    readonly property bool _isCompact: bar.width < _compactThreshold && bar.width >= _iconOnlyThreshold
    readonly property bool _isTiny: bar.width < _iconOnlyThreshold

    readonly property var _aiActions: bar.canvasLanguage.length > 0 ? CanvasAiActions.availableForLanguage(bar.canvasLanguage) : []
    readonly property var _aiPrimary: bar._aiActions.filter(function (a) {
            return a.primary === true;
        })
    readonly property var _aiOverflow: bar._aiActions.filter(function (a) {
            return a.primary !== true;
        })

    component AiChip: Controls.AbstractButton {
        id: chip
        property string iconName: ""
        property bool compact: false

        implicitHeight: 40
        implicitWidth: chip.compact ? 40 : Math.max(140, chipContent.implicitWidth + chip.leftPadding + chip.rightPadding)

        leftPadding: chip.compact ? 0 : 14
        rightPadding: chip.compact ? 0 : 14
        topPadding: 6
        bottomPadding: 6

        hoverEnabled: true
        focusPolicy: Qt.TabFocus

        background: Rectangle {
            radius: ThemeController.radius
            color: !chip.enabled ? Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.5) : (chip.pressed ? ThemeController.pressTint : (chip.checked ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, chip.hovered ? 0.34 : 0.22) : (chip.hovered ? ThemeController.hoverTint : Kirigami.Theme.backgroundColor)))
            border.width: 1
            border.color: !chip.enabled ? ThemeController.borderSubtle : (chip.checked ? Kirigami.Theme.highlightColor : (chip.hovered || chip.pressed ? Kirigami.Theme.disabledTextColor : ThemeController.borderSubtle))
            Behavior on color  {
                ColorAnimation {
                    duration: 100
                }
            }
            Behavior on border.color  {
                ColorAnimation {
                    duration: 100
                }
            }
        }

        contentItem: RowLayout {
            id: chipContent
            spacing: chip.compact ? 0 : 8

            Item {
                visible: chip.compact
                Layout.fillWidth: true
            }

            Kirigami.Icon {
                source: chip.iconName
                visible: chip.iconName.length > 0
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
                color: chip.enabled ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignVCenter
            }

            Controls.Label {
                text: chip.text
                visible: !chip.compact && chip.text.length > 0
                color: chip.enabled ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
            }

            Item {
                visible: chip.compact
                Layout.fillWidth: true
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: ThemeController.borderSubtle
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 8

        Repeater {
            model: bar._isTiny ? 0 : bar._aiPrimary.length
            delegate: AiChip {
                required property int index
                readonly property var actionEntry: bar._aiPrimary[index]
                readonly property bool _hasSubmenu: actionEntry.submenu && actionEntry.submenu.length > 0
                iconName: actionEntry.iconName
                text: _hasSubmenu ? actionEntry.label + " ▾" : actionEntry.label
                compact: bar._isCompact
                enabled: bar.hasActiveCanvas

                Layout.preferredWidth: bar._isCompact ? bar._chipCompactWidth : bar._chipWideWidth
                Layout.preferredHeight: bar._chipHeight
                Layout.alignment: Qt.AlignVCenter

                Controls.ToolTip.text: actionEntry.label + (actionEntry.description ? " - " + actionEntry.description : "")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 600

                onClicked: {
                    if (_hasSubmenu) {
                        primarySubmenu.actionId = actionEntry.id;
                        primarySubmenu.choices = actionEntry.submenu;
                        primarySubmenu.popup();
                    } else {
                        CanvasAiActions.trigger(actionEntry.id, "");
                    }
                }
            }
        }

        Controls.Menu {
            id: primarySubmenu
            property string actionId: ""
            property var choices: []
            Repeater {
                model: primarySubmenu.choices
                delegate: Controls.MenuItem {
                    required property int index
                    text: primarySubmenu.choices[index]
                    onTriggered: CanvasAiActions.trigger(primarySubmenu.actionId, primarySubmenu.choices[index])
                }
            }
        }

        AiChip {
            id: overflowBtn
            visible: bar.hasActiveCanvas && (bar._aiOverflow.length > 0 || (bar._isTiny && bar._aiPrimary.length > 0))
            iconName: "view-more-symbolic"
            text: qsTr("More")
            compact: bar._isTiny
            enabled: bar.hasActiveCanvas

            Layout.preferredWidth: bar._isTiny ? bar._moreCompactWidth : bar._moreWideWidth
            Layout.preferredHeight: bar._chipHeight
            Layout.alignment: Qt.AlignVCenter

            Controls.ToolTip.text: qsTr("More AI actions")
            Controls.ToolTip.visible: hovered
            Controls.ToolTip.delay: 600
            onClicked: overflowMenu.popup()

            Controls.Menu {
                id: overflowMenu

                Repeater {
                    model: bar._isTiny ? bar._aiPrimary.length : 0
                    delegate: Controls.MenuItem {
                        required property int index
                        readonly property var entry: bar._aiPrimary[index]
                        text: entry.label
                        icon.name: entry.iconName
                        onTriggered: {
                            if (entry.submenu && entry.submenu.length > 0) {
                                primarySubmenu.actionId = entry.id;
                                primarySubmenu.choices = entry.submenu;
                                primarySubmenu.popup();
                            } else {
                                CanvasAiActions.trigger(entry.id, "");
                            }
                        }
                    }
                }
                Controls.MenuSeparator {
                    visible: bar._isTiny && bar._aiPrimary.length > 0 && bar._aiOverflow.length > 0
                }
                Repeater {
                    model: bar._aiOverflow.length
                    delegate: Controls.MenuItem {
                        required property int index
                        readonly property var entry: bar._aiOverflow[index]
                        text: entry.label
                        icon.name: entry.iconName
                        onTriggered: {
                            if (entry.submenu && entry.submenu.length > 0) {
                                primarySubmenu.actionId = entry.id;
                                primarySubmenu.choices = entry.submenu;
                                primarySubmenu.popup();
                            } else {
                                CanvasAiActions.trigger(entry.id, "");
                            }
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
        }

        AiChip {
            visible: bar.hasActiveCanvas
            iconName: "utilities-terminal"
            compact: true
            checked: bar.consoleVisible

            Layout.preferredWidth: bar._chipCompactWidth
            Layout.minimumWidth: bar._chipCompactWidth
            Layout.preferredHeight: bar._chipHeight
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight

            Controls.ToolTip.text: bar.consoleVisible ? qsTr("Hide console") : qsTr("Show console")
            Controls.ToolTip.visible: hovered
            Controls.ToolTip.delay: 600
            onClicked: bar.toggleConsoleRequested()
        }
    }
}

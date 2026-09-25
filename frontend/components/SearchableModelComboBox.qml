// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

RowLayout {
    id: control

    property var models: []
    property string currentValue: ""
    property string placeholderText: qsTr("Model name")

    spacing: 0

    readonly property var _sorted: {
        var a = (control.models || []).slice();
        a.sort(function (x, y) {
                return String(x).toLowerCase().localeCompare(String(y).toLowerCase());
            });
        return a;
    }

    AppTextField {
        id: field
        Layout.fillWidth: true
        font.pointSize: Kirigami.Theme.defaultFont.pointSize
        placeholderText: control.placeholderText
        text: control.currentValue
        onTextEdited: control.currentValue = text
    }

    Controls.ToolButton {
        id: dropButton
        Layout.preferredHeight: field.implicitHeight
        enabled: control._sorted.length > 0
        text: popup.opened ? "▴" : "▾"
        Controls.ToolTip.text: qsTr("Browse %1 model(s)").arg(control._sorted.length)
        Controls.ToolTip.visible: hovered && control._sorted.length > 0
        onClicked: {
            if (popup.opened) {
                popup.close();
            } else {
                searchField.text = "";
                popup.open();
                searchField.forceActiveFocus();
            }
        }
    }

    Controls.Popup {
        id: popup
        y: control.height
        width: Math.min(Math.max(control.width, Kirigami.Units.gridUnit * 22), applicationWindow().width - Kirigami.Units.largeSpacing * 2)
        height: Math.min(Kirigami.Units.gridUnit * 18, applicationWindow().height * 0.5)
        padding: Kirigami.Units.smallSpacing

        background: Rectangle {
            color: ThemeController.surfaceCard
            radius: ThemeController.radius
            border.width: 1
            border.color: ThemeController.borderSubtle
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            AppTextField {
                id: searchField
                Layout.fillWidth: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                placeholderText: qsTr("Search models…")
            }

            ListView {
                id: listView
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: {
                    var q = searchField.text.trim().toLowerCase();
                    if (q.length === 0)
                        return control._sorted;
                    return control._sorted.filter(function (m) {
                            return String(m).toLowerCase().indexOf(q) >= 0;
                        });
                }
                Controls.ScrollBar.vertical: Controls.ScrollBar {
                }

                delegate: Controls.ItemDelegate {
                    id: row
                    required property var modelData
                    width: listView.width
                    implicitHeight: Math.round(Kirigami.Units.gridUnit * 1.8)
                    highlighted: control.currentValue === row.modelData
                    contentItem: Controls.Label {
                        text: row.modelData
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                        elide: Text.ElideMiddle
                        verticalAlignment: Text.AlignVCenter
                        color: Kirigami.Theme.textColor
                    }
                    onClicked: {
                        control.currentValue = row.modelData;
                        popup.close();
                    }
                }

                Controls.Label {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.largeSpacing * 2
                    visible: listView.count === 0
                    text: searchField.text.trim().length > 0 ? qsTr("No models match \"%1\". Type a model name in the field instead.").arg(searchField.text.trim()) : qsTr("No models fetched for this provider yet. Type a model name in the field instead.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }
    }
}

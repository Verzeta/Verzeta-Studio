// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.AbstractButton {
    id: pill

    checkable: true

    property string toolTipText: ""

    property bool iconOnly: false

    leftPadding: pill.iconOnly ? 9 : 15
    rightPadding: pill.iconOnly ? 9 : 15
    topPadding: 7
    bottomPadding: 7

    implicitWidth: pillRow.implicitWidth + leftPadding + rightPadding
    implicitHeight: pillRow.implicitHeight + topPadding + bottomPadding

    hoverEnabled: true

    background: Kirigami.ShadowedRectangle {
        radius: height / 2
        color: {
            if (pill.checked) {
                return ThemeController.selectionTint;
            }
            if (pill.hovered) {
                return Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.10);
            }
            return Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.6);
        }
        Behavior on color  {
            ColorAnimation {
                duration: 120
            }
        }

        border.width: pill.checked ? 1 : 1
        border.color: pill.checked ? Kirigami.Theme.highlightColor : Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.25)

        shadow {
            size: pill.checked ? Kirigami.Units.smallSpacing : Kirigami.Units.smallSpacing / 2
            color: Qt.rgba(0, 0, 0, pill.checked ? 0.25 : 0.15)
        }
    }

    contentItem: RowLayout {
        id: pillRow
        spacing: 6

        Kirigami.Icon {
            source: pill.icon.name
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
            color: pill.checked ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
        }

        Controls.Label {
            visible: !pill.iconOnly
            text: pill.text
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            font.bold: pill.checked
            color: pill.checked ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
            verticalAlignment: Text.AlignVCenter
        }
    }

    Controls.ToolTip.visible: hovered && toolTipText.length > 0
    Controls.ToolTip.text: toolTipText
    Controls.ToolTip.delay: 500
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.AbstractButton {
    id: btn

    property bool flat: false
    property bool pill: false
    property bool iconOnly: false
    property bool highlighted: false

    implicitWidth: Math.max(contentRow.implicitWidth + leftPadding + rightPadding, btn.iconOnly ? implicitHeight : 0)
    implicitHeight: 32

    leftPadding: btn.pill ? 14 : (btn.iconOnly ? 6 : 10)
    rightPadding: btn.pill ? 14 : (btn.iconOnly ? 6 : 10)
    topPadding: 4
    bottomPadding: 4

    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    background: Rectangle {
        id: bg
        radius: btn.pill ? btn.height / 2 : ThemeController.radius

        color: {
            if (btn.highlighted) {
                if (btn.pressed)
                    return Qt.darker(Kirigami.Theme.highlightColor, 1.2);
                if (btn.hovered)
                    return Qt.lighter(Kirigami.Theme.highlightColor, 1.1);
                return Kirigami.Theme.highlightColor;
            }
            if (btn.checked) {
                if (btn.pressed)
                    return ThemeController.pressTint;
                return ThemeController.selectionTint;
            }
            if (btn.flat) {
                if (btn.pressed)
                    return Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.14);
                if (btn.hovered)
                    return Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.08);
                return "transparent";
            }
            if (btn.pressed)
                return Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.14);
            if (btn.hovered)
                return Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.08);
            return "transparent";
        }

        border.color: {
            if (btn.pill) {
                if (btn.checked || btn.highlighted)
                    return Kirigami.Theme.highlightColor;
                return ThemeController.borderSubtle;
            }
            return "transparent";
        }
        border.width: btn.pill ? 1 : 0

        Behavior on color  {
            ColorAnimation {
                duration: 80
            }
        }
        Behavior on border.color  {
            ColorAnimation {
                duration: 80
            }
        }
    }

    contentItem: RowLayout {
        id: contentRow
        spacing: btn.iconOnly ? 0 : 6

        Item {
            Layout.fillWidth: true
        }

        Kirigami.Icon {
            source: btn.icon.name
            visible: btn.icon.name !== undefined && btn.icon.name.length > 0
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: Kirigami.Units.iconSizes.smallMedium
            color: btn.highlighted ? "white" : Kirigami.Theme.textColor
            Layout.alignment: Qt.AlignVCenter
        }

        Controls.Label {
            text: btn.text
            visible: !btn.iconOnly && btn.text.length > 0
            color: btn.highlighted ? "white" : btn.checked ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            Layout.alignment: Qt.AlignVCenter
        }

        Item {
            Layout.fillWidth: true
        }
    }

    Controls.ToolTip.text: btn.text
    Controls.ToolTip.visible: btn.hovered && btn.iconOnly && btn.text.length > 0
    Controls.ToolTip.delay: 600
}

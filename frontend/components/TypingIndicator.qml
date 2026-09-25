// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

Row {
    id: root
    spacing: 4
    height: 16

    Repeater {
        model: 3

        Rectangle {
            width: 8
            height: 8
            radius: height / 2
            color: Kirigami.Theme.disabledTextColor
            anchors.verticalCenter: parent.verticalCenter

            SequentialAnimation on opacity  {
                loops: Animation.Infinite
                running: root.visible

                PauseAnimation {
                    duration: index * 200
                }
                NumberAnimation {
                    from: 0.3
                    to: 1.0
                    duration: 400
                    easing.type: Easing.InOutSine
                }
                NumberAnimation {
                    from: 1.0
                    to: 0.3
                    duration: 400
                    easing.type: Easing.InOutSine
                }
                PauseAnimation {
                    duration: (2 - index) * 200
                }
            }
        }
    }
}

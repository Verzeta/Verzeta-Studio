// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Item {
    id: root

    property bool active: false

    property string label: qsTr("Working")

    property color baseColor: Kirigami.Theme.neutralTextColor
    property color midColor: Kirigami.Theme.negativeTextColor
    property color activeColor: Kirigami.Theme.highlightColor
    property int stepDuration: 250
    property color dynamicColor: baseColor

    function mixedColor(a, b) {
        return Qt.rgba((a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2, 1);
    }

    onActiveChanged: if (!active)
        dynamicColor = baseColor

    visible: opacity > 0.01
    opacity: active ? 1.0 : 0.0
    Behavior on opacity  {
        NumberAnimation {
            duration: Kirigami.Units.longDuration
            easing.type: Easing.InOutQuad
        }
    }

    implicitWidth: chipRow.implicitWidth + 16
    implicitHeight: 26

    Rectangle {
        anchors.fill: parent
        radius: ThemeController.radius
        color: ThemeController.surfaceSunken
        border.width: 1
        border.color: root.active ? root.activeColor : ThemeController.borderSubtle
        Behavior on border.color  {
            ColorAnimation {
                duration: 300
            }
        }
        antialiasing: true
    }

    Row {
        id: chipRow
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: 8
        spacing: 5

        Item {
            width: 18
            height: 18
            anchors.verticalCenter: parent.verticalCenter

            Loader {
                anchors.centerIn: parent
                active: root.active
                asynchronous: true
                sourceComponent: glyphComponent
            }
        }

        Controls.Label {
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            visible: root.label.length > 0
            color: Kirigami.Theme.disabledTextColor
            font.family: ThemeController.fontFamily
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
        }
    }

    Component {
        id: glyphComponent

        Item {
            id: graphicBounds
            width: 48
            height: 48
            scale: 18 / 48
            transformOrigin: Item.Center
            layer.enabled: true
            layer.smooth: true

            Rectangle {
                id: leftLine
                x: 14
                y: 6
                width: 5
                height: 36
                radius: width / 2
                color: root.dynamicColor
                antialiasing: true
                transformOrigin: Item.Center
            }
            Rectangle {
                id: rightLine
                x: 29
                y: 6
                width: 5
                height: 36
                radius: width / 2
                color: root.dynamicColor
                antialiasing: true
                transformOrigin: Item.Center
            }

            SequentialAnimation {
                running: root.active
                loops: Animation.Infinite

                PropertyAction {
                    target: leftLine
                    property: "x"
                    value: 14
                }
                PropertyAction {
                    target: leftLine
                    property: "rotation"
                    value: 0
                }
                PropertyAction {
                    target: rightLine
                    property: "x"
                    value: 29
                }
                PropertyAction {
                    target: rightLine
                    property: "rotation"
                    value: 0
                }
                PropertyAction {
                    target: root
                    property: "dynamicColor"
                    value: root.baseColor
                }
                PauseAnimation {
                    duration: 250
                }

                ParallelAnimation {
                    NumberAnimation {
                        target: leftLine
                        property: "x"
                        to: 16
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: leftLine
                        property: "rotation"
                        to: -22
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "x"
                        to: 27
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "rotation"
                        to: 22
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    ColorAnimation {
                        target: root
                        property: "dynamicColor"
                        to: root.mixedColor(root.baseColor, root.midColor)
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                }
                PauseAnimation {
                    duration: 150
                }

                ParallelAnimation {
                    NumberAnimation {
                        target: leftLine
                        property: "x"
                        to: 21.5
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: leftLine
                        property: "rotation"
                        to: 45
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "x"
                        to: 21.5
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "rotation"
                        to: -45
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    ColorAnimation {
                        target: root
                        property: "dynamicColor"
                        to: root.midColor
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                }
                PauseAnimation {
                    duration: 150
                }

                ParallelAnimation {
                    NumberAnimation {
                        target: leftLine
                        property: "x"
                        to: 16
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: leftLine
                        property: "rotation"
                        to: 22
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "x"
                        to: 27
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "rotation"
                        to: -22
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    ColorAnimation {
                        target: root
                        property: "dynamicColor"
                        to: root.activeColor
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                }
                PauseAnimation {
                    duration: 250
                }

                ParallelAnimation {
                    NumberAnimation {
                        target: leftLine
                        property: "x"
                        to: 21.5
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: leftLine
                        property: "rotation"
                        to: 45
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "x"
                        to: 21.5
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "rotation"
                        to: -45
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    ColorAnimation {
                        target: root
                        property: "dynamicColor"
                        to: root.midColor
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                }
                PauseAnimation {
                    duration: 150
                }

                ParallelAnimation {
                    NumberAnimation {
                        target: leftLine
                        property: "x"
                        to: 16
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: leftLine
                        property: "rotation"
                        to: -22
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "x"
                        to: 27
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "rotation"
                        to: 22
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    ColorAnimation {
                        target: root
                        property: "dynamicColor"
                        to: root.mixedColor(root.baseColor, root.midColor)
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                }
                PauseAnimation {
                    duration: 150
                }

                ParallelAnimation {
                    NumberAnimation {
                        target: leftLine
                        property: "x"
                        to: 14
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: leftLine
                        property: "rotation"
                        to: 0
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "x"
                        to: 29
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    NumberAnimation {
                        target: rightLine
                        property: "rotation"
                        to: 0
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                    ColorAnimation {
                        target: root
                        property: "dynamicColor"
                        to: root.baseColor
                        duration: root.stepDuration
                        easing.type: Easing.InOutQuad
                    }
                }
            }
        }
    }
}

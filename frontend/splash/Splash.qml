// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Window

Window {
    id: splash

    flags: Qt.SplashScreen | Qt.FramelessWindowHint
    color: "transparent"

    width: 440
    height: 300
    visible: true

    x: Screen.virtualX + (Screen.width - width) / 2
    y: Screen.virtualY + (Screen.height - height) / 2

    Rectangle {
        anchors.fill: parent
        radius: 16
        antialiasing: true
        color: "#1e1f22"
        border.width: 1
        border.color: "#3a3b3f"

        Column {
            anchors.centerIn: parent
            spacing: 18

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "verzeta-studio.png"
                fillMode: Image.PreserveAspectFit
                width: 144
                height: 108
                sourceSize.width: 256
                sourceSize.height: 192
                smooth: true
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Verzeta Studio"
                color: "#f2f2f3"
                font.pixelSize: 22
                font.bold: true
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Starting…"
                color: "#8a8b90"
                font.pixelSize: 13
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 9

                Repeater {
                    model: 3

                    delegate: Rectangle {
                        id: dot
                        required property int index
                        width: 9
                        height: 9
                        radius: 4.5
                        color: "#6aa6e0"
                        opacity: 0.3

                        SequentialAnimation on opacity  {
                            loops: Animation.Infinite
                            PauseAnimation {
                                duration: dot.index * 170
                            }
                            NumberAnimation {
                                to: 1.0
                                duration: 330
                                easing.type: Easing.InOutQuad
                            }
                            NumberAnimation {
                                to: 0.3
                                duration: 330
                                easing.type: Easing.InOutQuad
                            }
                            PauseAnimation {
                                duration: (2 - dot.index) * 170
                            }
                        }
                    }
                }
            }
        }
    }
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtMultimedia
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: root

    required property string audioUrl

    color: ThemeController.surfaceCard
    radius: height / 2
    implicitWidth: 300
    implicitHeight: 48

    MediaPlayer {
        id: player
        source: root.audioUrl
        audioOutput: AudioOutput {
            id: audioOut
        }
    }

    RowLayout {
        anchors {
            fill: parent
            margins: 8
        }
        spacing: 8

        Controls.ToolButton {
            icon.name: player.playbackState === MediaPlayer.PlayingState ? "media-playback-pause" : "media-playback-start"
            onClicked: {
                if (player.playbackState === MediaPlayer.PlayingState) {
                    player.pause();
                } else {
                    player.play();
                }
            }
        }

        Controls.Slider {
            id: seekSlider
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, player.duration)
            value: player.position
            onMoved: player.position = value

            background: Rectangle {
                y: (seekSlider.availableHeight - height) / 2
                width: seekSlider.availableWidth
                height: 4
                radius: 2
                color: ThemeController.surfaceSunken

                Rectangle {
                    width: seekSlider.visualPosition * parent.width
                    height: parent.height
                    radius: 2
                    color: Kirigami.Theme.highlightColor
                }
            }

            handle: Rectangle {
                x: seekSlider.leftPadding + seekSlider.visualPosition * (seekSlider.availableWidth - width)
                y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
                width: 12
                height: 12
                radius: 6
                color: Kirigami.Theme.highlightColor
            }
        }

        Controls.Label {
            text: formatTime(player.position) + " / " + formatTime(player.duration)
            font.pointSize: 9
            color: Kirigami.Theme.disabledTextColor
            Layout.preferredWidth: 80
        }
    }

    function formatTime(ms) {
        var totalSecs = Math.floor(ms / 1000);
        var mins = Math.floor(totalSecs / 60);
        var secs = totalSecs % 60;
        return mins + ":" + (secs < 10 ? "0" : "") + secs;
    }
}

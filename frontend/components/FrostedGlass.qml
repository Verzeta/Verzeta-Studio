// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Effects
import org.kde.kirigami as Kirigami

Item {
    id: root
    clip: true

    property url textureSource1: "qrc:/textures/fabric.png"
    property url textureSource2: "qrc:/textures/groovepaper.png"
    property int textureFillMode: Image.Tile
    property real textureOpacity: 1.0

    property bool blurEnabled: true
    property real blurAmount: 0.60
    property int blurMax: 24
    property real blurMultiplier: 1.0
    property bool hideSource: true

    property color tintColor: Kirigami.Theme.backgroundColor
    property real tintOpacity: 0.30

    property int radius: Kirigami.Units.cornerRadius
    property int topLeftRadius: radius
    property int topRightRadius: radius
    property int bottomLeftRadius: radius
    property int bottomRightRadius: radius
    property bool rounded: true

    Item {
        id: textureContainer
        anchors.fill: parent

        Image {
            id: texture
            anchors.fill: parent
            fillMode: root.textureFillMode
            source: root.textureSource1
            smooth: true
            opacity: root.textureOpacity

            MultiEffect {
                source: texture
                anchors.fill: parent
                colorization: tintOpacity
                colorizationColor: root.tintColor
            }
        }

        Image {
            id: texture2
            anchors.fill: parent
            fillMode: root.textureFillMode
            source: root.textureSource2
            smooth: true
            opacity: root.textureOpacity

            MultiEffect {
                source: texture2
                anchors.fill: parent
                colorization: tintOpacity
                colorizationColor: root.tintColor
            }
        }
    }
    ShaderEffectSource {
        id: capture
        anchors.fill: parent
        visible: false
        live: true
        recursive: false
        hideSource: root.blurEnabled && root.hideSource
        sourceItem: textureContainer
        onWidthChanged: scheduleUpdate()
        onHeightChanged: scheduleUpdate()
    }

    Kirigami.ShadowedRectangle {
        id: mask
        anchors.fill: parent
        visible: false
        layer.enabled: true
        color: "black"
        corners.topLeftRadius: root.rounded ? root.topLeftRadius : 0
        corners.topRightRadius: root.rounded ? root.topRightRadius : 0
        corners.bottomLeftRadius: root.rounded ? root.bottomLeftRadius : 0
        corners.bottomRightRadius: root.rounded ? root.bottomRightRadius : 0
    }

    MultiEffect {
        anchors.fill: parent
        autoPaddingEnabled: false
        visible: root.blurEnabled
        source: capture
        blurEnabled: root.blurEnabled
        blur: root.blurAmount
        blurMax: root.blurMax
        blurMultiplier: root.blurMultiplier
        maskEnabled: root.rounded
        maskSource: root.rounded ? mask : null
    }

    Kirigami.ShadowedRectangle {
        anchors.fill: parent
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false
        color: Qt.rgba(root.tintColor.r, root.tintColor.g, root.tintColor.b, root.tintOpacity)
        corners.topLeftRadius: root.rounded ? root.topLeftRadius : 0
        corners.topRightRadius: root.rounded ? root.topRightRadius : 0
        corners.bottomLeftRadius: root.rounded ? root.bottomLeftRadius : 0
        corners.bottomRightRadius: root.rounded ? root.bottomRightRadius : 0
    }
}

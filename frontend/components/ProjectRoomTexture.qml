// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

Item {
    id: root

    property int stripeStep: 14

    property real stripeAlpha: 0.05

    Connections {
        target: Kirigami.Theme
        function onColorSetChanged() {
            textureCanvas.requestPaint();
        }
    }
    onStripeStepChanged: textureCanvas.requestPaint()
    onStripeAlphaChanged: textureCanvas.requestPaint()

    Canvas {
        id: textureCanvas
        anchors.fill: parent
        antialiasing: true

        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            ctx.strokeStyle = Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, root.stripeAlpha);
            ctx.lineWidth = 1.0;
            ctx.beginPath();
            for (var x = -height; x < width + height; x += root.stripeStep) {
                ctx.moveTo(x, 0);
                ctx.lineTo(x + height, height);
            }
            ctx.stroke();
        }
    }
}

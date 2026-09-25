// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

Item {
    id: root

    property string geometryKind: "circles"

    property real baseHue: 210

    property real cornerRadius: 0

    onGeometryKindChanged: bannerCanvas.requestPaint()
    onBaseHueChanged: bannerCanvas.requestPaint()
    onCornerRadiusChanged: bannerCanvas.requestPaint()
    Connections {
        target: Kirigami.Theme
        function onColorSetChanged() {
            bannerCanvas.requestPaint();
        }
    }

    Canvas {
        id: bannerCanvas
        anchors.fill: parent
        antialiasing: true

        function _roundRectPath(ctx, x, y, w, h, r) {
            var rr = Math.min(r, w * 0.5, h * 0.5);
            ctx.beginPath();
            ctx.moveTo(x + rr, y);
            ctx.lineTo(x + w - rr, y);
            ctx.arcTo(x + w, y, x + w, y + rr, rr);
            ctx.lineTo(x + w, y + h - rr);
            ctx.arcTo(x + w, y + h, x + w - rr, y + h, rr);
            ctx.lineTo(x + rr, y + h);
            ctx.arcTo(x, y + h, x, y + h - rr, rr);
            ctx.lineTo(x, y + rr);
            ctx.arcTo(x, y, x + rr, y, rr);
            ctx.closePath();
        }

        function _shapeColor(alpha) {
            return Qt.hsla(root.baseHue / 360.0, 0.55, 0.55, alpha);
        }

        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            var w = width, h = height;
            if (w <= 0 || h <= 0)
                return;
            if (root.cornerRadius > 0) {
                _roundRectPath(ctx, 0, 0, w, h, root.cornerRadius);
                ctx.clip();
            }
            ctx.fillStyle = Kirigami.Theme.alternateBackgroundColor;
            ctx.fillRect(0, 0, w, h);
            ctx.strokeStyle = Qt.rgba(0, 0, 0, 0.15);
            ctx.lineWidth = 1.0;
            ctx.beginPath();
            for (var sx = -h; sx < w + h; sx += 10) {
                ctx.moveTo(sx, 0);
                ctx.lineTo(sx + h, h);
            }
            ctx.stroke();
            var cx = w * 0.5, cy = h * 0.55;
            switch (root.geometryKind) {
            case "circles":
                ctx.strokeStyle = _shapeColor(0.9);
                ctx.lineWidth = 2.0;
                for (var i = 1; i <= 4; ++i) {
                    var r = (Math.min(w, h) * 0.32) * i;
                    ctx.beginPath();
                    ctx.arc(cx, cy, r, 0, Math.PI * 2);
                    ctx.globalAlpha = 1.0 - (i - 1) * 0.22;
                    ctx.stroke();
                }
                ctx.globalAlpha = 1.0;
                break;
            case "grid":
                ctx.strokeStyle = _shapeColor(0.9);
                ctx.lineWidth = 1.5;
                var cols = 6, rows = 4;
                var gx0 = w * 0.15, gx1 = w * 0.85;
                var gy0 = h * 0.20, gy1 = h * 0.85;
                for (var c = 0; c <= cols; ++c) {
                    var px = gx0 + (gx1 - gx0) * (c / cols);
                    ctx.beginPath();
                    ctx.moveTo(px, gy0);
                    ctx.lineTo(px, gy1);
                    ctx.stroke();
                }
                for (var rr2 = 0; rr2 <= rows; ++rr2) {
                    var py = gy0 + (gy1 - gy0) * (rr2 / rows);
                    ctx.beginPath();
                    ctx.moveTo(gx0, py);
                    ctx.lineTo(gx1, py);
                    ctx.stroke();
                }
                break;
            case "triangle":
                ctx.fillStyle = _shapeColor(0.85);
                var tx = w * 0.62, ty = h * 0.20;
                var size = Math.min(w, h) * 0.55;
                ctx.beginPath();
                ctx.moveTo(tx, ty);
                ctx.lineTo(tx - size * 0.5, ty + size * 0.866);
                ctx.lineTo(tx + size * 0.5, ty + size * 0.866);
                ctx.closePath();
                ctx.fill();
                break;
            case "wave":
                ctx.strokeStyle = _shapeColor(0.85);
                ctx.lineWidth = 2.5;
                for (var k = 0; k < 4; ++k) {
                    var amp = h * 0.10;
                    var midY = h * (0.30 + k * 0.13);
                    ctx.globalAlpha = 1.0 - k * 0.18;
                    ctx.beginPath();
                    ctx.moveTo(0, midY);
                    for (var xx = 0; xx <= w; xx += 4) {
                        var yy = midY + Math.sin((xx / w) * Math.PI * 3 + k) * amp;
                        ctx.lineTo(xx, yy);
                    }
                    ctx.stroke();
                }
                ctx.globalAlpha = 1.0;
                break;
            case "bars":
                ctx.fillStyle = _shapeColor(0.85);
                var nBars = 8;
                var barW = (w * 0.7) / nBars;
                var barX0 = w * 0.18;
                var barBottom = h * 0.85;
                var heights = [0.30, 0.55, 0.42, 0.70, 0.50, 0.85, 0.60, 0.78];
                for (var b = 0; b < nBars; ++b) {
                    var bh = (h * 0.55) * heights[b];
                    ctx.fillRect(barX0 + b * barW + 2, barBottom - bh, barW - 4, bh);
                }
                break;
            case "arrows":
                ctx.strokeStyle = _shapeColor(0.9);
                ctx.lineWidth = 3.0;
                for (var a = 0; a < 5; ++a) {
                    var ax = w * (0.20 + a * 0.13);
                    var ay = h * 0.55;
                    var asz = Math.min(w, h) * 0.18;
                    ctx.globalAlpha = 0.4 + a * 0.13;
                    ctx.beginPath();
                    ctx.moveTo(ax - asz * 0.5, ay - asz * 0.5);
                    ctx.lineTo(ax + asz * 0.5, ay);
                    ctx.lineTo(ax - asz * 0.5, ay + asz * 0.5);
                    ctx.stroke();
                }
                ctx.globalAlpha = 1.0;
                break;
            case "spiral":
                ctx.strokeStyle = _shapeColor(0.95);
                ctx.lineWidth = 2.0;
                var maxR = Math.min(w, h) * 0.4;
                ctx.beginPath();
                for (var t = 0; t < Math.PI * 6; t += 0.08) {
                    var rr3 = maxR * (t / (Math.PI * 6));
                    var px2 = cx + rr3 * Math.cos(t);
                    var py2 = cy + rr3 * Math.sin(t);
                    if (t === 0)
                        ctx.moveTo(px2, py2);
                    else
                        ctx.lineTo(px2, py2);
                }
                ctx.stroke();
                break;
            case "dots":
                ctx.fillStyle = _shapeColor(0.9);
                var dx0 = w * 0.12, dy0 = h * 0.18;
                var dx1 = w * 0.88, dy1 = h * 0.82;
                var dcols = 12, drows = 6;
                for (var dc = 0; dc < dcols; ++dc) {
                    for (var dr = 0; dr < drows; ++dr) {
                        var dpx = dx0 + (dx1 - dx0) * (dc / (dcols - 1));
                        var dpy = dy0 + (dy1 - dy0) * (dr / (drows - 1));
                        var sz = 3.0 * (0.6 + 0.6 * Math.sin((dc + dr * 1.3) * 0.6));
                        ctx.beginPath();
                        ctx.arc(dpx, dpy, sz, 0, Math.PI * 2);
                        ctx.fill();
                    }
                }
                break;
            default:
                break;
            }
        }
    }
}

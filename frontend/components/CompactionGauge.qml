// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Shapes
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppButton {
    id: compactButton

    property bool veryCompact: false

    flat: true
    iconOnly: true
    text: qsTr("Compact conversation")
    visible: ChatController.activeConversationId.length > 0 && !compactButton.veryCompact
    enabled: !ChatController.isGenerating && !ChatController.isCompacting
    onClicked: ChatController.compactNow()

    readonly property int cadencePct: ChatController.compactionTurnsTotal > 0 ? Math.min(100, Math.round(ChatController.compactionTurnsUsed * 100 / ChatController.compactionTurnsTotal)) : -1
    readonly property int turnsLeft: Math.max(0, ChatController.compactionTurnsTotal - ChatController.compactionTurnsUsed)
    readonly property int contextPct: ChatController.contextFillPercent

    function bandColor(p) {
        if (p < 0)
            return Kirigami.Theme.disabledTextColor;
        if (p <= 35)
            return Kirigami.Theme.textColor;
        if (p <= 65)
            return Kirigami.Theme.positiveTextColor;
        if (p <= 85)
            return "#d9a23b";
        if (p <= 95)
            return "#e0793b";
        return "#e05d5d";
    }

    Item {
        id: gauge
        anchors.centerIn: parent
        width: 18
        height: 18
        opacity: compactButton.enabled ? 1.0 : 0.4

        RingTrack {
            r: gauge.width / 2 - 1.5
        }
        RingValue {
            r: gauge.width / 2 - 1.5
            pct: compactButton.cadencePct
            col: compactButton.bandColor(compactButton.cadencePct)
        }
        RingTrack {
            r: gauge.width / 2 - 5
        }
        RingValue {
            r: gauge.width / 2 - 5
            pct: compactButton.contextPct
            col: compactButton.bandColor(compactButton.contextPct)
        }

        Shape {
            id: compactSpinner
            anchors.fill: parent
            visible: ChatController.isCompacting
            preferredRendererType: Shape.CurveRenderer
            transformOrigin: Item.Center
            ShapePath {
                strokeWidth: 2
                strokeColor: Kirigami.Theme.highlightColor
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: gauge.width / 2
                    centerY: gauge.height / 2
                    radiusX: gauge.width / 2 - 1
                    radiusY: gauge.height / 2 - 1
                    startAngle: 0
                    sweepAngle: 100
                }
            }
            RotationAnimator on rotation  {
                running: compactSpinner.visible
                loops: Animation.Infinite
                from: 0
                to: 360
                duration: 900
            }
        }
    }

    component RingTrack: Shape {
        id: ringTrack
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        property real r: 1
        ShapePath {
            strokeWidth: 1.6
            strokeColor: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.16)
            fillColor: "transparent"
            PathAngleArc {
                centerX: gauge.width / 2
                centerY: gauge.height / 2
                radiusX: ringTrack.r
                radiusY: ringTrack.r
                startAngle: 0
                sweepAngle: 360
            }
        }
    }
    component RingValue: Shape {
        id: ringValue
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        property real r: 1
        property int pct: 0
        property color col: Kirigami.Theme.disabledTextColor
        visible: pct > 0
        ShapePath {
            strokeWidth: 1.8
            strokeColor: ringValue.col
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: gauge.width / 2
                centerY: gauge.height / 2
                radiusX: ringValue.r
                radiusY: ringValue.r
                startAngle: -90
                sweepAngle: 360 * Math.min(100, ringValue.pct) / 100
                Behavior on sweepAngle  {
                    NumberAnimation {
                        duration: 400
                        easing.type: Easing.InOutQuad
                    }
                }
            }
        }
    }

    Controls.ToolTip.text: {
        var cad;
        if (ChatController.compactionWaitingForIdle)
            cad = qsTr("refresh is due: waiting for the current round " + "to finish (it never interrupts a reply)");
        else if (compactButton.cadencePct < 0)
            cad = qsTr("auto-refresh off: runs only on context pressure or " + "when you compact");
        else
            cad = qsTr("~%1 turn(s) until the next refresh (%2/%3)").arg(compactButton.turnsLeft).arg(ChatController.compactionTurnsUsed).arg(ChatController.compactionTurnsTotal);
        var ctx = ChatController.contextFillPercent > 0 ? qsTr("%1% full").arg(ChatController.contextFillPercent) : qsTr("measured after the first reply");
        return qsTr("Outer ring, memory refresh: %1\n" + "Inner ring, context window: %2\n\n" + "Whichever ring fills first triggers an automatic refresh " + "(older messages are summarised so decisions stay in " + "context). Click to compact now.").arg(cad).arg(ctx);
    }
    Controls.ToolTip.visible: hovered
    Controls.ToolTip.delay: 400
}

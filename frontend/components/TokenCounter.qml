// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

RowLayout {
    id: root
    spacing: 6

    readonly property int _msgCount: ChatController.messages ? ChatController.messages.count : 0

    StatChip {
        iconSource: "dialog-messages"
        iconFallback: "mail-message"
        value: root._msgCount.toLocaleString(Qt.locale(), "f", 0)
        tip: qsTr("%1 message(s) in this conversation").arg(root._msgCount)
        visible: root._msgCount > 0
    }

    StatChip {
        iconSource: "view-statistics"
        iconFallback: "office-chart-bar"
        value: ChatController.totalTokens.toLocaleString(Qt.locale(), "f", 0)
        tip: qsTr("%1 tokens used").arg(ChatController.totalTokens.toLocaleString(Qt.locale(), "f", 0))
        visible: ChatController.totalTokens > 0
    }

    StatChip {
        iconSource: "help-donate"
        iconFallback: "wallet-open"
        value: "$" + ChatController.estimatedCostUsd.toFixed(4)
        tip: qsTr("Estimated cost so far (USD)")
        visible: ChatController.estimatedCostUsd > 0
    }

    StatChip {
        iconSource: "chronometer"
        iconFallback: "clock"
        value: qsTr("%1 ms").arg(ChatController.lastResponseTimeMs)
        tip: qsTr("Last response time")
        visible: ChatController.lastResponseTimeMs > 0
    }
}

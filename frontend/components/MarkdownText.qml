// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.TextArea {
    id: root

    required property string rawText

    textFormat: TextEdit.RichText
    text: rawText.length > 0 ? MarkdownConverter.toHtml(rawText) : ""
    wrapMode: TextEdit.Wrap
    readOnly: true
    selectByMouse: true

    onLinkActivated: link => Qt.openUrlExternally(link);

    font.pointSize: Kirigami.Theme.defaultFont.pointSize
    font.family: ThemeController.fontFamily
    color: Kirigami.Theme.textColor

    background: null
    leftPadding: 0
    rightPadding: 0
    topPadding: 2
    bottomPadding: 2

    Layout.fillWidth: true
}

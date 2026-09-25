// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.syntaxhighlighting
import org.verzeta.studio 1.0

Rectangle {
    id: root

    required property string code

    required property string language

    readonly property string _ksyntaxDefinition: {
        var lang = (root.language || "").toLowerCase().trim();
        var m = {
            "python": "Python",
            "py": "Python",
            "javascript": "JavaScript",
            "js": "JavaScript",
            "typescript": "TypeScript",
            "ts": "TypeScript",
            "cpp": "C++",
            "c++": "C++",
            "cxx": "C++",
            "cc": "C++",
            "c": "C",
            "h": "C",
            "hpp": "C++",
            "java": "Java",
            "rust": "Rust",
            "rs": "Rust",
            "go": "Go",
            "ruby": "Ruby",
            "rb": "Ruby",
            "shell": "Bash",
            "bash": "Bash",
            "sh": "Bash",
            "zsh": "Bash",
            "html": "HTML",
            "css": "CSS",
            "json": "JSON",
            "xml": "XML",
            "yaml": "YAML",
            "yml": "YAML",
            "toml": "TOML",
            "ini": "INI Files",
            "markdown": "Markdown",
            "md": "Markdown",
            "qml": "QML",
            "sql": "SQL",
            "dockerfile": "Dockerfile",
            "makefile": "Makefile",
            "cmake": "CMake",
            "diff": "Diff",
            "patch": "Diff"
        };
        return m[lang] || "";
    }

    color: ThemeController.codeBackground
    radius: ThemeController.radius
    implicitHeight: codeLayout.implicitHeight + 20
    Layout.fillWidth: true

    ColumnLayout {
        id: codeLayout
        anchors.fill: parent
        anchors.margins: 10
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Rectangle {
                color: "#2d2d2d"
                radius: 3
                implicitWidth: langLabel.implicitWidth + 8
                implicitHeight: langLabel.implicitHeight + 4

                Controls.Label {
                    id: langLabel
                    anchors.centerIn: parent
                    text: root.language.length > 0 ? root.language : "code"
                    font.pointSize: 9
                    font.bold: true
                    color: "#9cdcfe"
                }
            }

            Item {
                Layout.fillWidth: true
            }

            Controls.Label {
                id: copyLabel
                text: qsTr("Copied!")
                color: "#4ec9b0"
                font.pointSize: 9
                visible: false

                Timer {
                    id: copyTimer
                    interval: 2000
                    onTriggered: copyLabel.visible = false
                }
            }

            Controls.ToolButton {
                id: copyBtn
                icon.name: "edit-copy"
                icon.color: "#cccccc"
                onClicked: {
                    clipboardHelper.text = root.code;
                    clipboardHelper.selectAll();
                    clipboardHelper.copy();
                    copyLabel.visible = true;
                    copyTimer.start();
                }
                Controls.ToolTip {
                    text: qsTr("Copy code to clipboard")
                    visible: copyBtn.hovered
                }
            }
        }

        Controls.TextArea {
            id: codeArea
            text: root.code
            readOnly: true

            font.family: ThemeController.codeFontFamily
            font.pointSize: Kirigami.Theme.defaultFont.pointSize
            color: ThemeController.codeText
            background: null
            wrapMode: TextEdit.NoWrap
            selectByMouse: true

            Layout.fillWidth: true
        }

        SyntaxHighlighter {
            textEdit: codeArea
            definition: root._ksyntaxDefinition
            theme: ThemeController.isDarkMode ? "Breeze Dark" : "Breeze Light"
        }
    }

    TextEdit {
        id: clipboardHelper
        visible: false
        text: ""
    }
}

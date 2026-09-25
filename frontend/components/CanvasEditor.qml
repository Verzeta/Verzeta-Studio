// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.syntaxhighlighting
import org.verzeta.studio 1.0

Item {
    id: root

    property string content: ""

    property string language: "plaintext"

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
            "patch": "Diff",
            "plaintext": "",
            "": ""
        };
        return m[lang] !== undefined ? m[lang] : "";
    }

    property bool readOnly: true

    property bool wordWrap: false

    signal saveRequested(string content)

    property int saveDebounceMs: 1000

    function flushPendingSave() {
        if (root.readOnly)
            return;
        if (bodyTextArea.text === root.content)
            return;
        saveDebounceTimer.stop();
        root.saveRequested(bodyTextArea.text);
    }

    readonly property int gutterWidth: 56

    readonly property font monoFont: {
        var f = Qt.font({
                "family": "monospace"
            });
        f.pointSize = Kirigami.Theme.defaultFont.pointSize;
        return f;
    }

    Rectangle {
        anchors.fill: parent
        color: Kirigami.Theme.backgroundColor
        border.width: 0

        RowLayout {
            anchors.fill: parent
            spacing: 0

            CanvasLineNumberGutter {
                id: gutter
                Layout.fillHeight: true
                Layout.preferredWidth: root.gutterWidth
                textDocument: bodyTextArea.textDocument
                scrollY: {
                    if (bodyScroll && bodyScroll.contentItem && bodyScroll.contentItem.contentY !== undefined) {
                        return bodyScroll.contentItem.contentY;
                    }
                    return bodyTextArea.contentY > 0 ? bodyTextArea.contentY : 0;
                }
                font: root.monoFont
                textColor: Kirigami.Theme.disabledTextColor
                backgroundColor: ThemeController.surfaceSunken
                rightPadding: 8
            }

            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                color: ThemeController.borderSubtle
            }

            Controls.ScrollView {
                id: bodyScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Controls.TextArea {
                    id: bodyTextArea
                    text: root.content
                    readOnly: root.readOnly
                    selectByMouse: true
                    selectByKeyboard: true
                    font: root.monoFont
                    width: bodyScroll.availableWidth - (root.wordWrap ? 1 : 0)
                    wrapMode: root.wordWrap ? TextEdit.Wrap : TextEdit.NoWrap
                    persistentSelection: true
                    color: Kirigami.Theme.textColor
                    background: Rectangle {
                        color: Kirigami.Theme.backgroundColor
                    }

                    onTextChanged: {
                        if (root.readOnly)
                            return;
                        if (text === root.content)
                            return;
                        saveDebounceTimer.restart();
                    }

                    SyntaxHighlighter {
                        textEdit: bodyTextArea
                        definition: root._ksyntaxDefinition
                        theme: ThemeController.isDarkMode ? "Breeze Dark" : "Breeze Light"
                    }
                }
            }
        }
    }

    Timer {
        id: saveDebounceTimer
        interval: root.saveDebounceMs
        repeat: false
        onTriggered: {
            if (root.readOnly)
                return;
            if (bodyTextArea.text === root.content)
                return;
            root.saveRequested(bodyTextArea.text);
        }
    }

    Shortcut {
        sequences: [StandardKey.Save]
        enabled: !root.readOnly
        onActivated: {
            saveDebounceTimer.stop();
            if (bodyTextArea.text === root.content)
                return;
            root.saveRequested(bodyTextArea.text);
        }
    }

    onContentChanged: {
        if (bodyTextArea.text !== content) {
            bodyTextArea.text = content;
        }
    }
}

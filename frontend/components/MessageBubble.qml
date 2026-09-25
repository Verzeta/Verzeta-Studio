// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Item {
    id: root

    required property string messageId
    required property string role
    required property string content
    required property string contentHtml
    required property bool isStreaming
    required property int tokenCount
    required property string modelUsed
    required property string finishReason

    property string thinkingContent: ""

    readonly property bool _thinkingOnlyFallback: !root.isStreaming && root.content.trim().length === 0 && root.thinkingContent.length > 0
    property string agentId: ""
    property string agentName: ""
    property string agentRoleName: ""
    property string agentIcon: ""
    property var metadata: ({})

    property var attachments: []

    readonly property bool echoFailed: {
        if (metadata && metadata.echo_failed === true)
            return true;
        return content.startsWith("[ECHO-FAILED");
    }

    readonly property string displayContent: {
        if (!echoFailed)
            return content;
        return content.replace(/^\[ECHO-FAILED[^\]]*\]\s*\n*/, "");
    }

    readonly property var contentSegments: {
        if (root.isStreaming)
            return [];
        return MarkdownConverter.splitContentSegments(root.displayContent);
    }

    readonly property string _pollId: {
        if (!root.metadata)
            return "";
        var v = root.metadata.poll_id;
        if (v === undefined || v === null)
            return "";
        var s = String(v);
        if (s === "undefined" || s === "null")
            return "";
        return s;
    }
    readonly property bool _hasPollCard: _pollId.length > 0

    implicitHeight: col.height

    Column {
        id: col
        width: parent.width
        spacing: 4

        Row {
            spacing: 6

            Kirigami.Icon {
                visible: root.role === "assistant" && root.agentName.length > 0
                source: root.agentIcon.length > 0 ? root.agentIcon : "face-smile"
                fallback: "user"
                width: Kirigami.Units.iconSizes.small
                height: Kirigami.Units.iconSizes.small
                anchors.verticalCenter: parent.verticalCenter
                color: Kirigami.Theme.positiveTextColor
            }

            Controls.Label {
                text: {
                    if (root.role === "user")
                        return qsTr("You");
                    var alias = root.agentName;
                    var role = root.agentRoleName;
                    if (alias.length > 0 && role.length > 0 && alias !== role) {
                        return role + ": @" + alias;
                    }
                    if (alias.length > 0)
                        return alias;
                    if (role.length > 0)
                        return role;
                    return qsTr("Assistant");
                }
                font.bold: true
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                color: root.role === "user" ? Kirigami.Theme.activeTextColor : Kirigami.Theme.positiveTextColor
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Rectangle {
            width: parent.width
            height: contentCol.height + 28
            radius: ThemeController.radius
            color: {
                if (root.echoFailed) {
                    return Qt.rgba(Kirigami.Theme.negativeTextColor.r, Kirigami.Theme.negativeTextColor.g, Kirigami.Theme.negativeTextColor.b, 0.08);
                }
                return root.role === "user" ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.15) : ThemeController.surfaceCard;
            }
            border.color: root.echoFailed ? Kirigami.Theme.negativeTextColor : ThemeController.borderSubtle
            border.width: root.echoFailed ? 2 : 1

            Column {
                id: contentCol
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 14
                spacing: 8

                Row {
                    visible: root.echoFailed
                    spacing: 6

                    Kirigami.Icon {
                        source: "dialog-warning"
                        fallback: "dialog-error"
                        width: Kirigami.Units.iconSizes.small
                        height: Kirigami.Units.iconSizes.small
                        anchors.verticalCenter: parent.verticalCenter
                        color: Kirigami.Theme.negativeTextColor
                    }
                    Controls.Label {
                        text: qsTr("Echo-failed: this agent could not produce an original reply after multiple retries. Content kept for transparency.")
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                        font.italic: true
                        color: Kirigami.Theme.negativeTextColor
                        wrapMode: Text.Wrap
                        width: contentCol.width - 22
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Controls.TextArea {
                    visible: root.isStreaming
                    width: contentCol.width
                    text: root.content
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    readOnly: true
                    selectByMouse: true
                    background: null
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 2
                    bottomPadding: 2
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    font.family: ThemeController.fontFamily
                    color: Kirigami.Theme.textColor
                    opacity: root.echoFailed ? 0.55 : 1.0
                }

                Controls.Label {
                    visible: root._thinkingOnlyFallback
                    width: contentCol.width
                    text: qsTr("The model produced reasoning only. Expand below to see it.")
                    wrapMode: Text.Wrap
                    font.italic: true
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    color: Kirigami.Theme.disabledTextColor
                }

                AssistantThinkingDisclosure {
                    width: contentCol.width
                    thinkingContent: root.thinkingContent
                    startExpanded: root._thinkingOnlyFallback
                }

                Repeater {
                    model: root.isStreaming ? [] : root.contentSegments

                    delegate: Loader {
                        width: contentCol.width
                        required property var modelData
                        opacity: root.echoFailed ? 0.55 : 1.0
                        sourceComponent: {
                            if (!modelData)
                                return null;
                            return modelData.type === "code" ? codeBlockComp : markdownComp;
                        }
                        onLoaded: {
                            if (!modelData || !item)
                                return;
                            if (modelData.type === "code") {
                                item.code = modelData.content;
                                item.language = modelData.language;
                            } else {
                                item.rawText = modelData.content;
                            }
                        }
                    }
                }

                TypingIndicator {
                    visible: root.isStreaming
                }

                Repeater {
                    model: root.attachments
                    delegate: Loader {
                        required property var modelData
                        active: modelData && modelData.type === "image" && modelData.dataPath && modelData.dataPath.length > 0
                        sourceComponent: imageAttachComp
                        onLoaded: {
                            if (item && modelData) {
                                item.imagePath = modelData.dataPath;
                                item.caption = modelData.filename || "";
                            }
                        }
                    }
                }

                Loader {
                    width: contentCol.width
                    active: root._hasPollCard
                    sourceComponent: pollCardComp
                    onLoaded: {
                        if (item)
                            item.pollId = root._pollId;
                    }
                }
            }
        }

        Row {
            visible: !root.isStreaming && root.role === "assistant"
            spacing: 8
            Controls.Label {
                text: root.modelUsed
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                color: Kirigami.Theme.disabledTextColor
                visible: root.modelUsed.length > 0
            }
            Controls.Label {
                text: root.tokenCount > 0 ? qsTr("%1 tokens").arg(root.tokenCount) : ""
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                color: Kirigami.Theme.disabledTextColor
                visible: root.tokenCount > 0
            }
        }
    }

    Component {
        id: markdownComp
        MarkdownText {
            rawText: ""
        }
    }
    Component {
        id: codeBlockComp
        CodeBlock {
            code: ""
            language: ""
        }
    }
    Component {
        id: imageAttachComp
        ImageAttachment {
            imagePath: ""
            caption: ""
        }
    }
    Component {
        id: pollCardComp
        PollCard {
            pollId: ""
        }
    }
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: sheet

    title: qsTr("Conversation Settings")
    dialogIcon: "settings-configure"

    ColumnLayout {
        id: contentColumn
        spacing: Kirigami.Units.largeSpacing

        Rectangle {
            id: samplingProfileBanner

            readonly property string warningText: AgentSettings.samplingProfileWarning

            visible: warningText.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: bannerColumn.implicitHeight + (Kirigami.Units.largeSpacing * 2)
            radius: Kirigami.Units.smallSpacing
            color: Kirigami.Theme.neutralBackgroundColor
            border.color: Kirigami.Theme.neutralTextColor
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    source: "documentinfo"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    Layout.alignment: Qt.AlignTop
                    color: Kirigami.Theme.neutralTextColor
                }

                ColumnLayout {
                    id: bannerColumn
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        text: qsTr("Sampling profile active")
                        font.bold: true
                        color: Kirigami.Theme.neutralTextColor
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        text: samplingProfileBanner.warningText
                        color: Kirigami.Theme.neutralTextColor
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
        }

        Kirigami.FormLayout {
            id: formLayout

            Controls.TextArea {
                id: systemPromptField

                Kirigami.FormData.label: qsTr("System Prompt:")
                text: AgentSettings.activeSystemPrompt
                placeholderText: qsTr("You are a helpful assistant...")
                wrapMode: TextEdit.Wrap

                Layout.fillWidth: true
                Layout.preferredHeight: 100

                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        systemPromptField.text = AgentSettings.activeSystemPrompt;
                    }
                }
            }

            ModelSelector {
                Kirigami.FormData.label: qsTr("Model:")
            }

            RowLayout {
                Kirigami.FormData.label: qsTr("Temperature:")
                spacing: Kirigami.Units.smallSpacing

                Controls.Slider {
                    id: tempSlider

                    from: 0.0
                    to: 2.0
                    stepSize: 0.1
                    value: AgentSettings.activeTemperature

                    Layout.fillWidth: true

                    Connections {
                        target: AgentSettings
                        function onActiveConversationSettingsChanged() {
                            tempSlider.value = AgentSettings.activeTemperature;
                        }
                    }
                }

                Controls.Label {
                    text: tempSlider.value.toFixed(1)
                    Layout.preferredWidth: 32
                    horizontalAlignment: Text.AlignRight
                }
            }

            Controls.SpinBox {
                id: maxTokensSpin

                Kirigami.FormData.label: qsTr("Max Tokens:")
                from: -1
                to: 128000
                stepSize: 256
                value: AgentSettings.activeMaxTokens
                editable: true

                textFromValue: function (value, locale) {
                    if (value < 0) {
                        return qsTr("Auto (no cap)");
                    }
                    return Number(value).toLocaleString(locale, 'f', 0);
                }

                valueFromText: function (text, locale) {
                    const normalised = text.trim().toLowerCase();
                    if (normalised === "" || normalised === "-1" || normalised.indexOf("auto") >= 0 || normalised.indexOf("no cap") >= 0) {
                        return -1;
                    }
                    const parsed = Number.fromLocaleString(locale, text);
                    return isNaN(parsed) ? -1 : parsed;
                }

                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        maxTokensSpin.value = AgentSettings.activeMaxTokens;
                    }
                }
            }

            Controls.Switch {
                id: streamingSwitch

                Kirigami.FormData.label: qsTr("Streaming:")
                checked: AgentSettings.activeStreaming

                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        streamingSwitch.checked = AgentSettings.activeStreaming;
                    }
                }
            }

            Controls.Switch {
                id: thinkingSwitch

                Kirigami.FormData.label: qsTr("Thinking Mode:")
                checked: AgentSettings.activeThinking

                Connections {
                    target: AgentSettings
                    function onActiveConversationSettingsChanged() {
                        thinkingSwitch.checked = AgentSettings.activeThinking;
                    }
                }
            }

            Controls.Button {
                text: qsTr("Save")
                icon.name: "document-save"

                onClicked: {
                    AgentSettings.saveConversationConfig({
                            "systemPrompt": systemPromptField.text,
                            "temperature": tempSlider.value,
                            "maxTokens": maxTokensSpin.value,
                            "streaming": streamingSwitch.checked,
                            "thinking": thinkingSwitch.checked
                        });
                    sheet.close();
                }
            }
        }
    }
}

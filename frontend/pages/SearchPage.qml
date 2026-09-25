// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.delegates as KirigamiDelegates
import org.verzeta.studio 1.0

Kirigami.ScrollablePage {
    id: searchPage
    title: qsTr("Search")

    header: Controls.Pane {
        padding: Kirigami.Units.smallSpacing

        ColumnLayout {
            anchors.fill: parent
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Controls.TextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search conversations…")
                    Keys.onReturnPressed: performSearch()
                    Keys.onEnterPressed: performSearch()
                    onTextChanged: {
                        if (text.length === 0) {
                            searchResults.clear();
                        }
                    }
                }

                Controls.ToolButton {
                    icon.name: "search"
                    onClicked: performSearch()
                    Controls.ToolTip.text: qsTr("Search")
                    Controls.ToolTip.visible: hovered
                }

                Controls.ToolButton {
                    icon.name: "edit-clear"
                    visible: searchField.text.length > 0
                    onClicked: {
                        searchField.text = "";
                        searchResults.clear();
                    }
                    Controls.ToolTip.text: qsTr("Clear")
                    Controls.ToolTip.visible: hovered
                }
            }

            RowLayout {
                spacing: Kirigami.Units.largeSpacing

                AppRadioButton {
                    id: fullTextRadio
                    text: qsTr("Full Text")
                    checked: true
                }
                AppRadioButton {
                    id: semanticRadio
                    text: qsTr("Semantic")
                }
                AppRadioButton {
                    id: combinedRadio
                    text: qsTr("Combined")
                }
            }
        }
    }

    ListView {
        id: resultsList
        model: searchResults
        spacing: 0
        clip: true

        delegate: Controls.ItemDelegate {
            id: resultItem
            width: resultsList.width

            contentItem: RowLayout {
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: model.role === "assistant" ? "computer" : "user-identity"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                    Layout.alignment: Qt.AlignVCenter
                }

                KirigamiDelegates.TitleSubtitle {
                    Layout.fillWidth: true
                    title: model.conversationTitle.length > 0 ? model.conversationTitle : qsTr("(untitled conversation)")
                    subtitle: model.snippet
                }

                Controls.Label {
                    text: model.timestamp.toLocaleString(Qt.locale(), Locale.ShortFormat)
                    font.pointSize: ThemeController.fontSize - 2
                    color: Kirigami.Theme.disabledTextColor
                    Layout.alignment: Qt.AlignVCenter
                }
            }

            onClicked: {
                ChatController.switchConversation(model.conversationId);
                searchPage.pageStack.pop();
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            visible: resultsList.count === 0 && searchField.text.length > 0
            text: qsTr("No results found")
            explanation: qsTr("Try different keywords or switch to Semantic search mode.")
            icon.name: "search"
            width: parent.width - Kirigami.Units.largeSpacing * 4
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            visible: resultsList.count === 0 && searchField.text.length === 0
            text: qsTr("Search your conversations")
            explanation: qsTr("Type a query above and press Return or click Search.")
            icon.name: "search-symbolic"
            width: parent.width - Kirigami.Units.largeSpacing * 4
        }
    }

    ListModel {
        id: searchResults
    }

    Connections {
        target: SearchService
        function onSearchResultsReady(results) {
            searchPage.populateResults(results);
        }
    }

    function performSearch() {
        searchResults.clear();
        var query = searchField.text.trim();
        if (query.length === 0) {
            return;
        }
        if (semanticRadio.checked) {
            SearchService.searchSemantic(query);
        } else if (combinedRadio.checked) {
            SearchService.searchCombined(query);
        } else {
            populateResults(SearchService.searchFullText(query));
        }
    }

    function populateResults(results) {
        searchResults.clear();
        for (var i = 0; i < results.length; ++i) {
            searchResults.append({
                    "conversationId": results[i].conversationId,
                    "conversationTitle": results[i].conversationTitle,
                    "messageId": results[i].messageId,
                    "role": results[i].role,
                    "snippet": results[i].snippet,
                    "timestamp": results[i].timestamp,
                    "relevanceScore": results[i].relevanceScore
                });
        }
    }
}

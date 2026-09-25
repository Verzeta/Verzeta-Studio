// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: page
    color: ThemeController.surfacePage

    signal back

    property int refreshTick: 0
    property var providerList: []

    Component.onCompleted: page.providerList = WebSearchProviders.providers

    Connections {
        target: WebSearchProviders
        function onProvidersChanged() {
            if (page.providerList.length === 0)
                page.providerList = WebSearchProviders.providers;
            page.refreshTick++;
        }
    }

    function activeLabel() {
        var id = WebSearchProviders.activeProviderId();
        for (var i = 0; i < page.providerList.length; ++i) {
            if (page.providerList[i].id === id)
                return page.providerList[i].displayName;
        }
        return id;
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            color: ThemeController.surfaceCard
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 16
                spacing: 8
                Controls.ToolButton {
                    icon.name: "go-previous"
                    text: qsTr("Back")
                    display: Controls.AbstractButton.IconOnly
                    onClicked: page.back()
                }
                Kirigami.Heading {
                    text: qsTr("Web Search")
                    level: 3
                    Layout.fillWidth: true
                }
            }
        }
        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Controls.ScrollView {
            id: searchProvScroll
            contentWidth: applicationWindow().isCompact ? availableWidth : -1
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ColumnLayout {
                width: applicationWindow().isCompact ? Math.min(searchProvScroll.availableWidth - 32, 720) : Math.min(page.width - 32, 720)
                x: applicationWindow().isCompact ? Math.max(16, (searchProvScroll.availableWidth - width) / 2) : Math.max(16, (page.width - width) / 2)
                spacing: 16

                Item {
                    Layout.preferredHeight: 16
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: bannerRow.implicitHeight + 24
                    radius: ThemeController.radius
                    color: Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.10)
                    border.color: Kirigami.Theme.positiveTextColor
                    border.width: 1
                    RowLayout {
                        id: bannerRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10
                        Kirigami.Icon {
                            source: "emblem-success"
                            fallback: "dialog-ok"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            color: Kirigami.Theme.positiveTextColor
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Controls.Label {
                                text: {
                                    page.refreshTick;
                                    return qsTr("Active: %1").arg(page.activeLabel());
                                }
                                font.bold: true
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            Controls.Label {
                                text: qsTr("The search_web tool uses the active provider, and falls back to DuckDuckGo if it is unavailable.")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }

                Repeater {
                    model: page.providerList
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: cardCol.implicitHeight + 24
                        radius: ThemeController.radius
                        color: ThemeController.surfaceCard
                        border.color: card.isActive ? Kirigami.Theme.positiveTextColor : ThemeController.borderSubtle
                        border.width: card.isActive ? 2 : 1

                        readonly property string provId: modelData.id
                        readonly property bool isSearxng: provId === "searxng"
                        readonly property bool isCustom: provId === "custom"
                        readonly property bool requiresKey: modelData.requiresApiKey === true

                        readonly property bool isActive: {
                            page.refreshTick;
                            return WebSearchProviders.activeProviderId() === card.provId;
                        }
                        readonly property bool hasKey: {
                            page.refreshTick;
                            return WebSearchProviders.hasApiKey(card.provId);
                        }
                        readonly property string urlVal: {
                            page.refreshTick;
                            return WebSearchProviders.baseUrl(card.provId);
                        }

                        readonly property string customUrlVal: {
                            page.refreshTick;
                            var c = WebSearchProviders.customConfig();
                            return (c && c.url) ? String(c.url) : "";
                        }

                        readonly property bool needsKey: requiresKey && !hasKey
                        readonly property bool needsUrl: isSearxng ? (urlVal.length === 0) : (isCustom ? (customUrlVal.length === 0) : false)
                        readonly property bool canActivate: !isActive && !needsKey && !needsUrl

                        property bool useCustomEndpoint: false
                        Component.onCompleted: card.useCustomEndpoint = (card.urlVal.length > 0)

                        property int testState: -1
                        property bool testing: false
                        property string testMsg: ""

                        Connections {
                            target: WebSearchProviders
                            function onTestResult(providerId, ok, message) {
                                if (providerId !== card.provId)
                                    return;
                                card.testing = false;
                                card.testState = ok ? 1 : 0;
                                card.testMsg = message;
                            }
                        }

                        function requirementText() {
                            if (requiresKey)
                                return hasKey ? qsTr("API key set.") : qsTr("Requires an API key.");
                            if (isSearxng)
                                return needsUrl ? qsTr("Requires your SearXNG base URL. API key optional.") : qsTr("Ready. API key optional.");
                            if (isCustom)
                                return needsUrl ? qsTr("Requires an endpoint URL. Configure it below.") : qsTr("Custom endpoint configured.");
                            return qsTr("No key or URL needed (best-effort; may be rate-limited).");
                        }

                        ColumnLayout {
                            id: cardCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                Kirigami.Icon {
                                    source: "globe"
                                    fallback: "internet-web-browser"
                                    implicitWidth: Kirigami.Units.iconSizes.medium
                                    implicitHeight: Kirigami.Units.iconSizes.medium
                                    color: Kirigami.Theme.textColor
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Controls.Label {
                                            text: card.modelData.displayName
                                            font.bold: true
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Rectangle {
                                            visible: card.isActive
                                            implicitHeight: activeLbl.implicitHeight + 6
                                            implicitWidth: activeLbl.implicitWidth + 14
                                            radius: implicitHeight / 2
                                            color: Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.18)
                                            Controls.Label {
                                                id: activeLbl
                                                anchors.centerIn: parent
                                                text: qsTr("ACTIVE")
                                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.75
                                                font.bold: true
                                                color: Kirigami.Theme.positiveTextColor
                                            }
                                        }
                                    }
                                    Controls.Label {
                                        text: card.requirementText()
                                        color: Kirigami.Theme.disabledTextColor
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                }
                            }

                            Controls.Label {
                                Layout.fillWidth: true
                                visible: card.isActive && (card.needsKey || card.needsUrl)
                                text: qsTr("⚠ Active but not configured. Searches fall back to DuckDuckGo until you add the %1.").arg(card.needsUrl ? qsTr("base URL") : qsTr("API key"))
                                color: Kirigami.Theme.neutralTextColor
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                wrapMode: Text.WordWrap
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                visible: card.isSearxng
                                Controls.Label {
                                    text: qsTr("Base URL (required)")
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Controls.TextField {
                                        id: searxUrlField
                                        Layout.fillWidth: true
                                        Component.onCompleted: text = card.urlVal
                                        placeholderText: qsTr("https://your-searxng.example")
                                    }
                                    Controls.Button {
                                        text: qsTr("Save")
                                        onClicked: WebSearchProviders.setBaseUrl(card.provId, searxUrlField.text)
                                    }
                                }
                            }

                            Controls.CheckBox {
                                id: customEndpointToggle
                                visible: card.requiresKey
                                text: qsTr("Use a custom endpoint (advanced)")
                                checked: card.useCustomEndpoint
                                onToggled: {
                                    card.useCustomEndpoint = checked;
                                    if (!checked) {
                                        keyedUrlField.text = "";
                                        WebSearchProviders.setBaseUrl(card.provId, "");
                                    }
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                visible: card.requiresKey && customEndpointToggle.checked
                                Controls.Label {
                                    text: qsTr("Endpoint URL (optional override)")
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Controls.TextField {
                                        id: keyedUrlField
                                        Layout.fillWidth: true
                                        Component.onCompleted: text = card.urlVal
                                        placeholderText: qsTr("Default endpoint (leave blank to use it)")
                                    }
                                    Controls.Button {
                                        text: qsTr("Save")
                                        onClicked: WebSearchProviders.setBaseUrl(card.provId, keyedUrlField.text)
                                    }
                                }
                            }

                            ColumnLayout {
                                id: customForm
                                Layout.fillWidth: true
                                spacing: 6
                                visible: card.isCustom
                                property var cfg: ({})
                                Component.onCompleted: customForm.cfg = WebSearchProviders.customConfig()

                                Controls.Label {
                                    text: qsTr("Endpoint URL (required)")
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                }
                                Controls.TextField {
                                    id: cuUrl
                                    Layout.fillWidth: true
                                    Component.onCompleted: text = customForm.cfg.url || ""
                                    placeholderText: qsTr("https://api.example.com/search")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    ColumnLayout {
                                        spacing: 2
                                        Controls.Label {
                                            text: qsTr("Method")
                                            color: Kirigami.Theme.disabledTextColor
                                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                        }
                                        Controls.ComboBox {
                                            id: cuMethod
                                            model: ["GET", "POST"]
                                            Component.onCompleted: currentIndex = (customForm.cfg.method === "POST" ? 1 : 0)
                                        }
                                    }
                                    ColumnLayout {
                                        spacing: 2
                                        Controls.Label {
                                            text: qsTr("Auth")
                                            color: Kirigami.Theme.disabledTextColor
                                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                        }
                                        Controls.ComboBox {
                                            id: cuAuth
                                            model: ["none", "bearer", "header", "query"]
                                            Component.onCompleted: currentIndex = Math.max(0, ["none", "bearer", "header", "query"].indexOf(customForm.cfg.auth || "none"))
                                        }
                                    }
                                }
                                Controls.TextField {
                                    id: cuAuthParam
                                    Layout.fillWidth: true
                                    visible: cuAuth.currentText === "header" || cuAuth.currentText === "query"
                                    Component.onCompleted: text = customForm.cfg.authHeader || customForm.cfg.authQueryParam || ""
                                    placeholderText: cuAuth.currentText === "header" ? qsTr("Auth header name (default x-api-key)") : qsTr("Auth query param (default key)")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Controls.TextField {
                                        id: cuQueryParam
                                        Layout.fillWidth: true
                                        Component.onCompleted: text = customForm.cfg.queryParam || ""
                                        placeholderText: qsTr("Query field (default q)")
                                    }
                                    Controls.TextField {
                                        id: cuCountParam
                                        Layout.fillWidth: true
                                        Component.onCompleted: text = customForm.cfg.countParam || ""
                                        placeholderText: qsTr("Count field (optional)")
                                    }
                                }
                                Controls.Label {
                                    text: qsTr("Response mapping")
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                }
                                Controls.TextField {
                                    id: cuResultsPath
                                    Layout.fillWidth: true
                                    Component.onCompleted: text = customForm.cfg.resultsPath || ""
                                    placeholderText: qsTr("Results path (e.g. results or data.webPages.value)")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Controls.TextField {
                                        id: cuTitleKey
                                        Layout.fillWidth: true
                                        Component.onCompleted: text = customForm.cfg.titleKey || ""
                                        placeholderText: qsTr("title key")
                                    }
                                    Controls.TextField {
                                        id: cuUrlKey
                                        Layout.fillWidth: true
                                        Component.onCompleted: text = customForm.cfg.urlKey || ""
                                        placeholderText: qsTr("url key")
                                    }
                                    Controls.TextField {
                                        id: cuSnippetKey
                                        Layout.fillWidth: true
                                        Component.onCompleted: text = customForm.cfg.snippetKey || ""
                                        placeholderText: qsTr("snippet key")
                                    }
                                }
                                Controls.Button {
                                    text: qsTr("Save configuration")
                                    onClicked: {
                                        var c = {
                                            "url": cuUrl.text,
                                            "method": cuMethod.currentText,
                                            "auth": cuAuth.currentText,
                                            "queryParam": cuQueryParam.text,
                                            "countParam": cuCountParam.text,
                                            "resultsPath": cuResultsPath.text,
                                            "titleKey": cuTitleKey.text,
                                            "urlKey": cuUrlKey.text,
                                            "snippetKey": cuSnippetKey.text
                                        };
                                        if (cuAuth.currentText === "header")
                                            c["authHeader"] = cuAuthParam.text;
                                        else if (cuAuth.currentText === "query")
                                            c["authQueryParam"] = cuAuthParam.text;
                                        WebSearchProviders.setCustomConfig(c);
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                visible: card.requiresKey || card.isSearxng || card.isCustom
                                Controls.Label {
                                    text: card.requiresKey ? qsTr("API key (required)") : qsTr("API key (optional)")
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Controls.TextField {
                                        id: keyField
                                        Layout.fillWidth: true
                                        echoMode: TextInput.Password
                                        placeholderText: card.hasKey ? qsTr("Key set: type to replace") : qsTr("Paste API key")
                                    }
                                    Controls.Button {
                                        text: qsTr("Save")
                                        enabled: keyField.text.length > 0
                                        onClicked: {
                                            WebSearchProviders.setApiKey(card.provId, keyField.text);
                                            keyField.text = "";
                                        }
                                    }
                                    Controls.Button {
                                        text: qsTr("Clear")
                                        visible: card.hasKey
                                        onClicked: {
                                            WebSearchProviders.setApiKey(card.provId, "");
                                            keyField.text = "";
                                        }
                                    }
                                }
                            }

                            Controls.Label {
                                Layout.fillWidth: true
                                visible: card.testing || card.testState !== -1
                                text: card.testing ? qsTr("Testing…") : card.testMsg
                                wrapMode: Text.WordWrap
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                                color: card.testing ? Kirigami.Theme.disabledTextColor : (card.testState === 1 ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor)
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Controls.Label {
                                    visible: !card.isActive && (card.needsKey || card.needsUrl)
                                    text: card.needsUrl ? qsTr("Set a base URL to activate.") : qsTr("Add an API key to activate.")
                                    color: Kirigami.Theme.neutralTextColor
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                Controls.Button {
                                    text: qsTr("Test")
                                    enabled: !card.testing
                                    onClicked: {
                                        card.testing = true;
                                        card.testState = -1;
                                        card.testMsg = "";
                                        WebSearchProviders.testProvider(card.provId);
                                    }
                                }
                                Controls.Button {
                                    text: card.isActive ? qsTr("In Use") : qsTr("Set as active")
                                    enabled: card.canActivate
                                    onClicked: WebSearchProviders.setActiveProvider(card.provId)
                                }
                            }
                        }
                    }
                }

                Item {
                    Layout.preferredHeight: 16
                }
            }
        }
    }
}

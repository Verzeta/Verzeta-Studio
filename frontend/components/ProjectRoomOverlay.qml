// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: overlay
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: applicationWindow().width * 0.95
    implicitHeight: applicationWindow().height * 0.92

    readonly property bool _isMobile: applicationWindow().width < Kirigami.Units.gridUnit * 36

    readonly property color _cPanel: Qt.darker(Kirigami.Theme.backgroundColor, 0.6)
    readonly property color _cCardFoot: Qt.darker(Kirigami.Theme.backgroundColor, 0.82)

    readonly property real _cardWidth: Kirigami.Units.gridUnit * 27

    function openSheet() {
        overlay._refresh();
        overlay.open();
    }

    signal openConversationRequested(string convId)

    property string _activeFilter: "all"
    property string _searchText: ""
    property var _templates: []
    property var _projectFolders: []

    function _refresh() {
        overlay._templates = ProjectTemplates.landingTemplates();
        var raw = Conversations.listProjectFolders();
        var enriched = [];
        for (var i = 0; i < raw.length; ++i) {
            var info = Conversations.folderInfo(raw[i].id) || {};
            enriched.push({
                    "id": raw[i].id,
                    "name": raw[i].name,
                    "type": raw[i].type || "project",
                    "goalText": info.goal || ""
                });
        }
        overlay._projectFolders = enriched;
    }

    readonly property var _filteredTemplates: {
        var out = [];
        var needle = overlay._searchText.toLowerCase();
        for (var i = 0; i < overlay._templates.length; ++i) {
            var t = overlay._templates[i];
            if (overlay._activeFilter !== "all" && t.category !== overlay._activeFilter)
                continue;
            if (needle.length > 0) {
                var hay = ((t.name || "") + " " + (t.description || "") + " " + (t.tagLabel || "")).toLowerCase();
                if (hay.indexOf(needle) < 0)
                    continue;
            }
            out.push(t);
        }
        return out;
    }

    function _memberNamesFor(folderId) {
        var members = MembershipService.projectMembersList(folderId);
        var names = [];
        for (var i = 0; i < members.length; ++i) {
            names.push(members[i].alias || "");
        }
        return names;
    }

    function _openQuickStart(templateData) {
        quickStartLoader.active = true;
        quickStartLoader.item.openForTemplate(templateData);
    }
    function _openKickoff(folderId, folderName, memberCount) {
        kickoffLoader.active = true;
        kickoffLoader.item.openForProject(folderId, folderName, memberCount);
    }
    function _openDetail(templateData) {
        detailLoader.active = true;
        detailLoader.item.openForTemplate(templateData);
    }
    function _openLibrary() {
        libraryLoader.active = true;
        libraryLoader.item.openLibrary();
    }

    readonly property var _filterModel: [{
            "id": "all",
            "label": qsTr("All")
        }, {
            "id": "sales",
            "label": qsTr("Sales")
        }, {
            "id": "engineering",
            "label": qsTr("Engineering")
        }, {
            "id": "marketing",
            "label": qsTr("Marketing")
        }, {
            "id": "exec",
            "label": qsTr("Exec")
        }, {
            "id": "discovery",
            "label": qsTr("Discovery")
        }]

    title: qsTr("Project Rooms")
    subtitle: qsTr("HOME · NEW PROJECT")
    dialogIcon: "folder-projects"
    subHeader: AppTextField {
        id: searchField
        placeholderText: qsTr("Search projects and templates…")
        text: overlay._searchText
        onTextChanged: overlay._searchText = text
    }

    footer: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Close")
            icon.name: "dialog-close"
            onClicked: overlay.close()
        }
    }

    Item {
        id: bodyRoot
        implicitWidth: applicationWindow().width * 0.92
        implicitHeight: contentCol.implicitHeight

        ColumnLayout {
            id: contentCol
            width: bodyRoot.width
            spacing: 0

            ColumnLayout {
                id: content
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.gridUnit
                spacing: Kirigami.Units.gridUnit

                Rectangle {
                    Layout.fillWidth: true
                    radius: ThemeController.radius
                    color: overlay._cPanel
                    clip: true
                    antialiasing: true
                    implicitHeight: (overlay._isMobile ? heroMobile.implicitHeight : heroDesktop.implicitHeight) + Kirigami.Units.gridUnit * 2

                    ProjectRoomTexture {
                        anchors.fill: parent
                    }

                    RowLayout {
                        id: heroDesktop
                        visible: !overlay._isMobile
                        anchors {
                            left: parent.left
                            right: parent.right
                            verticalCenter: parent.verticalCenter
                            leftMargin: Kirigami.Units.gridUnit * 1.5
                            rightMargin: Kirigami.Units.gridUnit * 1.5
                        }
                        spacing: Kirigami.Units.largeSpacing

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            Controls.Label {
                                text: qsTr("Start a project room")
                                color: Kirigami.Theme.textColor
                                font.family: ThemeController.fontFamily
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.35
                            }
                            Controls.Label {
                                text: qsTr("Create a chat, roster, " + "goal, tasks, and artifacts " + "in one step.")
                                color: Kirigami.Theme.disabledTextColor
                                font.family: ThemeController.fontFamily
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.95
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }

                        AppButton {
                            text: qsTr("+  Blank project")
                            onClicked: overlay._openQuickStart(null)
                            Layout.alignment: Qt.AlignVCenter
                        }
                        AppButton {
                            text: qsTr("⚡  Quick start from template")
                            highlighted: true
                            onClicked: overlay._openLibrary()
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }

                    ColumnLayout {
                        id: heroMobile
                        visible: overlay._isMobile
                        anchors {
                            left: parent.left
                            right: parent.right
                            verticalCenter: parent.verticalCenter
                            leftMargin: Kirigami.Units.gridUnit
                            rightMargin: Kirigami.Units.gridUnit
                        }
                        spacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: qsTr("Start a project room")
                            color: Kirigami.Theme.textColor
                            font.family: ThemeController.fontFamily
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.2
                            Layout.fillWidth: true
                        }
                        Controls.Label {
                            text: qsTr("Create a chat, roster, goal, " + "tasks, and artifacts in one step.")
                            color: Kirigami.Theme.disabledTextColor
                            font.family: ThemeController.fontFamily
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Kirigami.Units.smallSpacing
                            spacing: Kirigami.Units.smallSpacing
                            AppButton {
                                text: qsTr("+ Blank")
                                onClicked: overlay._openQuickStart(null)
                                Layout.fillWidth: true
                            }
                            AppButton {
                                text: qsTr("⚡ Quick start")
                                highlighted: true
                                onClicked: overlay._openLibrary()
                                Layout.fillWidth: true
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    visible: overlay._projectFolders.length > 0
                    radius: ThemeController.radius
                    color: overlay._cPanel
                    antialiasing: true
                    implicitHeight: roomsInner.implicitHeight + Kirigami.Units.gridUnit * 2

                    ColumnLayout {
                        id: roomsInner
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                            margins: Kirigami.Units.gridUnit
                        }
                        spacing: Kirigami.Units.largeSpacing

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            Controls.Label {
                                text: qsTr("Your rooms")
                                color: Kirigami.Theme.textColor
                                font.family: ThemeController.fontFamily
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.15
                            }
                            Controls.Label {
                                text: "· " + overlay._projectFolders.length
                                color: Kirigami.Theme.disabledTextColor
                                font.family: ThemeController.fontFamily
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.9
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            implicitHeight: roomsFlow.implicitHeight

                            Flow {
                                id: roomsFlow
                                readonly property real gap: Kirigami.Units.largeSpacing
                                readonly property real cw: overlay._isMobile ? parent.width : overlay._cardWidth
                                readonly property int fitN: overlay._isMobile ? 1 : Math.max(1, Math.floor((parent.width + gap) / (cw + gap)))
                                readonly property int useN: Math.max(1, Math.min(fitN, overlay._projectFolders.length))
                                width: useN * cw + (useN - 1) * gap
                                height: implicitHeight
                                anchors.horizontalCenter: parent.horizontalCenter
                                spacing: gap

                                Repeater {
                                    model: overlay._projectFolders
                                    delegate: ProjectRoomCard {
                                        cardWidth: roomsFlow.cw
                                        folderId: modelData.id || ""
                                        folderName: modelData.name || qsTr("(untitled project)")
                                        folderType: modelData.type || "project"
                                        goalText: modelData.goalText || ""
                                        status: "idle"
                                        memberNames: overlay._memberNamesFor(modelData.id || "")
                                        compactTouchTarget: overlay._isMobile
                                        onOpenRequested: overlay.close()
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    id: templatesPanel
                    Layout.fillWidth: true
                    radius: ThemeController.radius
                    color: overlay._cPanel
                    antialiasing: true
                    implicitHeight: templatesInner.implicitHeight + Kirigami.Units.gridUnit * 2

                    ColumnLayout {
                        id: templatesInner
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                            margins: Kirigami.Units.gridUnit
                        }
                        spacing: Kirigami.Units.largeSpacing

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.largeSpacing

                            Controls.Label {
                                text: qsTr("Start from a template")
                                color: Kirigami.Theme.textColor
                                font.family: ThemeController.fontFamily
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.15
                            }

                            Flow {
                                visible: !overlay._isMobile
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing
                                Repeater {
                                    model: overlay._filterModel
                                    delegate: ProjectFilterPill {
                                        label: modelData.label
                                        selected: overlay._activeFilter === modelData.id
                                        compactTouchTarget: false
                                        onClicked: overlay._activeFilter = modelData.id
                                    }
                                }
                            }

                            Item {
                                visible: overlay._isMobile
                                Layout.fillWidth: true
                            }
                        }

                        Flow {
                            visible: overlay._isMobile
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            Repeater {
                                model: overlay._filterModel
                                delegate: ProjectFilterPill {
                                    label: modelData.label
                                    selected: overlay._activeFilter === modelData.id
                                    compactTouchTarget: true
                                    onClicked: overlay._activeFilter = modelData.id
                                }
                            }
                        }

                        Kirigami.PlaceholderMessage {
                            Layout.fillWidth: true
                            Layout.topMargin: Kirigami.Units.largeSpacing
                            Layout.bottomMargin: Kirigami.Units.largeSpacing
                            visible: overlay._filteredTemplates.length === 0
                            icon.name: "view-filter"
                            text: qsTr("No templates match")
                            explanation: qsTr("Try a different category " + "or clear the search.")
                            helpfulAction: Kirigami.Action {
                                icon.name: "edit-clear"
                                text: qsTr("Reset filters")
                                onTriggered: {
                                    overlay._activeFilter = "all";
                                    overlay._searchText = "";
                                }
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            visible: overlay._filteredTemplates.length > 0
                            implicitHeight: templatesFlow.implicitHeight

                            Flow {
                                id: templatesFlow
                                readonly property real gap: Kirigami.Units.largeSpacing
                                readonly property real cw: overlay._isMobile ? parent.width : overlay._cardWidth
                                readonly property int fitN: overlay._isMobile ? 1 : Math.max(1, Math.floor((parent.width + gap) / (cw + gap)))
                                readonly property int useN: Math.max(1, Math.min(fitN, overlay._filteredTemplates.length))
                                width: useN * cw + (useN - 1) * gap
                                height: implicitHeight
                                anchors.horizontalCenter: parent.horizontalCenter
                                spacing: gap

                                Repeater {
                                    model: overlay._filteredTemplates
                                    delegate: ProjectTemplateCard {
                                        cardWidth: templatesFlow.cw
                                        templateData: modelData
                                        compactTouchTarget: overlay._isMobile
                                        onReadMoreClicked: overlay._openDetail(modelData)
                                        onQuickStartClicked: overlay._openQuickStart(modelData)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Loader {
            id: quickStartLoader
            active: false
            sourceComponent: Component {
                ProjectQuickStartSheet {
                    onProjectCreated: function (folderId, folderName, count) {
                        overlay._openKickoff(folderId, folderName, count);
                    }
                }
            }
        }
        Loader {
            id: kickoffLoader
            active: false
            sourceComponent: Component {
                ProjectRoomKickoffSheet {
                    onFinished: function (openConvId) {
                        overlay.close();
                        if (openConvId.length > 0) {
                            overlay.openConversationRequested(openConvId);
                        }
                    }
                }
            }
        }
        Loader {
            id: detailLoader
            active: false
            sourceComponent: Component {
                ProjectTemplateDetailSheet {
                    onQuickStartRequested: function (templateData) {
                        overlay._openQuickStart(templateData);
                    }
                }
            }
        }
        Loader {
            id: libraryLoader
            active: false
            sourceComponent: Component {
                ProjectTemplateLibrarySheet {
                    onQuickStartRequested: function (templateData) {
                        overlay._openQuickStart(templateData);
                    }
                    onReadMoreRequested: function (templateData) {
                        overlay._openDetail(templateData);
                    }
                }
            }
        }

        Connections {
            target: ProjectTemplates
            function onCatalogChanged() {
                overlay._refresh();
            }
        }
    }
}

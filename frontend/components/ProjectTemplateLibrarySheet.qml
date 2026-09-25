// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: library
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: applicationWindow().width * 0.95
    implicitHeight: applicationWindow().height * 0.92

    readonly property bool _isMobile: applicationWindow().width < Kirigami.Units.gridUnit * 36
    readonly property color _cPanel: Qt.darker(Kirigami.Theme.backgroundColor, 0.6)

    readonly property real _cardWidth: Kirigami.Units.gridUnit * 27
    readonly property real _gap: Kirigami.Units.largeSpacing

    property var _allTemplates: []
    property string _activeFilter: "all"
    property string _searchText: ""

    signal quickStartRequested(var templateData)
    signal readMoreRequested(var templateData)

    function openLibrary() {
        library._refresh();
        library.open();
    }

    function _refresh() {
        library._allTemplates = ProjectTemplates.templates();
    }

    readonly property var _filteredTemplates: {
        var out = [];
        var needle = library._searchText.toLowerCase();
        for (var i = 0; i < library._allTemplates.length; ++i) {
            var t = library._allTemplates[i];
            if (library._activeFilter !== "all" && t.category !== library._activeFilter)
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

    Connections {
        target: ProjectTemplates
        function onCatalogChanged() {
            library._refresh();
        }
    }

    title: qsTr("Template Library")
    subtitle: qsTr("%1 templates").arg(library._filteredTemplates.length)
    dialogIcon: "folder-templates"
    subHeader: AppTextField {
        id: searchField
        placeholderText: qsTr("Search templates…")
        text: library._searchText
        onTextChanged: library._searchText = text
    }

    footer: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: qsTr("Close")
            icon.name: "dialog-close"
            onClicked: library.close()
        }
    }

    Item {
        id: bodyRoot
        implicitWidth: applicationWindow().width * 0.92
        implicitHeight: applicationWindow().height * 0.74

        Rectangle {
            anchors.fill: parent
            radius: ThemeController.radius
            color: library._cPanel
            antialiasing: true

            ColumnLayout {
                anchors {
                    fill: parent
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.largeSpacing

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    Repeater {
                        model: library._filterModel
                        delegate: ProjectFilterPill {
                            required property var modelData
                            label: modelData.label
                            selected: library._activeFilter === modelData.id
                            compactTouchTarget: library._isMobile
                            onClicked: library._activeFilter = modelData.id
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: library._filteredTemplates.length === 0
                    icon.name: "view-filter"
                    text: qsTr("No templates match")
                    explanation: qsTr("Try a different category or " + "clear the search.")
                    helpfulAction: Kirigami.Action {
                        icon.name: "edit-clear"
                        text: qsTr("Reset filters")
                        onTriggered: {
                            library._activeFilter = "all";
                            library._searchText = "";
                        }
                    }
                }

                GridView {
                    id: grid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: library._filteredTemplates.length > 0
                    clip: true

                    model: library._filteredTemplates

                    cellWidth: library._isMobile ? width : (library._cardWidth + library._gap)
                    cellHeight: (library._isMobile ? Kirigami.Units.gridUnit * 16 : Kirigami.Units.gridUnit * 15) + library._gap

                    Controls.ScrollBar.vertical: Controls.ScrollBar {
                    }

                    delegate: ProjectTemplateCard {
                        required property var modelData
                        cardWidth: library._isMobile ? GridView.view.cellWidth : library._cardWidth
                        compactTouchTarget: library._isMobile
                        templateData: modelData
                        showPinControl: modelData.isUserSaved === true
                        isPinned: modelData.isPinned === true
                        showDeleteControl: modelData.isUserSaved === true

                        onQuickStartClicked: {
                            library.close();
                            library.quickStartRequested(modelData);
                        }
                        onReadMoreClicked: {
                            library.close();
                            library.readMoreRequested(modelData);
                        }
                        onPinToggleClicked: {
                            ProjectTemplates.setTemplatePinned(modelData.id, !(modelData.isPinned === true));
                        }
                        onDeleteClicked: {
                            deleteConfirmDialog.templateId = modelData.id;
                            deleteConfirmDialog.templateName = modelData.name || "";
                            deleteConfirmDialog.open();
                        }
                    }
                }
            }
        }

        Kirigami.PromptDialog {
            id: deleteConfirmDialog
            parent: applicationWindow().overlay

            property string templateId: ""
            property string templateName: ""

            title: qsTr("Delete template")
            subtitle: qsTr("Delete the template \"%1\"? This permanently " + "removes the saved template. Projects " + "already created from it are not affected.").arg(deleteConfirmDialog.templateName)
            standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.No

            onAccepted: {
                if (deleteConfirmDialog.templateId.length > 0) {
                    ProjectTemplates.deleteUserTemplate(deleteConfirmDialog.templateId);
                }
            }
        }
    }
}

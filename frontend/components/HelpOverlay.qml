// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: helpSheet
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: Math.min(applicationWindow().width * 0.95, Kirigami.Units.gridUnit * 60)
    title: qsTr("Help")
    dialogIcon: "help-contents"
    subtitle: qsTr("User documentation bundled with the app.")

    readonly property bool _isMobile: applicationWindow().width < Kirigami.Units.gridUnit * 36

    property string _currentId: "00"
    property string _currentTitle: ""
    property string _currentBody: ""

    readonly property var _docs: Help.docs()

    function openAt(docId) {
        _setCurrent(docId);
        open();
    }

    function openForOverview() {
        openAt("00");
    }

    function _setCurrent(docId) {
        if (!docId || docId.length === 0)
            return;
        helpSheet._currentId = docId;
        helpSheet._currentTitle = Help.titleFor(docId);
        helpSheet._currentBody = Help.body(docId);
    }

    function _currentIndex() {
        for (var i = 0; i < helpSheet._docs.length; ++i) {
            if (helpSheet._docs[i].id === helpSheet._currentId)
                return i;
        }
        return -1;
    }

    Component.onCompleted: _setCurrent(helpSheet._currentId)

    footer: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Item {
            Layout.fillWidth: true
        }

        AppButton {
            text: qsTr("Close")
            icon.name: "dialog-close"
            onClicked: helpSheet.close()
        }
    }

    Item {
        id: bodyRoot
        implicitWidth: Math.min(applicationWindow().width * 0.92, Kirigami.Units.gridUnit * 56)
        implicitHeight: Math.min(applicationWindow().height * 0.80, Kirigami.Units.gridUnit * 40)

        Controls.ComboBox {
            id: mobilePicker
            anchors {
                top: parent.top
                left: parent.left
                right: parent.right
            }
            visible: helpSheet._isMobile
            model: helpSheet._docs.map(function (d) {
                    return d.section + " - " + d.title;
                })
            currentIndex: helpSheet._currentIndex()
            onActivated: {
                const doc = helpSheet._docs[currentIndex];
                if (doc)
                    helpSheet._setCurrent(doc.id);
            }
        }

        RowLayout {
            anchors.fill: parent
            spacing: Kirigami.Units.largeSpacing
            visible: !helpSheet._isMobile

            Rectangle {
                Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                Layout.minimumWidth: Kirigami.Units.gridUnit * 12
                Layout.fillHeight: true
                color: ThemeController.surfaceCard
                border.color: Kirigami.Theme.disabledTextColor
                border.width: 1
                radius: Kirigami.Units.smallSpacing

                ListView {
                    id: navList
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    clip: true
                    model: helpSheet._docs
                    spacing: 2
                    boundsBehavior: Flickable.StopAtBounds

                    section.property: "section"
                    section.delegate: ColumnLayout {
                        width: navList.width
                        spacing: Kirigami.Units.smallSpacing

                        Item {
                            Layout.preferredHeight: Kirigami.Units.smallSpacing
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: section.toUpperCase()
                            color: Kirigami.Theme.highlightColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.75
                            font.bold: true
                            Layout.leftMargin: Kirigami.Units.smallSpacing
                            Layout.rightMargin: Kirigami.Units.smallSpacing
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Kirigami.Separator {
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.smallSpacing
                            Layout.rightMargin: Kirigami.Units.smallSpacing
                        }
                    }

                    delegate: Controls.ItemDelegate {
                        id: navItem
                        required property var modelData
                        required property int index

                        width: navList.width
                        implicitHeight: Math.max(Kirigami.Units.gridUnit * 2.2, iconCol.implicitHeight + Kirigami.Units.largeSpacing)
                        padding: Kirigami.Units.smallSpacing
                        hoverEnabled: true

                        readonly property bool _isActive: helpSheet._currentId === modelData.id

                        onClicked: helpSheet._setCurrent(modelData.id)

                        background: Rectangle {
                            radius: ThemeController.radius
                            color: navItem._isActive ? ThemeController.selectionTint : (navItem.hovered ? ThemeController.hoverTint : "transparent")
                            Behavior on color  {
                                ColorAnimation {
                                    duration: 80
                                }
                            }
                        }

                        contentItem: RowLayout {
                            id: iconCol
                            spacing: Kirigami.Units.smallSpacing
                            Kirigami.Icon {
                                source: navItem.modelData.iconName
                                fallback: navItem.modelData.iconFallback
                                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                color: navItem._isActive ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
                            }
                            Controls.Label {
                                text: navItem.modelData.title
                                color: navItem._isActive ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
                                font.bold: navItem._isActive
                                elide: Text.ElideRight
                                wrapMode: Text.NoWrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Kirigami.Theme.backgroundColor
                border.color: Kirigami.Theme.disabledTextColor
                border.width: 1
                radius: Kirigami.Units.smallSpacing

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.largeSpacing
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Heading {
                        text: helpSheet._currentTitle
                        level: 3
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        color: Kirigami.Theme.textColor
                    }

                    Kirigami.Separator {
                        Layout.fillWidth: true
                    }

                    Controls.ScrollView {
                        id: contentScrollDesktop
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: availableWidth
                        Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
                        Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded

                        Controls.TextArea {
                            id: textBodyDesktop
                            text: helpSheet._currentBody
                            textFormat: TextEdit.RichText
                            readOnly: true
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            color: Kirigami.Theme.textColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize
                            background: null
                            onLinkActivated: function (link) {
                                if (link.indexOf("http") === 0) {
                                    Qt.openUrlExternally(link);
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            anchors {
                top: mobilePicker.bottom
                topMargin: Kirigami.Units.largeSpacing
                left: parent.left
                right: parent.right
                bottom: parent.bottom
            }
            visible: helpSheet._isMobile
            color: Kirigami.Theme.backgroundColor
            border.color: Kirigami.Theme.disabledTextColor
            border.width: 1
            radius: Kirigami.Units.smallSpacing

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    text: helpSheet._currentTitle
                    level: 3
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: Kirigami.Theme.textColor
                }

                Kirigami.Separator {
                    Layout.fillWidth: true
                }

                Controls.ScrollView {
                    id: contentScrollMobile
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: availableWidth
                    Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
                    Controls.ScrollBar.vertical.policy: Controls.ScrollBar.AsNeeded

                    Controls.TextArea {
                        id: textBodyMobile
                        text: helpSheet._currentBody
                        textFormat: TextEdit.RichText
                        readOnly: true
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        color: Kirigami.Theme.textColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        background: null
                        onLinkActivated: function (link) {
                            if (link.indexOf("http") === 0) {
                                Qt.openUrlExternally(link);
                            }
                        }
                    }
                }
            }
        }

        Controls.Label {
            anchors.centerIn: parent
            width: parent.width * 0.7
            visible: helpSheet._currentBody.length === 0
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("This help page could not be loaded from the bundled " + "resources. Please report this as a bug. The doc " + "files should be embedded into the binary at build " + "time.")
            color: Kirigami.Theme.negativeTextColor
            font.italic: true
        }
    }
}

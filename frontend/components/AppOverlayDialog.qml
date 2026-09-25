// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later



import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Controls.Popup {
    id: root


    property string title

    property string subtitle

    property string dialogIcon

    property bool showBranding: true

    property bool showCloseButton: true

    property Item subHeader: null

    property Item headerTrailing: null

    property Item header: null

    property Item footer: null

    default property alias contentData: bodyHolder.data

    parent: Controls.Overlay.overlay
    modal: true
    dim: true
    padding: 0
    focus: true
    closePolicy: Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside
    width: (_compact && parent) ? parent.width : implicitWidth
    height: (_compact && parent) ? parent.height : implicitHeight
    x: _compact ? 0 : Math.round((((parent ? parent.width : 0)) - width) / 2)
    y: (_compact ? 0 : Math.round((((parent ? parent.height : 0)) - height) / 2))
       + _dragOffset
    onAboutToShow: _dragOffset = 0   

    Controls.Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.55)   
    }
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0.0; to: 1.0
                duration: Kirigami.Units.shortDuration; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.96; to: 1.0
                duration: Kirigami.Units.shortDuration; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1.0; to: 0.0
                duration: Kirigami.Units.shortDuration; easing.type: Easing.InCubic }
            NumberAnimation { property: "scale"; from: 1.0; to: 0.96
                duration: Kirigami.Units.shortDuration; easing.type: Easing.InCubic }
        }
    }

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    readonly property bool _compact:
        (parent ? parent.width : 0) > 0 &&
        (parent ? parent.width : 0) < Kirigami.Units.gridUnit * 32
    readonly property int _radius: _compact ? 0 : Kirigami.Units.cornerRadius
    readonly property int _pad: Kirigami.Units.largeSpacing
    readonly property color _separatorColor: ThemeController.borderSubtle

    property real _dragOffset: 0
    Behavior on _dragOffset {
        enabled: !brandDragHandler.active   
        NumberAnimation { duration: Kirigami.Units.shortDuration; easing.type: Easing.OutCubic }
    }

    implicitWidth: Math.min((parent ? parent.width : 600) * 0.9,
                            Kirigami.Units.gridUnit * 32)
    implicitHeight: Math.min((parent ? parent.height : 600) * 0.92,
                             layout.implicitHeight)

    background: Kirigami.ShadowedRectangle {
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false
        radius: root._radius
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: root._separatorColor
        shadow.size: Kirigami.Units.gridUnit
        shadow.color: Qt.rgba(0, 0, 0, 0.25)
        shadow.yOffset: 2
    }

    contentItem: ColumnLayout {
        id: layout
        spacing: 0

        Item {
            id: brandingBand
            Layout.fillWidth: true
            visible: root.showBranding
            implicitHeight: visible
                ? brandingRow.implicitHeight + root._pad : 0

            DragHandler {
                id: brandDragHandler
                enabled: root._compact
                target: null
                xAxis.enabled: false
                yAxis.enabled: true
                onActiveChanged: {
                    if (!active) {
                        if (root._dragOffset > root.height * 0.22) root.close()
                        else root._dragOffset = 0
                    }
                }
                onTranslationChanged: {
                    if (active && translation.y > 0)
                        root._dragOffset = translation.y
                }
            }

            FrostedGlass {
                anchors.fill: parent
                topLeftRadius: root._radius
                topRightRadius: root._radius
                bottomLeftRadius: 0
                bottomRightRadius: 0
            }

            RowLayout {
                id: brandingRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: root._pad
                anchors.rightMargin: root._pad
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: "qrc:/icons/verzeta-studio.svg"
                    fallback: "application-x-executable"
                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                    implicitHeight: Kirigami.Units.iconSizes.smallMedium
                    Layout.alignment: Qt.AlignVCenter
                }
                Controls.Label {
                    text: qsTr("Verzeta Studio")
                    font.bold: true
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    color: Kirigami.Theme.disabledTextColor
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    elide: Text.ElideRight
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root._separatorColor
            visible: brandingBand.visible
        }

        RowLayout {
            id: titleBand
            Layout.fillWidth: true
            Layout.leftMargin: root._pad
            Layout.rightMargin: root._pad
            Layout.topMargin: root._pad
            Layout.bottomMargin: root._pad
            spacing: Kirigami.Units.smallSpacing
            visible: root.header === null

            Kirigami.Icon {
                source: root.dialogIcon
                visible: root.dialogIcon.length > 0
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
                color: Kirigami.Theme.highlightColor
                Layout.alignment: Qt.AlignVCenter
            }
            Kirigami.Heading {
                text: root.title
                level: 2
                elide: Text.ElideRight
                visible: root.title.length > 0
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
            }
            Item {
                id: headerTrailingHost
                visible: root.headerTrailing !== null
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: root.headerTrailing ? root.headerTrailing.implicitWidth : 0
                implicitHeight: root.headerTrailing ? root.headerTrailing.implicitHeight : 0
                children: root.headerTrailing ? [root.headerTrailing] : []
            }
            Controls.ToolButton {
                visible: root.showCloseButton
                icon.name: hovered ? "window-close" : "window-close-symbolic"
                text: qsTr("Close")
                display: Controls.AbstractButton.IconOnly
                onClicked: root.close()
                Layout.alignment: Qt.AlignTop
            }
        }

        RowLayout {
            id: legacyHeaderBand
            Layout.fillWidth: true
            Layout.leftMargin: root._pad
            Layout.rightMargin: root._pad
            Layout.topMargin: root._pad
            Layout.bottomMargin: root._pad
            spacing: Kirigami.Units.smallSpacing
            visible: root.header !== null

            Item {
                id: legacyHeaderHost
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: root.header ? root.header.implicitHeight : 0
                children: root.header ? [root.header] : []
                onWidthChanged: if (root.header) root.header.width = width
                Component.onCompleted: if (root.header)
                    root.header.width = Qt.binding(() => legacyHeaderHost.width)
            }
            Controls.ToolButton {
                visible: root.showCloseButton
                icon.name: hovered ? "window-close" : "window-close-symbolic"
                text: qsTr("Close")
                display: Controls.AbstractButton.IconOnly
                onClicked: root.close()
                Layout.alignment: Qt.AlignTop
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root._separatorColor
            visible: titleBand.visible || legacyHeaderBand.visible
        }

        ColumnLayout {
            id: subHeaderBand
            Layout.fillWidth: true
            Layout.leftMargin: root._pad
            Layout.rightMargin: root._pad
            Layout.topMargin: root._pad
            Layout.bottomMargin: root._pad
            spacing: Kirigami.Units.smallSpacing
            visible: root.subtitle.length > 0 || root.subHeader !== null

            Controls.Label {
                text: root.subtitle
                visible: root.subtitle.length > 0
                color: Kirigami.Theme.disabledTextColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Item {
                id: subHeaderHost
                Layout.fillWidth: true
                visible: root.subHeader !== null
                implicitHeight: root.subHeader ? root.subHeader.implicitHeight : 0
                children: root.subHeader ? [root.subHeader] : []
                onWidthChanged: if (root.subHeader) root.subHeader.width = width
                Component.onCompleted: if (root.subHeader)
                    root.subHeader.width = Qt.binding(() => subHeaderHost.width)
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root._separatorColor
            visible: subHeaderBand.visible
        }

        Controls.ScrollView {
            id: bodyScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: bodyHolder._fill ? 0 : bodyHolder.implicitHeight
            clip: true
            contentWidth: availableWidth

            Item {
                id: bodyHolder
                width: bodyScroll.availableWidth

                readonly property real _contentImplicit: {
                    let h = 0
                    for (let i = 0; i < children.length; ++i)
                        h = Math.max(h, children[i].implicitHeight)
                    return h
                }
                readonly property bool _fill: _contentImplicit <= 0
                implicitHeight: _fill ? Math.max(bodyScroll.availableHeight, 0)
                                      : _contentImplicit + root._pad * 2

                function _fitChildren() {
                    for (let i = 0; i < children.length; ++i) {
                        children[i].x = root._pad
                        children[i].y = root._pad
                        children[i].width =
                            Qt.binding(() => bodyHolder.width - root._pad * 2)
                        children[i].height = Qt.binding(() =>
                            children[i].implicitHeight > 0
                                ? children[i].implicitHeight
                                : Math.max(bodyHolder.height - root._pad * 2, 0))
                    }
                }
                onChildrenChanged: _fitChildren()
                Component.onCompleted: _fitChildren()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root._separatorColor
            visible: footerHost.visible
        }
        Item {
            id: footerHost
            Layout.fillWidth: true
            Layout.leftMargin: root._pad
            Layout.rightMargin: root._pad
            Layout.topMargin: root._pad
            Layout.bottomMargin: root._pad
            visible: root.footer !== null
            implicitHeight: root.footer ? root.footer.implicitHeight : 0
            children: root.footer ? [root.footer] : []
            onWidthChanged: if (root.footer) root.footer.width = width
            Component.onCompleted: if (root.footer)
                root.footer.width = Qt.binding(() => footerHost.width)
        }
    }
}

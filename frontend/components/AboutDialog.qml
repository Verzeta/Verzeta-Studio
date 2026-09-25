// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: aboutDialog

    title: qsTr("About Verzeta Studio")
    preferredWidth: Kirigami.Units.gridUnit * 28
    standardButtons: Kirigami.Dialog.Close
    padding: 0

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("Report Issue")
            icon.name: "tools-report-bug"
            onTriggered: Qt.openUrlExternally(BuildInfo.bugTracker)
        },
        Kirigami.Action {
            text: qsTr("Source")
            icon.name: "folder-git"
            onTriggered: Qt.openUrlExternally(BuildInfo.sourceRepo)
        }
    ]

    ColumnLayout {
        spacing: 18

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            spacing: 16

            Image {
                source: "qrc:/icons/verzeta-studio.svg"
                Layout.preferredWidth: 64
                Layout.preferredHeight: 48
                fillMode: Image.PreserveAspectFit
                asynchronous: false
                sourceSize.width: 128
                sourceSize.height: 96
                Layout.alignment: Qt.AlignTop
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Kirigami.Heading {
                    text: qsTr("Verzeta Studio")
                    level: 2
                }
                Controls.Label {
                    text: qsTr("Version %1 (%2 build, commit %3)").arg(BuildInfo.version).arg(BuildInfo.buildType.length > 0 ? BuildInfo.buildType : qsTr("local")).arg(BuildInfo.commit)
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Controls.Label {
                    text: qsTr("Your AI team, running on your machine, in one chat.")
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            columns: 2
            columnSpacing: 16
            rowSpacing: 6

            Controls.Label {
                text: qsTr("Qt")
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
            }
            Controls.Label {
                text: BuildInfo.qtVersion
                Layout.fillWidth: true
            }

            Controls.Label {
                text: qsTr("KDE Frameworks")
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
            }
            Controls.Label {
                text: BuildInfo.kf6Version
                Layout.fillWidth: true
            }

            Controls.Label {
                text: qsTr("llama.cpp")
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
            }
            Controls.Label {
                text: BuildInfo.hasLlamaCpp ? qsTr("Built in (local inference available)") : qsTr("Disabled in this build")
                color: BuildInfo.hasLlamaCpp ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
                Layout.fillWidth: true
            }

            Controls.Label {
                text: qsTr("License")
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
            }
            Controls.Label {
                text: BuildInfo.licenseId
                Layout.fillWidth: true
            }

            Controls.Label {
                text: qsTr("Author")
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
            }
            Controls.Label {
                text: BuildInfo.author + " <" + BuildInfo.authorEmail + ">"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            Controls.Label {
                text: qsTr("Homepage")
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
            }
            Controls.Label {
                text: '<a href="' + BuildInfo.homepage + '">' + BuildInfo.homepage + '</a>'
                onLinkActivated: link => Qt.openUrlExternally(link);
                color: Kirigami.Theme.linkColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                MouseArea {
                    anchors.fill: parent
                    cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    acceptedButtons: Qt.NoButton
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.bottomMargin: 16
            spacing: 4

            Controls.Label {
                text: qsTr("Acknowledgements")
                font.bold: true
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                text: qsTr("Built with Qt, KDE Frameworks, SQLite, " + "llama.cpp and bubblewrap.")
            }
        }
    }
}

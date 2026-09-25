// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: skillsPage
    color: ThemeController.surfacePage

    SkillReviewDialog { id: reviewDialog }
    Dialogs.FolderDialog {
        id: folderPicker
        title: qsTr("Select Skill Folder to Import")
        onAccepted: {
            const path = PathUtils.toLocalFile(selectedFolder)
            if (path.length === 0) return
            const err  = Skills.importSkillFolder(path)
            if (err.length > 0) {
                statusBanner.text    = qsTr("Import failed: %1").arg(err)
                statusBanner.isError = true
            } else {
                statusBanner.text    = qsTr("Imported. Review it in the Installed tab.")
                statusBanner.isError = false
            }
        }
    }

    property var    clawhubResults: []
    property string clawhubCursor:  ""

    Connections {
        target: ClawHub
        function onSearchCompleted(results, nextCursor) {
            skillsPage.clawhubResults = results
            skillsPage.clawhubCursor  = nextCursor
            statusBanner.text = ""
        }
        function onSearchFailed(err) {
            statusBanner.text = qsTr("Search failed: %1").arg(err)
            statusBanner.isError = true
        }
        function onSearchResultEnriched(slug, row) {
            const updated = []
            for (let i = 0; i < skillsPage.clawhubResults.length; ++i) {
                const orig = skillsPage.clawhubResults[i]
                if (orig.slug === slug) {
                    const merged = Object.assign({}, orig)
                    for (const k in row) merged[k] = row[k]
                    updated.push(merged)
                } else {
                    updated.push(orig)
                }
            }
            skillsPage.clawhubResults = updated
        }
        function onDownloadCompleted(slug, version, _localPath) {
            statusBanner.text = qsTr("Installed %1@%2. Review it in the Installed tab.")
                                  .arg(slug).arg(version)
            statusBanner.isError = false
        }
        function onDownloadFailed(slug, err) {
            statusBanner.text = qsTr("Install failed for %1: %2").arg(slug).arg(err)
            statusBanner.isError = true
        }
    }

    Controls.ScrollView {
        contentWidth: applicationWindow().isCompact ? availableWidth : -1  
        anchors.fill: parent
        Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff
        clip: true

        ColumnLayout {
            width: Math.min(skillsPage.width - 64, 700)
            x: (skillsPage.width - width) / 2
            spacing: 4

            Item { Layout.preferredHeight: 32 }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Kirigami.Heading {
                    text: qsTr("Skills")
                    level: 2
                    Layout.fillWidth: true
                }

                AppButton {
                    text: qsTr("Import Folder")
                    icon.name: "folder-add"
                    highlighted: true
                    visible: tabBar.currentIndex === 0 || tabBar.currentIndex === 1
                    onClicked: folderPicker.open()
                }
            }

            Controls.Label {
                text: qsTr("Skills are reusable instruction bundles that agents can follow. They never grant tools and never run scripts on their own. Every install stays unreviewed until you approve it.")
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.bottomMargin: 8
            }

            Controls.Label {
                id: statusBanner
                property bool isError: false
                text: ""
                visible: text.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: isError
                       ? Kirigami.Theme.negativeTextColor
                       : Kirigami.Theme.positiveTextColor
                font.pointSize: Kirigami.Theme.defaultFont.pointSize
                Layout.bottomMargin: 4
            }

            Controls.TabBar {
                id: tabBar
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 8

                Controls.TabButton {
                    text: qsTr("Installed (%1)").arg(installedRepeater.count)
                    width: applicationWindow().isCompact
                           ? Math.floor(tabBar.width / tabBar.count)
                           : implicitWidth
                }
                Controls.TabButton {
                    text: applicationWindow().isCompact
                          ? qsTr("Review (%1)").arg(reviewRepeater.count)
                          : qsTr("Review Queue (%1)").arg(reviewRepeater.count)
                    width: applicationWindow().isCompact
                           ? Math.floor(tabBar.width / tabBar.count)
                           : implicitWidth
                }
                Controls.TabButton {
                    text: applicationWindow().isCompact
                          ? qsTr("Search")
                          : qsTr("Search & Install")
                    width: applicationWindow().isCompact
                           ? Math.floor(tabBar.width / tabBar.count)
                           : implicitWidth
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: tabBar.currentIndex === 0
                spacing: 4

                Controls.Label {
                    visible: installedRepeater.count === 0
                    text: qsTr("No skills installed yet.\nClick \"Import Folder\" above to import a local skill, or use the Search & Install tab to fetch one from ClawHub.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 16
                }

                Repeater {
                    id: installedRepeater
                    model: Skills.installedSkills()
                    delegate: skillCardComponent
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: tabBar.currentIndex === 1
                spacing: 4

                Controls.Label {
                    text: qsTr("Skills awaiting your review (still unreviewed) or carrying parser warnings. Approving or blocking a skill removes it from this list.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 8
                }

                Controls.Label {
                    visible: reviewRepeater.count === 0
                    text: qsTr("Nothing to review.")
                    color: Kirigami.Theme.disabledTextColor
                    font.italic: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    Layout.fillWidth: true
                    Layout.bottomMargin: 16
                }

                Repeater {
                    id: reviewRepeater
                    model: {
                        const all = Skills.installedSkills()
                        return all.filter(function(s) {
                            return s.reviewState === "unreviewed"
                                || (s.warnings && s.warnings.length > 0)
                        })
                    }
                    delegate: skillCardComponent
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: tabBar.currentIndex === 2
                spacing: 4

                Controls.Label {
                    text: qsTr("Search ClawHub for skills. Every install lands as 'unreviewed' until you approve it explicitly. Results flagged by the registry as suspicious or malware-blocked are surfaced with the registry's verdict. Malware-blocked items cannot be installed.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 8
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppTextField {
                        id: searchField
                        Layout.fillWidth: true
                        placeholderText: qsTr("e.g. browser automation, github triage…")
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        onAccepted: {
                            statusBanner.text    = qsTr("Searching…")
                            statusBanner.isError = false
                            ClawHub.search(text, "", 10)
                        }
                    }
                    AppButton {
                        text: qsTr("Search")
                        icon.name: "system-search"
                        highlighted: true
                        enabled: !ClawHub.searchInFlight
                        onClicked: {
                            statusBanner.text    = qsTr("Searching…")
                            statusBanner.isError = false
                            ClawHub.search(searchField.text, "", 10)
                        }
                    }
                }

                Item { Layout.preferredHeight: 8 }

                Repeater {
                    id: searchRepeater
                    model: skillsPage.clawhubResults
                    delegate: searchResultCardComponent
                }

                Controls.Label {
                    visible: skillsPage.clawhubResults.length === 0
                            && !ClawHub.searchInFlight
                    text: qsTr("No results yet. Type a query above and press Enter.")
                    color: Kirigami.Theme.disabledTextColor
                    font.italic: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                }

                AppButton {
                    text: qsTr("Next page →")
                    visible: skillsPage.clawhubCursor.length > 0
                    Layout.topMargin: 8
                    onClicked: ClawHub.search(searchField.text,
                                               skillsPage.clawhubCursor, 10)
                }
            }

            Item { Layout.preferredHeight: 40 }
        }
    }

    function refresh() {
        installedRepeater.model = Skills.installedSkills()
        const all = Skills.installedSkills()
        reviewRepeater.model = all.filter(function(s) {
            return s.reviewState === "unreviewed"
                || (s.warnings && s.warnings.length > 0)
        })
    }
    Connections {
        target: Skills
        function onSkillsChanged() { skillsPage.refresh() }
    }

    Component {
        id: skillCardComponent

        Rectangle {
            required property var modelData

            Layout.fillWidth: true
            Layout.bottomMargin: 8
            implicitHeight: skillCardContent.implicitHeight + 24
            radius: ThemeController.radius
            color: ThemeController.surfaceCard
            border.color: ThemeController.borderSubtle
            border.width: 1

            TapHandler { onTapped: reviewDialog.openFor(modelData.id) }

            ColumnLayout {
                id: skillCardContent
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Kirigami.Icon {
                        source: "applications-education"
                        fallback: "applications-science"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                        Layout.alignment: Qt.AlignVCenter
                        color: {
                            if (modelData.reviewState === "approved")
                                return Kirigami.Theme.positiveTextColor
                            if (modelData.reviewState === "blocked")
                                return Kirigami.Theme.negativeTextColor
                            if (modelData.warnings && modelData.warnings.length > 0)
                                return Kirigami.Theme.neutralTextColor
                            return Kirigami.Theme.disabledTextColor
                        }
                    }

                    Controls.Label {
                        text: modelData.displayName || modelData.id
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        color: Kirigami.Theme.textColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        text: modelData.version || ""
                        visible: text.length > 0
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Rectangle {
                        radius: ThemeController.radius
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: stateBadge.implicitWidth + 10
                        Layout.preferredHeight: stateBadge.implicitHeight + 4
                        color: {
                            if (modelData.reviewState === "approved")
                                return Qt.rgba(Kirigami.Theme.positiveTextColor.r,
                                                Kirigami.Theme.positiveTextColor.g,
                                                Kirigami.Theme.positiveTextColor.b, 0.15)
                            if (modelData.reviewState === "blocked")
                                return Qt.rgba(Kirigami.Theme.negativeTextColor.r,
                                                Kirigami.Theme.negativeTextColor.g,
                                                Kirigami.Theme.negativeTextColor.b, 0.15)
                            return Qt.rgba(Kirigami.Theme.neutralTextColor.r,
                                            Kirigami.Theme.neutralTextColor.g,
                                            Kirigami.Theme.neutralTextColor.b, 0.15)
                        }
                        Controls.Label {
                            id: stateBadge
                            anchors.centerIn: parent
                            text: modelData.reviewState
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            font.bold: true
                            color: {
                                if (modelData.reviewState === "approved")
                                    return Kirigami.Theme.positiveTextColor
                                if (modelData.reviewState === "blocked")
                                    return Kirigami.Theme.negativeTextColor
                                return Kirigami.Theme.neutralTextColor
                            }
                        }
                    }

                    Controls.ToolButton {
                        icon.name: "dialog-ok"
                        visible: modelData.reviewState !== "approved"
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: reviewDialog.openFor(modelData.id)
                        Controls.ToolTip.text: qsTr("Review & approve")
                        Controls.ToolTip.visible: hovered
                    }
                    Controls.ToolButton {
                        icon.name: "edit-delete"
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: Skills.removeSkill(modelData.id)
                        Controls.ToolTip.text: qsTr("Remove skill")
                        Controls.ToolTip.visible: hovered
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    visible: (modelData.description || "").length > 0
                    text: modelData.description || ""
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    spacing: 8
                    Controls.Label {
                        visible: (modelData.tags || []).length > 0
                        text: qsTr("tags: %1").arg((modelData.tags || []).join(", "))
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Controls.Label {
                        visible: modelData.warnings && modelData.warnings.length > 0
                        text: qsTr("⚠ %1 warning(s)").arg((modelData.warnings || []).length)
                        color: Kirigami.Theme.neutralTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        Layout.alignment: Qt.AlignRight
                    }
                    Controls.Label {
                        text: modelData.source || "manual"
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        Layout.alignment: Qt.AlignRight
                    }
                }
            }
        }
    }

    Component {
        id: searchResultCardComponent

        Rectangle {
            required property var modelData

            Layout.fillWidth: true
            Layout.bottomMargin: 8
            implicitHeight: searchCardContent.implicitHeight + 24
            radius: ThemeController.radius
            color: ThemeController.surfaceCard
            border.color: modelData.isMalwareBlocked
                ? Kirigami.Theme.negativeTextColor
                : (modelData.isSuspicious
                    ? Kirigami.Theme.neutralTextColor
                    : ThemeController.borderSubtle)
            border.width: (modelData.isMalwareBlocked || modelData.isSuspicious) ? 2 : 1

            ColumnLayout {
                id: searchCardContent
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Kirigami.Icon {
                        source: "applications-education"
                        fallback: "applications-science"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                        Layout.alignment: Qt.AlignVCenter
                        color: modelData.isMalwareBlocked
                            ? Kirigami.Theme.negativeTextColor
                            : (modelData.isSuspicious
                                ? Kirigami.Theme.neutralTextColor
                                : Kirigami.Theme.highlightColor)
                    }

                    Controls.Label {
                        text: modelData.name || modelData.slug
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                        color: Kirigami.Theme.textColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        text: (modelData.latestVersion && modelData.latestVersion.length > 0)
                              ? "v" + modelData.latestVersion
                              : ""
                        visible: text.length > 0
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Rectangle {
                        visible: modelData.isMalwareBlocked || modelData.isSuspicious
                        radius: ThemeController.radius
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: modBadge.implicitWidth + 10
                        Layout.preferredHeight: modBadge.implicitHeight + 4
                        color: modelData.isMalwareBlocked
                            ? Qt.rgba(Kirigami.Theme.negativeTextColor.r,
                                       Kirigami.Theme.negativeTextColor.g,
                                       Kirigami.Theme.negativeTextColor.b, 0.18)
                            : Qt.rgba(Kirigami.Theme.neutralTextColor.r,
                                       Kirigami.Theme.neutralTextColor.g,
                                       Kirigami.Theme.neutralTextColor.b, 0.18)
                        Controls.Label {
                            id: modBadge
                            anchors.centerIn: parent
                            text: modelData.isMalwareBlocked
                                ? qsTr("⛔ %1").arg(modelData.verdict || "blocked")
                                : qsTr("⚠ %1").arg(modelData.verdict || "suspicious")
                            font.bold: true
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.8
                            color: modelData.isMalwareBlocked
                                ? Kirigami.Theme.negativeTextColor
                                : Kirigami.Theme.neutralTextColor
                        }
                    }

                    AppButton {
                        text: modelData.isMalwareBlocked
                              ? qsTr("Blocked")
                              : qsTr("Install")
                        icon.name: modelData.isMalwareBlocked ? "edit-delete" : "list-add"
                        highlighted: !modelData.isMalwareBlocked
                        enabled: !ClawHub.downloadInFlight && !modelData.isMalwareBlocked
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: {
                            statusBanner.text    = qsTr("Downloading %1…").arg(modelData.slug)
                            statusBanner.isError = false
                            ClawHub.downloadLatest(modelData.slug)
                        }
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    visible: (modelData.reasonCodes || []).length > 0
                    text: qsTr("ClawHub moderation: %1")
                          .arg((modelData.reasonCodes || []).join(", "))
                    color: modelData.isMalwareBlocked
                        ? Kirigami.Theme.negativeTextColor
                        : Kirigami.Theme.neutralTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    wrapMode: Text.WordWrap
                }

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    text: modelData.description || qsTr("(no summary)")
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.WordWrap
                }

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    text: {
                        var parts = []
                        if (modelData.author && modelData.author.length > 0)
                            parts.push(qsTr("by %1").arg(modelData.author))
                        if (modelData.downloads > 0)
                            parts.push(qsTr("%1 downloads").arg(modelData.downloads))
                        if (modelData.stars > 0)
                            parts.push(qsTr("★ %1").arg(modelData.stars))
                        if (modelData.installsCurrent > 0)
                            parts.push(qsTr("%1 currently installed").arg(modelData.installsCurrent))
                        if (modelData.versionsCount > 0)
                            parts.push(qsTr("%1 version(s)").arg(modelData.versionsCount))
                        return parts.join(" · ")
                    }
                    visible: text.length > 0
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}

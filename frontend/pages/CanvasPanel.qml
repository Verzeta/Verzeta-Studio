// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

Rectangle {
    id: canvasPanel
    color: ThemeController.surfacePage

    property string conversationId: ""

    property var activeCanvas: ({})

    readonly property bool hasActiveCanvas: activeCanvas && activeCanvas.id !== undefined && activeCanvas.id !== ""

    signal closeRequested

    function refreshActiveCanvas() {
        if (canvasPanel.conversationId.length === 0) {
            canvasPanel.activeCanvas = ({});
            return;
        }
        canvasPanel.activeCanvas = Canvas.activeCanvasFor(canvasPanel.conversationId);
    }

    function showActionResult(r) {
        actionBanner.ok = (r && r.ok === true);
        actionBanner.message = r && r.message ? r.message : (actionBanner.ok ? qsTr("Done") : qsTr("Action failed"));
        actionBanner.visible = true;
        actionBannerTimer.restart();
    }

    property bool wordWrap: true

    property bool consoleVisible: false

    property bool _sandboxBannerCollapsed: false
    property bool _sandboxBannerDismissed: false

    readonly property string _canvasLang: canvasPanel.hasActiveCanvas ? (canvasPanel.activeCanvas.language || "") : ""
    readonly property bool _isRunLanguage: _canvasLang.length > 0 && CanvasRunner.supportedRunLanguages().indexOf(_canvasLang) >= 0
    readonly property bool _isIdeLanguage: _canvasLang.length > 0 && CanvasRunner.supportedIdeLanguages().indexOf(_canvasLang) >= 0

    onConversationIdChanged: refreshActiveCanvas()

    Connections {
        target: Canvas
        function onCanvasOpened(convId, canvasId) {
            if (convId === canvasPanel.conversationId) {
                canvasPanel.refreshActiveCanvas();
            }
        }
        function onCanvasUpdated(convId, canvasId, revision) {
            if (convId === canvasPanel.conversationId) {
                canvasPanel.refreshActiveCanvas();
            }
        }
        function onCanvasClosed(convId, canvasId) {
            if (convId === canvasPanel.conversationId) {
                canvasPanel.refreshActiveCanvas();
            }
        }
    }

    Component.onCompleted: refreshActiveCanvas()

    FileDialog {
        id: exportDialog
        title: qsTr("Export Canvas")
        fileMode: FileDialog.SaveFile
        defaultSuffix: ""
        onAccepted: {
            if (canvasPanel.conversationId.length === 0)
                return;
            const path = PathUtils.toLocalFile(selectedFile);
            if (path.length === 0)
                return;
            Canvas.exportCanvasToFile(canvasPanel.conversationId, path);
        }
    }

    function openExportDialog() {
        if (canvasPanel.conversationId.length === 0)
            return;
        const suggested = Canvas.suggestedExportName(canvasPanel.conversationId);
        if (suggested.length > 0) {
            exportDialog.currentFile = PathUtils.fromLocalFile(suggested);
        }
        exportDialog.open();
    }

    Shortcut {
        sequences: [StandardKey.Close]
        enabled: canvasPanel.hasActiveCanvas
        context: Qt.WindowShortcut
        onActivated: {
            if (canvasPanel.conversationId.length === 0)
                return;
            Canvas.closeCanvas(canvasPanel.conversationId);
            canvasPanel.closeRequested();
        }
    }

    Shortcut {
        sequences: ["Ctrl+Return", "Ctrl+Enter"]
        enabled: canvasPanel.hasActiveCanvas && CanvasRunner.supportedRunLanguages().indexOf(canvasPanel.activeCanvas ? (canvasPanel.activeCanvas.language || "") : "") >= 0
        context: Qt.WindowShortcut
        onActivated: {
            if (canvasPanel.conversationId.length === 0)
                return;
            if (CanvasRunner.running)
                return;
            canvasPanel.consoleVisible = true;
            CanvasRunner.runActiveCanvas(canvasPanel.conversationId);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: header
            Layout.fillWidth: true
            implicitHeight: 48
            color: ThemeController.surfaceCard

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 6

                AppButton {
                    id: toolsButton
                    icon.name: "view-more-symbolic"
                    flat: true
                    iconOnly: true
                    enabled: canvasPanel.hasActiveCanvas
                    Controls.ToolTip.text: qsTr("Tools")
                    Controls.ToolTip.visible: hovered
                    onClicked: toolsMenu.open()

                    Controls.Menu {
                        id: toolsMenu

                        function refresh() {
                            actionsModel.clear();
                            if (!canvasPanel.hasActiveCanvas)
                                return;
                            var lang = canvasPanel.activeCanvas.language || "plaintext";
                            var rows = Canvas.availableActionsForLanguage(lang);
                            for (var i = 0; i < rows.length; ++i) {
                                actionsModel.append(rows[i]);
                            }
                        }

                        onAboutToShow: toolsMenu.refresh()

                        Instantiator {
                            model: ListModel {
                                id: actionsModel
                            }
                            delegate: Controls.MenuItem {
                                text: model.label
                                enabled: model.enabled
                                Controls.ToolTip.text: model.description || ""
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.delay: 600
                                onTriggered: {
                                    if (canvasPanel.conversationId.length === 0) {
                                        return;
                                    }
                                    var r = Canvas.performAction(canvasPanel.conversationId, model.id);
                                    canvasPanel.showActionResult(r);
                                }
                            }
                            onObjectAdded: function (index, object) {
                                toolsMenu.insertItem(index, object);
                            }
                            onObjectRemoved: function (index, object) {
                                toolsMenu.removeItem(object);
                            }
                        }
                    }
                }

                AppButton {
                    icon.name: "text-wrap"
                    flat: true
                    iconOnly: true
                    checkable: true
                    checked: canvasPanel.wordWrap
                    enabled: canvasPanel.hasActiveCanvas
                    opacity: canvasPanel.wordWrap ? 1.0 : 0.45
                    Behavior on opacity  {
                        NumberAnimation {
                            duration: 120
                        }
                    }
                    Controls.ToolTip.text: canvasPanel.wordWrap ? qsTr("Word wrap: ON (click to disable)") : qsTr("Word wrap: OFF (click to enable)")
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                    onClicked: canvasPanel.wordWrap = !canvasPanel.wordWrap
                }

                AppButton {
                    icon.name: "window-close-symbolic"
                    flat: true
                    iconOnly: true
                    enabled: canvasPanel.hasActiveCanvas
                    Controls.ToolTip.text: qsTr("Close canvas")
                    Controls.ToolTip.visible: hovered
                    onClicked: {
                        if (canvasPanel.conversationId.length > 0) {
                            editor.flushPendingSave();
                            Canvas.closeCanvas(canvasPanel.conversationId);
                        }
                        canvasPanel.closeRequested();
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Controls.ComboBox {
                        id: historyCombo
                        Layout.fillWidth: true
                        flat: true
                        textRole: "filename"
                        valueRole: "id"
                        model: ListModel {
                            id: historyModel
                        }

                        displayText: {
                            if (canvasPanel.hasActiveCanvas) {
                                return canvasPanel.activeCanvas.filename || "";
                            }
                            if (currentIndex >= 0 && currentIndex < historyModel.count) {
                                return historyModel.get(currentIndex).filename || "";
                            }
                            return "";
                        }

                        property bool _refreshing: false

                        function refresh() {
                            _refreshing = true;
                            historyModel.clear();
                            if (canvasPanel.conversationId.length === 0) {
                                _refreshing = false;
                                return;
                            }
                            var rows = Canvas.historyForConversation(canvasPanel.conversationId);
                            for (var i = 0; i < rows.length; ++i) {
                                historyModel.append({
                                        "id": rows[i].id,
                                        "filename": rows[i].filename,
                                        "isArchived": rows[i].isArchived
                                    });
                            }
                            if (canvasPanel.hasActiveCanvas) {
                                var activeId = canvasPanel.activeCanvas.id;
                                for (var j = 0; j < historyModel.count; ++j) {
                                    if (historyModel.get(j).id === activeId) {
                                        historyCombo.currentIndex = j;
                                        break;
                                    }
                                }
                            }
                            _refreshing = false;
                        }

                        Connections {
                            target: canvasPanel
                            function onActiveCanvasChanged() {
                                historyCombo.refresh();
                            }
                        }
                        Component.onCompleted: historyCombo.refresh()

                        onActivated: function (index) {
                            if (_refreshing)
                                return;
                            if (index < 0 || index >= historyModel.count)
                                return;
                            var row = historyModel.get(index);
                            if (!row || row.id.length === 0)
                                return;
                            if (canvasPanel.hasActiveCanvas && canvasPanel.activeCanvas.id === row.id) {
                                return;
                            }
                            Canvas.switchToCanvas(canvasPanel.conversationId, row.id);
                        }

                        Connections {
                            target: Canvas
                            function onCanvasOpened(c, id) {
                                historyCombo.refresh();
                            }
                            function onCanvasUpdated(c, id, rev) {
                                historyCombo.refresh();
                            }
                            function onCanvasClosed(c, id) {
                                historyCombo.refresh();
                            }
                        }
                    }
                }

                AppButton {
                    visible: canvasPanel._isRunLanguage
                    icon.name: CanvasRunner.running ? "media-playback-stop" : "media-playback-start"
                    text: CanvasRunner.running ? qsTr("Stop") : qsTr("Run")
                    flat: !CanvasRunner.running
                    highlighted: CanvasRunner.running
                    enabled: canvasPanel.hasActiveCanvas
                    Layout.preferredWidth: 96
                    Layout.preferredHeight: 36
                    Layout.alignment: Qt.AlignVCenter
                    Controls.ToolTip.text: CanvasRunner.running ? qsTr("Stop the running canvas") : (CanvasRunner.sandboxAvailable ? qsTr("Run the active canvas in a sandbox") : qsTr("Run the active canvas (no sandbox, see the warning above)"))
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                    onClicked: {
                        console.log("[CanvasPanel] Run clicked: running:", CanvasRunner.running, "convId:", canvasPanel.conversationId);
                        if (CanvasRunner.running) {
                            CanvasRunner.cancel();
                            return;
                        }
                        if (canvasPanel.conversationId.length === 0) {
                            console.warn("[CanvasPanel] Run aborted: no conversationId");
                            return;
                        }
                        canvasPanel.consoleVisible = true;
                        CanvasRunner.runActiveCanvas(canvasPanel.conversationId);
                    }
                }

                AppButton {
                    visible: canvasPanel._isIdeLanguage
                    icon.name: "document-edit"
                    text: qsTr("Open in IDE")
                    flat: true
                    enabled: canvasPanel.hasActiveCanvas
                    Layout.preferredWidth: 130
                    Layout.preferredHeight: 36
                    Layout.alignment: Qt.AlignVCenter
                    Controls.ToolTip.text: qsTr("Open this canvas in your default editor")
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                    onClicked: {
                        console.log("[CanvasPanel] Open-in-IDE clicked, convId:", canvasPanel.conversationId);
                        if (canvasPanel.conversationId.length > 0) {
                            CanvasRunner.sendToIde(canvasPanel.conversationId);
                        }
                    }
                }

                AppButton {
                    icon.name: "document-export"
                    text: qsTr("Export")
                    flat: true
                    enabled: canvasPanel.hasActiveCanvas
                    Layout.preferredWidth: 100
                    Layout.preferredHeight: 36
                    Layout.alignment: Qt.AlignVCenter
                    Controls.ToolTip.text: qsTr("Export the active canvas to a file…")
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                    onClicked: {
                        console.log("[CanvasPanel] Export clicked");
                        canvasPanel.openExportDialog();
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Rectangle {
            id: sandboxWarningBanner
            Layout.fillWidth: true
            visible: canvasPanel._isRunLanguage && !CanvasRunner.sandboxAvailable && !canvasPanel._sandboxBannerDismissed
            implicitHeight: visible ? (canvasPanel._sandboxBannerCollapsed ? sandboxWarningCollapsedRow.implicitHeight + 12 : sandboxWarningRow.implicitHeight + 18) : 0
            color: Qt.rgba(Kirigami.Theme.negativeTextColor.r, Kirigami.Theme.negativeTextColor.g, Kirigami.Theme.negativeTextColor.b, 0.10)
            border.color: Qt.rgba(Kirigami.Theme.negativeTextColor.r, Kirigami.Theme.negativeTextColor.g, Kirigami.Theme.negativeTextColor.b, 0.30)
            border.width: 1

            RowLayout {
                id: sandboxWarningRow
                visible: !canvasPanel._sandboxBannerCollapsed
                anchors.fill: parent
                anchors.margins: 9
                spacing: 10

                Kirigami.Icon {
                    source: "dialog-warning"
                    fallback: "emblem-warning"
                    width: Kirigami.Units.iconSizes.smallMedium
                    height: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                    Layout.alignment: Qt.AlignTop
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.negativeTextColor
                    text: qsTr("<b>No OS-level sandbox available on this platform.</b> ") + qsTr("Scripts you run from Canvas execute directly ") + qsTr("with your user account's privileges. They can ") + qsTr("read and write your files, use the network, and persist ") + qsTr("data on disk. The safety scanner blocks obviously destructive ") + qsTr("patterns (rm -rf, format, sudo, etc.), ") + qsTr("but that is not a complete defence. The ") + qsTr("application takes no responsibility for the behaviour ") + qsTr("of scripts you choose to execute.")
                    textFormat: Text.RichText
                }

                Controls.ToolButton {
                    icon.name: "go-up"
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignTop
                    onClicked: canvasPanel._sandboxBannerCollapsed = true
                    Controls.ToolTip.text: qsTr("Minimize")
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                }
                Controls.ToolButton {
                    icon.name: "window-close-symbolic"
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignTop
                    onClicked: canvasPanel._sandboxBannerDismissed = true
                    Controls.ToolTip.text: qsTr("Dismiss for this session")
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                }
            }

            RowLayout {
                id: sandboxWarningCollapsedRow
                visible: canvasPanel._sandboxBannerCollapsed
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8

                Kirigami.Icon {
                    source: "dialog-warning"
                    fallback: "emblem-warning"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    Layout.alignment: Qt.AlignVCenter
                }
                Controls.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: Kirigami.Theme.negativeTextColor
                    text: qsTr("No OS-level sandbox. Run executes scripts directly, without isolation.")
                    Layout.alignment: Qt.AlignVCenter
                }
                Controls.ToolButton {
                    icon.name: "go-down"
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: canvasPanel._sandboxBannerCollapsed = false
                    Controls.ToolTip.text: qsTr("Show full warning")
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                }
                Controls.ToolButton {
                    icon.name: "window-close-symbolic"
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: canvasPanel._sandboxBannerDismissed = true
                    Controls.ToolTip.text: qsTr("Dismiss for this session")
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: 600
                }
            }
        }

        Rectangle {
            id: actionBanner
            Layout.fillWidth: true
            implicitHeight: visible ? bannerLabel.implicitHeight + 16 : 0
            visible: false
            property bool ok: true
            property string message: ""
            color: actionBanner.ok ? Qt.rgba(Kirigami.Theme.positiveTextColor.r, Kirigami.Theme.positiveTextColor.g, Kirigami.Theme.positiveTextColor.b, 0.10) : Qt.rgba(Kirigami.Theme.negativeTextColor.r, Kirigami.Theme.negativeTextColor.g, Kirigami.Theme.negativeTextColor.b, 0.10)
            border.width: 0

            Controls.Label {
                id: bannerLabel
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                text: actionBanner.message
                wrapMode: Text.Wrap
                color: actionBanner.ok ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
            }
        }

        Timer {
            id: actionBannerTimer
            interval: 4000
            repeat: false
            onTriggered: actionBanner.visible = false
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            CanvasEditor {
                id: editor
                anchors.fill: parent
                visible: canvasPanel.hasActiveCanvas
                content: canvasPanel.hasActiveCanvas ? canvasPanel.activeCanvas.content : ""
                language: canvasPanel.hasActiveCanvas ? (canvasPanel.activeCanvas.language || "plaintext") : "plaintext"
                readOnly: false
                wordWrap: canvasPanel.wordWrap
                onSaveRequested: function (newContent) {
                    if (canvasPanel.conversationId.length === 0)
                        return;
                    Canvas.editCanvas(canvasPanel.conversationId, newContent);
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                visible: !canvasPanel.hasActiveCanvas
                spacing: 8

                Kirigami.Icon {
                    Layout.alignment: Qt.AlignHCenter
                    source: "view-pim-notes"
                    fallback: "text-x-generic"
                    implicitWidth: Kirigami.Units.iconSizes.large
                    implicitHeight: Kirigami.Units.iconSizes.large
                    color: Kirigami.Theme.disabledTextColor
                }
                Controls.Label {
                    text: qsTr("No active canvas in this conversation.")
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: canvasPanel.hasActiveCanvas && canvasPanel.consoleVisible
        }
        CanvasConsole {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(180, Math.max(120, Math.round(canvasPanel.height * 0.25)))
            visible: canvasPanel.hasActiveCanvas && canvasPanel.consoleVisible
            onCloseRequested: canvasPanel.consoleVisible = false
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: canvasPanel.hasActiveCanvas
        }
        CanvasActionBar {
            Layout.fillWidth: true
            visible: canvasPanel.hasActiveCanvas
            conversationId: canvasPanel.conversationId
            hasActiveCanvas: canvasPanel.hasActiveCanvas
            canvasLanguage: canvasPanel.hasActiveCanvas ? (canvasPanel.activeCanvas.language || "plaintext") : ""
            consoleVisible: canvasPanel.consoleVisible
            onToggleConsoleRequested: canvasPanel.consoleVisible = !canvasPanel.consoleVisible
        }
    }
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

AppOverlayDialog {
    id: dialog
    parent: applicationWindow().overlay
    closePolicy: Controls.Popup.CloseOnEscape

    implicitWidth: Math.min(applicationWindow().width * 0.9,
                             Kirigami.Units.gridUnit * 40)

    readonly property bool _isMobile:
        applicationWindow().width < Kirigami.Units.gridUnit * 36

    readonly property color _cPanel:
        Qt.darker(Kirigami.Theme.backgroundColor, 0.6)
    readonly property color _cWell:
        Qt.darker(Kirigami.Theme.backgroundColor, 1.15)

    property string lastPairCode: ""
    property string statusText: ""

    Connections {
        target: RemoteAccess
        function onRemoteServerRunningChanged() { dialog.refresh() }
    }
    onVisibleChanged: if (visible) {
        bindField.text     = RemoteAccess.preferredBindAddr()
        portField.text     = RemoteAccess.preferredPort().toString()
        tlsSwitch.checked  = RemoteAccess.tlsEnabled()
        autoSwitch.checked = RemoteAccess.autoStartEnabled()
        refresh()
    }

    function refresh() {
        if (!RemoteAccess) return
        clientsModel.clear()
        revokedClientsModel.clear()
        const list = RemoteAccess.listPairedClients()
        for (let i = 0; i < list.length; ++i) {
            const row = {
                "client_id":    list[i].id || "",
                "name":         list[i].name || "",
                "created_at":   list[i].created_at || 0,
                "last_seen_at": list[i].last_seen_at || 0,
                "revoked":      list[i].revoked || false,
            }
            if (row.revoked) {
                revokedClientsModel.append(row)
            } else {
                clientsModel.append(row)
            }
        }
        lanModel.clear()
        const lan = RemoteAccess.lanAddresses()
        for (let j = 0; j < lan.length; ++j) lanModel.append({"addr": lan[j]})
        if (tlsSwitch.checked) {
            fingerprintLabel.text = RemoteAccess.certFingerprint()
        }
    }

    ListModel { id: clientsModel }
    ListModel { id: revokedClientsModel }
    ListModel { id: lanModel }

    RevokedDevicesSheet {
        id: revokedDevicesSheet
        clientsModel: revokedClientsModel
    }

    component FieldLabel: Controls.Label {
        font.family: ThemeController.fontFamily
        font.bold: true
        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.76
        font.letterSpacing: 0.8
        color: Kirigami.Theme.disabledTextColor
    }

    title: qsTr("Remote Access")
    subtitle: qsTr("HOST · PAIR DEVICES")
    dialogIcon: "network-server"

    footer: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Controls.Label {
            text: dialog.statusText.length > 0
                ? dialog.statusText
                : qsTr("Runs as a separate process.")
            color: dialog.statusText.length > 0
                ? Kirigami.Theme.activeTextColor
                : Kirigami.Theme.disabledTextColor
            font.family: ThemeController.fontFamily
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 0.85
            visible: !dialog._isMobile
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        Item { visible: dialog._isMobile; Layout.fillWidth: true }

        AppButton {
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    }

    ColumnLayout {
        Layout.margins: Kirigami.Units.gridUnit
        spacing: Kirigami.Units.gridUnit

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: heroLayout.implicitHeight
                + Kirigami.Units.gridUnit * 2

            RowLayout {
                id: heroLayout
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.gridUnit

                Kirigami.Icon {
                    source: RemoteAccess.remoteRunning
                        ? "network-connect" : "network-disconnect"
                    fallback: "network-server"
                    color: RemoteAccess.remoteRunning
                        ? Kirigami.Theme.positiveTextColor
                        : Kirigami.Theme.disabledTextColor
                    Layout.preferredWidth: Kirigami.Units.iconSizes.large
                    Layout.preferredHeight: Kirigami.Units.iconSizes.large
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: RemoteAccess.remoteRunning
                            ? qsTr("Server is running")
                            : qsTr("Server is stopped")
                        font.bold: true
                        font.family: ThemeController.fontFamily
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        visible: RemoteAccess.remoteRunning && text.length > 0
                        wrapMode: Text.WrapAnywhere
                        text: RemoteAccess.remoteUrl
                        color: Kirigami.Theme.disabledTextColor
                        font.family: "monospace"
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                    }
                }

                Controls.Switch {
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                    checked: RemoteAccess.remoteRunning
                    onToggled: {
                        if (checked) {
                            const port = parseInt(portField.text, 10) || 9180
                            RemoteAccess.setPreferredBindAddr(bindField.text)
                            RemoteAccess.setPreferredPort(port)
                            RemoteAccess.setTlsEnabled(tlsSwitch.checked)
                            const ok = RemoteAccess.startRemoteServer(
                                bindField.text, port, tlsSwitch.checked, "", "")
                            dialog.statusText = ok
                                ? qsTr("Server started")
                                : qsTr("Failed to start: check the log")
                        } else {
                            RemoteAccess.stopRemoteServer()
                            dialog.statusText = qsTr("Server stopped")
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: netCol.implicitHeight
                + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: netCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Network")
                    font.family: ThemeController.fontFamily
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                }

                FieldLabel { text: qsTr("BIND ADDRESS") }
                AppTextField {
                    id: bindField
                    Layout.fillWidth: true
                    placeholderText: "0.0.0.0"
                    text: "0.0.0.0"
                    onEditingFinished: RemoteAccess.setPreferredBindAddr(text)
                }

                Item { Layout.preferredHeight: Kirigami.Units.smallSpacing }

                FieldLabel { text: qsTr("PORT") }
                AppTextField {
                    id: portField
                    Layout.fillWidth: true
                    text: "9180"
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 1; top: 65535 }
                    onEditingFinished: {
                        const p = parseInt(text, 10) || 9180
                        RemoteAccess.setPreferredPort(p)
                    }
                }

                Rectangle {
                    visible: lanModel.count > 0
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    radius: ThemeController.radius
                    color: dialog._cWell
                    implicitHeight: lanCol.implicitHeight
                        + Kirigami.Units.gridUnit

                    ColumnLayout {
                        id: lanCol
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                            margins: Kirigami.Units.smallSpacing * 2
                        }
                        spacing: 2

                        FieldLabel {
                            text: qsTr("CLIENTS CAN REACH THIS HOST AT")
                        }
                        Repeater {
                            model: lanModel
                            delegate: Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WrapAnywhere
                                font.family: "monospace"
                                color: Kirigami.Theme.activeTextColor
                                text: (tlsSwitch.checked ? "wss://" : "ws://")
                                    + model.addr + ":" + portField.text + "/ws"
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: tlsCol.implicitHeight
                + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: tlsCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.fillWidth: true

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 4
                        text: qsTr("TLS encryption")
                        font.family: ThemeController.fontFamily
                    }
                    Controls.Switch {
                        id: tlsSwitch
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                        checked: false
                        onToggled: {
                            RemoteAccess.setTlsEnabled(checked)
                            if (checked) {
                                RemoteAccess.ensureSelfSignedCert()
                                fingerprintLabel.text = RemoteAccess.certFingerprint()
                            } else {
                                fingerprintLabel.text = ""
                            }
                        }
                    }
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    text: tlsSwitch.checked
                        ? qsTr("Encrypted (wss://). Pin this fingerprint on each client.")
                        : qsTr("Unencrypted (ws://). Acceptable on a trusted LAN.")
                }

                Rectangle {
                    visible: tlsSwitch.checked && fingerprintLabel.text.length > 0
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    radius: ThemeController.radius
                    color: dialog._cWell
                    implicitHeight: fpCol.implicitHeight
                        + Kirigami.Units.gridUnit

                    ColumnLayout {
                        id: fpCol
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                            margins: Kirigami.Units.smallSpacing * 2
                        }
                        spacing: 2

                        FieldLabel { text: qsTr("SHA-256 FINGERPRINT") }
                        Controls.Label {
                            id: fingerprintLabel
                            Layout.fillWidth: true
                            wrapMode: Text.WrapAnywhere
                            font.family: "monospace"
                            font.pointSize: Kirigami.Theme.smallFont.pointSize
                            color: Kirigami.Theme.activeTextColor
                        }
                    }
                }

                AppButton {
                    visible: tlsSwitch.checked
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    text: qsTr("Regenerate certificate")
                    icon.name: "view-refresh"
                    onClicked: {
                        RemoteAccess.regenerateSelfSignedCert()
                        fingerprintLabel.text = RemoteAccess.certFingerprint()
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: autoLayout.implicitHeight
                + Kirigami.Units.gridUnit * 2

            RowLayout {
                id: autoLayout
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.gridUnit

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Auto-start at app launch")
                        font.bold: true
                        font.family: ThemeController.fontFamily
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Kirigami.Theme.disabledTextColor
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                        text: qsTr("Start the server automatically when Verzeta Studio opens.")
                    }
                }
                Controls.Switch {
                    id: autoSwitch
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                    checked: false
                    onToggled: RemoteAccess.setAutoStartEnabled(checked)
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: pairCol.implicitHeight
                + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: pairCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 4
                    text: qsTr("Pair a device")
                    font.family: ThemeController.fontFamily
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    text: qsTr("Generate a 6-digit code and enter it on the Android or VS Code client within 5 minutes.")
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    radius: ThemeController.radius
                    color: dialog._cWell
                    implicitHeight: Kirigami.Units.gridUnit * 4

                    Controls.Label {
                        anchors.centerIn: parent
                        text: dialog.lastPairCode.length > 0
                            ? dialog.lastPairCode
                            : qsTr(":::::: ")
                        font.family: "monospace"
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 2.0
                        font.bold: dialog.lastPairCode.length > 0
                        color: dialog.lastPairCode.length > 0
                            ? Kirigami.Theme.highlightColor
                            : Kirigami.Theme.disabledTextColor
                    }
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    text: qsTr("Generate pair code")
                    icon.name: "view-refresh"
                    highlighted: true
                    onClicked: {
                        const code = RemoteAccess.generatePairCode()
                        dialog.lastPairCode = code
                        dialog.statusText = (code && code.length > 0)
                            ? qsTr("Code valid for 5 minutes.")
                            : qsTr("Generation failed.")
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: ThemeController.radius
            color: dialog._cPanel
            antialiasing: true
            implicitHeight: clientsCol.implicitHeight
                + Kirigami.Units.gridUnit * 2

            ColumnLayout {
                id: clientsCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.gridUnit
                }
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.fillWidth: true

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 4
                        text: qsTr("Paired devices (%1)").arg(clientsModel.count)
                        font.family: ThemeController.fontFamily
                    }
                    AppButton {
                        text: qsTr("Refresh")
                        icon.name: "view-refresh"
                        flat: true
                        onClicked: dialog.refresh()
                    }
                }
                Controls.Label {
                    Layout.fillWidth: true
                    visible: clientsModel.count === 0 && revokedClientsModel.count === 0
                    wrapMode: Text.WordWrap
                    color: Kirigami.Theme.disabledTextColor
                    text: qsTr("No paired devices yet. Generate a pair code above to add the first.")
                }

                Repeater {
                    model: clientsModel
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: index === 0
                            ? Kirigami.Units.smallSpacing : 0
                        radius: ThemeController.radius
                        color: dialog._cWell
                        antialiasing: true
                        implicitHeight: clientRowCol.implicitHeight
                            + Kirigami.Units.gridUnit

                        ColumnLayout {
                            id: clientRowCol
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: parent.top
                                margins: Kirigami.Units.smallSpacing * 2
                            }
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                Kirigami.Icon {
                                    source: "smartphone"
                                    fallback: "user-identity"
                                    color: Kirigami.Theme.textColor
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                                }
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: model.name && model.name.length > 0
                                        ? model.name : qsTr("(unnamed)")
                                    font.bold: true
                                    font.family: ThemeController.fontFamily
                                    elide: Text.ElideRight
                                    wrapMode: Text.WrapAnywhere
                                }
                                AppButton {
                                    text: qsTr("Revoke")
                                    icon.name: "list-remove"
                                    flat: true
                                    onClicked: {
                                        const ok = RemoteAccess.revokeClient(model.client_id)
                                        dialog.statusText = ok
                                            ? qsTr("Revoked %1").arg(model.name)
                                            : qsTr("Revoke failed for %1").arg(model.name)
                                        dialog.refresh()
                                    }
                                }
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                Layout.leftMargin:
                                    Kirigami.Units.iconSizes.smallMedium
                                    + Kirigami.Units.smallSpacing
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                wrapMode: Text.WordWrap
                                text: {
                                    const last = Number(model.last_seen_at || 0)
                                    return last > 0
                                        ? qsTr("Last seen %1").arg(
                                            new Date(last).toLocaleString(Qt.locale(),
                                                                            Locale.ShortFormat))
                                        : qsTr("Never connected")
                                }
                            }
                        }
                    }
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    visible: revokedClientsModel.count > 0
                    flat: true
                    icon.name: "view-history"
                    text: revokedClientsModel.count === 1
                        ? qsTr("View 1 revoked device")
                        : qsTr("View %1 revoked devices").arg(revokedClientsModel.count)
                    onClicked: revokedDevicesSheet.open()
                }
            }
        }
    }
}

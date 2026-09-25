// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.verzeta.studio 1.0

RowLayout {
    id: root

    spacing: Kirigami.Units.smallSpacing

    Layout.maximumWidth: 380

    Controls.ComboBox {
        id: providerCombo

        model: AgentSettings.availableProviders
        textRole: "displayName"
        valueRole: "providerId"

        currentIndex: {
            for (var i = 0; i < model.length; ++i) {
                if (model[i].providerId === AgentSettings.activeProvider) {
                    return i;
                }
            }
            return 0;
        }

        onActivated: {
            const models = AgentSettings.modelsForProvider(currentValue);
            const firstModel = models.length > 0 ? models[0] : "";
            AgentSettings.setModel(currentValue, firstModel);
        }

        Layout.preferredWidth: 140
    }

    SearchableModelComboBox {
        id: modelPicker

        Layout.preferredWidth: 200
        Layout.maximumWidth: 220

        placeholderText: qsTr("Pick or type a model")

        property int _rev: 0

        models: {
            var r = modelPicker._rev;
            return AgentSettings.modelsForProvider(providerCombo.currentValue);
        }

        Component.onCompleted: currentValue = AgentSettings.activeModel

        Connections {
            target: AgentSettings
            function onActiveModelChanged() {
                if (modelPicker.currentValue !== AgentSettings.activeModel) {
                    modelPicker.currentValue = AgentSettings.activeModel;
                }
            }
            function onModelsRefreshed(providerId, models) {
                if (providerId === providerCombo.currentValue) {
                    modelPicker._rev++;
                }
            }
        }

        onCurrentValueChanged: {
            if (currentValue !== AgentSettings.activeModel && providerCombo.currentValue.length > 0) {
                AgentSettings.setModel(providerCombo.currentValue, currentValue);
            }
        }
    }
}

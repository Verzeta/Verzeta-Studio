// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file agent.cpp
 * @brief Implementation of Agent JSON serialisation helpers.
 * @layer Data Model
 * @dependencies Qt6::Core (QJsonObject, QJsonArray).
 */

#include "agent.h"

#include <QJsonArray>

QJsonObject Agent::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("name")] = name;
    obj[QStringLiteral("description")] = description;
    obj[QStringLiteral("iconName")] = iconName;
    obj[QStringLiteral("systemPrompt")] = systemPrompt;
    obj[QStringLiteral("defaultPattern")] = defaultPattern;
    obj[QStringLiteral("modelProvider")] = modelProvider;
    obj[QStringLiteral("modelName")] = modelName;
    obj[QStringLiteral("isBuiltIn")] = isBuiltIn;
    obj[QStringLiteral("isCoordinator")] = isCoordinator;
    obj[QStringLiteral("createdAt")] = createdAt.toMSecsSinceEpoch();

    QJsonArray toolsArr;
    for (const QString& t : allowedTools)
        toolsArr.append(t);
    obj[QStringLiteral("allowedTools")] = toolsArr;

    obj[QStringLiteral("defaultHeartbeatGoal")] = defaultHeartbeatGoal;
    obj[QStringLiteral("defaultHeartbeatSchedule")] = defaultHeartbeatSchedule;
    obj[QStringLiteral("defaultHeartbeatSurfaceCriteria")] = defaultHeartbeatSurfaceCriteria;

    return obj;
}

Agent Agent::fromJson(const QJsonObject& json) {
    Agent a;
    a.id = json[QStringLiteral("id")].toString();
    a.name = json[QStringLiteral("name")].toString();
    a.description = json[QStringLiteral("description")].toString();
    a.iconName = json[QStringLiteral("iconName")].toString();
    a.systemPrompt = json[QStringLiteral("systemPrompt")].toString();
    a.defaultPattern = json[QStringLiteral("defaultPattern")].toString(QStringLiteral("direct"));
    a.modelProvider = json[QStringLiteral("modelProvider")].toString();
    a.modelName = json[QStringLiteral("modelName")].toString();
    a.isBuiltIn = json[QStringLiteral("isBuiltIn")].toBool();
    a.isCoordinator = json[QStringLiteral("isCoordinator")].toBool();
    a.createdAt =
        QDateTime::fromMSecsSinceEpoch(json[QStringLiteral("createdAt")].toVariant().toLongLong());

    const QJsonArray toolsArr = json[QStringLiteral("allowedTools")].toArray();
    for (const QJsonValue& v : toolsArr) {
        a.allowedTools.append(v.toString());
    }

    a.defaultHeartbeatGoal = json[QStringLiteral("defaultHeartbeatGoal")].toString();
    a.defaultHeartbeatSchedule = json[QStringLiteral("defaultHeartbeatSchedule")].toString();
    a.defaultHeartbeatSurfaceCriteria =
        json[QStringLiteral("defaultHeartbeatSurfaceCriteria")].toString();

    return a;
}

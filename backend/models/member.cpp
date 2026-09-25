// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file member.cpp
 * @brief Implementation of the Member POD JSON serialisation helper.
 * @layer Data Access
 * @dependencies Qt6::Core (QJsonObject, QJsonArray).
 */

#include "member.h"

#include <QJsonArray>

QJsonObject Member::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("containerId")] = containerId;
    obj[QStringLiteral("agentId")] = agentId;
    obj[QStringLiteral("alias")] = alias;
    obj[QStringLiteral("isCoordinator")] = isCoordinator;
    obj[QStringLiteral("joinedAt")] = joinedAt.toMSecsSinceEpoch();
    obj[QStringLiteral("addedByKind")] = addedByKind;
    obj[QStringLiteral("addedByAgentId")] = addedByAgentId;
    obj[QStringLiteral("modelProvider")] = modelProvider;
    obj[QStringLiteral("modelName")] = modelName;
    obj[QStringLiteral("allowedTools")] = QJsonArray::fromStringList(allowedTools);
    obj[QStringLiteral("agentName")] = agentName;
    obj[QStringLiteral("agentDescription")] = agentDescription;
    obj[QStringLiteral("agentIconName")] = agentIconName;
    return obj;
}

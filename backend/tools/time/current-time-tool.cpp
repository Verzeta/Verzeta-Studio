// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file current-time-tool.cpp
 * @brief Implementation of Tools::CurrentTimeTool.
 * @layer Service (Tool subsystem)
 * @dependencies Qt6::Core (QDateTime).
 */

#include "current-time-tool.h"

#include <QDateTime>
#include <QJsonObject>

namespace Tools {

CurrentTimeTool::CurrentTimeTool() = default;

QString CurrentTimeTool::name() const {
    return QStringLiteral("get_current_time");
}

QString CurrentTimeTool::description() const {
    return QStringLiteral("Returns the current date and time in ISO 8601 format.");
}

QList<ToolParameterSchema> CurrentTimeTool::parameters() const {
    return {};
}

bool CurrentTimeTool::runsOnMainThread() const {
    return false;
}

QJsonValue CurrentTimeTool::invoke(const QJsonObject& /*args*/) {
    const QDateTime now = QDateTime::currentDateTime();
    QJsonObject result;
    result[QStringLiteral("datetime")] = now.toString(Qt::ISODate);
    result[QStringLiteral("timezone")] = now.timeZoneAbbreviation();
    return result;
}

}  // namespace Tools

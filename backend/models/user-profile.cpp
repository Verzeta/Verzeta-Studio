// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file user-profile.cpp
 * @brief JSON and SQL serialization for the UserProfile data model.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#include "user-profile.h"

#include <QSqlRecord>

/**
 * @brief Serializes this user profile to a JSON object.
 * @return QJsonObject with all profile fields.
 */
QJsonObject UserProfile::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("name")] = name;
    obj[QStringLiteral("avatar_path")] = avatarPath;
    obj[QStringLiteral("created_at")] = createdAt.toMSecsSinceEpoch();
    return obj;
}

/**
 * @brief Deserializes a user profile from a JSON object.
 * @param json JSON object with profile fields.
 * @return Populated UserProfile struct.
 */
UserProfile UserProfile::fromJson(const QJsonObject& json) {
    UserProfile p;
    p.id = json[QStringLiteral("id")].toString();
    p.name = json[QStringLiteral("name")].toString();
    p.avatarPath = json[QStringLiteral("avatar_path")].toString();
    p.createdAt = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(json[QStringLiteral("created_at")].toDouble()));
    return p;
}

/*
 * @brief Constructs a UserProfile from a QSqlRecord.
 * @param record SQL record with profile column values.
 * @return Populated UserProfile struct.
 */
UserProfile UserProfile::fromSqlRecord(const QSqlRecord& record) {
    UserProfile p;
    p.id = record.value(QStringLiteral("id")).toString();
    p.name = record.value(QStringLiteral("name")).toString();
    p.avatarPath = record.value(QStringLiteral("avatar_path")).toString();
    p.createdAt =
        QDateTime::fromMSecsSinceEpoch(record.value(QStringLiteral("created_at")).toLongLong());
    return p;
}

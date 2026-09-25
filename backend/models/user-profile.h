// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file user-profile.h
 * @brief Data model for user profile information.
 *        Stores display name and avatar path for personalized UI.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

class QSqlRecord;

/**
 * @brief Data model representing the local user's profile.
 *
 * Verzeta Studio is a single-user application; only one profile is expected.
 * The profile is stored in the settings table as JSON under key "user_profile".
 */
struct UserProfile {
    QString id;           ///< UUID v4 identifier
    QString name;         ///< Display name shown in the UI
    QString avatarPath;   ///< Relative path to avatar image in app data directory
    QDateTime createdAt;  ///< UTC creation timestamp

    /**
     * @brief Serializes this profile to a JSON object.
     * @return QJsonObject with all fields.
     */
    QJsonObject toJson() const;

    /**
     * @brief Deserializes a profile from a JSON object.
     * @param json JSON object with profile fields.
     * @return Populated UserProfile struct.
     */
    static UserProfile fromJson(const QJsonObject& json);

    /**
     * @brief Constructs a UserProfile from a QSqlRecord.
     *        Profile rows are stored as JSON in settings table, but this
     *        method is provided for completeness if a dedicated table is used.
     * @param record SQL record with profile fields.
     * @return Populated UserProfile struct.
     */
    static UserProfile fromSqlRecord(const QSqlRecord& record);

    /**
     * @brief Returns true if this profile has a valid ID and name.
     * @return true if id and name are non-empty.
     */
    bool isValid() const { return !id.isEmpty() && !name.isEmpty(); }
};

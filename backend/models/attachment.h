// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file attachment.h
 * @brief Data model for file attachments linked to messages.
 *        Supports images, audio, code files, and generic files.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

class QSqlRecord;

/**
 * @brief Data model representing a file attached to a message.
 *
 * Small attachments (< 256 KB) are stored inline in the `dataInline` field.
 * Larger files are stored on disk at a path relative to the app data directory,
 * with the relative path stored in `dataPath`.
 *
 * Types: "image" | "audio" | "file" | "code"
 */
struct Attachment {
    QString id;             ///< UUID v4 primary key
    QString messageId;      ///< References messages.id
    QString type;           ///< "image" | "audio" | "file" | "code"
    QString filename;       ///< Original filename shown to user
    QString mimeType;       ///< MIME type string (e.g., "image/png", "text/plain")
    QString dataPath;       ///< Relative path on disk (relative to AppDataLocation)
    QByteArray dataInline;  ///< Inline data for small files (< 256 KB)
    QDateTime createdAt;    ///< UTC creation timestamp

    /**
     * @brief Serializes this attachment to a JSON object.
     * @return QJsonObject; dataInline encoded as base64 string.
     */
    QJsonObject toJson() const;

    /**
     * @brief Deserializes an attachment from a JSON object.
     * @param json JSON object with attachment fields.
     * @return Populated Attachment struct.
     */
    static Attachment fromJson(const QJsonObject& json);

    /**
     * @brief Constructs an Attachment from a QSqlRecord row.
     * @param record SQL record from SELECT on the attachments table.
     * @return Populated Attachment struct.
     */
    static Attachment fromSqlRecord(const QSqlRecord& record);

    /**
     * @brief Returns true if this attachment has an ID and a valid type.
     * @return true if id is non-empty and type is recognized.
     */
    bool isValid() const;
};

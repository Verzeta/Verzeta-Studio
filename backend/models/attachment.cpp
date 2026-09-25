// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file attachment.cpp
 * @brief JSON and SQL serialization for the Attachment data model.
 * @layer Data Access
 * @dependencies Qt6::Core, Qt6::Sql
 */

#include "attachment.h"

#include <QSet>
#include <QSqlRecord>

/**
 * @brief Serializes this attachment to a JSON object.
 *        dataInline is base64-encoded for JSON transport.
 * @return QJsonObject with all attachment fields.
 */
QJsonObject Attachment::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("message_id")] = messageId;
    obj[QStringLiteral("type")] = type;
    obj[QStringLiteral("filename")] = filename;
    obj[QStringLiteral("mime_type")] = mimeType;
    obj[QStringLiteral("data_path")] = dataPath;
    obj[QStringLiteral("data_inline")] = QString::fromLatin1(dataInline.toBase64());
    obj[QStringLiteral("created_at")] = createdAt.toMSecsSinceEpoch();
    return obj;
}

/**
 * @brief Deserializes an attachment from a JSON object.
 * @param json JSON object with attachment fields.
 * @return Populated Attachment struct.
 */
Attachment Attachment::fromJson(const QJsonObject& json) {
    Attachment a;
    a.id = json[QStringLiteral("id")].toString();
    a.messageId = json[QStringLiteral("message_id")].toString();
    a.type = json[QStringLiteral("type")].toString();
    a.filename = json[QStringLiteral("filename")].toString();
    a.mimeType = json[QStringLiteral("mime_type")].toString();
    a.dataPath = json[QStringLiteral("data_path")].toString();
    a.dataInline =
        QByteArray::fromBase64(json[QStringLiteral("data_inline")].toString().toLatin1());
    a.createdAt = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(json[QStringLiteral("created_at")].toDouble()));
    return a;
}

/**
 * @brief Constructs an Attachment from a QSqlRecord row.
 * @param record SQL record from SELECT on the attachments table.
 * @return Populated Attachment struct.
 */
Attachment Attachment::fromSqlRecord(const QSqlRecord& record) {
    Attachment a;
    a.id = record.value(QStringLiteral("id")).toString();
    a.messageId = record.value(QStringLiteral("message_id")).toString();
    a.type = record.value(QStringLiteral("type")).toString();
    a.filename = record.value(QStringLiteral("filename")).toString();
    a.mimeType = record.value(QStringLiteral("mime_type")).toString();
    a.dataPath = record.value(QStringLiteral("data_path")).toString();
    a.dataInline = record.value(QStringLiteral("data_inline")).toByteArray();
    a.createdAt =
        QDateTime::fromMSecsSinceEpoch(record.value(QStringLiteral("created_at")).toLongLong());
    return a;
}

/**
 * @brief Returns true if this attachment has a valid ID and recognized type.
 * @return true if id is non-empty and type is one of the allowed values.
 */
bool Attachment::isValid() const {
    static const QSet<QString> validTypes = {QStringLiteral("image"),
                                             QStringLiteral("audio"),
                                             QStringLiteral("file"),
                                             QStringLiteral("code")};
    return !id.isEmpty() && validTypes.contains(type);
}

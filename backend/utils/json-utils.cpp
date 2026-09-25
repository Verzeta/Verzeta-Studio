// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file json-utils.cpp
 * @brief Implementation of JSON helper utilities.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#include "json-utils.h"

#include "logger.h"

#include <QJsonDocument>

namespace JsonUtils {

/*
 * @brief Serializes a QJsonObject to a JSON string.
 * @param obj JSON object to serialize.
 * @param compact If true, produces compact single-line JSON.
 * @return UTF-8 encoded JSON string.
 */
QString jsonToString(const QJsonObject& obj, bool compact) {
    QJsonDocument doc(obj);
    return QString::fromUtf8(
        doc.toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented));
}

/*
 * @brief Parses a JSON string into a QJsonObject.
 * @param str UTF-8 JSON string.
 * @return Parsed QJsonObject, or empty object on failure.
 */
QJsonObject stringToJson(const QString& str) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(str.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaUi) << "stringToJson parse error at offset" << err.offset << ":"
                             << err.errorString();
        return {};
    }
    return doc.object();
}

/*
 * @brief Merges two JSON objects; overlay values overwrite base on key conflicts.
 * @param base Base JSON object.
 * @param overlay JSON object whose values take precedence.
 * @return Merged QJsonObject.
 * @complexity O(n) where n is total key count.
 */
QJsonObject mergeJson(const QJsonObject& base, const QJsonObject& overlay) {
    QJsonObject result = base;
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        result[it.key()] = it.value();
    }
    return result;
}

/*
 * @brief Safely extracts a string field.
 * @param obj Source JSON object.
 * @param key Field name.
 * @param defaultValue Fallback if absent or wrong type.
 * @return String value or defaultValue.
 */
QString
extractStringField(const QJsonObject& obj, const QString& key, const QString& defaultValue) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->isString()) {
        return defaultValue;
    }
    return it->toString();
}

/*
 * @brief Safely extracts an integer field.
 * @param obj Source JSON object.
 * @param key Field name.
 * @param defaultValue Fallback if absent or wrong type.
 * @return Integer value or defaultValue.
 */
int extractIntField(const QJsonObject& obj, const QString& key, int defaultValue) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->isDouble()) {
        return defaultValue;
    }
    return it->toInt(defaultValue);
}

/*
 * @brief Safely extracts a double field.
 * @param obj Source JSON object.
 * @param key Field name.
 * @param defaultValue Fallback if absent or wrong type.
 * @return Double value or defaultValue.
 */
double extractDoubleField(const QJsonObject& obj, const QString& key, double defaultValue) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->isDouble()) {
        return defaultValue;
    }
    return it->toDouble(defaultValue);
}

/*
 * @brief Safely extracts a boolean field.
 * @param obj Source JSON object.
 * @param key Field name.
 * @param defaultValue Fallback if absent or wrong type.
 * @return Boolean value or defaultValue.
 */
bool extractBoolField(const QJsonObject& obj, const QString& key, bool defaultValue) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->isBool()) {
        return defaultValue;
    }
    return it->toBool(defaultValue);
}

}  // namespace JsonUtils

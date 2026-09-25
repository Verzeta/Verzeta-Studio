// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file json-utils.h
 * @brief Helper functions for QJsonDocument and QJsonObject manipulation.
 *        Provides safe field extraction with defaults and document merging.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#pragma once

#include <QJsonObject>
#include <QString>

/**
 * @brief Namespace for JSON helper utilities.
 *
 * All extraction functions return safe defaults rather than throwing on missing
 * or mistyped fields. This prevents crashes when parsing malformed provider
 * responses.
 */
namespace JsonUtils {

/**
 * @brief Serializes a QJsonObject to a JSON string.
 * @param obj The JSON object to serialize.
 * @param compact If true, produces compact single-line JSON (default pretty-print).
 * @return UTF-8 encoded JSON string.
 */
QString jsonToString(const QJsonObject& obj, bool compact = false);

/**
 * @brief Parses a JSON string into a QJsonObject.
 * @param str UTF-8 JSON string to parse.
 * @return Parsed QJsonObject, or empty object if parsing fails.
 * @sideeffects Logs a warning if the string is not valid JSON.
 */
QJsonObject stringToJson(const QString& str);

/**
 * @brief Merges two JSON objects. Overlay values overwrite base values for shared keys.
 * @param base The base JSON object.
 * @param overlay Values to overlay on top of base.
 * @return Merged QJsonObject containing all keys from both, overlay wins on conflicts.
 * @complexity O(n) where n is the total number of keys.
 */
QJsonObject mergeJson(const QJsonObject& base, const QJsonObject& overlay);

/**
 * @brief Safely extracts a string field from a JSON object.
 * @param obj The JSON object to read from.
 * @param key The field name.
 * @param defaultValue Value to return if field is absent or not a string.
 * @return The string value, or defaultValue.
 */
QString
extractStringField(const QJsonObject& obj, const QString& key, const QString& defaultValue = {});

/**
 * @brief Safely extracts an integer field from a JSON object.
 * @param obj The JSON object to read from.
 * @param key The field name.
 * @param defaultValue Value to return if field is absent or not numeric.
 * @return The integer value, or defaultValue.
 */
int extractIntField(const QJsonObject& obj, const QString& key, int defaultValue = 0);

/**
 * @brief Safely extracts a double field from a JSON object.
 * @param obj The JSON object to read from.
 * @param key The field name.
 * @param defaultValue Value to return if field is absent or not numeric.
 * @return The double value, or defaultValue.
 */
double extractDoubleField(const QJsonObject& obj, const QString& key, double defaultValue = 0.0);

/**
 * @brief Safely extracts a boolean field from a JSON object.
 * @param obj The JSON object to read from.
 * @param key The field name.
 * @param defaultValue Value to return if field is absent or not boolean.
 * @return The boolean value, or defaultValue.
 */
bool extractBoolField(const QJsonObject& obj, const QString& key, bool defaultValue = false);

}  // namespace JsonUtils

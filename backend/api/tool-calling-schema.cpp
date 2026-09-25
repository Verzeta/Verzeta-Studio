// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-calling-schema.cpp
 * @brief Serialization implementation for ToolSchema and ToolParameterSchema.
 *        Produces provider-specific JSON formats for LLM function calling.
 * @layer API
 * @dependencies Qt6::Core
 */


#include "tool-calling-schema.h"

#include <QJsonDocument>

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Builds shared JSON Schema properties and required arrays from parameters.
 * @param properties Output object: {name: {type, description, ...additional}}
 * @param required   Output array: [required_param_names...]
 */
void ToolSchema::buildJsonSchemaProps(QJsonObject& properties, QJsonArray& required) const {
    for (const ToolParameterSchema& param : parameters) {
        QJsonObject propObj;
        propObj[QStringLiteral("type")] = param.type;
        propObj[QStringLiteral("description")] = param.description;

        // Merge additional props (enum, items, nested properties)
        for (auto it = param.additionalProps.begin(); it != param.additionalProps.end(); ++it) {
            propObj[it.key()] = it.value();
        }

        properties[param.name] = propObj;

        if (param.required) {
            required.append(param.name);
        }
    }
}

// ---------------------------------------------------------------------------
// Serialization — OpenAI
// ---------------------------------------------------------------------------

/**
 * @brief Serializes to OpenAI function calling format.
 * @return {"type":"function","function":{"name":"...","description":"...",
 *          "parameters":{"type":"object","properties":{...},"required":[...]}}}
 */
QJsonObject ToolSchema::toOpenAIFunction() const {
    QJsonObject props;
    QJsonArray req;
    buildJsonSchemaProps(props, req);

    QJsonObject paramSchema;
    paramSchema[QStringLiteral("type")] = QStringLiteral("object");
    paramSchema[QStringLiteral("properties")] = props;
    if (!req.isEmpty()) {
        paramSchema[QStringLiteral("required")] = req;
    }

    QJsonObject funcObj;
    funcObj[QStringLiteral("name")] = name;
    funcObj[QStringLiteral("description")] = description;
    funcObj[QStringLiteral("parameters")] = paramSchema;

    QJsonObject root;
    root[QStringLiteral("type")] = QStringLiteral("function");
    root[QStringLiteral("function")] = funcObj;
    return root;
}

// ---------------------------------------------------------------------------
// Serialization — Anthropic
// ---------------------------------------------------------------------------

/**
 * @brief Serializes to Anthropic tool format.
 * @return {"name":"...","description":"...","input_schema":{"type":"object",...}}
 */
QJsonObject ToolSchema::toAnthropicTool() const {
    QJsonObject props;
    QJsonArray req;
    buildJsonSchemaProps(props, req);

    QJsonObject inputSchema;
    inputSchema[QStringLiteral("type")] = QStringLiteral("object");
    inputSchema[QStringLiteral("properties")] = props;
    if (!req.isEmpty()) {
        inputSchema[QStringLiteral("required")] = req;
    }

    QJsonObject root;
    root[QStringLiteral("name")] = name;
    root[QStringLiteral("description")] = description;
    root[QStringLiteral("input_schema")] = inputSchema;
    return root;
}

// ---------------------------------------------------------------------------
// Serialization — Gemini
// ---------------------------------------------------------------------------

/**
 * @brief Serializes to Gemini function declaration format.
 * @return {"name":"...","description":"...","parameters":{"type":"OBJECT",...}}
 *
 * Gemini uses uppercase type names: STRING, NUMBER, INTEGER, BOOLEAN, ARRAY, OBJECT.
 */
QJsonObject ToolSchema::toGeminiFunction() const {
    // Gemini uses all-caps type names
    auto geminiType = [](const QString& t) -> QString {
        const QString u = t.toUpper();
        // Map "integer" -> "INTEGER" etc.; Gemini uses "NUMBER" for both
        return u == QStringLiteral("INTEGER") ? QStringLiteral("INTEGER") : u;
    };

    QJsonObject props;
    for (const ToolParameterSchema& param : parameters) {
        QJsonObject propObj;
        propObj[QStringLiteral("type")] = geminiType(param.type);
        propObj[QStringLiteral("description")] = param.description;

        for (auto it = param.additionalProps.begin(); it != param.additionalProps.end(); ++it) {
            propObj[it.key()] = it.value();
        }

        props[param.name] = propObj;
    }

    // Required array (Gemini format)
    QJsonArray req;
    for (const ToolParameterSchema& p : parameters) {
        if (p.required) {
            req.append(p.name);
        }
    }

    QJsonObject paramObj;
    paramObj[QStringLiteral("type")] = QStringLiteral("OBJECT");
    paramObj[QStringLiteral("properties")] = props;
    if (!req.isEmpty()) {
        paramObj[QStringLiteral("required")] = req;
    }

    QJsonObject root;
    root[QStringLiteral("name")] = name;
    root[QStringLiteral("description")] = description;
    root[QStringLiteral("parameters")] = paramObj;
    return root;
}

// ---------------------------------------------------------------------------
// Deserialization
// ---------------------------------------------------------------------------

/*
 * @brief Deserializes a ToolSchema from a generic JSON object.
 * @param json Expected keys: "name" (string), "description" (string),
 *             "parameters" (array of {name, type, description, required}).
 * @return Populated ToolSchema. Returns empty-name schema on invalid input.
 */
ToolSchema ToolSchema::fromJson(const QJsonObject& json) {
    ToolSchema schema;
    schema.name = json[QStringLiteral("name")].toString();
    schema.description = json[QStringLiteral("description")].toString();

    const QJsonArray params = json[QStringLiteral("parameters")].toArray();
    for (const QJsonValue& pv : params) {
        const QJsonObject pObj = pv.toObject();
        ToolParameterSchema p;
        p.name = pObj[QStringLiteral("name")].toString();
        p.type = pObj[QStringLiteral("type")].toString(QStringLiteral("string"));
        p.description = pObj[QStringLiteral("description")].toString();
        p.required = pObj[QStringLiteral("required")].toBool(false);

        // Absorb any extra keys as additionalProps
        for (auto it = pObj.begin(); it != pObj.end(); ++it) {
            if (it.key() != QLatin1String("name") && it.key() != QLatin1String("type") &&
                it.key() != QLatin1String("description") && it.key() != QLatin1String("required")) {
                p.additionalProps[it.key()] = it.value();
            }
        }

        schema.parameters.append(p);
    }

    return schema;
}

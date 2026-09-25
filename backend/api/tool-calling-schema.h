// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-calling-schema.h
 * @brief Data types defining tool schemas for LLM function calling.
 *        Provides serialization to OpenAI, Anthropic, and Gemini formats.
 * @layer API
 * @dependencies Qt6::Core
 */


#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

/**
 * @brief Schema for a single parameter in a tool definition.
 *
 * Maps directly to a JSON Schema property object in the tool's parameter schema.
 */
struct ToolParameterSchema {
    QString name;  ///< Parameter name (e.g., "command")
    QString type;  ///< JSON Schema type: "string","number","integer","boolean","array","object"
    QString description;          ///< Human-readable description for the LLM
    bool required = false;        ///< Whether this parameter is mandatory
    QJsonObject additionalProps;  ///< Enum values, array item types, nested object properties
};

/**
 * @brief Conversation-capability scope of a tool.
 *
 * Determines whether a tool's schema is offered to the LLM for a given
 * conversation. A tool declares its own scope (mirroring the
 * `runsOnMainThread()` self-declaration); RequestBuilder filters the
 * offered set by the conversation's actual capability so a single-agent
 * chat is not handed group/project-only tools it can never use (which
 * otherwise waste a large share of the context window on dead schemas).
 *
 * The filter ONLY narrows: a group chat keeps its GroupOnly tools, a
 * project/org chat keeps its ProjectOnly tools; nothing is granted.
 */
enum class ToolScope {
    Universal,   ///< Offered in every conversation (default).
    GroupOnly,   ///< Only when the conversation is a group chat (cascade / polls).
    ProjectOnly  ///< Only when the conversation lives in a project/org folder (membership).
};

/**
 * @brief Complete schema defining a callable tool exposed to the LLM.
 *
 * A ToolSchema can be serialized to the three major LLM provider formats:
 * - OpenAI: `{"type":"function","function":{...}}`
 * - Anthropic: `{"name":"...","description":"...","input_schema":{...}}`
 * - Gemini: `{"name":"...","description":"...","parameters":{"type":"OBJECT",...}}`
 *
 * `scope` is host-side metadata only. It gates whether the schema is
 * offered to the LLM and is never serialized to any provider.
 */
struct ToolSchema {
    QString name;         ///< Unique tool identifier (e.g., "run_shell")
    QString description;  ///< Description used by the LLM to decide when to invoke
    QList<ToolParameterSchema> parameters;   ///< Ordered parameter definitions
    ToolScope scope = ToolScope::Universal;  ///< Conversation-capability gate

    /**
     * @brief Serializes to OpenAI function calling format.
     * @return JSON object:
     *   {"type":"function","function":{"name":"...","description":"...",
     *    "parameters":{"type":"object","properties":{...},"required":[...]}}}
     * @complexity O(p) where p is parameter count.
     */
    QJsonObject toOpenAIFunction() const;

    /**
     * @brief Serializes to Anthropic tool format.
     * @return JSON object:
     *   {"name":"...","description":"...","input_schema":{"type":"object","properties":{...},"required":[...]}}
     * @complexity O(p) where p is parameter count.
     */
    QJsonObject toAnthropicTool() const;

    /**
     * @brief Serializes to Gemini function declaration format.
     * @return JSON object:
     *   {"name":"...","description":"...","parameters":{"type":"OBJECT","properties":{...},"required":[...]}}
     * @complexity O(p) where p is parameter count.
     */
    QJsonObject toGeminiFunction() const;

    /**
     * @brief Deserializes a ToolSchema from a generic JSON object.
     * @param json JSON object with "name" (string), "description" (string)
     *             and an optional "parameters" array whose entries carry
     *             "name", "type" (default "string"), "description" and
     *             "required" (default false).
     * @return Populated ToolSchema. Empty name signals an invalid/unrecognized schema.
     * @complexity O(p) where p is parameter count.
     */
    static ToolSchema fromJson(const QJsonObject& json);

  private:
    /**
     * @brief Builds the shared JSON Schema properties+required objects used by all formats.
     * @param properties Output: {"paramName": {"type":"...","description":"..."}, ...}
     * @param required   Output: ["param1", "param2", ...]
     */
    void buildJsonSchemaProps(QJsonObject& properties, QJsonArray& required) const;
};

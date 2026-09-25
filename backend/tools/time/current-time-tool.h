// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file current-time-tool.h
 * @brief `get_current_time` tool: returns the current local-time
 *        timestamp formatted as ISO-8601 along with the system
 *        timezone abbreviation.
 * @layer Service (Tool subsystem)
 * @dependencies backend/tools/itool.h.
 *
 * Stateless; no external dependencies beyond Qt's clock. Safe to
 * instantiate as a singleton-like shared resource.
 *
 * Threading: runsOnMainThread() returns false. QDateTime::currentDateTime()
 * is thread-safe.
 */
#pragma once

#include "../itool.h"

namespace Tools {

/**
 * @brief ITool implementation for the `get_current_time` built-in tool.
 */
class CurrentTimeTool : public ITool {
  public:
    /**
     * @brief Constructs the stateless tool.
     */
    CurrentTimeTool();

    /**
     * @brief Canonical tool name.
     * @returns "get_current_time".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the current-time behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Empty list, because the tool takes no parameters.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns false, because QDateTime::currentDateTime is thread-safe.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Produce the current local time.
     * @param args Ignored; the tool takes no parameters.
     * @return JSON object with:
     *           - "datetime": ISO-8601 local-time timestamp
     *                         (QDateTime::currentDateTime(), Qt::ISODate)
     *           - "timezone": Timezone abbreviation reported by
     *                         QDateTime::timeZoneAbbreviation()
     *                         (e.g. "UTC", "PDT", "CET")
     * @complexity O(1).
     * @sideeffects None.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

}  // namespace Tools

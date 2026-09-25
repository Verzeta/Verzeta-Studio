// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file get-skill-status-tool.cpp
 * @brief Implementation of the `get_skill_status` tool body.
 *
 *        Returns the readiness diagnostic for a given skill id:
 *
 *            {
 *              "skill_id":        "...",
 *              "ready":           true/false,
 *              "declared_tools":  ["run_shell", "read_file"],
 *              "available_tools": ["read_file", "write_file", ...],
 *              "missing_tools":   ["run_shell"],
 *              "approved":        true/false,
 *              "installed":       true/false
 *            }
 *
 *        `available_tools` is filtered to the intersection with the
 *        skill's declared list so the response stays small even when
 *        the registry has 30+ tools registered. Callers wanting the
 *        full registry can use existing introspection paths.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, ToolService.
 */


#include "get-skill-status-tool.h"

#include "../../api/tool-calling-schema.h"
#include "../../models/skill.h"
#include "../../services/skill-service.h"
#include "../../services/tool-service.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace Tools {

QString GetSkillStatusTool::name() const {
    return QStringLiteral("get_skill_status");
}

QString GetSkillStatusTool::description() const {
    return QStringLiteral("Check whether a skill is ready to execute on the current "
                          "platform by comparing its declared_tools frontmatter to the "
                          "actual registered tool list. Returns ready=true when every "
                          "declared tool is available, or ready=false with a "
                          "missing_tools list otherwise. Use this BEFORE picking a "
                          "skill whose AVAILABLE SKILLS entry is tagged "
                          "[BLOCKED: missing ...] to confirm what would have to change "
                          "for it to run.");
}

QList<ToolParameterSchema> GetSkillStatusTool::parameters() const {
    ToolParameterSchema sid;
    sid.name = QStringLiteral("skill_id");
    sid.type = QStringLiteral("string");
    sid.description = QStringLiteral("The skill id (matches the id field returned by "
                                     "discover_skills or shown in your AVAILABLE SKILLS prompt "
                                     "layer).");
    sid.required = true;
    return {sid};
}

namespace {

QJsonObject makeError(const QString& msg) {
    QJsonObject o;
    o.insert(QStringLiteral("error"), msg);
    return o;
}

}  // namespace

QJsonValue GetSkillStatusTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("skill tool deps not configured"));
    }
    if (!m_deps.tools) {
        return makeError(QStringLiteral("tool registry not available — get_skill_status requires "
                                        "the ToolService dependency"));
    }

    const QString skillId = args.value(QStringLiteral("skill_id")).toString().trimmed();
    if (skillId.isEmpty()) {
        return makeError(QStringLiteral("skill_id required"));
    }

    const Skill s = m_deps.skills->skillById(skillId);
    const bool installed = s.isValid();
    const bool approved = installed && s.isApproved();

    // Build the available-tool name set once.
    QSet<QString> availableNames;
    const QList<ToolSchema> registered = m_deps.tools->availableTools();
    for (const ToolSchema& t : registered) {
        availableNames.insert(t.name);
    }

    QJsonArray declaredArr;
    QJsonArray missingArr;
    QJsonArray intersectArr;  // declared ∩ available
    if (installed) {
        for (const QString& declared : s.declaredTools) {
            declaredArr.append(declared);
            if (availableNames.contains(declared)) {
                intersectArr.append(declared);
            } else {
                missingArr.append(declared);
            }
        }
    }

    // ready iff: skill is installed, approved, and every declared
    // tool is present. A skill with NO declared_tools is trivially
    // ready (declaredTools.empty() → missing.empty()).
    const bool ready = installed && approved && missingArr.isEmpty();

    QJsonObject out;
    out.insert(QStringLiteral("skill_id"), skillId);
    out.insert(QStringLiteral("installed"), installed);
    out.insert(QStringLiteral("approved"), approved);
    out.insert(QStringLiteral("ready"), ready);
    out.insert(QStringLiteral("declared_tools"), declaredArr);
    out.insert(QStringLiteral("missing_tools"), missingArr);
    out.insert(QStringLiteral("available_tools"), intersectArr);
    return out;
}

}  // namespace Tools

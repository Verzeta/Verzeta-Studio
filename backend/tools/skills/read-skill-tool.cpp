// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-skill-tool.cpp
 * @brief Implementation of the `read_skill` tool body.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, ConversationService, Chat::CascadeController.
 */

#include "read-skill-tool.h"

#include "../../models/conversation.h"
#include "../../services/chat/cascade-controller.h"
#include "../../services/conversation-service.h"
#include "../../services/skill-service.h"

#include <QFile>

namespace Tools {

namespace {
constexpr int kLlmReturnCapBytes = 32 * 1024;  // LLM return cap
}

QString ReadSkillTool::name() const {
    return QStringLiteral("read_skill");
}

QString ReadSkillTool::description() const {
    return QStringLiteral("Read the full SKILL.md instructions for an approved skill. "
                          "Returns the markdown text capped at 32 KiB. Use this AFTER "
                          "discover_skills or to expand on a skill in your AVAILABLE "
                          "SKILLS list — never blindly call this on every turn.");
}

QList<ToolParameterSchema> ReadSkillTool::parameters() const {
    ToolParameterSchema id;
    id.name = QStringLiteral("skill_id");
    id.type = QStringLiteral("string");
    id.description =
        QStringLiteral("The skill id (matches the `id` field returned by discover_skills "
                       "or shown in your AVAILABLE SKILLS prompt layer).");
    id.required = true;
    return {id};
}

QJsonValue ReadSkillTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("skill tool deps not configured")}};
    }
    // Args-injected conv id wins over the captured-LOCAL getter so
    // wire-side per-client cascades resolve skill scope against
    // THEIR conversation's folder / project, not the LOCAL CC's.
    QString activeConv = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (activeConv.isEmpty())
        activeConv = m_deps.activeConvIdGetter();
    if (activeConv.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("no active responder context")}};
    }
    const QString skillId = args.value(QStringLiteral("skill_id")).toString();
    if (skillId.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("skill_id required")}};
    }

    const Skill s = m_deps.skills->skillById(skillId);
    if (!s.isValid()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("skill not installed")}};
    }
    if (!s.isApproved()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("skill is not approved")}};
    }

    // Scope-aware gate: when exposeOnly, the skill must be in the
    // preferred list.
    const auto resolved = m_deps.skills->resolveForConversation(activeConv);
    if (resolved.exposeOnly && !resolved.preferredSkillIds.contains(skillId)) {
        return QJsonObject{{QStringLiteral("error"),
                            QStringLiteral("skill not in preferred list (exposeOnly active)")}};
    }

    const QString content = m_deps.skills->readSkillFile(skillId, QStringLiteral("SKILL.md"));
    if (content.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("could not read SKILL.md")}};
    }

    QString out = content;
    bool truncated = false;
    if (out.toUtf8().size() > kLlmReturnCapBytes) {
        // Truncate by bytes, then convert back. Simpler: clamp by char
        // count at ratio of bytes/chars.
        out = out.left(kLlmReturnCapBytes);
        truncated = true;
        out += QStringLiteral("\n[truncated: > 32 KiB]");
    }
    // Frame the SKILL.md as an authoritative procedure rather than reference
    // text. Without this the model tends to skim the file, lift one detail
    // (a URL) and stop; the directive tells it to execute the steps in order.
    // Prepended to `content` (not a separate field) because that is the field
    // models actually read from a tool result.
    const QString framed =
        QStringLiteral("The following are the authoritative steps for skill '%1'. "
                       "Follow them in order and do not stop until each step is done or "
                       "you are blocked. Do not cherry-pick or summarise. Execute the "
                       "procedure as written.\n\n")
            .arg(skillId) +
        out;
    QJsonObject result;
    result.insert(QStringLiteral("skill_id"), skillId);
    result.insert(QStringLiteral("content"), framed);
    result.insert(QStringLiteral("truncated"), truncated);
    return result;
}

}  // namespace Tools

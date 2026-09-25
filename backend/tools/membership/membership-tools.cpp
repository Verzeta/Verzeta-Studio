// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file membership-tools.cpp
 * @brief Implementations of the four membership tools.
 *
 *        The coordinator gate, alias-uniqueness, and the
 *        "cannot remove the last coordinator" invariant are enforced
 *        inside each tool's invoke() body.
 * @layer Service (Tool subsystem)
 * @dependencies MembershipService, AgentRegistry, ConversationService,
 *               Chat::CascadeController.
 */


#include "membership-tools.h"

#include "../../models/agent.h"
#include "../../models/conversation.h"
#include "../../models/member.h"
#include "../../services/agent-registry.h"
#include "../../services/chat/cascade-controller.h"
#include "../../services/conversation-service.h"
#include "../../services/membership-service.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace Tools {

namespace {

QJsonObject makeError(const QString& message) {
    QJsonObject e;
    e[QStringLiteral("error")] = message;
    return e;
}

QJsonObject memberToJson(const Member& m) {
    QJsonObject obj;
    obj[QStringLiteral("alias")] = m.alias;
    obj[QStringLiteral("agent_id")] = m.agentId;
    obj[QStringLiteral("agent_name")] = m.agentName;
    obj[QStringLiteral("is_coordinator")] = m.isCoordinator;
    obj[QStringLiteral("added_by_kind")] = m.addedByKind;
    obj[QStringLiteral("added_by_agent_id")] = m.addedByAgentId;
    obj[QStringLiteral("removable_by_agent")] = (m.addedByKind == QStringLiteral("agent"));
    return obj;
}

}  // namespace

// ---------------------------------------------------------------------------
// MembershipToolBase
// ---------------------------------------------------------------------------

MembershipToolBase::MembershipToolBase(const MembershipToolDeps& deps) : m_deps(deps) {}

bool MembershipToolBase::runsOnMainThread() const {
    // All four tools touch MembershipService / AgentRegistry /
    // ConversationService — every one is a SQLite-backed main-thread
    // service. Inline dispatch is mandatory.
    return true;
}

QJsonValue MembershipToolBase::resolveProjectContext(const QJsonObject& args,
                                                     QString& outFolderId,
                                                     QString& outCallerAgentId,
                                                     QString& outCallerAlias) {
    outFolderId.clear();
    outCallerAgentId.clear();
    outCallerAlias.clear();

    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("Membership tools are unavailable in this build."));
    }

    // Args-injected caller identity wins over the captured-LOCAL
    // CascadeController pointer.  Wire-side per-client cascades supply
    // their own responder identity via __caller_agent_{id,alias};
    // direct callers / tests that bypass ToolService fall back to the
    // cascade pointer.
    outCallerAgentId = args.value(QStringLiteral("__caller_agent_id")).toString();
    outCallerAlias = args.value(QStringLiteral("__caller_agent_alias")).toString();
    if (outCallerAgentId.isEmpty()) {
        outCallerAgentId = m_deps.cascade->currentResponderAgentId();
    }
    if (outCallerAlias.isEmpty()) {
        outCallerAlias = m_deps.cascade->currentResponderAlias();
    }
    if (outCallerAgentId.isEmpty()) {
        return makeError(QStringLiteral("Membership tools can only be called from inside a cascade "
                                        "turn; no responder identity is currently set."));
    }

    // Args-injected conv id wins over the captured-LOCAL getter
    // (same per-client routing reason).
    QString convId = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (convId.isEmpty() && m_deps.activeConvIdGetter) {
        convId = m_deps.activeConvIdGetter();
    }
    if (convId.isEmpty()) {
        return makeError(QStringLiteral("Membership tools require an active conversation."));
    }

    const auto convOpt = m_deps.convs->getConversation(convId);
    if (!convOpt) {
        return makeError(QStringLiteral("Active conversation no longer exists."));
    }
    if (convOpt->folderId.isEmpty()) {
        return makeError(QStringLiteral("Active conversation is not in any project folder. "
                                        "Membership tools only operate on conversations inside a "
                                        "project or organization."));
    }

    const auto folderOpt = m_deps.convs->getFolder(convOpt->folderId);
    if (!folderOpt || !folderOpt->isValid()) {
        return makeError(QStringLiteral("Active conversation's folder no longer exists."));
    }
    if (!folderOpt->isProject()) {
        return makeError(QStringLiteral("Active folder is not a project or organization. "
                                        "Membership tools only operate on project / organization "
                                        "folders."));
    }

    outFolderId = folderOpt->id;
    return QJsonValue();  // empty == success
}

QString MembershipToolBase::callerConversationId(const QJsonObject& args) const {
    QString convId = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (convId.isEmpty() && m_deps.activeConvIdGetter) {
        convId = m_deps.activeConvIdGetter();
    }
    return convId;
}

QJsonValue MembershipToolBase::resolveCoordinatorContext(const QJsonObject& args,
                                                         QString& outFolderId,
                                                         QString& outCallerAgentId,
                                                         QString& outCallerAlias) {
    const QJsonValue err =
        resolveProjectContext(args, outFolderId, outCallerAgentId, outCallerAlias);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }

    // Find the caller's membership row. Per-project coordinator status
    // is the authoritative check — Agent::isCoordinator is the
    // registry-level default but not the authorization gate.
    const QList<Member> roster = m_deps.members->projectMembers(outFolderId);
    bool foundSelf = false;
    bool selfIsCoordinator = false;
    for (const Member& m : roster) {
        if (m.agentId == outCallerAgentId && m.alias == outCallerAlias) {
            foundSelf = true;
            selfIsCoordinator = m.isCoordinator;
            break;
        }
    }
    if (!foundSelf) {
        // Fall back to agent-id-only match in case the alias was
        // mutated mid-cascade (defensive — should not happen in
        // practice but the cost is one extra pass through the roster).
        for (const Member& m : roster) {
            if (m.agentId == outCallerAgentId) {
                foundSelf = true;
                selfIsCoordinator = m.isCoordinator;
                break;
            }
        }
    }
    if (!foundSelf) {
        return makeError(QStringLiteral("Caller is not a member of this project — only project "
                                        "members can manage membership."));
    }
    if (!selfIsCoordinator) {
        return makeError(QStringLiteral("Only coordinators can manage project membership. Ask the "
                                        "project's coordinator to make the change."));
    }
    return QJsonValue();  // empty == success
}

// ---------------------------------------------------------------------------
// add_project_member
// ---------------------------------------------------------------------------

QString AddProjectMemberTool::name() const {
    return QStringLiteral("add_project_member");
}

QString AddProjectMemberTool::description() const {
    return QStringLiteral("Add a new agent to the active project's team. ONLY coordinators "
                          "can use this tool. Specify the new member's alias (unique within "
                          "the project) and the agent template by id OR name. Optional "
                          "is_coordinator promotes the new member to coordinator role "
                          "(rare). When called from a GROUP conversation the new member "
                          "also JOINS that conversation and can be @-mentioned immediately "
                          "(the result reports added_to_conversation). Call "
                          "list_agent_templates first to see available templates; call "
                          "list_project_members to confirm the alias is free.");
}

QList<ToolParameterSchema> AddProjectMemberTool::parameters() const {
    ToolParameterSchema alias;
    alias.name = QStringLiteral("alias");
    alias.type = QStringLiteral("string");
    alias.description =
        QStringLiteral("The @mention alias for the new member. Must be non-empty and "
                       "unique within this project. Spaces are converted to "
                       "underscores for @mention use.");
    alias.required = true;

    ToolParameterSchema agentId;
    agentId.name = QStringLiteral("agent_id");
    agentId.type = QStringLiteral("string");
    agentId.description =
        QStringLiteral("Agent template UUID. Either agent_id OR agent_name must be "
                       "supplied — not both. Get ids from list_agent_templates.");
    agentId.required = false;

    ToolParameterSchema agentName;
    agentName.name = QStringLiteral("agent_name");
    agentName.type = QStringLiteral("string");
    agentName.description =
        QStringLiteral("Agent template name (case-sensitive, e.g. \"Visual Designer\"). "
                       "Used when you have the human-readable template name but not the "
                       "id. Either agent_id OR agent_name must be supplied.");
    agentName.required = false;

    ToolParameterSchema isCoord;
    isCoord.name = QStringLiteral("is_coordinator");
    isCoord.type = QStringLiteral("boolean");
    isCoord.description =
        QStringLiteral("Whether the new member should be a coordinator (default false). "
                       "Use sparingly — most projects have one coordinator.");
    isCoord.required = false;

    return {alias, agentId, agentName, isCoord};
}

QJsonValue AddProjectMemberTool::invoke(const QJsonObject& args) {
    QString folderId, callerAgentId, callerAlias;
    const QJsonValue err = resolveCoordinatorContext(args, folderId, callerAgentId, callerAlias);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }

    // LLMs routinely pass the alias WITH its @ sigil ("@Engineer") —
    // normalize at the boundary so the duplicate check below, the
    // stored rows, and the reported alias all use the canonical
    // sigil-free form (the service normalizes on write anyway; doing it
    // here too keeps the friendly duplicate error instead of a raw
    // UNIQUE-constraint failure).
    const QString newAlias =
        MembershipService::normalizedAlias(args.value(QStringLiteral("alias")).toString());
    const QString suppliedAgentId = args.value(QStringLiteral("agent_id")).toString().trimmed();
    const QString suppliedAgentName = args.value(QStringLiteral("agent_name")).toString().trimmed();
    const bool wantsCoordinator = args.value(QStringLiteral("is_coordinator")).toBool(false);

    if (newAlias.isEmpty()) {
        return makeError(QStringLiteral("alias is required and non-empty."));
    }
    if (suppliedAgentId.isEmpty() && suppliedAgentName.isEmpty()) {
        return makeError(QStringLiteral("One of agent_id or agent_name is required."));
    }
    if (!suppliedAgentId.isEmpty() && !suppliedAgentName.isEmpty()) {
        return makeError(QStringLiteral("Specify either agent_id or agent_name, not both."));
    }

    // Resolve the agent template.
    Agent picked;
    if (!suppliedAgentId.isEmpty()) {
        picked = m_deps.agents->getAgent(suppliedAgentId);
        if (!picked.isValid()) {
            return makeError(
                QStringLiteral("No agent template with id \"%1\".").arg(suppliedAgentId));
        }
    } else {
        picked = m_deps.agents->getAgentByName(suppliedAgentName);
        if (!picked.isValid()) {
            return makeError(
                QStringLiteral("No agent template named \"%1\". Call list_agent_templates "
                               "to enumerate available templates.")
                    .arg(suppliedAgentName));
        }
    }

    // Alias uniqueness — MembershipService::addProjectMember relies on
    // the (folder_id, alias) UNIQUE constraint but the error path is a
    // generic false return. Pre-check so we can return a specific
    // message the model can act on.
    const QList<Member> roster = m_deps.members->projectMembers(folderId);
    for (const Member& m : roster) {
        if (m.alias.compare(newAlias, Qt::CaseInsensitive) == 0) {
            return makeError(
                QStringLiteral("Alias \"%1\" is already taken in this project by agent "
                               "\"%2\". Pick a different alias.")
                    .arg(newAlias, m.agentName));
        }
    }

    const bool ok = m_deps.members->addProjectMember(
        folderId, picked.id, newAlias, wantsCoordinator, QStringLiteral("agent"), callerAgentId);
    if (!ok) {
        return makeError(QStringLiteral("Failed to add member — database write rejected the row."));
    }

    bool addedToConversation = false;
    QString conversationNote;
    const QString convId = callerConversationId(args);
    std::optional<Conversation> convOpt;
    if (!convId.isEmpty()) {
        convOpt = m_deps.convs->getConversation(convId);
    }
    const bool callerIsGroupConv = convOpt.has_value() && convOpt->isGroup;
    if (callerIsGroupConv) {
        bool aliasTakenInConv = false;
        const QList<Member> convRoster = m_deps.members->conversationMembers(convId);
        for (const Member& m : convRoster) {
            if (m.alias.compare(newAlias, Qt::CaseInsensitive) == 0) {
                aliasTakenInConv = true;
                break;
            }
        }
        if (aliasTakenInConv) {
            conversationNote =
                QStringLiteral("Alias \"%1\" is already present in this group "
                               "conversation — the project roster was updated, but no "
                               "conversation member was added.")
                    .arg(newAlias);
        } else if (m_deps.members->addConversationMember(convId,
                                                         picked.id,
                                                         newAlias,
                                                         wantsCoordinator,
                                                         QStringLiteral("agent"),
                                                         callerAgentId)) {
            addedToConversation = true;
        } else {
            conversationNote =
                QStringLiteral("The project roster was updated, but joining this group "
                               "conversation FAILED (database write rejected the row). "
                               "The new member will NOT receive turns here until added "
                               "via the conversation's member editor.");
        }
    }

    QJsonObject result;
    result[QStringLiteral("status")] = (callerIsGroupConv && !addedToConversation)
                                           ? QStringLiteral("partial")
                                           : QStringLiteral("ok");
    result[QStringLiteral("folder_id")] = folderId;
    result[QStringLiteral("alias")] = newAlias;
    result[QStringLiteral("agent_id")] = picked.id;
    result[QStringLiteral("agent_name")] = picked.name;
    result[QStringLiteral("is_coordinator")] = wantsCoordinator;
    result[QStringLiteral("added_by_kind")] = QStringLiteral("agent");
    result[QStringLiteral("added_by_agent_id")] = callerAgentId;
    result[QStringLiteral("added_to_conversation")] = addedToConversation;
    if (addedToConversation) {
        result[QStringLiteral("conversation_id")] = convId;
    }
    if (!conversationNote.isEmpty()) {
        result[QStringLiteral("note")] = conversationNote;
    }
    return result;
}

// ---------------------------------------------------------------------------
// remove_project_member
// ---------------------------------------------------------------------------

QString RemoveProjectMemberTool::name() const {
    return QStringLiteral("remove_project_member");
}

QString RemoveProjectMemberTool::description() const {
    return QStringLiteral("Remove a member from the active project. RESTRICTIONS: (1) Only "
                          "coordinators can use this tool. (2) You can ONLY remove members "
                          "that were added by an agent — members added by the user are "
                          "protected and can only be removed by the user via the project "
                          "settings panel. Check `removable_by_agent` in "
                          "list_project_members to see which rows you may remove. (3) You "
                          "cannot remove yourself. (4) You cannot remove the last "
                          "coordinator. Conversations of the removed agent stay intact; "
                          "they just no longer appear in new group chats for this project.");
}

QList<ToolParameterSchema> RemoveProjectMemberTool::parameters() const {
    ToolParameterSchema alias;
    alias.name = QStringLiteral("alias");
    alias.type = QStringLiteral("string");
    alias.description = QStringLiteral("The @mention alias of the member to remove.");
    alias.required = true;
    return {alias};
}

QJsonValue RemoveProjectMemberTool::invoke(const QJsonObject& args) {
    QString folderId, callerAgentId, callerAlias;
    const QJsonValue err = resolveCoordinatorContext(args, folderId, callerAgentId, callerAlias);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }

    const QString targetAlias = args.value(QStringLiteral("alias")).toString().trimmed();
    if (targetAlias.isEmpty()) {
        return makeError(QStringLiteral("alias is required and non-empty."));
    }

    // Self-removal block.
    if (targetAlias.compare(callerAlias, Qt::CaseInsensitive) == 0) {
        return makeError(QStringLiteral("Coordinators cannot remove themselves. Ask another "
                                        "coordinator or the user to do it."));
    }

    // Locate the target + verify last-coordinator invariant.
    const QList<Member> roster = m_deps.members->projectMembers(folderId);
    const Member* target = nullptr;
    int coordinatorCount = 0;
    for (const Member& m : roster) {
        if (m.alias.compare(targetAlias, Qt::CaseInsensitive) == 0) {
            target = &m;
        }
        if (m.isCoordinator)
            ++coordinatorCount;
    }
    if (!target) {
        return makeError(
            QStringLiteral("No member with alias \"%1\" in this project.").arg(targetAlias));
    }

    if (target->addedByKind != QStringLiteral("agent")) {
        return makeError(QStringLiteral("Member \"%1\" was added by the user and cannot be removed "
                                        "by an agent. Ask the user to remove this member via the "
                                        "project settings panel (e.g. by @-mentioning them and "
                                        "explaining why removal is needed).")
                             .arg(target->alias));
    }

    if (target->isCoordinator && coordinatorCount <= 1) {
        return makeError(QStringLiteral("Cannot remove the last coordinator from a project — "
                                        "promote another member to coordinator first."));
    }

    const bool ok = m_deps.members->removeProjectMember(folderId, target->alias);
    if (!ok) {
        return makeError(
            QStringLiteral("Failed to remove member — database write rejected the row."));
    }

    QJsonObject result;
    result[QStringLiteral("status")] = QStringLiteral("ok");
    result[QStringLiteral("folder_id")] = folderId;
    result[QStringLiteral("alias")] = target->alias;
    return result;
}

// ---------------------------------------------------------------------------
// list_project_members
// ---------------------------------------------------------------------------

QString ListProjectMembersTool::name() const {
    return QStringLiteral("list_project_members");
}

QString ListProjectMembersTool::description() const {
    return QStringLiteral("List the members of the active project. Returns each member's "
                          "alias, agent template name, agent id, and coordinator flag. "
                          "Any agent in the project may call this tool — it is read-only. "
                          "Useful before adding or removing members, or to check who's on "
                          "the team.");
}

QList<ToolParameterSchema> ListProjectMembersTool::parameters() const {
    return {};  // no parameters
}

QJsonValue ListProjectMembersTool::invoke(const QJsonObject& args) {
    QString folderId, callerAgentId, callerAlias;
    const QJsonValue err = resolveProjectContext(args, folderId, callerAgentId, callerAlias);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }

    const QList<Member> roster = m_deps.members->projectMembers(folderId);
    QJsonArray rows;
    for (const Member& m : roster) {
        rows.append(memberToJson(m));
    }
    QJsonObject result;
    result[QStringLiteral("folder_id")] = folderId;
    result[QStringLiteral("members")] = rows;
    result[QStringLiteral("count")] = static_cast<int>(rows.size());
    return result;
}

// ---------------------------------------------------------------------------
// list_agent_templates
// ---------------------------------------------------------------------------

ListAgentTemplatesTool::ListAgentTemplatesTool(const MembershipToolDeps& deps) : m_deps(deps) {}

QString ListAgentTemplatesTool::name() const {
    return QStringLiteral("list_agent_templates");
}

QString ListAgentTemplatesTool::description() const {
    return QStringLiteral("List installed agent templates from the application registry. "
                          "Read-only; any agent may call. Use this before "
                          "add_project_member to pick an appropriate template by name. "
                          "Pagination via page and page_size (default 20, max 50).");
}

QList<ToolParameterSchema> ListAgentTemplatesTool::parameters() const {
    ToolParameterSchema query;
    query.name = QStringLiteral("query");
    query.type = QStringLiteral("string");
    query.description =
        QStringLiteral("Optional case-insensitive substring filter against template "
                       "name and description.");
    query.required = false;

    ToolParameterSchema page;
    page.name = QStringLiteral("page");
    page.type = QStringLiteral("integer");
    page.description = QStringLiteral("1-indexed page number (default 1).");
    page.required = false;

    ToolParameterSchema pageSize;
    pageSize.name = QStringLiteral("page_size");
    pageSize.type = QStringLiteral("integer");
    pageSize.description = QStringLiteral("Results per page (default 20, max 50).");
    pageSize.required = false;
    return {query, page, pageSize};
}

bool ListAgentTemplatesTool::runsOnMainThread() const {
    return true;  // AgentRegistry hits SQLite
}

QJsonValue ListAgentTemplatesTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("Membership tools are unavailable in this build."));
    }

    const QString query = args.value(QStringLiteral("query")).toString().trimmed().toLower();
    int page = args.value(QStringLiteral("page")).toInt(1);
    int pageSize = args.value(QStringLiteral("page_size")).toInt(20);
    if (page < 1)
        page = 1;
    if (pageSize < 1)
        pageSize = 20;
    if (pageSize > 50)
        pageSize = 50;

    const QList<Agent> all = m_deps.agents->allAgents();
    QJsonArray matches;
    for (const Agent& a : all) {
        if (!query.isEmpty()) {
            const bool hit =
                a.name.toLower().contains(query) || a.description.toLower().contains(query);
            if (!hit)
                continue;
        }
        QJsonObject row;
        row[QStringLiteral("id")] = a.id;
        row[QStringLiteral("name")] = a.name;
        row[QStringLiteral("description")] = a.description;
        row[QStringLiteral("default_pattern")] = a.defaultPattern;
        row[QStringLiteral("is_coordinator")] = a.isCoordinator;
        matches.append(row);
    }

    const int total = matches.size();
    const int begin = (page - 1) * pageSize;
    QJsonArray slice;
    for (int i = begin; i < total && i < begin + pageSize; ++i) {
        slice.append(matches.at(i));
    }
    QJsonObject result;
    result[QStringLiteral("templates")] = slice;
    result[QStringLiteral("page")] = page;
    result[QStringLiteral("page_size")] = pageSize;
    result[QStringLiteral("total")] = total;
    return result;
}

}  // namespace Tools

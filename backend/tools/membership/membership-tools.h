// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file membership-tools.h
 * @brief LLM-callable tools that let a coordinator agent self-manage
 *        the membership of its active project / organization folder.
 *
 *        Four tools in one header (mirroring the heartbeat self-config
 *        family):
 *
 *          - `add_project_member`       (coordinator-only WRITE)
 *          - `remove_project_member`    (coordinator-only WRITE)
 *          - `list_project_members`     (any agent READ)
 *          - `list_agent_templates`     (any agent READ, registry browse)
 *
 *        All four are main-thread-only because every dependency
 *        (MembershipService, AgentRegistry, ConversationService)
 *        touches SQLite through the main-thread DB connection. The
 *        coordinator gate is enforced inside each WRITE tool's
 *        invoke() body by looking up the calling agent's membership
 *        row via MembershipService::projectMembers and matching on
 *        the cascade's current-responder agent id.
 * @layer Service (Tool subsystem)
 * @dependencies MembershipService, AgentRegistry, ConversationService,
 *               Chat::CascadeController (caller-identity lookup).
 */

#pragma once

#include "../itool.h"
#include "membership-tool-deps.h"

namespace Tools {

/**
 * @brief Common base that resolves the (callerAgentId, callerAlias,
 *        activeConvId, folderId, isCoordinator) tuple from the LLM
 *        dispatch context. Every subclass invokes one of the helper
 *        methods below at the top of its invoke().
 */
class MembershipToolBase : public ITool {
  public:
    /**
     * @brief Constructs the base with the injected dependency bundle.
     * @param deps  Bundle of membership-tool dependencies.
     */
    explicit MembershipToolBase(const MembershipToolDeps& deps);

    /**
     * @brief Thread-residency declaration.
     * @returns true, because every dependency touches SQLite via the main
     *          thread DB connection.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Conversation-capability scope.
     * @returns ProjectOnly, because membership management only applies to a
     *          conversation inside a project/organization folder.
     *          Inherited by AddProjectMemberTool / RemoveProjectMemberTool /
     *          ListProjectMembersTool.
     */
    ToolScope scope() const override { return ToolScope::ProjectOnly; }

  protected:
    /**
     * @brief Resolves the active project / organization folder for the
     *        calling agent.
     *
     *        Caller identity precedence:
     *          1. `args["__caller_agent_id"]` / `args["__caller_agent_alias"]`,
     *             host-injected by ToolDispatcher from the calling CC's
     *             batch inputs so wire-side per-client cascades resolve
     *             to their own responder identity, not a
     *             statically-captured pointer.
     *          2. `m_deps.cascade->currentResponder*()` fallback for
     *             direct invocations / tests that bypass ToolService.
     *
     *        Conv id precedence:
     *          1. `args["__caller_conv_id"]`, host-injected.
     *          2. `m_deps.activeConvIdGetter()`, the captured fallback.
     * @param args              Tool args including (optionally) the
     *                          `__caller_*` keys.
     * @param outFolderId       Folder UUID (output).
     * @param outCallerAgentId  Calling agent's template id (output).
     * @param outCallerAlias    Calling agent's alias in the project (output).
     * @returns Empty QJsonValue on success; structured error JSON when:
     *            - the dependency bundle is incomplete,
     *            - there is no responder identity (called outside a
     *              cascade turn),
     *            - there is no active conversation,
     *            - the active conversation lives at root (no folder), or
     *            - the active folder is not a project / organization.
     */
    QJsonValue resolveProjectContext(const QJsonObject& args,
                                     QString& outFolderId,
                                     QString& outCallerAgentId,
                                     QString& outCallerAlias);

    /**
     * @brief Coordinator-gated variant of resolveProjectContext().
     *        Authoritative coordinator-flag check is the wire-side
     *        responder's per-project membership row.  The args-
     *        injection precedence chain (see resolveProjectContext)
     *        ensures the WIRE caller's identity is what the
     *        membership lookup sees, closing the privilege-escalation
     *        path where a LOCAL coordinator's identity could pass a
     *        WIRE non-coordinator through the gate.
     * @param args              Tool args including (optionally) the
     *                          `__caller_*` keys.
     * @param outFolderId       Folder UUID (output).
     * @param outCallerAgentId  Calling agent's template id (output).
     * @param outCallerAlias    Calling agent's alias in the project (output).
     * @returns Empty QJsonValue on success; structured error JSON when
     *          any of the resolveProjectContext() failure modes apply,
     *          or when the caller is a member but not a coordinator,
     *          or not a member at all.
     */
    QJsonValue resolveCoordinatorContext(const QJsonObject& args,
                                         QString& outFolderId,
                                         QString& outCallerAgentId,
                                         QString& outCallerAlias);

    /**
     * @brief Resolve the CALLING conversation's id: args-injected
     *        `__caller_conv_id` first (per-client routing correctness),
     *        the captured active-conv getter as the fallback for direct
     *        callers / tests.
     * @param args Tool arguments (post ToolService args-injection).
     * @returns Conversation UUID, or empty when neither source has one.
     */
    QString callerConversationId(const QJsonObject& args) const;

    MembershipToolDeps m_deps;  ///< Injected dependencies.
};

// ---------------------------------------------------------------------------
// add_project_member
// ---------------------------------------------------------------------------

/**
 * @brief Add a new agent to the active project's member list.
 *        Coordinator-only. Picks an agent template by id or by name.
 *        The new member gets the given alias (must be unique in the
 *        project). Optional is_coordinator flag promotes the new
 *        member to coordinator role (rare; typically a project has
 *        one coordinator).
 *
 *        After success the new agent is available for group-chat
 *        membership and Direct-chat opens in this project. The host
 *        emits MembershipService::projectMembersChanged so the sidebar
 *        and project settings refresh automatically.
 */
class AddProjectMemberTool : public MembershipToolBase {
  public:
    using MembershipToolBase::MembershipToolBase;

    /**
     * @brief Canonical tool name.
     * @returns "add_project_member".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the add-member behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `agent_id` or `agent_name`, `alias`,
     *          and optional `is_coordinator`.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Add the requested agent to the active project.
     * @param args  JSON object with the schema-defined fields.
     * @returns `{"alias": ..., "agent_id": ...}` on success; structured
     *          error JSON when the gate fails or alias is taken.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

// ---------------------------------------------------------------------------
// remove_project_member
// ---------------------------------------------------------------------------

/**
 * @brief Remove a member from the active project. Coordinator-only.
 *        The coordinator cannot remove themselves; removing the last
 *        coordinator in a project is rejected with an explicit error
 *        (leaves the project unmanaged).
 */
class RemoveProjectMemberTool : public MembershipToolBase {
  public:
    using MembershipToolBase::MembershipToolBase;

    /**
     * @brief Canonical tool name.
     * @returns "remove_project_member".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the remove-member behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `alias` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Remove the member with the given alias.
     * @param args  JSON object with required `alias`.
     * @returns `{"ok": true}` on success; structured error JSON when
     *          the caller is removing themselves or removing the last
     *          coordinator.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

// ---------------------------------------------------------------------------
// list_project_members
// ---------------------------------------------------------------------------

/**
 * @brief Returns the active project's member list with aliases, agent
 *        template names, and coordinator flags. Read-only; any agent
 *        in the project may call. Useful before add_project_member
 *        ("who do we already have?") or for general team awareness.
 */
class ListProjectMembersTool : public MembershipToolBase {
  public:
    using MembershipToolBase::MembershipToolBase;

    /**
     * @brief Canonical tool name.
     * @returns "list_project_members".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the member-list behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Empty list, because the tool takes no parameters.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Return the active project's member list.
     * @param args  Ignored.
     * @returns `{"folder_id": ..., "members": [{alias, agent_id,
     *          agent_name, is_coordinator}, ...]}` on success;
     *          structured error JSON otherwise.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

// ---------------------------------------------------------------------------
// list_agent_templates
// ---------------------------------------------------------------------------

/**
 * @brief Returns every installed agent template (registry-wide).
 *        Coordinator-facing helper for "who can I recruit?". Returns
 *        id, name, description, isCoordinator-default, defaultPattern
 *        for each template. Pagination via page / page_size.
 */
class ListAgentTemplatesTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of membership-tool dependencies.
     */
    explicit ListAgentTemplatesTool(const MembershipToolDeps& deps);

    /**
     * @brief Canonical tool name.
     * @returns "list_agent_templates".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the registry-browse behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for optional `page` and `page_size`.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because AgentRegistry is main-thread-only.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Conversation-capability scope.
     * @returns ProjectOnly, because browsing agent templates to add as members
     *          is only meaningful inside a project/organization.
     */
    ToolScope scope() const override { return ToolScope::ProjectOnly; }

    /**
     * @brief Enumerate every installed agent template.
     * @param args  Optional pagination fields.
     * @returns Paginated `{"agents": [{id, name, description,
     *          is_coordinator_default, default_pattern}, ...]}`.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    MembershipToolDeps m_deps;
};

}  // namespace Tools

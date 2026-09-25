// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file membership-tool-deps.h
 * @brief Non-owning service references shared by every LLM-callable
 *        membership tool.
 *
 *        Same shape as SkillToolDeps / HeartbeatToolDeps: a small
 *        bundle of pointers + an active-conversation getter, so tools
 *        depend on the smallest possible interface and tests can stand
 *        them up without ChatController.
 *
 *        Coordinator gating: every WRITE tool in this family verifies
 *        the calling agent is a coordinator of the active project by
 *        looking up the membership row via
 *        `members->projectMembers(folderId)` and matching on the
 *        caller's agent id.  Caller identity resolves via the
 *        args-injection precedence chain (`__caller_agent_id` /
 *        `__caller_agent_alias` from per-CC ToolDispatcher first,
 *        cascade-pointer fallback second), so wire-side per-client
 *        cascades' identities are correctly authorized against THEIR
 *        membership rows, not whatever the LOCAL cascade pointer
 *        last seated.  The lookup is the only authorization check;
 *        there is NO additional template-level gate (Agent::isCoordinator
 *        is the registry-level default; the per-membership row is what
 *        authorizes inside a project).
 * @layer Service (Tool subsystem)
 * @dependencies MembershipService, AgentRegistry, ConversationService,
 *               CascadeController. No transitive includes here; the
 *               header is intentionally thin.
 */

#pragma once

#include <functional>
#include <QString>

class AgentRegistry;
class ConversationService;
class MembershipService;

namespace Chat {
class CascadeController;
}

namespace Tools {

/**
 * @brief Service references + active-conv getter shared by every
 *        membership tool. AppController constructs this once at
 *        registration time; tools store it by value.
 */
struct MembershipToolDeps {
    MembershipService* members = nullptr;         ///< project/conv member CRUD
    AgentRegistry* agents = nullptr;              ///< agent template lookup
    ConversationService* convs = nullptr;         ///< conv + folder lookup
    Chat::CascadeController* cascade = nullptr;   ///< current-responder identity
    std::function<QString()> activeConvIdGetter;  ///< current chat id

    /**
     * @brief Reports whether the bundle has every required field.
     * @returns True iff every pointer plus the getter is populated.
     */
    bool isValid() const {
        return members && agents && convs && cascade && static_cast<bool>(activeConvIdGetter);
    }
};

}  // namespace Tools

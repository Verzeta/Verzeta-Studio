// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-tool-deps.h
 * @brief Non-owning service references for the skill tool family.
 *
 *        Tools depend on the smallest possible interface (an
 *        `activeConvIdGetter` lambda rather than a ChatController*) so
 *        tests can stand them up without ChatController.
 *
 *        Conversation context resolves via an args-injection
 *        precedence chain: tools read `__caller_conv_id` from args
 *        first (host-injected by ToolDispatcher from the calling
 *        CC's batch inputs), falling back to `activeConvIdGetter`
 *        only when the arg is absent.  The captured getter remains
 *        on the deps struct as the back-compat safety net for
 *        direct invocations and tests that bypass ToolService.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, ConversationService, CascadeController,
 *               optionally ToolService.
 */


#pragma once

#include <functional>
#include <QString>

class ConversationService;
class SkillService;
class ToolService;
namespace Chat {
class CascadeController;
}

namespace Tools {

/**
 * @brief Bundle of non-owning service references that the skill tool
 *        family needs at invocation time. All pointers are non-owning;
 *        callers must keep the referenced services alive for the
 *        lifetime of any tool that captures the bundle.
 */
struct SkillToolDeps {
    SkillService* skills = nullptr;              ///< Skill library and scope resolution.
    ConversationService* convs = nullptr;        ///< Required by isValid(); not read by the tools.
    Chat::CascadeController* cascade = nullptr;  ///< Required by isValid(); not read by the tools.
    /// Returns the UI-active conversation id; used when the call carries
    /// no `__caller_conv_id`.
    std::function<QString()> activeConvIdGetter;

    /**
     * Required by `get_skill_status` to cross-reference a skill's
     * declared_tools against the actual registered tool surface.
     * OPTIONAL for the rest of the family (discover_skills /
     * read_skill / read_skill_file do not consult the tool registry),
     * so isValid() does NOT require this field; each consumer that
     * needs it checks `tools != nullptr` itself.
     */
    ToolService* tools = nullptr;

    /**
     * @brief Reports whether the bundle has the minimum-required
     *        references for the discover / read family.
     * @returns True iff `skills`, `convs`, `cascade`, and
     *          `activeConvIdGetter` are all present.
     */
    bool isValid() const {
        return skills && convs && cascade && static_cast<bool>(activeConvIdGetter);
    }
};

}  // namespace Tools

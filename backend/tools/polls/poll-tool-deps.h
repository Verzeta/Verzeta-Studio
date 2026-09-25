// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file poll-tool-deps.h
 * @brief Non-owning service references for the poll tool family
 *        (`start_poll` / `cast_vote` / `get_poll_results` /
 *        `close_poll`).
 *
 *        Caller identity is resolved through an args-injection
 *        precedence chain: poll tools read `__caller_agent_alias`
 *        from args first (host-injected by ToolDispatcher from the
 *        calling CC's batch inputs), falling back to
 *        `cascade->currentResponderAlias()` only when the arg is
 *        absent.  Conversation context resolves the same way:
 *        `__caller_conv_id` first, `activeConvIdGetter()` fallback.
 *        The captured cascade pointer + getter lambda remain on the
 *        deps struct as the back-compat safety net for direct
 *        invocations and tests that bypass ToolService.
 * @layer Service (Tool subsystem)
 * @dependencies PollService, CascadeController.
 */


#pragma once

#include <functional>
#include <QString>

class PollService;
namespace Chat {
class CascadeController;
}

namespace Tools {

/**
 * @brief Bundle of non-owning service references that every poll tool
 *        needs at invocation time.
 */
struct PollToolDeps {
    PollService* polls = nullptr;  ///< Poll storage and voting.
    /// Fallback source of the caller's alias when the call carries no
    /// `__caller_agent_alias`.
    Chat::CascadeController* cascade = nullptr;
    /// Returns the UI-active conversation id; used when the call carries
    /// no `__caller_conv_id`.
    std::function<QString()> activeConvIdGetter;

    /**
     * @brief Reports whether the bundle has every required field.
     * @returns True iff `polls`, `cascade`, and `activeConvIdGetter`
     *          are all present.
     */
    bool isValid() const { return polls && cascade && static_cast<bool>(activeConvIdGetter); }
};

}  // namespace Tools

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-registration.h
 * @brief Composition-root helper that registers every built-in tool
 *        onto a ToolService instance.
 * @layer Service (Tool subsystem)
 * @dependencies ToolService (non-owning), RegistrationContext,
 *               api/tool-calling-schema.h transitively via ITool.
 *
 * AppController calls registerAllBuiltInTools() exactly once during
 * startup, after ToolService is constructed and its core service
 * dependencies (ProcessSandbox, FileService, ChatController) are
 * wired. The call replaces the previously scattered registration
 * paths: ToolService's own built-in + plan-tool methods, plus
 * ChatController's memory-tool and task-tool-handler registration.
 *
 * Behaviour guarantees:
 *   - Every tool previously registered by the legacy paths remains
 *     registered with the same name and same ToolKind::BuiltIn
 *     classification.
 *   - The QML-visible shape of registeredToolsList() (the "kind"
 *     field and the populated tool set) is unchanged.
 *   - No custom or MCP tool registration path is touched.
 */
#pragma once

class ToolService;

namespace Tools {

struct RegistrationContext;

/**
 * @brief Register every built-in tool on the supplied ToolService.
 * @param toolSvc Target registry. Must outlive every tool registered
 *                through this call.
 * @param context Non-owning service references. Each tool consumes
 *                only the subset of pointers its implementation needs;
 *                tools whose dependencies are null skip registration
 *                after logging a warning (rule 11: no silent drops).
 * @sideeffects Populates toolSvc's internal registry. The legacy
 *              registration paths this function delegates to fire
 *              individual `toolsChanged` signals per tool, so the
 *              emission count matches the earlier behaviour exactly.
 *
 * Intended to be called once per ToolService instance. A second call
 * would attempt to re-register tools already present and would emit
 * warnings through the existing name-uniqueness check in
 * ToolService::registerTool.
 */
void registerAllBuiltInTools(ToolService& toolSvc, const RegistrationContext& context);

/**
 * @brief Registers ONLY the heartbeat self-config
 *        tool family on the supplied ToolService. Called separately
 *        from registerAllBuiltInTools because the heartbeat services
 *        are constructed after the main tool registration in
 *        AppController, and registerAllBuiltInTools would otherwise
 *        warn on duplicate registration of every other tool.
 *
 * @param toolSvc  Target registry. Must outlive the registered tools.
 * @param context  RegistrationContext populated at minimum with the
 *                 four heartbeat-specific fields (heartbeatConfigService,
 *                 heartbeatSubagentService, agentRegistry,
 *                 cascadeController) plus convService + chatController.
 *                 Missing fields skip registration with a logged
 *                 warning (rule 11: no silent drops).
 */
void registerHeartbeatSelfConfigTools(ToolService& toolSvc, const RegistrationContext& context);

}  // namespace Tools

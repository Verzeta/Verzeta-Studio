// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file itool-adapter.h
 * @brief Bridge between an ITool implementation and the ToolService
 *        lambda-based handler contract.
 * @layer Service (Tool subsystem)
 * @dependencies ITool (interface), ToolService (registry surface).
 *
 * ToolService predates the ITool abstraction; its handlers are plain
 * std::function callables. The adapter in this header wraps an ITool
 * instance into a schema + handler pair and registers the pair with
 * a ToolService. Ownership of the ITool instance is transferred into
 * a shared_ptr captured by the handler lambda; the tool lives for
 * the lifetime of its entry in the registry.
 */
#pragma once

#include <memory>

class ToolService;
enum class ToolKind;

namespace Tools {

class ITool;

/**
 * @brief Register an ITool instance on a ToolService.
 * @param toolSvc Target registry; must outlive the tool instance
 *                (AppController owns both with matching lifetimes).
 * @param tool    Ownership is taken and stored inside the handler
 *                lambda. Must be non-null.
 * @param kind    Category tag used by ToolKind-aware consumers
 *                (QML ToolsPage filters by this field). Defaults to
 *                ToolKind::BuiltIn for built-in tool families.
 * @sideeffects Calls ToolService::registerTool(schema, handler, kind).
 *              Emits `toolsChanged` on toolSvc unless that service is
 *              currently inside a beginBatch/endBatch pair.
 */
void registerITool(ToolService& toolSvc, std::unique_ptr<ITool> tool, ToolKind kind);

}  // namespace Tools

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file itool-adapter.cpp
 * @brief Implementation of the ITool → ToolService registration bridge.
 * @layer Service (Tool subsystem)
 * @dependencies ITool, ToolService, utils/logger.h.
 */

#include "itool-adapter.h"

#include "../services/tool-service.h"
#include "../utils/logger.h"
#include "itool.h"

namespace Tools {

void registerITool(ToolService& toolSvc, std::unique_ptr<ITool> tool, ToolKind kind) {
    if (!tool) {
        qCWarning(verzetaTools) << "Tools::registerITool: null tool pointer; registration skipped";
        return;
    }

    ToolSchema schema;
    schema.name = tool->name();
    schema.description = tool->description();
    schema.parameters = tool->parameters();
    // Carry the tool's own conversation-capability scope into the schema
    // so RequestBuilder can gate the offered set by chat kind (single-agent
    // chats are not handed group/project-only tools they can never use).
    schema.scope = tool->scope();

    const bool mainThreadResidency = tool->runsOnMainThread();

    // Ownership moves into a shared_ptr so the capturing lambda can
    // be copied freely (QtConcurrent makes copies of handlers it
    // dispatches) while the tool itself is destroyed deterministically
    // when the last lambda copy dies with its ToolService registry
    // entry.
    auto shared = std::shared_ptr<ITool>(std::move(tool));

    toolSvc.registerTool(
        schema,
        [shared](const QJsonObject& args) -> QJsonValue { return shared->invoke(args); },
        kind);

    toolSvc.setRunsOnMainThread(schema.name, mainThreadResidency);
}

}  // namespace Tools

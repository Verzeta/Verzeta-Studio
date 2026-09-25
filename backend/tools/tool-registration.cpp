// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-registration.cpp
 * @brief Implementation of the built-in tool registration helper.
 *
 *        This helper owns the registration of every built-in tool
 *        whose dependencies are not available until after ChatController
 *        and its collaborator services are fully wired. Tools whose
 *        bodies already live inside ChatController (task-lifecycle
 *        handlers) still delegate through its public registration
 *        method; the delegation disappears as each family migrates
 *        into a dedicated class.
 *
 *        Shell / file / web / time / request_turn tools register
 *        earlier in AppController through ToolService's own
 *        registerBuiltInTools() because the built-in name set must
 *        be populated before loadCustomTools runs.
 *
 * @layer Service (Tool subsystem)
 * @dependencies ToolService, ChatController (for active-conversation
 *               id lookup and task-handler delegation), SearchService,
 *               MessageService, ConversationService, per-tool classes
 *               under backend/tools/memory/, backend/tools/itool-adapter.h,
 *               utils/logger.h.
 */

#include "tool-registration.h"

#include "../services/canvas-service.h"
#include "../services/chat-controller.h"
#include "../services/chat/cascade-controller.h"
#include "../services/skill-service.h"
#include "../services/tool-service.h"
#include "../utils/logger.h"
#include "canvas/edit-canvas-tool.h"
#include "canvas/open-canvas-tool.h"
#include "canvas/read-canvas-tool.h"
#include "heartbeat/heartbeat-self-config-tools.h"
#include "image/generate-image-tool.h"
#include "image/image-tool-deps.h"
#include "itool-adapter.h"
#include "membership/membership-tool-deps.h"
#include "membership/membership-tools.h"
#include "memory/list-project-conversations-tool.h"
#include "memory/read-conversation-tool.h"
#include "memory/remember-tool.h"
#include "memory/search-messages-tool.h"
#include "polls/poll-tools.h"
#include "skills/discover-skills-tool.h"
#include "skills/get-skill-status-tool.h"
#include "skills/read-skill-file-tool.h"
#include "skills/read-skill-tool.h"
#include "skills/skill-tool-deps.h"
#include "subagent/subagent-tools.h"
#include "tool-registration-context.h"

#include <memory>
#include <QPointer>

namespace Tools {

void registerAllBuiltInTools(ToolService& toolSvc, const RegistrationContext& context) {
    // ----- Memory-lookup family (search_messages / read_conversation /
    //       list_project_conversations) ---------------------------------
    //
    // Every memory tool needs a stable getter for the UI-active
    // conversation id. ChatController owns that state; we capture a
    // QPointer so the getter is safe even if ChatController is torn
    // down before the ToolService is (AppController's destruction
    // order guarantees the opposite, but the guard is cheap).
    if (context.searchService && context.msgService && context.convService &&
        context.chatController) {
        QPointer<ChatController> cc(context.chatController);
        auto activeConvIdGetter = [cc]() -> QString {
            return cc ? cc->activeConversationId() : QString();
        };

        registerITool(toolSvc,
                      std::make_unique<SearchMessagesTool>(*context.searchService),
                      ToolKind::BuiltIn);

        registerITool(
            toolSvc,
            std::make_unique<ReadConversationTool>(*context.msgService, activeConvIdGetter),
            ToolKind::BuiltIn);

        registerITool(toolSvc,
                      std::make_unique<ListProjectConversationsTool>(*context.convService,
                                                                     activeConvIdGetter),
                      ToolKind::BuiltIn);
    } else {
        qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: memory tools skipped —"
                                << "searchService:" << (context.searchService ? "present" : "null")
                                << "msgService:" << (context.msgService ? "present" : "null")
                                << "convService:" << (context.convService ? "present" : "null")
                                << "chatController:"
                                << (context.chatController ? "present" : "null");
    }

    // ----- Per-agent memory (AIM) — the SAVE tool ('remember') -------
    //
    // Recall is not a tool — it is surfaced automatically pre-turn by
    // MemoryRetriever. The tool derives its memory scope from the
    // dispatcher-injected caller identity, so it needs no conv-id getter.
    if (context.agentMemoryService) {
        registerITool(toolSvc,
                      std::make_unique<RememberTool>(*context.agentMemoryService),
                      ToolKind::BuiltIn);
    } else {
        qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: remember tool skipped —"
                                << "agentMemoryService: null";
    }


    if (context.canvasService && context.chatController) {
        QPointer<ChatController> ccGuard(context.chatController);
        auto activeConvIdGetter = [ccGuard]() -> QString {
            return ccGuard ? ccGuard->activeConversationId() : QString();
        };
        registerITool(toolSvc,
                      std::make_unique<OpenCanvasTool>(*context.canvasService, activeConvIdGetter),
                      ToolKind::BuiltIn);
        registerITool(toolSvc,
                      std::make_unique<ReadCanvasTool>(*context.canvasService, activeConvIdGetter),
                      ToolKind::BuiltIn);
        registerITool(toolSvc,
                      std::make_unique<EditCanvasTool>(*context.canvasService, activeConvIdGetter),
                      ToolKind::BuiltIn);
    } else {
        qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: canvas tools skipped —"
                                << "canvasService:" << (context.canvasService ? "present" : "null")
                                << "chatController:"
                                << (context.chatController ? "present" : "null");
    }

    if (context.skillService && context.convService && context.chatController) {
        QPointer<ChatController> ccGuard(context.chatController);
        auto activeConvIdGetter = [ccGuard]() -> QString {
            return ccGuard ? ccGuard->activeConversationId() : QString();
        };
        SkillToolDeps deps;
        deps.skills = context.skillService;
        deps.convs = context.convService;
        deps.cascade = nullptr;  // populated below if available
        deps.activeConvIdGetter = activeConvIdGetter;
        deps.tools = &toolSvc;
        // Cascade controller is owned by ChatController; reach it via
        // the same internal accessor heartbeat tools use.
        deps.cascade = ccGuard ? ccGuard->cascadeInternal() : nullptr;

        if (deps.cascade) {
            registerITool(toolSvc, std::make_unique<DiscoverSkillsTool>(deps), ToolKind::BuiltIn);
            registerITool(toolSvc, std::make_unique<ReadSkillTool>(deps), ToolKind::BuiltIn);
            registerITool(toolSvc, std::make_unique<ReadSkillFileTool>(deps), ToolKind::BuiltIn);
            registerITool(toolSvc, std::make_unique<GetSkillStatusTool>(deps), ToolKind::BuiltIn);
        } else {
            qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: skill tools skipped —"
                                    << "cascadeController not available via ChatController";
        }
    } else {
        qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: skill tools skipped —"
                                << "skillService:" << (context.skillService ? "present" : "null")
                                << "convService:" << (context.convService ? "present" : "null")
                                << "chatController:"
                                << (context.chatController ? "present" : "null");
    }

    if (context.membershipService && context.agentRegistry && context.convService &&
        context.chatController) {
        QPointer<ChatController> ccGuard(context.chatController);
        auto activeConvIdGetter = [ccGuard]() -> QString {
            return ccGuard ? ccGuard->activeConversationId() : QString();
        };
        MembershipToolDeps deps;
        deps.members = context.membershipService;
        deps.agents = context.agentRegistry;
        deps.convs = context.convService;
        deps.cascade = ccGuard ? ccGuard->cascadeInternal() : nullptr;
        deps.activeConvIdGetter = activeConvIdGetter;

        if (deps.cascade) {
            registerITool(toolSvc, std::make_unique<AddProjectMemberTool>(deps), ToolKind::BuiltIn);
            registerITool(
                toolSvc, std::make_unique<RemoveProjectMemberTool>(deps), ToolKind::BuiltIn);
            registerITool(
                toolSvc, std::make_unique<ListProjectMembersTool>(deps), ToolKind::BuiltIn);
            registerITool(
                toolSvc, std::make_unique<ListAgentTemplatesTool>(deps), ToolKind::BuiltIn);
        } else {
            qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: membership tools skipped —"
                                    << "cascadeController not available via ChatController";
        }
    } else {
        qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: membership tools skipped —"
                                << "membershipService:"
                                << (context.membershipService ? "present" : "null")
                                << "agentRegistry:" << (context.agentRegistry ? "present" : "null")
                                << "convService:" << (context.convService ? "present" : "null")
                                << "chatController:"
                                << (context.chatController ? "present" : "null");
    }

    if (context.imageService && context.settingsService && context.imageProviderRegistry &&
        context.chatController) {
        QPointer<ChatController> ccGuard(context.chatController);
        auto activeConvIdGetter = [ccGuard]() -> QString {
            return ccGuard ? ccGuard->activeConversationId() : QString();
        };
        ImageToolDeps deps;
        deps.image = context.imageService;
        deps.settings = context.settingsService;
        deps.registry = context.imageProviderRegistry;
        deps.activeConvIdGetter = activeConvIdGetter;

        registerITool(toolSvc, std::make_unique<GenerateImageTool>(deps), ToolKind::BuiltIn);
    } else {
        qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: image tools skipped —"
                                << "imageService:" << (context.imageService ? "present" : "null")
                                << "settingsService:"
                                << (context.settingsService ? "present" : "null")
                                << "imageProviderRegistry:"
                                << (context.imageProviderRegistry ? "present" : "null")
                                << "chatController:"
                                << (context.chatController ? "present" : "null");
    }

    if (context.pollService && context.chatController) {
        QPointer<ChatController> ccGuard(context.chatController);
        auto activeConvIdGetter = [ccGuard]() -> QString {
            return ccGuard ? ccGuard->activeConversationId() : QString();
        };
        PollToolDeps deps;
        deps.polls = context.pollService;
        deps.cascade = ccGuard ? ccGuard->cascadeInternal() : nullptr;
        deps.activeConvIdGetter = activeConvIdGetter;
        if (deps.cascade) {
            registerITool(toolSvc, std::make_unique<StartPollTool>(deps), ToolKind::BuiltIn);
            registerITool(toolSvc, std::make_unique<CastVoteTool>(deps), ToolKind::BuiltIn);
            registerITool(toolSvc, std::make_unique<GetPollResultsTool>(deps), ToolKind::BuiltIn);
            registerITool(toolSvc, std::make_unique<ClosePollTool>(deps), ToolKind::BuiltIn);
        } else {
            qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: poll tools skipped —"
                                    << "cascadeController not available via ChatController";
        }
    } else {
        qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: poll tools skipped —"
                                << "pollService:" << (context.pollService ? "present" : "null")
                                << "chatController:"
                                << (context.chatController ? "present" : "null");
    }

    // Agent-spawned sub-agents (spawn + check). Lives HERE because
    // AppController's main registration context is the one that wires
    // context.subagentService — the heartbeat helper below runs with
    // a separate, heartbeat-only context. (A previous revision placed
    // this block in that helper; the tools silently never registered
    // in the live app while unit tests, which construct the tools
    // directly, stayed green.)
    if (context.subagentService) {
        registerITool(toolSvc,
                      std::make_unique<SpawnSubagentTool>(*context.subagentService),
                      ToolKind::BuiltIn);
        registerITool(toolSvc,
                      std::make_unique<CheckSubagentTool>(*context.subagentService),
                      ToolKind::BuiltIn);
    } else {
        qCWarning(verzetaTools) << "Tools::registerAllBuiltInTools: sub-agent tools skipped —"
                                << "subagentService is null";
    }
}

void registerHeartbeatSelfConfigTools(ToolService& toolSvc, const RegistrationContext& context) {
    HeartbeatToolDeps hbDeps;
    hbDeps.configSvc = context.heartbeatConfigService;
    hbDeps.subagentSvc = context.heartbeatSubagentService;
    hbDeps.agents = context.agentRegistry;
    hbDeps.convs = context.convService;

    // The active-conversation-id getter is built once at registration
    // time and captured by every tool through the shared deps bundle.
    // Same QPointer guard pattern as the memory-tool family above.
    QPointer<ChatController> ccGuard(context.chatController);
    hbDeps.activeConvIdGetter = [ccGuard]() -> QString {
        return ccGuard ? ccGuard->activeConversationId() : QString();
    };

    // resolve the cascade per call through the
    // ChatController (inflightOrActiveRun) rather than pinning the
    // CascadeController value captured at registration time, so a tool
    // firing on a backgrounded run reads the right responder identity.
    // Falls back to the registration-time context.cascadeController only
    // when no ChatController is wired (legacy / test scaffolding).
    Chat::CascadeController* pinnedCascade = context.cascadeController;
    hbDeps.cascadeResolver = [ccGuard, pinnedCascade]() -> Chat::CascadeController* {
        if (ccGuard)
            return ccGuard->cascadeInternal();
        return pinnedCascade;
    };

    if (!hbDeps.isValid()) {
        qCWarning(verzetaTools) << "Tools::registerHeartbeatSelfConfigTools: skipped —"
                                << "configSvc:" << (hbDeps.configSvc ? "present" : "null")
                                << "subagentSvc:" << (hbDeps.subagentSvc ? "present" : "null")
                                << "agentRegistry:" << (hbDeps.agents ? "present" : "null")
                                << "convService:" << (hbDeps.convs ? "present" : "null")
                                << "cascadeResolver:"
                                << (hbDeps.cascadeResolver ? "present" : "null")
                                << "chatController:"
                                << (context.chatController ? "present" : "null");
        return;
    }
    registerITool(toolSvc, std::make_unique<SetHeartbeatGoalTool>(hbDeps), ToolKind::BuiltIn);
    registerITool(toolSvc, std::make_unique<SetHeartbeatScheduleTool>(hbDeps), ToolKind::BuiltIn);
    registerITool(
        toolSvc, std::make_unique<SetHeartbeatSurfaceCriteriaTool>(hbDeps), ToolKind::BuiltIn);
    registerITool(toolSvc, std::make_unique<EnableHeartbeatTool>(hbDeps), ToolKind::BuiltIn);
    registerITool(toolSvc, std::make_unique<DisableHeartbeatTool>(hbDeps), ToolKind::BuiltIn);
}

}  // namespace Tools

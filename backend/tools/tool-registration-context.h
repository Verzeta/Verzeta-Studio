// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-registration-context.h
 * @brief Non-owning service references passed into the built-in tool
 *        registration helper. Every pointer names a service owned by
 *        AppController whose lifetime covers the full ToolService
 *        lifetime, hence raw pointers, not unique_ptr.
 * @layer Service (Tool subsystem)
 * @dependencies Forward declarations only. No service headers here,
 *               so consumer translation units stay small.
 *
 * The registration helper forwards this context to each built-in tool
 * constructor. Tools consume only the subset of fields their
 * invocation body requires; the rest may be null. Members without a
 * default value have a nullable signalling that the corresponding
 * tool family will skip registration if the context is incomplete.
 */
#pragma once

class AgentMemoryService;
class AgentRegistry;
class CanvasService;
class ChatController;
class ConversationService;
class FileService;
class HeartbeatConfigService;
class HeartbeatSubagentService;
class ImageProviderRegistry;
class ImageService;
class MembershipService;
class MessageService;
class PollService;
class ProcessSandbox;
class SearchService;
class SettingsService;
class SkillService;

namespace Chat {
class CascadeController;
}

namespace Tools {

/**
 * @brief Bundle of non-owning service references the registration
 *        helper passes to each built-in tool's constructor.
 *
 * Adding a new field is visible at every call site, which keeps tool
 * dependencies explicit and auditable.
 */
struct RegistrationContext {
    /**
     * Execution sandbox handed to shell-invoking tool classes.
     * Unused by the current helper body because the shell/file tool
     * family still registers through ToolService::registerBuiltInTools
     * at the early AppController wiring point where the built-in
     * name set must be populated before loadCustomTools runs.
     * Start using this field when the shell/file family is migrated
     * into per-tool ITool classes.
     */
    ProcessSandbox* sandbox = nullptr;

    /**
     * Filesystem gateway handed to file-I/O tool classes. Same
     * transitional status as `sandbox` above.
     */
    FileService* fileService = nullptr;

    /**
     * FTS5 search gateway consumed by the memory-lookup tool family
     * (search_messages). Null disables memory-tool registration.
     */
    SearchService* searchService = nullptr;

    /**
     * Message persistence + retrieval used by read_conversation.
     * Null disables memory-tool registration.
     */
    MessageService* msgService = nullptr;

    /**
     * Conversation + folder lookups used by list_project_conversations.
     * Null disables memory-tool registration.
     */
    ConversationService* convService = nullptr;

    /**
     * Transitional: required until the task-lifecycle tool handlers
     * (start_task, submit_result, report_blocked, …) migrate into
     * their own classes. The helper uses it to obtain a getter for
     * the UI-active conversation id that memory tools inject into
     * their constructors. Remove this member once every tool body
     * has its own class.
     */
    ChatController* chatController = nullptr;

    /** Sub-agent run service. Null skips spawn_subagent /
     *  check_subagent registration (headless test harnesses). */
    class SubagentRunService* subagentService = nullptr;

    /**
     * `open_canvas` tool delegates to CanvasService.
     * The active-conversation-id getter is built from chatController
     * (same pattern the memory-tool family uses). Null disables the
     * canvas tool family; the canvas panel itself still works, just
     * without the agent trigger.
     */
    CanvasService* canvasService = nullptr;

    /**
     * SkillService for the discover_skills / read_skill /
     * read_skill_file tool family. Constructed by AppController BEFORE
     * registerAllBuiltInTools so the field is non-null at registration
     * time. Null disables the skill tool family with a logged warning.
     */
    SkillService* skillService = nullptr;

    /// Heartbeat self-configuration tools. Null skips the five tools
    /// with a logged warning.
    HeartbeatConfigService* heartbeatConfigService = nullptr;
    /// Heartbeat scheduler, rebuilt when a tool changes a schedule.
    /// Null skips the heartbeat tools.
    HeartbeatSubagentService* heartbeatSubagentService = nullptr;
    /// Agent templates. Required by the heartbeat and membership tool
    /// families; null skips both.
    AgentRegistry* agentRegistry = nullptr;
    /// Fallback cascade for the heartbeat tools' caller identity, used
    /// only when no chatController is set. Other families read the
    /// cascade from chatController.
    Chat::CascadeController* cascadeController = nullptr;

    /// Membership tools: add, remove and list project members, and list
    /// agent templates. Also needs agentRegistry, convService and
    /// chatController. Null skips the family with a logged warning.
    MembershipService* membershipService = nullptr;

    /// generate_image tool. Also needs settingsService,
    /// imageProviderRegistry and chatController. Null skips the tool
    /// with a logged warning.
    ImageService* imageService = nullptr;
    SettingsService* settingsService = nullptr;  ///< Required by generate_image.
    /// Image provider catalogue; generate_image builds its request from
    /// activeConfig(). Null skips the tool with a logged warning.
    ImageProviderRegistry* imageProviderRegistry = nullptr;

    /// Poll and voting tools. Also needs chatController. Null skips the
    /// family with a logged warning.
    PollService* pollService = nullptr;

    /// Per-agent memory, backing the `remember` tool. Recall is not a
    /// tool; memories are added to the prompt before each turn. Null
    /// skips the tool with a logged warning.
    AgentMemoryService* agentMemoryService = nullptr;
};

}  // namespace Tools

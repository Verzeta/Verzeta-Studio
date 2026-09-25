// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-service.h
 * @brief Registry of callable tools and dispatcher for LLM tool
 *        invocations. Tools are registered with schemas and handler
 *        functions. Built-in tools include shell commands, file
 *        operations, time, and web search.
 * @layer Service
 * @dependencies ProcessSandbox (Utility), FileService (Service),
 *               tool-calling-schema.h (API)
 */


#pragma once

#include "api/tool-calling-schema.h"

#include <functional>
#include <memory>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>

class BackgroundProcessService;
class FileService;
class ProcessSandbox;

namespace Search {
class WebSearchService;
}

/**
 * @brief Callable signature for a tool handler function.
 *
 * @param args  JSON object with the named tool arguments.
 * @return      JSON result value (string, object, array, etc.)
 *              On error: {"error": "message"} JSON object.
 */
using ToolHandler = std::function<QJsonValue(const QJsonObject& args)>;

/**
 * @brief The category a registered tool belongs to.
 *
 * Exposed via registeredToolsList() as a "kind" string ("builtin",
 * "custom", or "mcp") so QML can render each category distinctly
 * without resorting to naming-convention hacks on tool names.
 */
enum class ToolKind {
    BuiltIn,  ///< Hardcoded into the app (run_shell, read_file, etc)
    Custom,   ///< User-defined via the Add Custom Tool dialog
    Mcp       ///< Registered by an MCP server via McpService
};

/**
 * @brief Registry and dispatcher for LLM-callable tools.
 *
 * ## Lifecycle
 * 1. Create ToolService.
 * 2. Call registerBuiltInTools() with sandbox and fileService.
 * 3. Register custom tools with registerTool().
 * 4. Pass availableTools() to LlmRequest.tools when building a prompt.
 * 5. On tool_call response from LLM, call invokeTool(name, args).
 *
 * ## Threading
 * invokeTool() is synchronous and may block (file I/O, process spawn).
 * Always call it from a background thread (ToolExecutionWorker or similar).
 */
class ToolService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs an empty ToolService (no tools registered).
     * @param parent Optional Qt parent.
     */
    explicit ToolService(QObject* parent = nullptr);
    ~ToolService() override;

    // -----------------------------------------------------------------------
    // Registry
    // -----------------------------------------------------------------------

    /**
     * @brief Register a tool under its schema. Defaults the category
     *        to `ToolKind::Custom`.
     * @param schema  Tool schema (name + description + parameter spec).
     * @param handler Callable invoked when the LLM emits a tool_call
     *                for this tool name.
     */
    void registerTool(const ToolSchema& schema, ToolHandler handler);

    /**
     * @brief Register a tool with an explicit category. Used by
     *        McpService to tag MCP-registered tools as `ToolKind::Mcp`
     *        so they can be presented separately from user-defined
     *        custom tools in the UI. Without this, distinguishing MCP
     *        tools from custom tools relies on name-string hacks.
     * @param schema  Tool schema (name + description + parameter spec).
     * @param handler Callable invoked when the LLM emits a tool_call
     *                for this tool name.
     * @param kind    Category (BuiltIn / Custom / Mcp) for UI
     *                grouping.
     */
    void registerTool(const ToolSchema& schema, ToolHandler handler, ToolKind kind);

    /**
     * @brief Remove a registered tool by name. No-op if the name is
     *        unknown.
     * @param name Tool name to unregister.
     */
    void unregisterTool(const QString& name);

    /**
     * @brief Starts a bulk-registration batch. While a batch is active,
     *        registerTool / unregisterTool mutations do NOT emit
     *        `toolsChanged` individually. Call endBatch() to flush one
     *        coalesced signal.
     *
     *        Why: the QML ToolsPage rebuilds its Repeaters from
     *        registeredToolsList() on every toolsChanged. MCP servers
     *        register dozens of tools in a tight loop on first connect;
     *        without batching this produced O(N^2) main-thread work and
     *        froze the UI for several seconds on startup.
     *
     *        Nested begin/end pairs are supported via a refcount; the
     *        signal only flushes when the outermost endBatch runs.
     */
    void beginBatch();

    /**
     * @brief Close the current bulk-registration batch. When this is
     *        the outermost endBatch in a nested begin/end stack and
     *        any registration mutated state, emits one coalesced
     *        `toolsChanged`.
     */
    void endBatch();

    /**
     * @brief Replace the handler of an already-registered tool while
     *        keeping its schema intact. Used by ChatController to
     *        swap stub handlers for real implementations at
     *        AppController wiring time.
     * @param name    Tool name whose handler to replace.
     * @param handler New callable to install.
     * @returns true if the tool was found and its handler replaced.
     */
    bool replaceHandler(const QString& name, ToolHandler handler);

    /**
     * @brief Snapshot of all enabled tool schemas. Disabled tools are
     *        excluded so the LLM never sees them in the request.
     * @returns Schemas to attach to LlmRequest.tools.
     */
    QList<ToolSchema> availableTools() const;

    /**
     * @brief Whether a tool with the given name is registered.
     * @param name Tool name to look up.
     * @returns true iff the name is in the registry (enabled or not).
     */
    bool hasTool(const QString& name) const;

    /**
     * @brief Main-thread residency of a registered tool.
     *
     *        `ToolDispatcher::dispatchNext` consults this per-tool
     *        flag to decide whether to invoke inline on the main
     *        thread (SQLite-touching tools: every task-lifecycle
     *        tool and every memory-lookup tool) or hand off to a
     *        QtConcurrent worker. The flag is populated by
     *        `Tools::registerITool` from the ITool's own
     *        `runsOnMainThread()` declaration; other registration
     *        paths (custom tools, MCP tools, direct registerTool for
     *        stubs) leave it at the default `false`.
     * @param name Tool name to look up.
     * @returns true iff the tool is registered AND declared as
     *          main-thread-only. Unknown names return false, because a name
     *          the dispatcher has never heard of shouldn't be
     *          force-dispatched inline.
     * @complexity O(log n) in the tool registry.
     */
    bool runsOnMainThread(const QString& name) const;

    /**
     * @brief Set the main-thread residency flag of an
     *        already-registered tool. Idempotent; no-op if the name
     *        is unknown. Called by `Tools::registerITool` to
     *        propagate the ITool's own `runsOnMainThread()` into the
     *        registry entry.
     * @param name             Tool name to update.
     * @param runsOnMainThread New residency flag value.
     */
    void setRunsOnMainThread(const QString& name, bool runsOnMainThread);

    // -----------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------

    /**
     * @brief Invoke a registered tool.
     *
     *        Per-client tool routing: the optional caller-identity
     *        parameters are set by ToolDispatcher (running per-CC)
     *        from its CC's batch inputs so tools that consult caller
     *        context resolve to the CALLING CC's identity instead of
     *        falling back to a statically-captured LOCAL
     *        ChatController / CascadeController pointer.
     *
     *        Three host-controlled keys are injected into @p args
     *        when their corresponding parameters are non-empty:
     *
     *          - `__caller_conv_id`: calling CC's conversation id.
     *          - `__caller_agent_id`: calling CC's responder agent id.
     *          - `__caller_agent_alias`: calling CC's responder alias.
     *
     *        Double-underscore prefix avoids colliding with any
     *        LLM-supplied or user-defined arg name. Tools that need
     *        the values read from args first and fall back to their
     *        captured deps only when the args key is absent; tools
     *        that don't care ignore the keys.
     *
     *        Host-controlled means: the injection happens AFTER the
     *        LLM-supplied args are copied, so any LLM attempt to
     *        forge `__caller_*` keys is overwritten by the host
     *        values.
     *
     *        Default-empty preserves back-compat: older callers
     *        that don't pass caller-identity behave exactly as
     *        before (no injection; tools use their captured fallback).
     * @param name              Registered tool name to invoke.
     * @param args              JSON arguments supplied by the LLM.
     * @param callerConvId      Optional conversation id of the calling CC.
     * @param callerAgentId     Optional agent id of the calling CC's
     *                          currently-responding member.
     * @param callerAgentAlias  Optional alias of the calling CC's
     *                          currently-responding member.
     * @param callerFolderId    Optional folder id the calling conv
     *                          resolves to (first `isProject()` row in
     *                          the folder chain, or the conv's direct
     *                          folder when no project ancestor). When
     *                          non-empty it is injected as the
     *                          `__caller_folder_id` arg so FileService's
     *                          virtual-FS router can resolve
     *                          mount routing from worker-thread tool
     *                          handlers. Sibling of the conv / agent
     *                          identity triplet; purely additive, so tools
     *                          that don't consult it are unaffected.
     * @param callerClientId    Optional id of the paired client a
     *                          bridge-originated call entered through;
     *                          empty for host and agent calls. When
     *                          non-empty it is injected as the
     *                          `__caller_client_id` arg for caller-aware
     *                          mount routing in the file tools. It is
     *                          injected after the LLM's arguments are
     *                          copied, so a forged value is overwritten.
     * @returns Tool result JSON, or `{"error": "..."}` on failure /
     *          missing tool.
     */
    QJsonValue invokeTool(const QString& name,
                          const QJsonObject& args,
                          const QString& callerConvId = QString(),
                          const QString& callerAgentId = QString(),
                          const QString& callerAgentAlias = QString(),
                          const QString& callerFolderId = QString(),
                          const QString& callerClientId = QString());

    // -----------------------------------------------------------------------
    // Built-in tools
    // -----------------------------------------------------------------------

    /**
     * @brief Register the built-in tool set (run_shell, read_file,
     *        write_file, list_files, web_search, time, etc.). Called
     *        once at AppController initialize.
     * @param sandbox     Process sandbox used by run_shell.
     * @param fileService File service used by file tools for safe
     *                    path validation.
     * @param bgService   Optional background process registry backing the
     *                    run_shell background flag; may be null.
     */
    void registerBuiltInTools(ProcessSandbox& sandbox,
                              FileService& fileService,
                              BackgroundProcessService* bgService = nullptr);

    /**
     * @brief Inject the web-search backend used by the `search_web` tool.
     *        Must be called BEFORE registerBuiltInTools so the tool binds
     *        to the AppController-owned, settings-configured service.
     * @param svc Non-owning pointer to the WebSearchService (owned by
     *            AppController, which must outlive this ToolService). When
     *            never called (e.g. in tests), registerBuiltInTools binds
     *            search_web to an internally-owned default instance.
     */
    void setWebSearchService(Search::WebSearchService* svc);


    // -----------------------------------------------------------------------
    // QML-accessible tool management
    // -----------------------------------------------------------------------

    /**
     * @brief Returns all registered tools as a QVariantList for QML.
     * @return List of {name, description, parameters, enabled, isBuiltIn} maps.
     */
    Q_INVOKABLE QVariantList registeredToolsList() const;

    /**
     * @brief Enables or disables a tool. Disabled tools are excluded from
     *        availableTools() and cannot be invoked.
     * @param name    Tool name.
     * @param enabled Whether the tool should be enabled.
     */
    Q_INVOKABLE void setToolEnabled(const QString& name, bool enabled);

    /**
     * @brief Checks if a tool is enabled.
     * @param name Tool name.
     * @return true if the tool exists and is enabled.
     */
    Q_INVOKABLE bool isToolEnabled(const QString& name) const;

    /**
     * @brief Adds a custom tool from a QML-friendly map.
     * @param toolDef Map with: name, description, commandTemplate,
     *                parameters [{name, type, description, required}]
     * @return true on success, false if name is empty or conflicts with built-in.
     */
    Q_INVOKABLE bool addCustomTool(const QVariantMap& toolDef);

    /**
     * @brief Removes a custom tool. Built-in tools cannot be removed.
     * @param name Tool name to remove.
     * @return true if removed, false if not found or built-in.
     */
    Q_INVOKABLE bool removeCustomTool(const QString& name);

    /**
     * @brief Persists custom tool definitions and enabled state to a JSON file.
     */
    Q_INVOKABLE void saveCustomTools();

    /**
     * @brief Loads custom tool definitions from the JSON file.
     *        Called at startup after registerBuiltInTools().
     * @param sandbox ProcessSandbox for executing custom tool command templates.
     */
    void loadCustomTools(ProcessSandbox& sandbox);

  signals:
    /**
     * @brief Emitted at the entry of every invokeTool call.
     * @param name Tool name being invoked.
     * @param args Tool arguments supplied by the LLM.
     */
    void toolInvoked(const QString& name, const QJsonObject& args);

    /**
     * @brief Emitted after a tool returns successfully.
     * @param name   Tool name that completed.
     * @param result Tool result JSON value.
     */
    void toolResult(const QString& name, const QJsonValue& result);

    /**
     * @brief Emitted when a tool fails.
     * @param name  Tool name that failed.
     * @param error Error message.
     */
    void toolError(const QString& name, const QString& error);

    /** @brief Emitted when tools are added, removed, or enabled / disabled. */
    void toolsChanged();

  private:
    /**
     * @brief Registry entry for a single tool name.
     */
    struct RegisteredTool {
        ToolSchema schema;
        ToolHandler handler;
        ToolKind kind = ToolKind::Custom;  ///< First-class category
        bool enabled = true;               ///< false = excluded from LLM requests
        /** Cached main-thread-residency flag. Set by
         *  `Tools::registerITool` from the ITool's own declaration;
         *  kept at `false` for handler-lambda registrations (custom
         *  tools, MCP tools, direct stubs) whose bodies are
         *  worker-safe by default. Task-lifecycle stubs explicitly
         *  raise the flag to true because their real handlers (later
         *  installed by `Chat::TaskToolHandlers::registerOnto`) touch
         *  SQLite-backed services. */
        bool runsOnMainThread = false;
    };

    QMap<QString, RegisteredTool> m_tools;
    QSet<QString> m_builtInNames;  ///< Names of built-in tools (cannot be removed)

    /// Web-search backend for the search_web tool. Injected by AppController
    /// (non-owning); m_ownedWebSearch is the fallback used when none is
    /// injected (standalone / tests) so search_web always has a backend.
    Search::WebSearchService* m_webSearchService = nullptr;
    std::unique_ptr<Search::WebSearchService> m_ownedWebSearch;

    // Batch state for bulk tool registration (MCP servers).
    int m_batchDepth = 0;
    bool m_batchDirty = false;

    /**
     * @brief Returns the path to the custom tools JSON file.
     */
    static QString customToolsPath();
};

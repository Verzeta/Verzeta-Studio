// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file slash-command-service.h
 * @brief Top-level service for user slash commands (`/showtools`,
 *        `/showmcptools`, `/help`, `/clear`, `/artifacts`).
 *
 *        Promoted out of `Chat::SlashCommandHandler` (which lived in
 *        `backend/services/chat/`) so ChatController is no longer
 *        the host of slash-command wiring + context-build closures;
 *        slash commands are a self-contained subsystem with their
 *        own service refs and ownership by AppController.
 *
 * @layer Service
 * @dependencies Qt6::Core. Service refs (ConversationService,
 *               MessageService, FileService, ToolService,
 *               MessageListModel) attached via setters by
 *               AppController during initialize(); the production
 *               `tryHandle(QString)` overload uses them to build
 *               the per-call context. The legacy
 *               `tryHandle(SlashCommandContext)` overload takes
 *               its dependencies via the context struct's closures
 *               (used by the unit test, which runs without DB
 *               fixture or real services).
 *
 * Ownership: AppController owns the single instance via
 *   `std::unique_ptr<SlashCommandService> m_slashCommandService;`
 * declared BEFORE m_chatController so reverse-declaration
 * destruction keeps the service alive through ChatController's
 * full lifetime. ChatController holds a non-owning pointer
 * attached via setSlashCommandService().
 *
 * Threading: called from the main thread (ChatController::
 * sendMessage). No internal cross-thread access; service-ref
 * methods called are themselves main-thread only.
 */
#pragma once

#include <functional>
#include <QObject>
#include <QString>

class ConversationService;
class FileService;
class MessageListModel;
class MessageService;
class ToolService;

class ConversationSummarizer;

/**
 * @brief Per-call bundle carrying the text to parse, the UI-active
 *        conversation id, the ToolService to query for `/showtools`,
 *        and a set of closures that encapsulate ChatController-side
 *        side effects.
 */
struct SlashCommandContext {
    /** Trimmed user input. Only processed if it starts with `/`. */
    QString text;

    /** The UI-active conversation id. Used only for display paths
     *  (`/artifacts` header text), never for mutation. Empty is
     *  tolerated; the commands still behave sensibly. */
    QString activeConvId;

    /** Tool registry queried by `/showtools`, `/showmcptools`, and
     *  the filter variants. Null is tolerated: the `/showtools`
     *  output becomes a "no tool service configured" fallback and
     *  the command still returns true (handled). */
    ToolService* toolService = nullptr;

    /**
     * @brief Resolve the artifact directory and project-name context
     *        for the active conversation.
     *
     * The closure body does the folder-chain walk and FileService
     * context setup; see ChatController::activeArtifactsPath +
     * ConversationService::folderChainForConversation for the
     * pre-extraction reference. Returns `{path, projectName}`
     * where projectName is empty iff the conversation is NOT inside
     * a project/organization folder.
     *
     * Null/uncallable closure → `/artifacts` posts the
     * "*No active conversation.*" fallback.
     */
    struct ArtifactsLocation {
        QString path;         ///< Artifacts folder to report.
        QString projectName;  ///< Enclosing project or organization; empty if none.
    };
    std::function<ArtifactsLocation()> resolveArtifactsLocation;  ///< See ArtifactsLocation.

    /**
     * @brief Post a content string as an ephemeral assistant-authored
     *        system message in the active conversation.
     *
     * The closure is responsible for packaging the content into the
     * Message shape the pre-extraction code built (role=assistant,
     * finishReason="command", modelUsed="system", conversationId=
     * activeConvId, fresh id + createdAt timestamp) and calling
     * MessageService::postEphemeralMessage. Null/uncallable closure
     * is a silent no-op.
     */
    std::function<void(const QString& content)> postMessage;

    /**
     * @brief Reload the active conversation in the message model.
     *        Semantic equivalent of the pre-extraction
     *          m_msgModel->setActiveConversation(QString());
     *          m_msgModel->setActiveConversation(m_activeConvId);
     *        pair used by `/clear`. Null/uncallable closure is a
     *        silent no-op.
     */
    std::function<void()> reloadActiveConversation;

    /** Dynamic-compaction service for /compact and
     *  /flashmemory (nullable; commands degrade to an explanatory
     *  post when absent). */
    ConversationSummarizer* summarizer = nullptr;

    /** Destructive wipe of the active conversation
     *  (messages + tool_calls + canvas rows + token total). Returns
     *  deleted message count, -1 on error. Null tolerated. */
    std::function<int()> wipeActiveConversation;
};

/**
 * @brief Top-level QObject service hosting the slash-command
 *        dispatcher. See the file-level comment for the full command
 *        list.
 */
class SlashCommandService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the service.
     * @param parent Optional Qt parent.
     */
    explicit SlashCommandService(QObject* parent = nullptr);
    ~SlashCommandService() override;

    // -----------------------------------------------------------------
    // Service attachments — wired by AppController during initialize().
    // The legacy `tryHandle(SlashCommandContext)` overload below
    // doesn't need these (used by the closure-based unit test); the
    // production `tryHandle(QString)` overload uses them to build
    // the per-call SlashCommandContext from internal state.
    // -----------------------------------------------------------------

    /**
     * @brief Attach the conversation service.
     * @param svc Non-owning pointer; AppController owns.
     */
    void setConversationService(ConversationService* svc);

    /**
     * @brief Attach the message service.
     * @param svc Non-owning pointer; AppController owns.
     */
    void setMessageService(MessageService* svc);

    /**
     * @brief Attach the file service.
     * @param svc Non-owning pointer; AppController owns.
     */
    void setFileService(FileService* svc);

    /**
     * @brief Attach the tool service.
     * @param svc Non-owning pointer; AppController owns.
     */
    void setToolService(ToolService* svc);

    /**
     * @brief Attach the message list model.
     * @param model Non-owning pointer; AppController owns.
     */
    void setMessageListModel(MessageListModel* model);

    /**
     * @brief Attach the compaction service used by /compact and
     *        /flashmemory.
     * @param s Non-owning pointer; AppController owns. Null detaches
     *          (both commands then report compaction unavailable).
     */
    void setSummarizer(ConversationSummarizer* s) { m_summarizer = s; }

    /**
     * @brief Setter for the active-conversation-id getter. AppController
     *        wires this to a closure that reads
     *        `m_chatController->activeConversationId()` so the
     *        service can resolve the active conv on every call
     *        without holding a ChatController pointer.
     * @param getter Callable returning the id of the active conversation.
     */
    void setActiveConversationIdGetter(std::function<QString()> getter);

    // -----------------------------------------------------------------
    // tryHandle overloads
    // -----------------------------------------------------------------

    /**
     * @brief Production entry point. Builds the per-call
     *        SlashCommandContext from the service's internal state
     *        (deps wired via the setters above + the active-conv
     *        getter) and dispatches.
     * @param text Raw user input to test for a slash command.
     * @returns true iff the text was recognised as a slash command
     *          AND was handled. Caller short-circuits its LLM
     *          dispatch on true; false means ordinary chat text.
     */
    bool tryHandle(const QString& text);

    /**
     * @brief Legacy entry point with caller-provided context. Used by
     *        the unit test (which injects closures rather than
     *        wiring real services). The production `tryHandle`
     *        overload above calls this internally after building the
     *        context from its own state.
     * @param ctx Pre-built dispatch context.
     * @returns true iff the input was handled as a slash command.
     * @complexity O(n) in tool count for `/showtools` filter variants.
     */
    bool tryHandle(const SlashCommandContext& ctx);

    /**
     * @brief Machine-readable catalogue of the user-typeable slash
     *        commands, for the composer's autocomplete picker.
     *
     * Each entry is a QVariantMap with three string keys:
     *   - `name`: the command token, leading slash included
     *                 (e.g. `"/compact"`);
     *   - `summary`: a single-line description suitable for a
     *                 dropdown row;
     *   - `usage`: the invocation form including any argument
     *                 (e.g. `"/flashmemory confirm"`); equals `name`
     *                 for argument-less commands.
     *
     * The returned list and `tryHandle`'s dispatch arms read from one
     * shared static table (see kCommandCatalogue in the .cpp), so the
     * picker can never drift from what is actually handled.
     *
     * @returns One map per user-facing command. Stable order: the
     *          order a `/help`-style listing would use.
     * @complexity O(n) in the command count (small constant).
     */
    Q_INVOKABLE QVariantList availableCommands() const;

  private:
    /** Build the per-call context from current setter state. */
    SlashCommandContext buildContext(const QString& text) const;

    ConversationService* m_convSvc = nullptr;
    MessageService* m_msgSvc = nullptr;
    FileService* m_fileSvc = nullptr;
    ToolService* m_toolSvc = nullptr;
    MessageListModel* m_msgModel = nullptr;
    ConversationSummarizer* m_summarizer = nullptr;
    std::function<QString()> m_activeConvIdGetter;
};

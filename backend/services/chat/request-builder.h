// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file request-builder.h
 * @brief ChatController collaborator that assembles the LlmRequest
 *        payload dispatched to ILLMProvider.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core, ConversationService, MessageService,
 *               ModelRouter (non-owning references),
 *               api/llm-interface.h, models/message.h,
 *               models/tool-call.h, services/history-budgeter.h,
 *               and (via BuildRequestInputs) non-owning pointers
 *               to AgentRegistry, FileService, MembershipService,
 *               PlanService, RagService, ToolService, TaskObserver.
 *
 * Two public entry points:
 *
 *   1. assembleHistory: walk-and-pair reconstruction of a
 *      well-formed OpenAI-style tool-call history from the persisted
 *      messages + tool_calls side table.
 *
 *   2. buildRequest: full LlmRequest composition, covering the layered system
 *      prompt (org / project context → team roster → agent role →
 *      conversation override → group-chat contract → task-system
 *      trigger → time layer → active-task framing → cross-chat
 *      awareness), RAG augmentation, tool-list filtering, conversation-
 *      level config finalisation, history budgeter + history assembly,
 *      cascade-tail user-nudge, pending attachment + file-context
 *      attach. Error paths (conversation lookup fail, model not
 *      selected, history exceeds budget) propagate via
 *      BuildResult::success=false + errorReason.
 *
 * Ownership: constructed in ChatController's constructor via
 *   m_requestBuilder = std::make_unique<Chat::RequestBuilder>(
 *       m_convSvc, m_msgSvc, m_router, this);
 * Optional services (AgentRegistry, PlanService, RagService, …)
 * thread through each call via BuildRequestInputs as nullable
 * pointers. Holding them as members would force this class to mirror
 * every ChatController set*() call and invite staleness.
 *
 * Threading: strictly main-thread. Every public method begins with
 *   VERZETA_ASSERT_MAIN_THREAD();
 * No shared mutable state across threads; no mutexes inside this
 * class.
 */

#pragma once

#include "../../api/llm-interface.h"  // for LlmRequest, LlmMessage, LlmImageData

#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>

// Forward declarations — full includes are in the .cpp.
class AgentRegistry;
class ConversationService;
class FileService;
class HeartbeatConfigService;
class HistoryBudgeter;
class MembershipService;
class MessageService;
class ModelRouter;
class PlanService;
class PollService;
class RagService;
class SkillService;
class TaskObserver;
class ToolService;
struct Message;

class ConversationSummarizer;

namespace Chat {

/**
 * @brief Per-call inputs for RequestBuilder::buildRequest.
 *
 * Every field that RequestBuilder reads from ChatController's member
 * state is passed explicitly here. This makes the build function a
 * pure-ish transformation of its inputs: same inputs → same output,
 * no staleness from missed setter updates, no hidden dependencies.
 *
 * Service pointers are non-owning and nullable, with the same nullability
 * semantics ChatController applies today (services are attached via
 * set*() methods after construction, so they may be null when build
 * runs before they're wired). Each consumer inside buildRequest
 * checks for null before use.
 */
struct BuildRequestInputs {
    // -----------------------------------------------------------------
    // Services (non-owning, nullable). Caller snapshots pointers
    // from ChatController's m_* fields at send time.
    // -----------------------------------------------------------------
    AgentRegistry* agentRegistry = nullptr;          ///< Resolves the responding agent.
    FileService* fileService = nullptr;              ///< Lists project documents.
    MembershipService* membershipService = nullptr;  ///< Resolves the member roster.
    PlanService* planService = nullptr;              ///< Loads the active task plan.
    RagService* ragService = nullptr;                ///< Not read by the builder.
    SkillService* skillService = nullptr;            ///< Resolves listed skills.
    ToolService* toolService = nullptr;              ///< Supplies tool schemas.
    TaskObserver* taskObserver = nullptr;            ///< Supplies recent task activity.
    /// Looks up the responder's heartbeat configuration for the prompt.
    HeartbeatConfigService* heartbeatConfigService = nullptr;
    /// When attached, the builder appends an ACTIVE POLLS layer listing
    /// the open polls in the active conversation. Null disables it.
    PollService* pollService = nullptr;

    /// Preferred skill ids for this conversation, resolved by the caller
    /// through SkillService before buildRequest. An empty list omits the
    /// AVAILABLE SKILLS layer.
    QStringList resolvedPreferredSkillIds;
    /// When true, the listed skills are presented as the only skills
    /// available for this scope, with no pointer to the wider library.
    /// When false, they are presented as preferred and the prompt points
    /// at discover_skills for other approved skills; with no preferred
    /// list, a short pointer is added when approved skills exist.
    bool skillsExposeOnly = false;

    // -----------------------------------------------------------------
    // Identity / request-scoped state
    // -----------------------------------------------------------------
    /** UI-active conversation id, used for folderChainForConversation
     *  lookups (project/org context) and conversationMembers lookups
     *  (group-chat roster). */
    QString activeConvId;
    /** Snapshot conversation id for this in-flight request. Set by
     *  ChatController BEFORE calling buildRequest; stays stable across
     *  the request even if the user switches conversations mid-turn. */
    QString inflightConvId;
    /** Monotonic request id, already bumped by ChatController. Stamped
     *  onto the returned LlmRequest so downstream slots can gate stale
     *  chunks. */
    quint64 requestId = 0;
    /** Id of the streaming placeholder message (if any), passed to
     *  assembleHistory so the placeholder isn't emitted as an empty
     *  assistant row. */
    QString streamingMsgId;

    // -----------------------------------------------------------------
    // Cascade / group-chat state
    // -----------------------------------------------------------------
    /** Current cascade depth. Logged for diagnostics; not used in
     *  prompt composition directly. */
    int cascadeIterations = 0;
    /// Agent id of the member responding this turn. Empty falls back to
    /// the conversation's primary agent.
    QString responseMemberAgentId;
    /// Alias of the member responding this turn. Group chats use it to
    /// find the responder's member entry; 1:1 project chats use the
    /// conversation's bound member alias instead.
    QString responseMemberAlias;

    // -----------------------------------------------------------------
    // Task state (active-plan anchor — see TaskGateController).
    // -----------------------------------------------------------------
    /** When non-empty, ACTIVE TASK framing is appended to the system
     *  prompt, and buildRequest may *return* a request to clear this
     *  field (via BuildResult::clearActiveTaskPlanId) if the plan
     *  turns out to be terminal or missing. */
    QString activeTaskPlanId;

    // -----------------------------------------------------------------
    // Input state
    // -----------------------------------------------------------------
    /** Original user text, used by RAG augmentation (NOT the last
     *  message in the assembled history, which is post-processed). */
    QString lastUserText;
    /** Pre-formatted RAG context block (RagService::formatChunksAsContext),
     *  pre-fetched ASYNCHRONOUSLY off the turn path by ConversationRun and
     *  appended to the composed system prompt when non-empty. Empty = no
     *  augmentation. buildRequest never embeds/retrieves synchronously. */
    QString ragContext;
    /** Pending file-context string to append to the last user message
     *  (set by the attach-file flow). */
    QString pendingFileContext;
    /** Pending image attachments to attach to the last user message.
     *  Same type as ChatController::m_pendingImages. */
    QList<LlmImageData> pendingImages;
    /** Session-level tools toggle (the chat input pill). When false,
     *  req.availableTools is left empty even if a ToolService is
     *  attached. */
    bool toolsEnabled = true;

    // -----------------------------------------------------------------
    // Constants echoed from ChatController. Passed per-call instead
    // of #include'd from chat-controller.h to avoid a cyclic include
    // (ChatController includes this header). Caller fills from its
    // own private constant.
    // -----------------------------------------------------------------
    /** ChatController::kMaxAgentCascade, used in the group-chat
     *  contract text. */
    int maxAgentCascade = 10;

    /** Echo-recovery retry attempt counter. Zero on the responder's
     *  first attempt; 1, 2, 3 on successive retries triggered when
     *  CascadeController detected a near-verbatim copy or empty
     *  reply. RequestBuilder appends a progressively stronger anti-
     *  echo cascade-tail nudge based on this value. Reset to zero
     *  on a successful (non-echoed) reply or when the cascade moves
     *  to the next member. */
    int echoRetryAttempt = 0;

    /** Bad-reply class for the current retry ("echo" | "empty"; empty
     *  when not a retry). Selects the corrective anti-echo nudge text.
     *  The sampling jitter applies to both retry classes equally, since each
     *  is a symptom of the model settling into a bad attractor that
     *  deterministic re-sampling cannot escape. */
    QString retryReason;

    /** True when this turn is the continuation dispatched right
     *  after a tool batch completed. In a 1:1 chat the group cascade-tail
     *  nudge never fires, so RequestBuilder uses this to append a gentle
     *  "the tool result is ready; keep going" cue, reducing gemma's
     *  reflexive bare-EOS-after-tool. Default false (fresh user turns). */
    bool isPostToolContinuation = false;

    /** True on the
     *  NON-DESTRUCTIVE continuation turn dispatched after an LLM confirmed
     *  a deferred action (the responder announced a file/tool action then
     *  stopped without calling the tool). RequestBuilder appends a firm
     *  "run the tool now to do what you described, or say if you genuinely
     *  can't" cue, alias-addressed in group chats, with a hand-off escape
     *  so an impossible ask cannot loop. Default false. */
    bool isDeferredActionContinuation = false;

    /** True only on the turn dispatched after an ASYNC image generation
     *  completed while the conversation was idle (read-then-clear in
     *  ConversationRun::makeBuildInputs). Appends a final user-role
     *  follow-through telling the requesting agent its image is ready;
     *  the generated image rides `pendingImages` on that same message
     *  so the agent actually SEES its own output once (vision models),
     *  without ever replaying pixels through history. */
    bool isAsyncArtifactContinuation = false;

    /** Dynamic-compaction service (non-owning;
     *  nullable). When set and the conversation qualifies, the
     *  builder fire-and-forgets generateAsync and injects the fresh
     *  summary at context head. Null = compaction inactive
     *  (tests, headless builds). */
    ::ConversationSummarizer* summarizer = nullptr;

    QString canvasFilename;     ///< Active canvas file name; empty when none.
    QString canvasLanguage;     ///< Active canvas language.
    int canvasRevision = 0;     ///< Active canvas revision number.
    int canvasLineCount = 0;    ///< Active canvas line count.
    qint64 canvasByteSize = 0;  ///< Active canvas size in bytes.
};

/**
 * @brief Outcome of RequestBuilder::buildRequest.
 *
 * On success: `request` is a fully-populated LlmRequest ready to
 * hand to ModelRouter::route; `consumedAttachments` tells the caller
 * whether to clear ChatController's m_pendingImages /
 * m_pendingFileContext; `clearActiveTaskPlanId` tells the caller
 * whether to reset m_activeTaskPlanId (the build detected the plan
 * went terminal or missing).
 *
 * On failure: `success=false`, `errorReason` populated with a
 * user-readable message. ChatController emits errorOccurred(reason)
 * and calls onRequestError(requestId, reason), the same signal
 * surface it used when this logic lived inline in buildAndSendRequest.
 */
struct BuildResult {
    LlmRequest request;    ///< The built request.
    bool success = false;  ///< Whether the build succeeded.
    QString errorReason;   ///< User-readable reason on failure.
    /// True when the caller should clear its pending images and file
    /// context, because this request carries them.
    bool consumedAttachments = false;
    /// True when the active task plan went terminal or missing and the
    /// caller should reset it.
    bool clearActiveTaskPlanId = false;

    /** Context-window fill at build time, 0-100:
     *  (system + tools + summary + included history tokens) /
     *  contextWindow. Surfaced on ChatController for the chat-input
     *  compaction indicator (inner gauge ring = context pressure). */
    int contextFillPercent = 0;

    /** Cadence progress toward the next memory refresh: assistant turns
     *  accumulated since the current summary's coverage (`Used`) and the
     *  conversation's compactEveryTurns (`Total`; 0 = cadence off). Drives
     *  the OUTER gauge ring. */
    int compactionTurnsUsed = 0;
    int compactionTurnsTotal = 0;  ///< compactEveryTurns; 0 means off.

    /** True when this turn's state meets the compaction trigger
     *  (ConversationSummarizer::shouldSummarize). The summary is NOT
     *  dispatched from buildRequest, because doing so fired it concurrently with
     *  the turn about to be sent, saturating a single local provider and
     *  timing out RAGP. ConversationRun instead dispatches it in the idle
     *  gap after the cascade completes (or immediately when context is
     *  already critical), keeping the summary off the foreground path. */
    bool shouldSummarize = false;
};


/**
 * @brief Inputs for buildSurfaceReviewRequest.
 *
 *        The Tier-2 review call asks the PARENT agent (same id, same model
 *        as the agent that owns the heartbeat config) to look at its
 *        background routine's report and decide POST vs SKIP. The agent's
 *        own systemPrompt is preserved as the system role; the review
 *        framing (criteria + recent conv + the report + decision rules)
 *        is carried in a single user-role message at the end.
 *
 *        This struct is intentionally self-contained: it does NOT
 *        reuse BuildRequestInputs (which is shaped for chat-controller
 *        regular turns and would force every caller to populate fields
 *        the review path does not use). The only services it needs are
 *        already accessible via the RequestBuilder ctor; the inputs
 *        below are pure values.
 */
struct SurfaceReviewInputs {
    /** Agent being asked to review its own report. */
    QString agentId;
    QString agentName;          ///< Display name of the reviewing agent.
    QString agentSystemPrompt;  ///< System prompt of the reviewing agent.

    /** The heartbeat_configs row that produced the report. Used for
     *  goal + surfaceCriteria fallback (when surfaceCriteria is
     *  empty, the goal stands in). */
    QString goal;
    QString surfaceCriteria;  ///< When to post; empty falls back to goal.
    QString alias;            ///< "" for 1:1; non-empty for group/folder.

    /** The Tier-1 heartbeat_reports row this review covers. */
    QString reportId;
    QString reportTitle;    ///< TITLE line of the report.
    QString reportBody;     ///< RESULTS section of the report.
    QString reportSummary;  ///< SUMMARY section of the report.

    /** Target conversation (for context). The recent-messages list is
     *  computed by the caller via MessageService::getRecentMessages. */
    QString targetConvId;
    QList<LlmMessage> recentMessages;  ///< Most recent target messages, oldest first.

    /** Provider/model for the LLM call. The caller (HeartbeatSubagentService)
     *  resolves these through the same per-agent provider override that
     *  Tier-1 runs use (resolveProviderModelForAgent). */
    QString providerId;
    QString modelName;  ///< Model for the review call.
};

/**
 * @brief Build the Tier-2 surface-review LlmRequest.
 *
 *        On success: `request` carries the agent's system prompt + the
 *        recent conversation history + a final user-role message with
 *        the review framing (criteria + the structured report + the
 *        POST-vs-[SKIP] decision rules). The caller dispatches via
 *        ModelRouter::routeBackground and parses the model's response
 *        for the [SKIP] sentinel.
 *
 *        On failure: `success=false`, `errorReason` populated. The
 *        review path treats this as an error outcome on the report
 *        row and skips the cascade.
 *
 * @complexity O(N) in input length (history copy + prompt assembly).
 * @sideeffects None; pure function over its inputs.
 * @param inputs The reviewing agent, its heartbeat configuration, the
 *               report under review, the target conversation's recent
 *               messages, and the provider and model to call.
 * @returns On success, a BuildResult whose request is ready to dispatch;
 *          on failure, success is false and errorReason says why.
 */
BuildResult buildSurfaceReviewRequest(const SurfaceReviewInputs& inputs);

/**
 * @brief Pure-ish QObject collaborator owned by ChatController.
 *        Does NOT expose Q_PROPERTY / Q_INVOKABLE; all QML-facing
 *        surface stays on ChatController (facade-preservation
 *        invariant).
 */
class RequestBuilder : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the request builder.
     * @param convSvc  Non-owning reference to the ConversationService
     *                 (for folder-chain + conversation lookups).
     * @param msgSvc   Non-owning reference to the MessageService
     *                 (for message + tool-call row reads).
     * @param router   Non-owning reference to the ModelRouter
     *                 (for the fallback active provider/model when
     *                 the conversation config is empty).
     * @param parent   Qt parent (ChatController). Qt auto-deletes
     *                 children via the parent-chain as a backup
     *                 behind the std::unique_ptr member cleanup.
     *
     * All three service references MUST outlive this RequestBuilder;
     * ChatController owns all three AND the RequestBuilder, so the
     * invariant holds trivially.
     */
    explicit RequestBuilder(ConversationService& convSvc,
                            MessageService& msgSvc,
                            ModelRouter& router,
                            QObject* parent = nullptr);

    ~RequestBuilder() override;

    /**
     * @brief Walk-and-pair reconstruction of a well-formed LLM
     *        history from the persisted messages + tool_calls side
     *        table. See the extended design comment in the .cpp for
     *        the OpenAI tool-call-pairing contract this function
     *        enforces.
     *
     * @param dbMessages  Chronologically-sorted conversation rows.
     * @param isGroupChat Enables "(Alias said)\n" prefix.
     * @param excludeMsgId Optional streaming-placeholder id to omit.
     * @return Fresh QList<LlmMessage> ready to drop into
     *         LlmRequest::messages.
     * @complexity O(n) in `dbMessages` + O(k) tool-calls lookups.
     * @sideeffects None.
     */
    QList<LlmMessage> assembleHistory(const QList<Message>& dbMessages,
                                      bool isGroupChat,
                                      const QString& excludeMsgId);

    /**
     * @brief In-place compaction of verbatim tool payloads in an assembled
     *        history so they cannot fill the window and starve the reply.
     *
     * The function visits each tool-call group from most recent to oldest and
     * replaces historical file-body arguments and verbatim tool results with
     * faithful one-line references built by Chat::ToolPayloadDigest. The most
     * recent group is kept verbatim because the model is reacting to it this
     * turn, unless one of its payloads exceeds the per-payload ceiling. Across
     * the rest, kept-verbatim payloads may consume at most a fixed share of
     * historyBudgetTokens; older groups beyond that share are compacted. Any
     * single payload over the per-payload ceiling is compacted even when it is
     * the most recent, and a byte-identical payload already kept by a more
     * recent group is compacted as well so the same file written twice is not
     * stored in full twice.
     *
     * Pairing is preserved: only argument and result BODIES change; tool-call
     * ids, tool_call_id pairing, and group counts are untouched. Full content
     * stays in the database and remains re-fetchable via read_file /
     * read_canvas / search_messages.
     *
     * @param msgs                History to compact in place.
     * @param historyBudgetTokens The token budget history was selected for;
     *                            the verbatim share is a fraction of it.
     * @sideeffects Mutates @p msgs bodies only. Pure w.r.t. the DB.
     */
    static void digestToolPayloads(QList<LlmMessage>& msgs, int historyBudgetTokens);

    /**
     * @brief Guarantee the assembled request leaves real room to reply.
     *
     * Measures the ACTUAL assembled prompt (system prompt + tool schemas +
     * the post-digest messages, using the content-class estimator that counts
     * tool-call args and tool-result bodies as dense) and ensures
     * `estPrompt + HistoryBudgeter::kRealOutputFloor` fits the context window.
     * If it does not: when @p allowGrow is true it grows the window toward the
     * 32768 auto-ceiling (Ollama num_ctx). Otherwise (and after growth, if a
     * single turn still overflows even the ceiling) it sheds the oldest
     * droppable history groups (whole assistant+tool groups, never the leading
     * summary or the newest user turn) until it fits or only those remain.
     * This is the single post-assembly enforcement point that closes the
     * "prompt packed to the ceiling, no output room" failure (done_reason
     * "length", empty reply).
     *
     * @param req      Request to shape in place (mutates config.contextWindow
     *                 and, only as a last resort, messages).
     * @param sysTok   Estimated tokens of the final system prompt.
     * @param toolsTok Estimated tokens of the tool-schema block.
     * @param allowGrow True iff the window may grow (no explicit user override).
     * @returns The real post-shape context-fill percentage (0–100) for the
     *          chat-input gauge.
     * @sideeffects Mutates @p req. Pure w.r.t. the DB.
     */
    static int shapeRequestToWindow(LlmRequest& req, int sysTok, int toolsTok, bool allowGrow);

    /**
     * @brief Compose the full LlmRequest for dispatch via
     *        ModelRouter::route.
     *
     *        Side effects that live on the caller (ChatController)
     *        are NOT performed here:
     *          - m_inflightConvId assignment
     *          - m_currentRequestId bump
     *          - the m_router.route(req) call
     *          - clearing pending attachments
     *
     *        The active-task stale-plan self-repair does not mutate
     *        ChatController state directly; it reports back via
     *        BuildResult::clearActiveTaskPlanId so the caller owns
     *        the reset.
     *
     *        Error paths (conversation lookup fail, no model
     *        configured, history exceeds budget) return
     *        success=false + errorReason. The caller translates
     *        those into errorOccurred / onRequestError signals.
     *
     * @param inputs Fully-populated BuildRequestInputs.
     * @return BuildResult with either a ready-to-send LlmRequest
     *         (success=true) or an errorReason (success=false).
     *         Never throws.
     * @complexity O(n) in history size + O(m) in tools count where
     *             n = budgeted message count, m = registered tools.
     * @sideeffects Reads conv / msg / plan / membership / agent
     *              services; writes nothing to ChatController state.
     */
    BuildResult buildRequest(const BuildRequestInputs& inputs);

  private:
    // Non-owning; ChatController owns each of these and outlives us.
    ConversationService& m_convSvc;
    MessageService& m_msgSvc;
    ModelRouter& m_router;
};

}  // namespace Chat

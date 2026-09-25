// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file activity-event.h
 * @brief Immutable POD for one row of the `activity_log` audit table.
 *
 *        Carries the 13 columns of the audit-log schema. Factories live
 *        below; AuditService is the single write entry point.
 * @layer Data Access (POD model)
 * @dependencies Qt6::Core only (QString, QDateTime, QJsonObject).
 */


#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

/**
 * @brief One append-only row of `activity_log`.
 *
 * Each field maps to the activity_log column of the same name in
 * snake_case.
 *
 * Factories per event type live below. Every factory produces a fully-
 * valid event (id minted, createdAt stamped) so each recording-hook
 * call site is a single `record(...)` call with zero per-call
 * construction logic:
 *
 *   if (m_auditService) {
 *       m_auditService->record(ActivityEvent::forPollCreated(
 *           convId, pollId, creatorKind, creatorAlias, question));
 *   }
 */
struct ActivityEvent {
    QString id;               ///< UUID primary key.
    QDateTime createdAt;      ///< UTC time with milliseconds.
    QString projectFolderId;  ///< Empty for app-level events.
    QString conversationId;   ///< Empty for app-level events.
    /// Groups an agent turn with its tool calls and polls into one
    /// expandable timeline entry; the same id as messages.turn_id.
    QString turnId;
    /// "user", "agent", "system" or "client". "client" marks actions
    /// by a paired wire client, such as workspace mount changes, so the
    /// timeline can tell them apart from local user actions.
    QString actorKind;
    /// Agent alias, "user" for the human, empty for the system, or the
    /// client's owner label for client actions.
    QString actorAlias;
    QString actorAgentId;   ///< Agent template id when actorKind is "agent".
    QString actorClientId;  ///< Wire client UUID; empty for local actions.
    /// Stable event name: agent_turn, image_generated, tool_invoked,
    /// poll_created, poll_vote, poll_closed, member_added,
    /// member_removed, canvas_edited, file_written, permission_denied,
    /// deliverable_produced, or one of the workspace.mount.* events
    /// (registered, unregistered, replaced, stale, tree_updated,
    /// tier_changed).
    QString eventType;
    QString toolName;      ///< Tool that ran, when the event is a tool call.
    QString eventSummary;  ///< One human-readable line shown in the timeline.
    /// Optional JSON detail: a before and after diff, tool argument
    /// summary, provider and model, or denial reason. Never secrets.
    QJsonObject eventDetail;

    /**
     * @brief Reports whether the row is well-formed enough for persistence.
     * @returns True iff `id`, `actorKind`, `eventType`, and
     *          `eventSummary` are all non-empty.
     *
     * AuditService::record refuses to write an invalid event so a hook
     * that forgot a required field cannot insert a half-row.
     */
    bool isValid() const {
        return !id.isEmpty() && !actorKind.isEmpty() && !eventType.isEmpty() &&
               !eventSummary.isEmpty();
    }


    /**
     * @brief Builds an `agent_turn` event recorded by ChatController on
     *        every cascade-member turn-end.
     * @param projectFolderId  Owning project folder UUID, or empty for un-scoped.
     * @param conversationId   Conversation UUID the turn ran in.
     * @param turnId           Cascade turn id grouping turn + tool calls + poll.
     * @param actorAlias       Responder alias.
     * @param actorAgentId     Agent template id of the responder.
     * @param actorClientId    Wire client UUID, or empty for the local frontend.
     * @param providerId       Provider used (e.g. "openai", "anthropic").
     * @param modelName        Model id used for the turn.
     * @param finishReason     Final finish_reason ("stop", "tool_calls", etc.).
     * @param totalTokens      Estimated input + output token count.
     * @param elapsedMs        Wall-clock time the turn spent in flight.
     * @returns A fully-valid ActivityEvent (id minted, createdAt stamped).
     *
     * The provider + model + finish reason are folded into `eventDetail`.
     */
    static ActivityEvent forAgentTurn(const QString& projectFolderId,
                                      const QString& conversationId,
                                      const QString& turnId,
                                      const QString& actorAlias,
                                      const QString& actorAgentId,
                                      const QString& actorClientId,
                                      const QString& providerId,
                                      const QString& modelName,
                                      const QString& finishReason,
                                      int totalTokens,
                                      qint64 elapsedMs);

    /**
     * @brief Builds an `image_generated` event recorded by
     *        ImageService::onImageReady from the echoed JobContext.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID the generation belongs to.
     * @param jobId            Image generation job id.
     * @param actorAlias       Alias of the agent that requested the image.
     * @param actorAgentId     Agent template id of the requester.
     * @param prompt           Generation prompt text.
     * @param localPath        Absolute on-disk path of the saved image.
     * @returns A fully-valid ActivityEvent.
     *
     * `actorClientId` is empty for local-frontend-initiated generations.
     */
    static ActivityEvent forImageGenerated(const QString& projectFolderId,
                                           const QString& conversationId,
                                           const QString& jobId,
                                           const QString& actorAlias,
                                           const QString& actorAgentId,
                                           const QString& prompt,
                                           const QString& localPath);

    /**
     * @brief Builds a `tool_invoked` event recorded by ToolDispatcher
     *        when a tool batch completes.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID the tool ran in.
     * @param turnId           Cascade turn id the tool batch belongs to.
     * @param actorAlias       Alias of the agent that issued the tool call.
     * @param actorAgentId     Agent template id of the caller.
     * @param actorClientId    Wire client UUID, or empty for local.
     * @param toolName         Canonical tool name (e.g. "open_canvas").
     * @param argsSummary      Short human-readable arg summary.
     * @param status           Final status ("ok", "error", "denied").
     * @returns A fully-valid ActivityEvent with `actorKind` = "agent".
     */
    static ActivityEvent forToolInvoked(const QString& projectFolderId,
                                        const QString& conversationId,
                                        const QString& turnId,
                                        const QString& actorAlias,
                                        const QString& actorAgentId,
                                        const QString& actorClientId,
                                        const QString& toolName,
                                        const QString& argsSummary,
                                        const QString& status);

    /**
     * @brief Builds a `poll_created` event recorded by PollService::createPoll.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID the poll belongs to.
     * @param pollId           Poll UUID.
     * @param creatorKind      Actor kind ("user" | "agent").
     * @param creatorAlias     Alias of the creator.
     * @param creatorAgentId   Agent template id when creator is an agent.
     * @param question         Poll question text.
     * @param mode             Poll mode ("single_choice" | "multi_choice").
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forPollCreated(const QString& projectFolderId,
                                        const QString& conversationId,
                                        const QString& pollId,
                                        const QString& creatorKind,
                                        const QString& creatorAlias,
                                        const QString& creatorAgentId,
                                        const QString& question,
                                        const QString& mode);

    /**
     * @brief Builds a `poll_vote` event recorded by PollService::castVote.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID the poll belongs to.
     * @param pollId           Poll UUID.
     * @param voterKind        Actor kind ("user" | "agent").
     * @param voterAlias       Alias of the voter.
     * @param voterAgentId     Agent template id when voter is an agent.
     * @param optionText       Selected option text.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forPollVote(const QString& projectFolderId,
                                     const QString& conversationId,
                                     const QString& pollId,
                                     const QString& voterKind,
                                     const QString& voterAlias,
                                     const QString& voterAgentId,
                                     const QString& optionText);

    /**
     * @brief Builds a `poll_closed` event recorded by PollService::closePoll
     *        and by the lazy auto-close path inside `pollResults`.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID the poll belongs to.
     * @param pollId           Poll UUID.
     * @param closerKind       Actor kind that triggered the close.
     * @param closerAlias      Alias of the closer.
     * @param closerAgentId    Agent template id when closer is an agent.
     * @param winningOption    Resolved winning option text, or empty on tie.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forPollClosed(const QString& projectFolderId,
                                       const QString& conversationId,
                                       const QString& pollId,
                                       const QString& closerKind,
                                       const QString& closerAlias,
                                       const QString& closerAgentId,
                                       const QString& winningOption);

    /**
     * @brief Builds a `member_added` event recorded by MembershipService
     *        when a member is added to a project folder or conversation.
     * @param projectFolderId  Owning project folder UUID, or empty for
     *                         conversation-scoped membership only.
     * @param conversationId   Conversation UUID, or empty for project-only.
     * @param addedAlias       Alias of the added member.
     * @param addedAgentId     Agent template id of the added member.
     * @param addedByKind      Actor kind that performed the add.
     * @param addedByAlias     Alias of the actor that performed the add.
     * @param addedByAgentId   Agent template id of that actor, when agent.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forMemberAdded(const QString& projectFolderId,
                                        const QString& conversationId,
                                        const QString& addedAlias,
                                        const QString& addedAgentId,
                                        const QString& addedByKind,
                                        const QString& addedByAlias,
                                        const QString& addedByAgentId);

    /**
     * @brief Builds a `member_removed` event recorded by MembershipService
     *        when a member is removed from a project folder or conversation.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID, or empty.
     * @param removedAlias     Alias of the removed member.
     * @param removedAgentId   Agent template id of the removed member.
     * @param removedByKind    Actor kind that performed the removal.
     * @param removedByAlias   Alias of the actor that performed the removal.
     * @param removedByAgentId Agent template id of that actor, when agent.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forMemberRemoved(const QString& projectFolderId,
                                          const QString& conversationId,
                                          const QString& removedAlias,
                                          const QString& removedAgentId,
                                          const QString& removedByKind,
                                          const QString& removedByAlias,
                                          const QString& removedByAgentId);

    /**
     * @brief Builds a `canvas_edited` event recorded by CanvasService
     *        on `editCanvas` and `openCanvas`.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID the canvas belongs to.
     * @param turnId           Cascade turn id the edit happened in.
     * @param actorAlias       Alias of the agent / user that edited.
     * @param actorAgentId     Agent template id of the actor when agent.
     * @param actorClientId    Wire client UUID, or empty for local.
     * @param canvasFilename   Canvas filename being edited.
     * @param newRevision      Revision number after the edit.
     * @param newLines         Total line count after the edit.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forCanvasEdited(const QString& projectFolderId,
                                         const QString& conversationId,
                                         const QString& turnId,
                                         const QString& actorAlias,
                                         const QString& actorAgentId,
                                         const QString& actorClientId,
                                         const QString& canvasFilename,
                                         int newRevision,
                                         int newLines);

    /**
     * @brief Builds a `file_written` event recorded by FileService
     *        via the existing fileSaved signal subscription.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID, or empty.
     * @param turnId           Cascade turn id the write happened in.
     * @param actorAlias       Alias of the agent / user that wrote.
     * @param actorAgentId     Agent template id of the actor when agent.
     * @param actorClientId    Wire client UUID, or empty for local.
     * @param absolutePath     Absolute on-disk path written.
     * @param bytesWritten     Number of bytes the file ended up with.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forFileWritten(const QString& projectFolderId,
                                        const QString& conversationId,
                                        const QString& turnId,
                                        const QString& actorAlias,
                                        const QString& actorAgentId,
                                        const QString& actorClientId,
                                        const QString& absolutePath,
                                        qint64 bytesWritten);

    /**
     * @brief Builds a `permission_denied` event recorded by
     *        PermissionService when a capability check denies an action.
     * @param projectFolderId  Owning project folder UUID, or empty.
     * @param conversationId   Conversation UUID the denial happened in.
     * @param actorKind        Actor kind that requested the capability.
     * @param actorAlias       Alias of the requesting actor.
     * @param actorAgentId     Agent template id when actor is an agent.
     * @param actorClientId    Wire client UUID, or empty for local.
     * @param capability       Capability name that was denied.
     * @param reason           Human-readable reason for the denial.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forPermissionDenied(const QString& projectFolderId,
                                             const QString& conversationId,
                                             const QString& actorKind,
                                             const QString& actorAlias,
                                             const QString& actorAgentId,
                                             const QString& actorClientId,
                                             const QString& capability,
                                             const QString& reason);

    /**
     * @brief Builds a `deliverable_produced` event recorded by the
     *        deliverable-produced flow when an agent finalises a canvas
     *        as a project deliverable.
     * @param projectFolderId    Owning project folder UUID.
     * @param conversationId     Conversation UUID the deliverable came from.
     * @param actorAlias         Alias of the producing agent.
     * @param actorAgentId       Agent template id of the producer.
     * @param canvasId           Source canvas artifact UUID.
     * @param deliverableTitle   Human-readable title for the deliverable.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forDeliverableProduced(const QString& projectFolderId,
                                                const QString& conversationId,
                                                const QString& actorAlias,
                                                const QString& actorAgentId,
                                                const QString& canvasId,
                                                const QString& deliverableTitle);

    /**
     * @brief Builds a `workspace.mount.registered` event recorded by
     *        FolderMountRegistry when a paired client first attaches
     *        a virtual workspace to a project / organization folder.
     *        `actorKind` is fixed to `client` to disambiguate from
     *        agent / user / system actions in the timeline filter.
     * @param projectFolderId Folder the mount attaches to.
     * @param mountId         Stable client-side workspace UUID.
     * @param clientId        Wire-auth client UUID of the registering owner.
     * @param ownerLabel      Display string ("My Project on VS Code (laptop)").
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forWorkspaceMountRegistered(const QString& projectFolderId,
                                                     const QString& mountId,
                                                     const QString& clientId,
                                                     const QString& ownerLabel);

    /**
     * @brief Builds a `workspace.mount.unregistered` event recorded by
     *        FolderMountRegistry when the registered owner removes
     *        the mount.
     * @param projectFolderId Folder the mount was bound to.
     * @param mountId         Workspace UUID being detached.
     * @param clientId        Wire-auth client UUID performing the removal.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forWorkspaceMountUnregistered(const QString& projectFolderId,
                                                       const QString& mountId,
                                                       const QString& clientId);

    /**
     * @brief Builds a `workspace.mount.replaced` event recorded by
     *        FolderMountRegistry when a new client + workspace pair
     *        displaces an existing mount on the same folder.
     * @param projectFolderId Folder whose mount changed hands.
     * @param oldMountId      Workspace UUID that was displaced.
     * @param oldClientId     Previous owner.
     * @param newMountId      Workspace UUID that took over.
     * @param newClientId     New owner.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forWorkspaceMountReplaced(const QString& projectFolderId,
                                                   const QString& oldMountId,
                                                   const QString& oldClientId,
                                                   const QString& newMountId,
                                                   const QString& newClientId);

    /**
     * @brief Builds a `workspace.mount.tree_updated` event recorded
     *        by FolderMountRegistry on a coalesced top-level-
     *        structural-change push, an explicit user refresh, or a
     *        client reconnect re-register. Rare event by contract.
     * @param projectFolderId Folder whose mount was refreshed.
     * @param mountId         Workspace UUID whose manifest was replaced.
     * @param clientId        Wire-auth client UUID owning the mount.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forWorkspaceMountTreeUpdated(const QString& projectFolderId,
                                                      const QString& mountId,
                                                      const QString& clientId);

    /**
     * @brief Builds a `workspace.mount.tier_changed` event recorded
     *        by FolderMountRegistry when the owner flips the advisory
     *        permission tier (ask / smart / bypass).
     * @param projectFolderId Folder whose mount changed tier.
     * @param mountId         Workspace UUID whose tier changed.
     * @param clientId        Wire-auth client UUID owning the mount.
     * @param oldTier         Previous tier string.
     * @param newTier         New tier string.
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forWorkspaceMountTierChanged(const QString& projectFolderId,
                                                      const QString& mountId,
                                                      const QString& clientId,
                                                      const QString& oldTier,
                                                      const QString& newTier);

    /**
     * @brief Builds a `workspace.mount.stale` event recorded by the
     *        FolderMountRegistry's periodic sweeper when a mount's
     *        `last_seen_ms` falls behind the staleness threshold. Fires
     *        once per staleness episode; the registry suppresses
     *        repeats until the mount
     *        is touched again. `actorKind` is `system` because the
     *        sweeper, not a paired client or agent, drives this
     *        event.
     * @param projectFolderId Folder whose mount went stale.
     * @param mountId         Workspace UUID that hasn't been touched.
     * @param clientId        Wire-auth client UUID owning the mount.
     * @param lastSeenMs      Last successful op timestamp (epoch ms).
     * @returns A fully-valid ActivityEvent.
     */
    static ActivityEvent forWorkspaceMountStale(const QString& projectFolderId,
                                                const QString& mountId,
                                                const QString& clientId,
                                                qint64 lastSeenMs);
};

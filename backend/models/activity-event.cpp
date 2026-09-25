// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file activity-event.cpp
 * @brief Implementation of the ActivityEvent factories, one per
 *        `activity_log` event type.
 *
 *        Every factory is a pure function of its arguments: mints a
 *        fresh UUID, stamps now() UTC, populates the actor / event-type
 *        / summary fields, and attaches an event_detail JSON payload
 *        carrying the variable parts.
 * @layer Data Access (POD model)
 * @dependencies Qt6::Core (QUuid, QDateTime, QJsonObject).
 */


#include "activity-event.h"

#include <QJsonValue>
#include <QUuid>

namespace {

QString freshId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QDateTime nowUtc() {
    return QDateTime::currentDateTimeUtc();
}

// Lightweight helper — accepts the JSON pairs the caller supplies and
// returns a finished QJsonObject. Pure transform; no side effects.
QJsonObject detail(std::initializer_list<std::pair<const char*, QJsonValue>> pairs) {
    QJsonObject obj;
    for (const auto& p : pairs) {
        obj.insert(QLatin1String(p.first), p.second);
    }
    return obj;
}

// In a 1:1 chat the cascade carries no responder alias (CascadeController
// alias is empty by design — see ChatController::currentResponderAlias),
// so every agent-actor factory below would otherwise pass an empty
// QString into the `actor_alias` column which has NOT NULL.  Normalise
// at the factory boundary to "assistant" — the same canonical fallback
// every tool that reads `currentResponderAlias()` uses.
QString defaultAgentAlias(const QString& alias) {
    return alias.isEmpty() ? QStringLiteral("assistant") : alias;
}

}  // namespace

ActivityEvent ActivityEvent::forAgentTurn(const QString& projectFolderId,
                                          const QString& conversationId,
                                          const QString& turnId,
                                          const QString& actorAlias,
                                          const QString& actorAgentId,
                                          const QString& actorClientId,
                                          const QString& providerId,
                                          const QString& modelName,
                                          const QString& finishReason,
                                          int totalTokens,
                                          qint64 elapsedMs) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId = turnId;
    e.actorKind = QStringLiteral("agent");
    e.actorAlias = defaultAgentAlias(actorAlias);
    e.actorAgentId = actorAgentId;
    e.actorClientId = actorClientId;
    e.eventType = QStringLiteral("agent_turn");
    e.toolName.clear();
    e.eventSummary = QStringLiteral("@%1 turn (%2)")
                         .arg(e.actorAlias, modelName.isEmpty() ? QStringLiteral("?") : modelName);
    e.eventDetail = detail({
        {"provider_id", providerId},
        {"model_name", modelName},
        {"finish_reason", finishReason},
        {"total_tokens", totalTokens},
        {"elapsed_ms", static_cast<qint64>(elapsedMs)},
    });
    return e;
}

ActivityEvent ActivityEvent::forImageGenerated(const QString& projectFolderId,
                                               const QString& conversationId,
                                               const QString& jobId,
                                               const QString& actorAlias,
                                               const QString& actorAgentId,
                                               const QString& prompt,
                                               const QString& localPath) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId.clear();
    // Image generation is an agent-initiated artifact when an alias is
    // attached (cascade member called generate_image); otherwise the
    // QML "Generate Image" dialog is the source — actor_kind = "user".
    if (!actorAlias.isEmpty()) {
        e.actorKind = QStringLiteral("agent");
        e.actorAlias = actorAlias;
    } else {
        e.actorKind = QStringLiteral("user");
        e.actorAlias = QStringLiteral("user");
    }
    e.actorAgentId = actorAgentId;
    e.actorClientId.clear();
    e.eventType = QStringLiteral("image_generated");
    e.toolName = QStringLiteral("generate_image");
    QString trimmedPrompt = prompt.trimmed();
    if (trimmedPrompt.length() > 80) {
        trimmedPrompt = trimmedPrompt.left(80) + QStringLiteral("…");
    }
    e.eventSummary = trimmedPrompt.isEmpty()
                         ? QStringLiteral("Image generated")
                         : QStringLiteral("Image generated: %1").arg(trimmedPrompt);
    e.eventDetail = detail({
        {"job_id", jobId},
        {"prompt", prompt},
        {"local_path", localPath},
    });
    return e;
}

ActivityEvent ActivityEvent::forToolInvoked(const QString& projectFolderId,
                                            const QString& conversationId,
                                            const QString& turnId,
                                            const QString& actorAlias,
                                            const QString& actorAgentId,
                                            const QString& actorClientId,
                                            const QString& toolName,
                                            const QString& argsSummary,
                                            const QString& status) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId = turnId;
    e.actorKind = QStringLiteral("agent");
    e.actorAlias = defaultAgentAlias(actorAlias);
    e.actorAgentId = actorAgentId;
    e.actorClientId = actorClientId;
    e.eventType = QStringLiteral("tool_invoked");
    e.toolName = toolName;
    e.eventSummary = QStringLiteral("@%1 called %2 (%3)")
                         .arg(e.actorAlias,
                              toolName.isEmpty() ? QStringLiteral("?") : toolName,
                              status.isEmpty() ? QStringLiteral("?") : status);
    e.eventDetail = detail({
        {"args_summary", argsSummary},
        {"status", status},
    });
    return e;
}

ActivityEvent ActivityEvent::forPollCreated(const QString& projectFolderId,
                                            const QString& conversationId,
                                            const QString& pollId,
                                            const QString& creatorKind,
                                            const QString& creatorAlias,
                                            const QString& creatorAgentId,
                                            const QString& question,
                                            const QString& mode) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId.clear();
    e.actorKind = creatorKind;
    e.actorAlias = creatorAlias;
    e.actorAgentId = creatorAgentId;
    e.actorClientId.clear();
    e.eventType = QStringLiteral("poll_created");
    e.toolName = QStringLiteral("start_poll");
    QString shortQ = question.trimmed();
    if (shortQ.length() > 80)
        shortQ = shortQ.left(80) + QStringLiteral("…");
    e.eventSummary =
        QStringLiteral("@%1 opened poll: %2")
            .arg(creatorAlias.isEmpty() ? QStringLiteral("agent") : creatorAlias, shortQ);
    e.eventDetail = detail({
        {"poll_id", pollId},
        {"question", question},
        {"mode", mode},
    });
    return e;
}

ActivityEvent ActivityEvent::forPollVote(const QString& projectFolderId,
                                         const QString& conversationId,
                                         const QString& pollId,
                                         const QString& voterKind,
                                         const QString& voterAlias,
                                         const QString& voterAgentId,
                                         const QString& optionText) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId.clear();
    e.actorKind = voterKind;
    e.actorAlias = voterAlias;
    e.actorAgentId = voterAgentId;
    e.actorClientId.clear();
    e.eventType = QStringLiteral("poll_vote");
    e.toolName = QStringLiteral("cast_vote");
    QString shortOpt = optionText.trimmed();
    if (shortOpt.length() > 60)
        shortOpt = shortOpt.left(60) + QStringLiteral("…");
    e.eventSummary = QStringLiteral("@%1 voted: %2")
                         .arg(voterAlias.isEmpty() ? QStringLiteral("user") : voterAlias, shortOpt);
    e.eventDetail = detail({
        {"poll_id", pollId},
        {"option_text", optionText},
    });
    return e;
}

ActivityEvent ActivityEvent::forPollClosed(const QString& projectFolderId,
                                           const QString& conversationId,
                                           const QString& pollId,
                                           const QString& closerKind,
                                           const QString& closerAlias,
                                           const QString& closerAgentId,
                                           const QString& winningOption) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId.clear();
    e.actorKind = closerKind;
    e.actorAlias = closerAlias;
    e.actorAgentId = closerAgentId;
    e.actorClientId.clear();
    e.eventType = QStringLiteral("poll_closed");
    e.toolName = QStringLiteral("close_poll");
    e.eventSummary = winningOption.isEmpty()
                         ? QStringLiteral("Poll closed (no winning option)")
                         : QStringLiteral("Poll closed → %1").arg(winningOption);
    e.eventDetail = detail({
        {"poll_id", pollId},
        {"winning_option", winningOption},
    });
    return e;
}

ActivityEvent ActivityEvent::forMemberAdded(const QString& projectFolderId,
                                            const QString& conversationId,
                                            const QString& addedAlias,
                                            const QString& addedAgentId,
                                            const QString& addedByKind,
                                            const QString& addedByAlias,
                                            const QString& addedByAgentId) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId.clear();
    e.actorKind = addedByKind;
    // actor_alias is NOT NULL: "user" for the human, the
    // agent's alias for an agent, "" for system. The membership API
    // carries no addedByAlias parameter, so callers pass an empty
    // string — normalise here so the field is never a null QString.
    // A null QString binds as SQL NULL and trips the activity_log
    // .actor_alias NOT NULL constraint (the symptom seen when
    // ProjectTemplateService::createProjectFromTemplate adds members).
    e.actorAlias =
        addedByAlias.isEmpty()
            ? (addedByKind == QStringLiteral("user") ? QStringLiteral("user") : QStringLiteral(""))
            : addedByAlias;
    e.actorAgentId = addedByAgentId;
    e.actorClientId.clear();
    e.eventType = QStringLiteral("member_added");
    e.toolName =
        addedByKind == QStringLiteral("agent") ? QStringLiteral("add_project_member") : QString();
    e.eventSummary =
        QStringLiteral("@%1 added @%2")
            .arg(addedByAlias.isEmpty() ? QStringLiteral("user") : addedByAlias, addedAlias);
    e.eventDetail = detail({
        {"added_alias", addedAlias},
        {"added_agent_id", addedAgentId},
    });
    return e;
}

ActivityEvent ActivityEvent::forMemberRemoved(const QString& projectFolderId,
                                              const QString& conversationId,
                                              const QString& removedAlias,
                                              const QString& removedAgentId,
                                              const QString& removedByKind,
                                              const QString& removedByAlias,
                                              const QString& removedByAgentId) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId.clear();
    e.actorKind = removedByKind;
    // actor_alias is NOT NULL; see forMemberAdded above.
    // removeProjectMember / removeConversationMember record with
    // removedByKind="system" and no alias, so normalise to "" here.
    e.actorAlias = removedByAlias.isEmpty()
                       ? (removedByKind == QStringLiteral("user") ? QStringLiteral("user")
                                                                  : QStringLiteral(""))
                       : removedByAlias;
    e.actorAgentId = removedByAgentId;
    e.actorClientId.clear();
    e.eventType = QStringLiteral("member_removed");
    e.toolName = removedByKind == QStringLiteral("agent") ? QStringLiteral("remove_project_member")
                                                          : QString();
    e.eventSummary =
        QStringLiteral("@%1 removed @%2")
            .arg(removedByAlias.isEmpty() ? QStringLiteral("user") : removedByAlias, removedAlias);
    e.eventDetail = detail({
        {"removed_alias", removedAlias},
        {"removed_agent_id", removedAgentId},
    });
    return e;
}

ActivityEvent ActivityEvent::forCanvasEdited(const QString& projectFolderId,
                                             const QString& conversationId,
                                             const QString& turnId,
                                             const QString& actorAlias,
                                             const QString& actorAgentId,
                                             const QString& actorClientId,
                                             const QString& canvasFilename,
                                             int newRevision,
                                             int newLines) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId = turnId;
    // Canvas can be edited by the agent (edit_canvas tool) OR the
    // user (typing in CanvasEditor). The caller decides actor_kind
    // by passing the alias — "user" alias → user-kind row;
    // otherwise agent.
    if (actorAlias == QStringLiteral("user")) {
        e.actorKind = QStringLiteral("user");
    } else {
        e.actorKind = QStringLiteral("agent");
    }
    e.actorAlias = e.actorKind == QStringLiteral("user") ? QStringLiteral("user")
                                                         : defaultAgentAlias(actorAlias);
    e.actorAgentId = actorAgentId;
    e.actorClientId = actorClientId;
    e.eventType = QStringLiteral("canvas_edited");
    e.toolName = e.actorKind == QStringLiteral("agent") ? QStringLiteral("edit_canvas") : QString();
    e.eventSummary = QStringLiteral("@%1 edited canvas: %2 (rev %3)")
                         .arg(actorAlias.isEmpty() ? QStringLiteral("agent") : actorAlias,
                              canvasFilename,
                              QString::number(newRevision));
    e.eventDetail = detail({
        {"filename", canvasFilename},
        {"revision", newRevision},
        {"lines", newLines},
    });
    return e;
}

ActivityEvent ActivityEvent::forFileWritten(const QString& projectFolderId,
                                            const QString& conversationId,
                                            const QString& turnId,
                                            const QString& actorAlias,
                                            const QString& actorAgentId,
                                            const QString& actorClientId,
                                            const QString& absolutePath,
                                            qint64 bytesWritten) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId = turnId;
    e.actorKind = QStringLiteral("agent");
    e.actorAlias = defaultAgentAlias(actorAlias);
    e.actorAgentId = actorAgentId;
    e.actorClientId = actorClientId;
    e.eventType = QStringLiteral("file_written");
    e.toolName = QStringLiteral("write_file");
    // Bare filename keeps the timeline readable — full path goes in
    // event_detail for diagnostic.
    const int slashAt = absolutePath.lastIndexOf(QLatin1Char('/'));
    const QString baseName = (slashAt >= 0) ? absolutePath.mid(slashAt + 1) : absolutePath;
    e.eventSummary = QStringLiteral("@%1 wrote %2 (%3 bytes)")
                         .arg(e.actorAlias, baseName, QString::number(bytesWritten));
    e.eventDetail = detail({
        {"absolute_path", absolutePath},
        {"bytes", static_cast<qint64>(bytesWritten)},
    });
    return e;
}

ActivityEvent ActivityEvent::forPermissionDenied(const QString& projectFolderId,
                                                 const QString& conversationId,
                                                 const QString& actorKind,
                                                 const QString& actorAlias,
                                                 const QString& actorAgentId,
                                                 const QString& actorClientId,
                                                 const QString& capability,
                                                 const QString& reason) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId.clear();
    e.actorKind = actorKind;
    e.actorAlias = actorAlias;
    e.actorAgentId = actorAgentId;
    e.actorClientId = actorClientId;
    e.eventType = QStringLiteral("permission_denied");
    e.toolName.clear();
    e.eventSummary = QStringLiteral("Denied %1 for @%2")
                         .arg(capability, actorAlias.isEmpty() ? QStringLiteral("?") : actorAlias);
    e.eventDetail = detail({
        {"capability", capability},
        {"reason", reason},
    });
    return e;
}

ActivityEvent ActivityEvent::forDeliverableProduced(const QString& projectFolderId,
                                                    const QString& conversationId,
                                                    const QString& actorAlias,
                                                    const QString& actorAgentId,
                                                    const QString& canvasId,
                                                    const QString& deliverableTitle) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId = conversationId;
    e.turnId.clear();
    e.actorKind = QStringLiteral("agent");
    e.actorAlias = defaultAgentAlias(actorAlias);
    e.actorAgentId = actorAgentId;
    e.actorClientId.clear();
    e.eventType = QStringLiteral("deliverable_produced");
    e.toolName.clear();
    e.eventSummary =
        QStringLiteral("Deliverable produced: %1")
            .arg(deliverableTitle.isEmpty() ? QStringLiteral("(untitled)") : deliverableTitle);
    e.eventDetail = detail({
        {"canvas_id", canvasId},
        {"title", deliverableTitle},
    });
    return e;
}

ActivityEvent ActivityEvent::forWorkspaceMountRegistered(const QString& projectFolderId,
                                                         const QString& mountId,
                                                         const QString& clientId,
                                                         const QString& ownerLabel) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId.clear();
    e.turnId.clear();
    e.actorKind = QStringLiteral("client");
    e.actorAlias = ownerLabel;
    e.actorAgentId.clear();
    e.actorClientId = clientId;
    e.eventType = QStringLiteral("workspace.mount.registered");
    e.toolName.clear();
    e.eventSummary = QStringLiteral("Workspace mount registered: %1")
                         .arg(ownerLabel.isEmpty() ? QStringLiteral("(unnamed)") : ownerLabel);
    e.eventDetail = detail({
        {"mount_id", mountId},
        {"client_id", clientId},
        {"owner_label", ownerLabel},
    });
    return e;
}

ActivityEvent ActivityEvent::forWorkspaceMountUnregistered(const QString& projectFolderId,
                                                           const QString& mountId,
                                                           const QString& clientId) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId.clear();
    e.turnId.clear();
    e.actorKind = QStringLiteral("client");
    e.actorAlias = QStringLiteral("");
    e.actorAgentId.clear();
    e.actorClientId = clientId;
    e.eventType = QStringLiteral("workspace.mount.unregistered");
    e.toolName.clear();
    e.eventSummary = QStringLiteral("Workspace mount unregistered");
    e.eventDetail = detail({
        {"mount_id", mountId},
        {"client_id", clientId},
    });
    return e;
}

ActivityEvent ActivityEvent::forWorkspaceMountReplaced(const QString& projectFolderId,
                                                       const QString& oldMountId,
                                                       const QString& oldClientId,
                                                       const QString& newMountId,
                                                       const QString& newClientId) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId.clear();
    e.turnId.clear();
    e.actorKind = QStringLiteral("client");
    // See forWorkspaceMountUnregistered for the NOT-NULL rationale.
    e.actorAlias = QStringLiteral("");
    e.actorAgentId.clear();
    e.actorClientId = newClientId;
    e.eventType = QStringLiteral("workspace.mount.replaced");
    e.toolName.clear();
    e.eventSummary = QStringLiteral("Workspace mount replaced by a newer registration");
    e.eventDetail = detail({
        {"old_mount_id", oldMountId},
        {"old_client_id", oldClientId},
        {"new_mount_id", newMountId},
        {"new_client_id", newClientId},
    });
    return e;
}

ActivityEvent ActivityEvent::forWorkspaceMountTreeUpdated(const QString& projectFolderId,
                                                          const QString& mountId,
                                                          const QString& clientId) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId.clear();
    e.turnId.clear();
    e.actorKind = QStringLiteral("client");
    // See forWorkspaceMountUnregistered for the NOT-NULL rationale.
    e.actorAlias = QStringLiteral("");
    e.actorAgentId.clear();
    e.actorClientId = clientId;
    e.eventType = QStringLiteral("workspace.mount.tree_updated");
    e.toolName.clear();
    e.eventSummary = QStringLiteral("Workspace mount tree refreshed");
    e.eventDetail = detail({
        {"mount_id", mountId},
        {"client_id", clientId},
    });
    return e;
}

ActivityEvent ActivityEvent::forWorkspaceMountTierChanged(const QString& projectFolderId,
                                                          const QString& mountId,
                                                          const QString& clientId,
                                                          const QString& oldTier,
                                                          const QString& newTier) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId.clear();
    e.turnId.clear();
    e.actorKind = QStringLiteral("client");
    // See forWorkspaceMountUnregistered for the NOT-NULL rationale.
    e.actorAlias = QStringLiteral("");
    e.actorAgentId.clear();
    e.actorClientId = clientId;
    e.eventType = QStringLiteral("workspace.mount.tier_changed");
    e.toolName.clear();
    e.eventSummary =
        QStringLiteral("Workspace mount permission changed from %1 to %2").arg(oldTier, newTier);
    e.eventDetail = detail({
        {"mount_id", mountId},
        {"client_id", clientId},
        {"old_tier", oldTier},
        {"new_tier", newTier},
    });
    return e;
}

ActivityEvent ActivityEvent::forWorkspaceMountStale(const QString& projectFolderId,
                                                    const QString& mountId,
                                                    const QString& clientId,
                                                    qint64 lastSeenMs) {
    ActivityEvent e;
    e.id = freshId();
    e.createdAt = nowUtc();
    e.projectFolderId = projectFolderId;
    e.conversationId.clear();
    e.turnId.clear();
    // actorKind = system because the sweeper, not a client / agent /
    // user, drives this event. Disambiguates the row in audit-log
    // timeline filters.
    e.actorKind = QStringLiteral("system");
    // See forWorkspaceMountUnregistered for the NOT-NULL rationale.
    // The sweeper is system-driven and carries no human alias.
    e.actorAlias = QStringLiteral("");
    e.actorAgentId.clear();
    e.actorClientId = clientId;
    e.eventType = QStringLiteral("workspace.mount.stale");
    e.toolName.clear();
    e.eventSummary =
        QStringLiteral("Workspace mount has been idle for too long and is marked stale");
    e.eventDetail = detail({
        {"mount_id", mountId},
        {"client_id", clientId},
        {"last_seen_ms", QString::number(lastSeenMs)},
    });
    return e;
}

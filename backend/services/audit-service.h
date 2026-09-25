// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file audit-service.h
 * @brief Single append-only write entry-point for the
 *        `activity_log` audit table; query helpers for the timeline
 *        UI (per-project, per-conversation, per-turn).
 * @layer Service
 * @dependencies DbManager (Data Access), Qt6::Core, ActivityEvent
 *               (Data Access POD).
 */


#pragma once

#include "../models/activity-event.h"

#include <QObject>
#include <QString>
#include <QVariantList>

class ConversationService;
class DbManager;

/**
 * @brief Append-only audit-trail service.
 *
 * Owned by `AppController` via `std::unique_ptr<AuditService>`.
 * Construction takes a `DbManager&` (engine layer; non-owning).
 * Main-thread-only: every public method asserts via
 * `VERZETA_ASSERT_MAIN_THREAD()`.
 *
 * QML exposure: registered as the `Activity` singleton in
 * `AppController::registerTypes()`. QML calls the Q_INVOKABLE
 * `recentActivity*` helpers to populate the timeline + binds to the
 * `activityLogged` signal for live refresh.
 */
class AuditService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct against an open DbManager.
     * @param db     Engine-layer ref; must outlive this service.
     *               AppController owns both.
     * @param parent Optional Qt parent.
     */
    explicit AuditService(DbManager& db, QObject* parent = nullptr);
    ~AuditService() override;

    /**
     * @brief Attach a ConversationService for write-time folder-chain
     *        resolution. Non-owning; AppController wires this once
     *        after both services are constructed.
     *
     *        When set, `record()` resolves an empty `projectFolderId`
     *        from the event's `conversationId` by walking the
     *        conversation → folder → parent-folder chain to the
     *        nearest project / organization folder. This keeps every
     *        recording hook a literal one-liner:
     *        the caller passes the convId it already has; AuditService
     *        does the folder-scope enrichment so the
     *        `project_folder_id` column is populated for the
     *        project-scoped timeline filter.
     *
     *        Optional. When unset, `record()` writes the event as-is.
     *        This keeps unit tests minimal, since test-audit-service.cpp
     *        doesn't construct a ConversationService.
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setConversationService(ConversationService* svc);

    // -----------------------------------------------------------------
    // Write entry point
    // -----------------------------------------------------------------

    /**
     * @brief Persist the given event to `activity_log`.
     *
     * @param event Pre-populated via one of the `ActivityEvent::forXxx`
     *              factories. Invalid events (id / actorKind /
     *              eventType / eventSummary missing) are rejected
     *              with a `qCWarning` and the row is not written.
     *              This is a caller bug (the factory always produces
     *              a valid event; hand-construction is discouraged).
     *
     * @sideeffects On success, emits `activityLogged(projectFolderId,
     *              conversationId)` so the timeline UI can refresh
     *              filtered views without polling.
     */
    void record(const ActivityEvent& event);


    /**
     * @brief Rows where `project_folder_id == folderId`, newest first.
     *        Empty folder id is rejected (use the conversation- or
     *        turn-scoped helpers for non-project scopes).
     * @param folderId Project / organization folder UUID.
     * @param limit    Max rows to return; clamped to [1, 1000].
     * @return QVariantList of QVariantMap rows (camelCase keys).
     */
    Q_INVOKABLE QVariantList recentActivityForProject(const QString& folderId,
                                                      int limit = 100) const;

    /**
     * @brief Rows where `conversation_id == convId`, newest first.
     * @param convId Conversation UUID.
     * @param limit  Max rows to return; clamped to [1, 1000].
     * @returns QVariantList of QVariantMap rows (camelCase keys).
     */
    Q_INVOKABLE QVariantList recentActivityForConversation(const QString& convId,
                                                           int limit = 100) const;

    /**
     * @brief All rows for the given turn id, oldest first.
     *        Turn-scoped reads return chronological order so the
     *        expandable timeline unit (agent turn → N tool calls →
     *        1 poll) reads top-to-bottom in the user's order of
     *        events.
     * @param turnId Turn UUID (matches `messages.turn_id`).
     * @returns QVariantList of QVariantMap rows (camelCase keys).
     */
    Q_INVOKABLE QVariantList recentActivityByTurn(const QString& turnId) const;

  signals:
    /**
     * @brief Emitted after `record()` successfully writes a row.
     *        UI components filter on `projectFolderId` /
     *        `conversationId` (either may be empty for app-level
     *        events) to decide whether to refresh.
     * @param projectFolderId Project / org folder scope of the event,
     *                        or empty.
     * @param conversationId  Conversation scope of the event, or
     *                        empty.
     */
    void activityLogged(const QString& projectFolderId, const QString& conversationId);

  private:
    /**
     * @brief Resolve the project / organization folder id for the
     *        given conversation by walking the folder chain. Empty
     *        return when `convId` is empty, the conversation isn't
     *        in any folder, or no folder in the chain is a project /
     *        organization type. No-op when `m_convService` is unset.
     */
    QString resolveProjectFolderId(const QString& convId) const;

    DbManager& m_db;                               // non-owning
    ConversationService* m_convService = nullptr;  // non-owning, optional
};

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file audit-service.cpp
 * @brief AuditService implementation: one prepared INSERT
 *        per record(); indexed reads per scope.
 * @layer Service
 * @dependencies DbManager (Data Access), Qt6::Core, Qt6::Sql.
 */


#include "audit-service.h"

#include "../models/conversation.h"
#include "../models/db-manager.h"
#include "../services/conversation-service.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariantMap>

namespace {

QString toIsoMs(const QDateTime& dt) {
    return dt.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime fromIsoMs(const QString& s) {
    return QDateTime::fromString(s, Qt::ISODateWithMs);
}

// Decode the stored event_detail TEXT column back to QVariantMap so
// QML can drill into fields directly without a manual JSON parse.
QVariantMap parseDetail(const QString& detailJson) {
    if (detailJson.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(detailJson.toUtf8());
    if (!doc.isObject())
        return {};
    return doc.object().toVariantMap();
}

QVariantMap rowToMap(const QSqlQuery& q) {
    QVariantMap row;
    row.insert(QStringLiteral("id"), q.value(0).toString());
    row.insert(QStringLiteral("createdAt"), fromIsoMs(q.value(1).toString()));
    row.insert(QStringLiteral("projectFolderId"), q.value(2).toString());
    row.insert(QStringLiteral("conversationId"), q.value(3).toString());
    row.insert(QStringLiteral("turnId"), q.value(4).toString());
    row.insert(QStringLiteral("actorKind"), q.value(5).toString());
    row.insert(QStringLiteral("actorAlias"), q.value(6).toString());
    row.insert(QStringLiteral("actorAgentId"), q.value(7).toString());
    row.insert(QStringLiteral("actorClientId"), q.value(8).toString());
    row.insert(QStringLiteral("eventType"), q.value(9).toString());
    row.insert(QStringLiteral("toolName"), q.value(10).toString());
    row.insert(QStringLiteral("eventSummary"), q.value(11).toString());
    row.insert(QStringLiteral("eventDetail"), parseDetail(q.value(12).toString()));
    return row;
}

// Common SELECT projection. Column order is locked to match rowToMap.
const QString kSelectColumns =
    QStringLiteral("id, created_at, project_folder_id, conversation_id, turn_id, "
                   "actor_kind, actor_alias, actor_agent_id, actor_client_id, "
                   "event_type, tool_name, event_summary, event_detail");

int clampLimit(int limit) {
    if (limit < 1)
        return 1;
    if (limit > 1000)
        return 1000;
    return limit;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AuditService::AuditService(DbManager& db, QObject* parent) : QObject(parent), m_db(db) {
    qCInfo(verzetaDb) << "AuditService initialised";
}

AuditService::~AuditService() = default;

void AuditService::setConversationService(ConversationService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_convService = svc;
}

// ---------------------------------------------------------------------------
// Folder-chain resolution — write-time enrichment so hooks stay
// one-liners per the recording-hook binding.
// ---------------------------------------------------------------------------

QString AuditService::resolveProjectFolderId(const QString& convId) const {
    if (convId.isEmpty() || !m_convService)
        return {};

    const auto convOpt = m_convService->getConversation(convId);
    if (!convOpt.has_value())
        return {};

    QString folderId = convOpt->folderId;
    // Walk up the folder chain looking for project / organization.
    // Most chains are shallow (1-2 hops); bound the walk at 16 to
    // protect against any pathological data.
    for (int hops = 0; !folderId.isEmpty() && hops < 16; ++hops) {
        const auto folderOpt = m_convService->getFolder(folderId);
        if (!folderOpt.has_value())
            break;
        const QString type = folderOpt->folderType;
        if (type == QStringLiteral("project") || type == QStringLiteral("organization")) {
            return folderId;
        }
        folderId = folderOpt->parentId;
    }
    return {};
}


void AuditService::record(const ActivityEvent& event) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (!event.isValid()) {
        qCWarning(verzetaDb) << "AuditService::record: refusing invalid event"
                             << "(id:" << event.id << ", actorKind:" << event.actorKind
                             << ", eventType:" << event.eventType
                             << ", summary:" << event.eventSummary << ")";
        return;
    }

    QString resolvedFolderId = event.projectFolderId;
    if (resolvedFolderId.isEmpty() && !event.conversationId.isEmpty()) {
        resolvedFolderId = resolveProjectFolderId(event.conversationId);
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO activity_log("
                             "  id, created_at, project_folder_id, conversation_id, turn_id, "
                             "  actor_kind, actor_alias, actor_agent_id, actor_client_id, "
                             "  event_type, tool_name, event_summary, event_detail) "
                             "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(event.id);
    q.addBindValue(toIsoMs(event.createdAt));
    q.addBindValue(resolvedFolderId);
    q.addBindValue(event.conversationId);
    q.addBindValue(event.turnId);
    q.addBindValue(event.actorKind);
    q.addBindValue(event.actorAlias);
    q.addBindValue(event.actorAgentId);
    q.addBindValue(event.actorClientId);
    q.addBindValue(event.eventType);
    q.addBindValue(event.toolName);
    q.addBindValue(event.eventSummary);
    q.addBindValue(
        event.eventDetail.isEmpty()
            ? QString()
            : QString::fromUtf8(QJsonDocument(event.eventDetail).toJson(QJsonDocument::Compact)));

    if (!q.exec()) {
        qCWarning(verzetaDb) << "AuditService::record: INSERT failed:" << q.lastError().text()
                             << "event_type:" << event.eventType << "event_id:" << event.id;
        return;
    }

    emit activityLogged(resolvedFolderId, event.conversationId);
}


QVariantList AuditService::recentActivityForProject(const QString& folderId, int limit) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    if (folderId.isEmpty()) {
        qCWarning(verzetaDb) << "AuditService::recentActivityForProject: empty folderId";
        return out;
    }

    // Index hit: idx_activity_project (project_folder_id, created_at DESC).
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT %1 FROM activity_log "
                             "WHERE project_folder_id = ? "
                             "ORDER BY created_at DESC LIMIT ?")
                  .arg(kSelectColumns));
    q.addBindValue(folderId);
    q.addBindValue(clampLimit(limit));
    if (!q.exec()) {
        qCWarning(verzetaDb) << "AuditService::recentActivityForProject: SELECT failed:"
                             << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(rowToMap(q));
    }
    return out;
}

QVariantList AuditService::recentActivityForConversation(const QString& convId, int limit) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    if (convId.isEmpty()) {
        qCWarning(verzetaDb) << "AuditService::recentActivityForConversation: empty convId";
        return out;
    }

    // Index hit: idx_activity_conv (conversation_id, created_at DESC).
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT %1 FROM activity_log "
                             "WHERE conversation_id = ? "
                             "ORDER BY created_at DESC LIMIT ?")
                  .arg(kSelectColumns));
    q.addBindValue(convId);
    q.addBindValue(clampLimit(limit));
    if (!q.exec()) {
        qCWarning(verzetaDb) << "AuditService::recentActivityForConversation: SELECT failed:"
                             << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(rowToMap(q));
    }
    return out;
}

QVariantList AuditService::recentActivityByTurn(const QString& turnId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    if (turnId.isEmpty()) {
        qCWarning(verzetaDb) << "AuditService::recentActivityByTurn: empty turnId";
        return out;
    }

    // Index hit: idx_activity_turn (turn_id). Per-turn reads are
    // chronological so the timeline-unit expansion reads in event
    // order, not newest-first.
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT %1 FROM activity_log "
                             "WHERE turn_id = ? "
                             "ORDER BY created_at ASC")
                  .arg(kSelectColumns));
    q.addBindValue(turnId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "AuditService::recentActivityByTurn: SELECT failed:"
                             << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(rowToMap(q));
    }
    return out;
}

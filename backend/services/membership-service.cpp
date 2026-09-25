// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file membership-service.cpp
 * @brief Implementation of MembershipService: CRUD for the
 *        `project_members` and `conversation_members` tables.
 * @layer Service
 * @dependencies DbManager, AgentRegistry, Qt6::Core, Qt6::Sql.
 */

#include "membership-service.h"

#include "../models/activity-event.h"
#include "../models/db-manager.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "audit-service.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

namespace {

// The v14 model-override columns are TEXT NOT NULL DEFAULT ''. A
// default-constructed QString is *null*, and Qt's SQLite driver binds a
// null QString as SQL NULL — which violates the NOT NULL constraint and
// fails the INSERT (recovery-prompt gotcha #12). Every override value
// bound below is routed through nonNull() so an unset field is stored
// as '' (the column's own default), never NULL.
QString nonNull(const QString& s) {
    return s.isNull() ? QStringLiteral("") : s;
}

QString memberToolsToJson(const QStringList& tools) {
    if (tools.isEmpty())
        return QStringLiteral("");
    QJsonArray arr;
    for (const QString& t : tools)
        arr.append(t);
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QStringList memberToolsFromJson(const QString& json) {
    QStringList out;
    if (json.isEmpty())
        return out;
    const QJsonArray arr = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue& v : arr)
        out.append(v.toString());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MembershipService::MembershipService(DbManager& db, QObject* parent) : QObject(parent), m_db(db) {
    // Self-heal rows persisted before alias normalization existed
    // (leading-@ aliases render as "@@Name" and can never be
    // mention-routed). Runs before any signal consumer exists, so no
    // membersChanged emission is needed here.
    normalizeLegacyAliases();
}

MembershipService::~MembershipService() = default;

QString MembershipService::normalizedAlias(const QString& alias) {
    QString a = alias.trimmed();
    while (a.startsWith(QLatin1Char('@')))
        a.remove(0, 1);
    return a.trimmed();
}

void MembershipService::normalizeLegacyAliases() {
    /** @brief Table descriptor for the two membership tables the sweep
     *         walks: SQL table name + its container-id column. */
    struct Table {
        const char* name;
        const char* idCol;
    };
    const Table tables[] = {
        {"project_members", "folder_id"},
        {"conversation_members", "conversation_id"},
    };
    for (const Table& t : tables) {
        QSqlQuery sel(m_db.db());
        sel.prepare(QStringLiteral("SELECT rowid, %1, alias FROM %2 WHERE alias LIKE '@%'")
                        .arg(QLatin1String(t.idCol), QLatin1String(t.name)));
        if (!sel.exec()) {
            qCWarning(verzetaDb) << "normalizeLegacyAliases: select failed on" << t.name
                                 << sel.lastError().text();
            continue;
        }
        while (sel.next()) {
            const qlonglong rowId = sel.value(0).toLongLong();
            const QString containerId = sel.value(1).toString();
            const QString rawAlias = sel.value(2).toString();
            const QString clean = normalizedAlias(rawAlias);
            if (clean.isEmpty()) {
                qCWarning(verzetaDb)
                    << "normalizeLegacyAliases: alias" << rawAlias << "in" << t.name << containerId
                    << "is empty after stripping — left untouched";
                continue;
            }
            QSqlQuery clash(m_db.db());
            clash.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE %2 = ? AND alias = ? "
                                         "COLLATE NOCASE")
                              .arg(QLatin1String(t.name), QLatin1String(t.idCol)));
            clash.addBindValue(containerId);
            clash.addBindValue(clean);
            if (!clash.exec() || !clash.next())
                continue;
            if (clash.value(0).toInt() > 0) {
                qCWarning(verzetaDb) << "normalizeLegacyAliases: cannot strip" << rawAlias << "in"
                                     << t.name << containerId << "— alias" << clean
                                     << "already exists there; row left untouched";
                continue;
            }
            QSqlQuery upd(m_db.db());
            upd.prepare(QStringLiteral("UPDATE %1 SET alias = ? WHERE rowid = ?")
                            .arg(QLatin1String(t.name)));
            upd.addBindValue(clean);
            upd.addBindValue(rowId);
            if (upd.exec()) {
                qCInfo(verzetaDb) << "normalizeLegacyAliases: repaired" << rawAlias << "->" << clean
                                  << "in" << t.name << containerId;
            } else {
                qCWarning(verzetaDb)
                    << "normalizeLegacyAliases: update failed:" << upd.lastError().text();
            }
        }
    }
}

void MembershipService::setAuditService(AuditService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_auditService = svc;
}

// ---------------------------------------------------------------------------
// Internal: shared load / delete / insert helpers
// ---------------------------------------------------------------------------

bool MembershipService::insertMemberRow(const QString& table,
                                        const QString& idColumn,
                                        const Member& m,
                                        qint64 joinedAtMs) {
    // Sanitise the provenance kind: only 'user' or 'agent' are valid;
    // anything else coerces to 'user' (the safest — user-added rows are
    // protected from agent-driven removal). agentId is cleared for
    // user-kind rows.
    const QString safeKind = (m.addedByKind == QStringLiteral("agent")) ? QStringLiteral("agent")
                                                                        : QStringLiteral("user");
    const QString safeAgentId =
        (safeKind == QStringLiteral("agent")) ? m.addedByAgentId : QStringLiteral("");

    // Aliases are stored WITHOUT the @ sigil — every write path funnels
    // through here, so this is the storage guarantee (callers may pass
    // "@Engineer"; display prepends "@" and mention-parsing extracts
    // sigil-free handles, so a stored sigil breaks both).
    const QString cleanAlias = normalizedAlias(m.alias);
    if (cleanAlias.isEmpty()) {
        qCWarning(verzetaDb) << "insertMemberRow: alias" << m.alias
                             << "is empty after normalization — insert refused";
        return false;
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO %1(%2, agent_id, alias, is_coordinator, joined_at, "
                             "added_by_kind, added_by_agent_id, model_provider, model_name, "
                             "allowed_tools) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                  .arg(table, idColumn));
    q.addBindValue(m.containerId);
    q.addBindValue(m.agentId);
    q.addBindValue(cleanAlias);
    q.addBindValue(m.isCoordinator ? 1 : 0);
    q.addBindValue(joinedAtMs);
    q.addBindValue(safeKind);
    q.addBindValue(safeAgentId);
    q.addBindValue(nonNull(m.modelProvider));
    q.addBindValue(nonNull(m.modelName));
    q.addBindValue(memberToolsToJson(m.allowedTools));

    if (!q.exec()) {
        qCWarning(verzetaDb) << "insertMemberRow failed for" << table << ":"
                             << q.lastError().text();
        return false;
    }
    return true;
}

QList<Member> MembershipService::loadMembers(const QString& table,
                                             const QString& idColumn,
                                             const QString& containerId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<Member> result;
    if (containerId.isEmpty())
        return result;

    // JOIN to agents to populate the denormalized display fields so QML
    // doesn't have to make a second lookup per row.
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT m.%1, m.agent_id, m.alias, m.is_coordinator, m.joined_at, "
                             "m.added_by_kind, m.added_by_agent_id, "
                             "m.model_provider, m.model_name, m.allowed_tools, "
                             "a.name, a.description, a.icon_name "
                             "FROM %2 m LEFT JOIN agents a ON m.agent_id = a.id "
                             "WHERE m.%1 = ? ORDER BY m.joined_at ASC")
                  .arg(idColumn, table));
    q.addBindValue(containerId);

    if (!q.exec()) {
        qCWarning(verzetaDb) << "MembershipService::loadMembers failed:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        VERZETA_ASSERT_MAIN_THREAD();
        Member m;
        m.containerId = q.value(0).toString();
        m.agentId = q.value(1).toString();
        m.alias = q.value(2).toString();
        m.isCoordinator = q.value(3).toInt() != 0;
        m.joinedAt = QDateTime::fromMSecsSinceEpoch(q.value(4).toLongLong());
        m.addedByKind = q.value(5).toString();
        if (m.addedByKind.isEmpty())
            m.addedByKind = QStringLiteral("user");
        m.addedByAgentId = q.value(6).toString();
        m.modelProvider = q.value(7).toString();
        m.modelName = q.value(8).toString();
        m.allowedTools = memberToolsFromJson(q.value(9).toString());
        m.agentName = q.value(10).toString();
        m.agentDescription = q.value(11).toString();
        m.agentIconName = q.value(12).toString();
        result.append(m);
    }
    return result;
}

bool MembershipService::deleteAllMembers(const QString& table,
                                         const QString& idColumn,
                                         const QString& containerId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 = ?").arg(table, idColumn));
    q.addBindValue(containerId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "MembershipService::deleteAllMembers failed:"
                             << q.lastError().text();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Project memberships
// ---------------------------------------------------------------------------

QList<Member> MembershipService::projectMembers(const QString& folderId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    return loadMembers(QStringLiteral("project_members"), QStringLiteral("folder_id"), folderId);
}

bool MembershipService::addProjectMember(const QString& folderId,
                                         const QString& agentId,
                                         const QString& alias,
                                         bool isCoordinator,
                                         const QString& addedByKind,
                                         const QString& addedByAgentId,
                                         const QString& modelProvider,
                                         const QString& modelName,
                                         const QStringList& allowedTools) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (folderId.isEmpty() || agentId.isEmpty() || alias.isEmpty())
        return false;

    Member m;
    m.containerId = folderId;
    m.agentId = agentId;
    m.alias = alias;
    m.isCoordinator = isCoordinator;
    m.addedByKind = addedByKind;  // sanitised inside insertMemberRow
    m.addedByAgentId = addedByAgentId;
    m.modelProvider = modelProvider;
    m.modelName = modelName;
    m.allowedTools = allowedTools;

    if (!insertMemberRow(QStringLiteral("project_members"),
                         QStringLiteral("folder_id"),
                         m,
                         QDateTime::currentMSecsSinceEpoch())) {
        qCWarning(verzetaDb) << "addProjectMember failed for folder" << folderId;
        return false;
    }
    emit projectMembersChanged(folderId);

    if (m_auditService) {
        m_auditService->record(ActivityEvent::forMemberAdded(
            folderId, QString(), alias, agentId, addedByKind, QString(), addedByAgentId));
    }
    return true;
}

bool MembershipService::removeProjectMember(const QString& folderId, const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Sigil-tolerant: stored aliases are clean, callers may pass "@X".
    const QString a = normalizedAlias(alias);
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM project_members WHERE folder_id = ? AND alias = ?"));
    q.addBindValue(folderId);
    q.addBindValue(a);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "removeProjectMember failed:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() > 0) {
        emit projectMembersChanged(folderId);

        if (m_auditService) {
            m_auditService->record(ActivityEvent::forMemberRemoved(folderId,
                                                                   QString(),
                                                                   alias,
                                                                   QString(),
                                                                   QStringLiteral("system"),
                                                                   QString(),
                                                                   QString()));
        }
        return true;
    }
    return false;
}

bool MembershipService::setProjectMembers(const QString& folderId, const QList<Member>& members) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (folderId.isEmpty())
        return false;

    if (!m_db.transaction()) {
        qCWarning(verzetaDb) << "setProjectMembers: transaction start failed";
        return false;
    }

    if (!deleteAllMembers(
            QStringLiteral("project_members"), QStringLiteral("folder_id"), folderId)) {
        m_db.rollback();
        return false;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Member& m : members) {
        Member row = m;
        row.containerId = folderId;
        const qint64 joinedMs = m.joinedAt.isValid() ? m.joinedAt.toMSecsSinceEpoch() : now;
        if (!insertMemberRow(
                QStringLiteral("project_members"), QStringLiteral("folder_id"), row, joinedMs)) {
            m_db.rollback();
            return false;
        }
    }

    if (!m_db.commit()) {
        qCWarning(verzetaDb) << "setProjectMembers: commit failed";
        m_db.rollback();
        return false;
    }

    emit projectMembersChanged(folderId);
    return true;
}

// ---------------------------------------------------------------------------
// Conversation (group) memberships
// ---------------------------------------------------------------------------

QList<Member> MembershipService::conversationMembers(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    return loadMembers(
        QStringLiteral("conversation_members"), QStringLiteral("conversation_id"), conversationId);
}

bool MembershipService::addConversationMember(const QString& conversationId,
                                              const QString& agentId,
                                              const QString& alias,
                                              bool isCoordinator,
                                              const QString& addedByKind,
                                              const QString& addedByAgentId,
                                              const QString& modelProvider,
                                              const QString& modelName,
                                              const QStringList& allowedTools) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || agentId.isEmpty() || alias.isEmpty())
        return false;

    Member m;
    m.containerId = conversationId;
    m.agentId = agentId;
    m.alias = alias;
    m.isCoordinator = isCoordinator;
    m.addedByKind = addedByKind;  // sanitised inside insertMemberRow
    m.addedByAgentId = addedByAgentId;
    m.modelProvider = modelProvider;
    m.modelName = modelName;
    m.allowedTools = allowedTools;

    if (!insertMemberRow(QStringLiteral("conversation_members"),
                         QStringLiteral("conversation_id"),
                         m,
                         QDateTime::currentMSecsSinceEpoch())) {
        qCWarning(verzetaDb) << "addConversationMember failed for conv" << conversationId;
        return false;
    }
    emit conversationMembersChanged(conversationId);

    if (m_auditService) {
        m_auditService->record(ActivityEvent::forMemberAdded(
            QString(), conversationId, alias, agentId, addedByKind, QString(), addedByAgentId));
    }
    return true;
}

bool MembershipService::removeConversationMember(const QString& conversationId,
                                                 const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Sigil-tolerant: stored aliases are clean, callers may pass "@X".
    const QString a = normalizedAlias(alias);
    QSqlQuery q(m_db.db());
    q.prepare(
        QStringLiteral("DELETE FROM conversation_members WHERE conversation_id = ? AND alias = ?"));
    q.addBindValue(conversationId);
    q.addBindValue(a);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "removeConversationMember failed:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() > 0) {
        emit conversationMembersChanged(conversationId);

        if (m_auditService) {
            m_auditService->record(ActivityEvent::forMemberRemoved(QString(),
                                                                   conversationId,
                                                                   alias,
                                                                   QString(),
                                                                   QStringLiteral("system"),
                                                                   QString(),
                                                                   QString()));
        }
        return true;
    }
    return false;
}

bool MembershipService::setConversationMembers(const QString& conversationId,
                                               const QList<Member>& members) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty())
        return false;

    if (!m_db.transaction()) {
        return false;
    }

    if (!deleteAllMembers(QStringLiteral("conversation_members"),
                          QStringLiteral("conversation_id"),
                          conversationId)) {
        m_db.rollback();
        return false;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Member& m : members) {
        Member row = m;
        row.containerId = conversationId;
        const qint64 joinedMs = m.joinedAt.isValid() ? m.joinedAt.toMSecsSinceEpoch() : now;
        if (!insertMemberRow(QStringLiteral("conversation_members"),
                             QStringLiteral("conversation_id"),
                             row,
                             joinedMs)) {
            m_db.rollback();
            return false;
        }
    }

    if (!m_db.commit()) {
        m_db.rollback();
        return false;
    }

    emit conversationMembersChanged(conversationId);
    return true;
}

Member MembershipService::findConversationMemberByAlias(const QString& conversationId,
                                                        const QString& alias) const {
    VERZETA_ASSERT_MAIN_THREAD();
    // Sigil-tolerant lookup: "@X" and "X" resolve to the same member.
    const QString wanted = normalizedAlias(alias);
    const QList<Member> members = conversationMembers(conversationId);
    for (const Member& m : members) {
        if (m.alias.compare(wanted, Qt::CaseInsensitive) == 0)
            return m;
        // Also match with spaces replaced by underscores (so @Team_Lead works)
        const QString normalized = m.alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
        if (normalized.compare(wanted, Qt::CaseInsensitive) == 0)
            return m;
    }
    return {};
}

Member MembershipService::defaultResponderForConversation(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const QList<Member> members = conversationMembers(conversationId);
    // Prefer coordinator
    for (const Member& m : members) {
        if (m.isCoordinator)
            return m;
    }
    // Fall back to first member
    if (!members.isEmpty())
        return members.first();
    return {};
}

bool MembershipService::setConversationCoordinator(const QString& conversationId,
                                                   const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_db.transaction())
        return false;

    QSqlQuery clearQ(m_db.db());
    clearQ.prepare(QStringLiteral(
        "UPDATE conversation_members SET is_coordinator = 0 WHERE conversation_id = ?"));
    clearQ.addBindValue(conversationId);
    if (!clearQ.exec()) {
        qCWarning(verzetaDb) << "setConversationCoordinator clear failed:"
                             << clearQ.lastError().text();
        m_db.rollback();
        return false;
    }

    QSqlQuery setQ(m_db.db());
    setQ.prepare(QStringLiteral("UPDATE conversation_members SET is_coordinator = 1 "
                                "WHERE conversation_id = ? AND alias = ?"));
    setQ.addBindValue(conversationId);
    setQ.addBindValue(normalizedAlias(alias));
    if (!setQ.exec()) {
        qCWarning(verzetaDb) << "setConversationCoordinator set failed:" << setQ.lastError().text();
        m_db.rollback();
        return false;
    }

    if (!m_db.commit()) {
        m_db.rollback();
        return false;
    }
    emit conversationMembersChanged(conversationId);
    return true;
}

// ---------------------------------------------------------------------------
// QML API
// ---------------------------------------------------------------------------

/**
 * @brief Converts a Member to the map shape QML reads.
 * @param m Member to convert.
 * @returns Map keyed by the member's field names.
 */
static QVariantMap memberToMap(const Member& m) {
    QVariantMap map;
    map[QStringLiteral("containerId")] = m.containerId;
    map[QStringLiteral("agentId")] = m.agentId;
    map[QStringLiteral("alias")] = m.alias;
    map[QStringLiteral("isCoordinator")] = m.isCoordinator;
    map[QStringLiteral("addedByKind")] = m.addedByKind;
    map[QStringLiteral("addedByAgentId")] = m.addedByAgentId;
    map[QStringLiteral("modelProvider")] = m.modelProvider;
    map[QStringLiteral("modelName")] = m.modelName;
    map[QStringLiteral("allowedTools")] = m.allowedTools;
    map[QStringLiteral("agentName")] = m.agentName;
    map[QStringLiteral("agentDescription")] = m.agentDescription;
    map[QStringLiteral("agentIconName")] = m.agentIconName;
    return map;
}

QVariantList MembershipService::projectMembersList(const QString& folderId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    for (const Member& m : projectMembers(folderId))
        out.append(memberToMap(m));
    return out;
}

QVariantList MembershipService::conversationMembersList(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    for (const Member& m : conversationMembers(conversationId))
        out.append(memberToMap(m));
    return out;
}

bool MembershipService::addProjectMemberMap(const QVariantMap& map) {
    VERZETA_ASSERT_MAIN_THREAD();
    return addProjectMember(
        map.value(QStringLiteral("folderId")).toString(),
        map.value(QStringLiteral("agentId")).toString(),
        map.value(QStringLiteral("alias")).toString(),
        map.value(QStringLiteral("isCoordinator")).toBool(),
        map.value(QStringLiteral("addedByKind"), QStringLiteral("user")).toString(),
        map.value(QStringLiteral("addedByAgentId")).toString(),
        map.value(QStringLiteral("modelProvider")).toString(),
        map.value(QStringLiteral("modelName")).toString(),
        map.value(QStringLiteral("allowedTools")).toStringList());
}

bool MembershipService::removeProjectMemberAlias(const QString& folderId, const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();
    return removeProjectMember(folderId, alias);
}

bool MembershipService::setProjectMembersFromList(const QString& folderId,
                                                  const QVariantList& memberMaps) {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<Member> members;
    for (const QVariant& v : memberMaps) {
        const QVariantMap m = v.toMap();
        Member member;
        member.containerId = folderId;
        member.agentId = m.value(QStringLiteral("agentId")).toString();
        member.alias = m.value(QStringLiteral("alias")).toString();
        member.isCoordinator = m.value(QStringLiteral("isCoordinator")).toBool();
        member.addedByKind =
            m.value(QStringLiteral("addedByKind"), QStringLiteral("user")).toString();
        member.addedByAgentId = m.value(QStringLiteral("addedByAgentId")).toString();
        member.modelProvider = m.value(QStringLiteral("modelProvider")).toString();
        member.modelName = m.value(QStringLiteral("modelName")).toString();
        member.allowedTools = m.value(QStringLiteral("allowedTools")).toStringList();
        if (member.agentId.isEmpty() || member.alias.isEmpty())
            continue;
        members.append(member);
    }
    return setProjectMembers(folderId, members);
}

bool MembershipService::addConversationMemberMap(const QVariantMap& map) {
    VERZETA_ASSERT_MAIN_THREAD();
    return addConversationMember(
        map.value(QStringLiteral("conversationId")).toString(),
        map.value(QStringLiteral("agentId")).toString(),
        map.value(QStringLiteral("alias")).toString(),
        map.value(QStringLiteral("isCoordinator")).toBool(),
        map.value(QStringLiteral("addedByKind"), QStringLiteral("user")).toString(),
        map.value(QStringLiteral("addedByAgentId")).toString(),
        map.value(QStringLiteral("modelProvider")).toString(),
        map.value(QStringLiteral("modelName")).toString(),
        map.value(QStringLiteral("allowedTools")).toStringList());
}

bool MembershipService::removeConversationMemberAlias(const QString& conversationId,
                                                      const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();
    return removeConversationMember(conversationId, alias);
}

bool MembershipService::setConversationMembersFromList(const QString& conversationId,
                                                       const QVariantList& memberMaps) {
    VERZETA_ASSERT_MAIN_THREAD();
    QList<Member> members;
    for (const QVariant& v : memberMaps) {
        const QVariantMap m = v.toMap();
        Member member;
        member.containerId = conversationId;
        member.agentId = m.value(QStringLiteral("agentId")).toString();
        member.alias = m.value(QStringLiteral("alias")).toString();
        member.isCoordinator = m.value(QStringLiteral("isCoordinator")).toBool();
        member.addedByKind =
            m.value(QStringLiteral("addedByKind"), QStringLiteral("user")).toString();
        member.addedByAgentId = m.value(QStringLiteral("addedByAgentId")).toString();
        member.modelProvider = m.value(QStringLiteral("modelProvider")).toString();
        member.modelName = m.value(QStringLiteral("modelName")).toString();
        member.allowedTools = m.value(QStringLiteral("allowedTools")).toStringList();
        if (member.agentId.isEmpty() || member.alias.isEmpty())
            continue;
        members.append(member);
    }
    return setConversationMembers(conversationId, members);
}

bool MembershipService::setConversationCoordinatorAlias(const QString& conversationId,
                                                        const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();
    return setConversationCoordinator(conversationId, alias);
}


bool MembershipService::updateProjectMemberModelOverride(const QString& folderId,
                                                         const QString& alias,
                                                         const QString& modelProvider,
                                                         const QString& modelName,
                                                         const QStringList& allowedTools) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (folderId.isEmpty() || alias.isEmpty())
        return false;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE project_members SET model_provider = ?, model_name = ?, "
                             "allowed_tools = ? WHERE folder_id = ? AND alias = ?"));
    q.addBindValue(nonNull(modelProvider));
    q.addBindValue(nonNull(modelName));
    q.addBindValue(memberToolsToJson(allowedTools));
    q.addBindValue(folderId);
    q.addBindValue(normalizedAlias(alias));
    if (!q.exec()) {
        qCWarning(verzetaDb) << "updateProjectMemberModelOverride failed:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0) {
        qCWarning(verzetaDb) << "updateProjectMemberModelOverride: no row matched folder"
                             << folderId << "alias" << alias;
        return false;
    }
    emit projectMembersChanged(folderId);
    return true;
}

bool MembershipService::updateConversationMemberModelOverride(const QString& conversationId,
                                                              const QString& alias,
                                                              const QString& modelProvider,
                                                              const QString& modelName,
                                                              const QStringList& allowedTools) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || alias.isEmpty())
        return false;

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE conversation_members SET model_provider = ?, model_name = ?, "
                             "allowed_tools = ? WHERE conversation_id = ? AND alias = ?"));
    q.addBindValue(nonNull(modelProvider));
    q.addBindValue(nonNull(modelName));
    q.addBindValue(memberToolsToJson(allowedTools));
    q.addBindValue(conversationId);
    q.addBindValue(normalizedAlias(alias));
    if (!q.exec()) {
        qCWarning(verzetaDb) << "updateConversationMemberModelOverride failed:"
                             << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0) {
        qCWarning(verzetaDb) << "updateConversationMemberModelOverride: no row matched conv"
                             << conversationId << "alias" << alias;
        return false;
    }
    emit conversationMembersChanged(conversationId);
    return true;
}

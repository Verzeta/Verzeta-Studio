// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file heartbeat-reports-model.cpp
 * @brief Implementation of the heartbeat reports list model.
 *
 *        Push-based: subscribes to HeartbeatSubagentService run-* signals
 *        and HeartbeatConfigService config-* signals so the overlay's
 *        ListView re-renders without polling.
 * @layer Service (Model)
 * @dependencies HeartbeatConfigService, HeartbeatSubagentService,
 *               DbManager, AgentRegistry, Qt6::Core, Qt6::Sql.
 */


#include "heartbeat-reports-model.h"

#include "../services/agent-registry.h"
#include "../services/heartbeat-config-service.h"
#include "../services/heartbeat-subagent-service.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "db-manager.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

HeartbeatReportsModel::HeartbeatReportsModel(HeartbeatConfigService& configSvc,
                                             HeartbeatSubagentService& subagentSvc,
                                             DbManager& db,
                                             AgentRegistry& agents,
                                             QObject* parent)
    : QAbstractListModel(parent)
    , m_configSvc(configSvc)
    , m_subagentSvc(subagentSvc)
    , m_db(db)
    , m_agents(agents) {
    connect(&m_subagentSvc,
            &HeartbeatSubagentService::runStarted,
            this,
            &HeartbeatReportsModel::onRunStarted,
            Qt::UniqueConnection);
    connect(&m_subagentSvc,
            &HeartbeatSubagentService::runCompleted,
            this,
            &HeartbeatReportsModel::onRunCompleted,
            Qt::UniqueConnection);
    connect(&m_subagentSvc,
            &HeartbeatSubagentService::runFailed,
            this,
            &HeartbeatReportsModel::onRunFailed,
            Qt::UniqueConnection);
    connect(&m_configSvc,
            &HeartbeatConfigService::configChanged,
            this,
            &HeartbeatReportsModel::onConfigChanged,
            Qt::UniqueConnection);
    connect(&m_configSvc,
            &HeartbeatConfigService::configRemoved,
            this,
            &HeartbeatReportsModel::onConfigRemoved,
            Qt::UniqueConnection);
}

// ---------------------------------------------------------------------------
// Active conversation switch — full reset.
// ---------------------------------------------------------------------------

void HeartbeatReportsModel::setActiveConversation(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Switching to a conversation scope clears any active folder
    // scope. The model holds exactly one scope at a time.
    const bool sameConv = (convId == m_activeConvId);
    const bool hadFolder = !m_activeFolderId.isEmpty();
    if (sameConv && !hadFolder)
        return;

    beginResetModel();
    m_activeConvId = convId;
    if (hadFolder) {
        m_activeFolderId.clear();
    }
    refreshConfigsCache();
    if (m_activeConvId.isEmpty()) {
        m_rows.clear();
    } else {
        m_rows = queryReports(configIdsForActiveScope());
    }
    endResetModel();

    emit activeConversationChanged();
    if (hadFolder)
        emit activeFolderChanged();
    emit countChanged();
    recomputePendingReviewCount();
}

void HeartbeatReportsModel::setActiveFolder(const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    const bool sameFolder = (folderId == m_activeFolderId);
    const bool hadConv = !m_activeConvId.isEmpty();
    if (sameFolder && !hadConv)
        return;

    beginResetModel();
    m_activeFolderId = folderId;
    if (hadConv) {
        m_activeConvId.clear();
    }
    refreshConfigsCache();
    if (m_activeFolderId.isEmpty()) {
        m_rows.clear();
    } else {
        m_rows = queryReports(configIdsForActiveScope());
    }
    endResetModel();

    emit activeFolderChanged();
    if (hadConv)
        emit activeConversationChanged();
    emit countChanged();
    recomputePendingReviewCount();
}

// ---------------------------------------------------------------------------
// Cascade cleanup — when the source conversation is deleted, the configs
// and reports are gone via FK CASCADE; mirror that in the in-memory model.
// ---------------------------------------------------------------------------

void HeartbeatReportsModel::onSourceConversationDeleted(const QString& convId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty() || convId != m_activeConvId)
        return;
    setActiveConversation(QString());
}

void HeartbeatReportsModel::onSourceFolderDeleted(const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (folderId.isEmpty() || folderId != m_activeFolderId)
        return;
    setActiveFolder(QString());
}

// ---------------------------------------------------------------------------
// QAbstractListModel surface.
// ---------------------------------------------------------------------------

int HeartbeatReportsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
}

QVariant HeartbeatReportsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const int row = index.row();
    if (row < 0 || row >= m_rows.size())
        return {};
    const HeartbeatReport& r = m_rows.at(row);
    const HeartbeatConfig cfg = m_configsByConfigId.value(r.configId);
    const Agent agent = cfg.agentId.isEmpty() ? Agent{} : m_agents.getAgent(cfg.agentId);

    switch (role) {
        case IdRole:
            return r.id;
        case ConfigIdRole:
            return r.configId;
        case AgentIdRole:
            return cfg.agentId;
        case AgentNameRole:
            return agent.name;
        case AgentIconNameRole:
            return agent.iconName;
        case AliasRole:
            return cfg.alias;
        case StartedAtMsRole:
            return r.startedAt.isValid() ? r.startedAt.toMSecsSinceEpoch() : qint64(-1);
        case CompletedAtMsRole:
            return r.completedAt.isValid() ? r.completedAt.toMSecsSinceEpoch() : qint64(-1);
        case DurationMsRole: {
            if (!r.startedAt.isValid() || !r.completedAt.isValid())
                return -1;
            return r.completedAt.toMSecsSinceEpoch() - r.startedAt.toMSecsSinceEpoch();
        }
        case OutcomeRole:
            return r.outcome;
        case TitleRole:
            return r.title;
        case BodyRole:
            return r.body;
        case SummaryRole:
            return r.summary;
        case ParentReviewStatusRole:
            return r.parentReviewStatus;
        case SurfaceStatusRole:
            return r.surfaceStatus;
        case SurfacedMessageIdRole:
            return r.surfacedMessageId;
        case ErrorRole:
            return r.error;
        default:
            return {};
    }
}

QHash<int, QByteArray> HeartbeatReportsModel::roleNames() const {
    return {
        {IdRole, "id"},
        {ConfigIdRole, "configId"},
        {AgentIdRole, "agentId"},
        {AgentNameRole, "agentName"},
        {AgentIconNameRole, "agentIconName"},
        {AliasRole, "alias"},
        {StartedAtMsRole, "startedAtMs"},
        {CompletedAtMsRole, "completedAtMs"},
        {DurationMsRole, "durationMs"},
        {OutcomeRole, "outcome"},
        {TitleRole, "title"},
        {BodyRole, "body"},
        {SummaryRole, "summary"},
        {ParentReviewStatusRole, "parentReviewStatus"},
        {SurfaceStatusRole, "surfaceStatus"},
        {SurfacedMessageIdRole, "surfacedMessageId"},
        {ErrorRole, "error"},
    };
}

// ---------------------------------------------------------------------------
// Subagent-service event handlers.
//
// runStarted  → no row exists yet (the report is persisted on completion).
//               H2 is read-only; we don't need to insert a placeholder row
//               here, since the report row arrives on runCompleted.
// runCompleted/runFailed → a heartbeat_reports row was just persisted with
//               this runId. Re-query that single row and insert it at the
//               top (started_at DESC ordering).
// ---------------------------------------------------------------------------

void HeartbeatReportsModel::onRunStarted(QString /*runId*/, QString /*configId*/) {
    // Intentionally empty for H2 — the row arrives on completion.
    // H3 may add an in-flight placeholder row here.
}

void HeartbeatReportsModel::onRunCompleted(QString runId, QString /*outcome*/) {
    reloadOneRow(runId);
}

void HeartbeatReportsModel::onRunFailed(QString runId, QString /*error*/) {
    reloadOneRow(runId);
}

void HeartbeatReportsModel::onConfigChanged(const QString& /*configId*/) {
    if (m_activeConvId.isEmpty() && m_activeFolderId.isEmpty())
        return;
    refreshConfigsCache();
    // Config edits don't change rows but they do change the alias /
    // agent identity rendered in existing rows. Emit dataChanged on
    // the visible range so the rendered alias / icon refreshes.
    if (m_rows.isEmpty())
        return;
    emit dataChanged(index(0),
                     index(static_cast<int>(m_rows.size()) - 1),
                     {AliasRole, AgentIdRole, AgentNameRole, AgentIconNameRole});
}

void HeartbeatReportsModel::onConfigRemoved(const QString& configId) {
    if ((m_activeConvId.isEmpty() && m_activeFolderId.isEmpty()) ||
        !m_configsByConfigId.contains(configId)) {
        return;
    }
    // Drop in-memory rows belonging to the removed config. The DB
    // already removed the report rows via FK CASCADE.
    int writeIdx = 0;
    for (int readIdx = 0; readIdx < m_rows.size(); ++readIdx) {
        if (m_rows.at(readIdx).configId == configId)
            continue;
        if (writeIdx != readIdx)
            m_rows[writeIdx] = m_rows[readIdx];
        ++writeIdx;
    }
    if (writeIdx == m_rows.size()) {
        // No removals — nothing to refresh beyond the config cache.
        m_configsByConfigId.remove(configId);
        return;
    }
    beginResetModel();
    m_rows.erase(m_rows.begin() + writeIdx, m_rows.end());
    m_configsByConfigId.remove(configId);
    endResetModel();
    emit countChanged();
    recomputePendingReviewCount();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QList<QString> HeartbeatReportsModel::configIdsForActiveScope() const {
    if (m_activeConvId.isEmpty() && m_activeFolderId.isEmpty())
        return {};
    QList<QString> ids;
    for (auto it = m_configsByConfigId.constBegin(); it != m_configsByConfigId.constEnd(); ++it) {
        ids.append(it.key());
    }
    return ids;
}

QList<HeartbeatReport> HeartbeatReportsModel::queryReports(const QList<QString>& configIds) const {
    QList<HeartbeatReport> out;
    if (configIds.isEmpty())
        return out;

    // Build the IN-clause placeholders. SQLite has a 999-default
    // bind-variable limit; for a per-conversation overlay this is
    // never an issue (a conv has at most a handful of HB members),
    // but we cap at 256 just in case to keep the query bounded.
    const int n = static_cast<int>(std::min<qsizetype>(configIds.size(), 256));
    QStringList placeholders;
    placeholders.reserve(n);
    for (int i = 0; i < n; ++i)
        placeholders.append(QStringLiteral("?"));

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM heartbeat_reports "
                             "WHERE config_id IN (%1) "
                             "ORDER BY started_at DESC")
                  .arg(placeholders.join(QStringLiteral(", "))));
    for (int i = 0; i < n; ++i) {
        q.addBindValue(configIds.at(i));
    }
    if (!q.exec()) {
        qCWarning(verzetaUi) << "HeartbeatReportsModel::queryReports failed:"
                             << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(HeartbeatReport::fromSqlRecord(q.record()));
    }
    return out;
}

int HeartbeatReportsModel::findRow(const QString& reportId) const {
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).id == reportId)
            return i;
    }
    return -1;
}

void HeartbeatReportsModel::recomputePendingReviewCount() {
    int n = 0;
    for (const HeartbeatReport& r : m_rows) {
        // "Needs review" = anything not posted and not dismissed.
        // The user can still take action on these via "Post anyway"
        // / "Dismiss" in the overlay (manual override path lands in
        // H3 — H2 just renders the count).
        if (r.surfaceStatus == HeartbeatSurfaceStatus::kPending ||
            r.surfaceStatus == HeartbeatSurfaceStatus::kSkippedByAgent ||
            r.surfaceStatus == HeartbeatSurfaceStatus::kSkippedByGate ||
            r.surfaceStatus == HeartbeatSurfaceStatus::kSkippedByRateLimit) {
            ++n;
        }
    }
    if (n != m_pendingReviewCount) {
        m_pendingReviewCount = n;
        emit pendingReviewCountChanged();
    }
}

void HeartbeatReportsModel::refreshConfigsCache() {
    m_configsByConfigId.clear();

    if (!m_activeConvId.isEmpty()) {
        const QList<HeartbeatConfig> oneToOne =
            m_configSvc.configsInScope(HeartbeatScopeType::Conversation1to1, m_activeConvId);
        for (const HeartbeatConfig& c : oneToOne)
            m_configsByConfigId.insert(c.id, c);

        const QList<HeartbeatConfig> group =
            m_configSvc.configsInScope(HeartbeatScopeType::ConversationGroup, m_activeConvId);
        for (const HeartbeatConfig& c : group)
            m_configsByConfigId.insert(c.id, c);
        return;
    }
    if (!m_activeFolderId.isEmpty()) {
        const QList<HeartbeatConfig> rows =
            m_configSvc.configsInScope(HeartbeatScopeType::Folder, m_activeFolderId);
        for (const HeartbeatConfig& c : rows)
            m_configsByConfigId.insert(c.id, c);
    }
}

void HeartbeatReportsModel::reloadOneRow(const QString& reportId) {
    if ((m_activeConvId.isEmpty() && m_activeFolderId.isEmpty()) || reportId.isEmpty()) {
        return;
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT * FROM heartbeat_reports WHERE id = ?"));
    q.addBindValue(reportId);
    if (!q.exec() || !q.next()) {
        return;
    }
    const HeartbeatReport fresh = HeartbeatReport::fromSqlRecord(q.record());
    if (!m_configsByConfigId.contains(fresh.configId)) {
        // The completed run belongs to a config in a different scope —
        // nothing to do for this conversation's model instance.
        return;
    }

    const int existingRow = findRow(reportId);
    if (existingRow >= 0) {
        // Replace in place — this happens when a runFailed lands on a
        // row that runCompleted previously inserted, or vice versa.
        m_rows[existingRow] = fresh;
        const QModelIndex idx = index(existingRow);
        emit dataChanged(idx, idx);
        recomputePendingReviewCount();
        return;
    }

    // Insert at the head — newest-first ordering.
    beginInsertRows({}, 0, 0);
    m_rows.prepend(fresh);
    endInsertRows();
    emit countChanged();
    recomputePendingReviewCount();
}

// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file poll-service.cpp
 * @brief Implementation of PollService: polls + voting persistence.
 * @layer Service
 * @dependencies DbManager, Qt6::Core, Qt6::Sql.
 */


#include "poll-service.h"

#include "../models/activity-event.h"
#include "../models/db-manager.h"
#include "../models/message.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "audit-service.h"
#include "message-service.h"

#include <QDateTime>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

constexpr int kMinOptions = 2;
constexpr int kMaxOptions = 10;

QString isoNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QDateTime parseIso(const QString& s) {
    if (s.isEmpty())
        return {};
    return QDateTime::fromString(s, Qt::ISODateWithMs).toUTC();
}

QString newUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

}  // namespace

PollService::PollService(DbManager& db, QObject* parent) : QObject(parent), m_db(db) {}

void PollService::setMessageService(MessageService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_msgService = svc;
}

void PollService::setAuditService(AuditService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_auditService = svc;
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

QString PollService::createPoll(const QString& conversationId,
                                const QString& creatorAlias,
                                const QString& creatorKind,
                                const QString& question,
                                const QStringList& options,
                                const QString& mode,
                                int closesInMinutes) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (conversationId.isEmpty() || creatorAlias.isEmpty() || question.trimmed().isEmpty()) {
        qCWarning(verzetaUi) << "PollService::createPoll: empty required field";
        return {};
    }
    if (creatorKind != QStringLiteral("agent") && creatorKind != QStringLiteral("user")) {
        qCWarning(verzetaUi) << "PollService::createPoll: invalid creator_kind" << creatorKind;
        return {};
    }
    if (options.size() < kMinOptions || options.size() > kMaxOptions) {
        qCWarning(verzetaUi) << "PollService::createPoll: option count out of range"
                             << options.size();
        return {};
    }
    const QString resolvedMode = (mode.isEmpty() ? QStringLiteral("single") : mode.toLower());
    if (resolvedMode != QStringLiteral("single") && resolvedMode != QStringLiteral("multi")) {
        qCWarning(verzetaUi) << "PollService::createPoll: unsupported mode" << mode;
        return {};
    }

    // Strip empties + de-dup case-insensitive while preserving order.
    QStringList cleanOptions;
    QStringList lowerSeen;
    for (const QString& raw : options) {
        const QString t = raw.trimmed();
        if (t.isEmpty())
            continue;
        const QString lower = t.toLower();
        if (lowerSeen.contains(lower))
            continue;
        lowerSeen.append(lower);
        cleanOptions.append(t);
    }
    if (cleanOptions.size() < kMinOptions) {
        qCWarning(verzetaUi) << "PollService::createPoll: too few distinct options after trim";
        return {};
    }

    const QString pollId = newUuid();
    const QString createdAt = isoNow();
    QString closesAt;
    if (closesInMinutes > 0) {
        closesAt = QDateTime::currentDateTimeUtc()
                       .addSecs(qint64(closesInMinutes) * 60)
                       .toString(Qt::ISODateWithMs);
    }

    if (!m_db.db().transaction()) {
        qCWarning(verzetaDb) << "PollService::createPoll: begin tx failed:"
                             << m_db.db().lastError().text();
        return {};
    }

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT INTO polls (id, conversation_id, creator_kind, creator_alias,"
                             " question, mode, status, created_at, closes_at, closed_at)"
                             " VALUES (?, ?, ?, ?, ?, ?, 'open', ?, ?, NULL)"));
    q.addBindValue(pollId);
    q.addBindValue(conversationId);
    q.addBindValue(creatorKind);
    q.addBindValue(creatorAlias);
    q.addBindValue(question.trimmed());
    q.addBindValue(resolvedMode);
    q.addBindValue(createdAt);
    q.addBindValue(closesAt.isEmpty() ? QVariant() : QVariant(closesAt));
    if (!q.exec()) {
        qCWarning(verzetaDb) << "PollService::createPoll: insert poll failed:"
                             << q.lastError().text();
        m_db.db().rollback();
        return {};
    }

    int ordering = 0;
    for (const QString& opt : cleanOptions) {
        QSqlQuery oq(m_db.db());
        oq.prepare(QStringLiteral("INSERT INTO poll_options (id, poll_id, ordering, text)"
                                  " VALUES (?, ?, ?, ?)"));
        oq.addBindValue(newUuid());
        oq.addBindValue(pollId);
        oq.addBindValue(ordering++);
        oq.addBindValue(opt);
        if (!oq.exec()) {
            qCWarning(verzetaDb) << "PollService::createPoll: insert option failed:"
                                 << oq.lastError().text();
            m_db.db().rollback();
            return {};
        }
    }

    if (!m_db.db().commit()) {
        qCWarning(verzetaDb) << "PollService::createPoll: commit failed:"
                             << m_db.db().lastError().text();
        m_db.db().rollback();
        return {};
    }

    // Auto-persist a poll-card message so the poll renders inline in
    // the conversation timeline (mirrors ImageService's auto-persist
    // for generated images). The message metadata carries the poll_id
    // so MessageBubble can render the PollCard.
    autoPersistPollMessage(conversationId, pollId, creatorKind, creatorAlias, question.trimmed());

    qCInfo(verzetaUi) << "PollService::createPoll: id=" << pollId << "conv=" << conversationId
                      << "creator=" << creatorAlias << "(" << creatorKind << ")"
                      << "mode=" << resolvedMode << "options=" << cleanOptions.size()
                      << "closesIn=" << closesInMinutes << "min";

    emit pollCreated(conversationId, pollId);
    emit pollUpdated(conversationId, pollId);

    if (m_auditService) {
        m_auditService->record(ActivityEvent::forPollCreated(QString(),
                                                             conversationId,
                                                             pollId,
                                                             creatorKind,
                                                             creatorAlias,
                                                             QString(),
                                                             question.trimmed(),
                                                             resolvedMode));
    }
    return pollId;
}

void PollService::autoPersistPollMessage(const QString& convId,
                                         const QString& pollId,
                                         const QString& creatorKind,
                                         const QString& creatorAlias,
                                         const QString& question) {
    if (!m_msgService || convId.isEmpty() || pollId.isEmpty())
        return;

    QJsonObject metadata;
    metadata.insert(QStringLiteral("poll_id"), pollId);
    metadata.insert(QStringLiteral("produced_by"), QStringLiteral("poll_service"));
    metadata.insert(QStringLiteral("poll_creator_kind"), creatorKind);
    metadata.insert(QStringLiteral("poll_creator_alias"), creatorAlias);
    // Stash the question on the metadata so any reader that only
    // looks at metadata (without resolving the live poll row) can
    // still surface "Poll: <Q>" — used by the Android summary path
    // and any future toast / notification preview.
    metadata.insert(QStringLiteral("poll_question"), question);

    const QString persistedId = m_msgService->addInterveningSystemMessage(
        convId, QStringLiteral("Poll started: %1").arg(question), metadata);
    if (persistedId.isEmpty()) {
        qCWarning(verzetaUi) << "PollService: failed to auto-persist poll-card message"
                             << "for poll" << pollId << "in conv" << convId;
    }
}

bool PollService::castVote(const QString& pollId,
                           const QString& voterAlias,
                           const QString& voterKind,
                           const QString& optionId,
                           const QString& optionText) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (pollId.isEmpty() || voterAlias.isEmpty())
        return false;
    if (voterKind != QStringLiteral("agent") && voterKind != QStringLiteral("user")) {
        return false;
    }
    if (optionId.isEmpty() && optionText.isEmpty()) {
        qCWarning(verzetaUi) << "PollService::castVote: both optionId and optionText empty";
        return false;
    }

    // Lazy auto-close before accepting a vote — past-deadline polls
    // should refuse new votes.
    {
        QString convIdForClose;
        if (maybeLazyAutoClose(pollId, convIdForClose)) {
            emit pollUpdated(convIdForClose, pollId);
            emit pollClosed(convIdForClose, pollId);
        }
    }

    const Poll poll = pollById(pollId);
    if (!poll.isValid())
        return false;
    if (!poll.isOpen()) {
        qCInfo(verzetaUi) << "PollService::castVote: poll" << pollId << "is closed";
        return false;
    }

    // Resolve optionId from text if needed (case-insensitive exact
    // match). Reject if the resolved id doesn't belong to this poll.
    QString resolvedOptionId = optionId;
    if (resolvedOptionId.isEmpty()) {
        const QList<PollOption> opts = optionsForPoll(pollId);
        for (const PollOption& o : opts) {
            if (o.text.compare(optionText, Qt::CaseInsensitive) == 0) {
                resolvedOptionId = o.id;
                break;
            }
        }
        if (resolvedOptionId.isEmpty()) {
            qCInfo(verzetaUi) << "PollService::castVote: option text" << optionText
                              << "not found in poll" << pollId;
            return false;
        }
    } else {
        // Sanity: the supplied id must belong to this poll.
        QSqlQuery sanity(m_db.db());
        sanity.prepare(
            QStringLiteral("SELECT 1 FROM poll_options WHERE id = ? AND poll_id = ? LIMIT 1"));
        sanity.addBindValue(resolvedOptionId);
        sanity.addBindValue(pollId);
        if (!sanity.exec() || !sanity.next()) {
            qCInfo(verzetaUi) << "PollService::castVote: option" << optionId
                              << "doesn't belong to poll" << pollId;
            return false;
        }
    }

    // Single mode enforces exactly one row per (poll, voter): replace
    // any prior vote by this voter so they can change their mind.
    if (poll.mode == QStringLiteral("single")) {
        QSqlQuery del(m_db.db());
        del.prepare(QStringLiteral("DELETE FROM poll_votes WHERE poll_id = ? AND voter_alias = ?"));
        del.addBindValue(pollId);
        del.addBindValue(voterAlias);
        if (!del.exec()) {
            qCWarning(verzetaDb) << "PollService::castVote: clearing prior single-vote failed:"
                                 << del.lastError().text();
            return false;
        }
    }
    // Multi mode: the UNIQUE(poll_id, voter_alias, option_id) constraint
    // collapses repeat votes on the same option to no-op. We just
    // INSERT OR IGNORE so the second click is benign.

    QSqlQuery ins(m_db.db());
    ins.prepare(QStringLiteral("INSERT OR IGNORE INTO poll_votes "
                               "(id, poll_id, voter_kind, voter_alias, option_id, rank, cast_at) "
                               "VALUES (?, ?, ?, ?, ?, NULL, ?)"));
    ins.addBindValue(newUuid());
    ins.addBindValue(pollId);
    ins.addBindValue(voterKind);
    ins.addBindValue(voterAlias);
    ins.addBindValue(resolvedOptionId);
    ins.addBindValue(isoNow());
    if (!ins.exec()) {
        qCWarning(verzetaDb) << "PollService::castVote: insert vote failed:"
                             << ins.lastError().text();
        return false;
    }

    qCInfo(verzetaUi) << "PollService::castVote: poll=" << pollId << "voter=" << voterAlias << "("
                      << voterKind << ")"
                      << "option=" << resolvedOptionId << "mode=" << poll.mode;

    emit voteCast(poll.conversationId, pollId, voterAlias, resolvedOptionId);
    emit pollUpdated(poll.conversationId, pollId);

    if (m_auditService) {
        m_auditService->record(ActivityEvent::forPollVote(
            QString(), poll.conversationId, pollId, voterKind, voterAlias, QString(), optionText));
    }
    return true;
}

bool PollService::closePoll(const QString& pollId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (pollId.isEmpty())
        return false;

    const Poll p = pollById(pollId);
    if (!p.isValid())
        return false;
    if (!p.isOpen())
        return true;  // already closed → idempotent ok

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE polls SET status='closed', closed_at=? WHERE id=?"));
    q.addBindValue(isoNow());
    q.addBindValue(pollId);
    if (!q.exec()) {
        qCWarning(verzetaDb) << "PollService::closePoll: update failed:" << q.lastError().text();
        return false;
    }
    qCInfo(verzetaUi) << "PollService::closePoll: id=" << pollId << "conv=" << p.conversationId;
    emit pollUpdated(p.conversationId, pollId);
    emit pollClosed(p.conversationId, pollId);

    if (m_auditService) {
        m_auditService->record(ActivityEvent::forPollClosed(QString(),
                                                            p.conversationId,
                                                            pollId,
                                                            QStringLiteral("system"),
                                                            QString(),
                                                            QString(),
                                                            QString()));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Read paths
// ---------------------------------------------------------------------------

QVariantMap PollService::pollResults(const QString& pollId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (pollId.isEmpty())
        return {};

    QString convIdForClose;
    if (maybeLazyAutoClose(pollId, convIdForClose)) {
        emit pollUpdated(convIdForClose, pollId);
        emit pollClosed(convIdForClose, pollId);
    }
    const Poll p = pollById(pollId);
    if (!p.isValid())
        return {};
    return projectPoll(p,
                       optionsForPoll(pollId),
                       votesForPoll(pollId),
                       /*elideOptionText*/ false);
}

QVariantList PollService::pollsForConversation(const QString& conversationId, bool openOnly) {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    if (conversationId.isEmpty())
        return out;

    QSqlQuery list(m_db.db());
    const QString sql =
        openOnly ? QStringLiteral("SELECT id FROM polls WHERE conversation_id=? AND status='open' "
                                  "ORDER BY created_at DESC")
                 : QStringLiteral("SELECT id FROM polls WHERE conversation_id=? "
                                  "ORDER BY created_at DESC");
    list.prepare(sql);
    list.addBindValue(conversationId);
    if (!list.exec()) {
        qCWarning(verzetaDb) << "PollService::pollsForConversation: query failed:"
                             << list.lastError().text();
        return out;
    }
    QStringList ids;
    while (list.next())
        ids.append(list.value(0).toString());

    // Lazy auto-close every poll first; collect any flips so we emit
    // the signals BEFORE we project (so the projected status is fresh).
    QStringList flippedIds;
    for (const QString& id : ids) {
        QString convId;
        if (maybeLazyAutoClose(id, convId))
            flippedIds.append(id);
    }
    for (const QString& id : flippedIds) {
        emit pollUpdated(conversationId, id);
        emit pollClosed(conversationId, id);
    }
    // Re-filter for openOnly (a poll we just flipped should drop out).
    QStringList finalIds;
    if (openOnly) {
        for (const QString& id : ids) {
            const Poll p = pollById(id);
            if (p.isOpen())
                finalIds.append(id);
        }
    } else {
        finalIds = ids;
    }

    for (const QString& id : finalIds) {
        const Poll p = pollById(id);
        if (!p.isValid())
            continue;
        out.append(projectPoll(p,
                               optionsForPoll(id),
                               votesForPoll(id),
                               /*elideOptionText*/ false));
    }
    return out;
}

// ---------------------------------------------------------------------------
// C++ accessors
// ---------------------------------------------------------------------------

Poll PollService::pollById(const QString& pollId) const {
    Poll p;
    if (pollId.isEmpty())
        return p;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT id, conversation_id, creator_kind, creator_alias, question,"
                             " mode, status, created_at, closes_at, closed_at"
                             " FROM polls WHERE id=? LIMIT 1"));
    q.addBindValue(pollId);
    if (!q.exec() || !q.next())
        return p;

    p.id = q.value(0).toString();
    p.conversationId = q.value(1).toString();
    p.creatorKind = q.value(2).toString();
    p.creatorAlias = q.value(3).toString();
    p.question = q.value(4).toString();
    p.mode = q.value(5).toString();
    p.status = q.value(6).toString();
    p.createdAt = parseIso(q.value(7).toString());
    p.closesAt = parseIso(q.value(8).toString());
    p.closedAt = parseIso(q.value(9).toString());
    return p;
}

QList<PollOption> PollService::optionsForPoll(const QString& pollId) const {
    QList<PollOption> out;
    if (pollId.isEmpty())
        return out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT id, poll_id, ordering, text FROM poll_options "
                             "WHERE poll_id=? ORDER BY ordering ASC"));
    q.addBindValue(pollId);
    if (!q.exec())
        return out;
    while (q.next()) {
        PollOption o;
        o.id = q.value(0).toString();
        o.pollId = q.value(1).toString();
        o.ordering = q.value(2).toInt();
        o.text = q.value(3).toString();
        out.append(o);
    }
    return out;
}

QList<PollVote> PollService::votesForPoll(const QString& pollId) const {
    QList<PollVote> out;
    if (pollId.isEmpty())
        return out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT id, poll_id, voter_kind, voter_alias, option_id, rank, cast_at"
                             " FROM poll_votes WHERE poll_id=? ORDER BY cast_at ASC"));
    q.addBindValue(pollId);
    if (!q.exec())
        return out;
    while (q.next()) {
        PollVote v;
        v.id = q.value(0).toString();
        v.pollId = q.value(1).toString();
        v.voterKind = q.value(2).toString();
        v.voterAlias = q.value(3).toString();
        v.optionId = q.value(4).toString();
        v.rank = q.value(5).isNull() ? -1 : q.value(5).toInt();
        v.castAt = parseIso(q.value(6).toString());
        out.append(v);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

QVariantMap PollService::projectPoll(const Poll& p,
                                     const QList<PollOption>& opts,
                                     const QList<PollVote>& votes,
                                     bool elideOptionText) const {
    QVariantMap m;
    m.insert(QStringLiteral("id"), p.id);
    m.insert(QStringLiteral("conversation_id"), p.conversationId);
    m.insert(QStringLiteral("creator_kind"), p.creatorKind);
    m.insert(QStringLiteral("creator_alias"), p.creatorAlias);
    m.insert(QStringLiteral("question"), p.question);
    m.insert(QStringLiteral("mode"), p.mode);
    m.insert(QStringLiteral("status"), p.status);
    m.insert(QStringLiteral("created_at"),
             p.createdAt.isValid() ? p.createdAt.toString(Qt::ISODateWithMs) : QString());
    m.insert(QStringLiteral("closes_at"),
             p.closesAt.isValid() ? p.closesAt.toString(Qt::ISODateWithMs) : QString());
    m.insert(QStringLiteral("closed_at"),
             p.closedAt.isValid() ? p.closedAt.toString(Qt::ISODateWithMs) : QString());

    // Per-option tally + winner detection.
    QVariantList outOpts;
    int maxVotes = -1;
    for (const PollOption& o : opts) {
        int n = 0;
        for (const PollVote& v : votes)
            if (v.optionId == o.id)
                ++n;
        if (n > maxVotes)
            maxVotes = n;
        QVariantMap em;
        em.insert(QStringLiteral("id"), o.id);
        em.insert(QStringLiteral("ordering"), o.ordering);
        if (!elideOptionText) {
            em.insert(QStringLiteral("text"), o.text);
        }
        em.insert(QStringLiteral("votes"), n);
        outOpts.append(em);
    }
    m.insert(QStringLiteral("options"), outOpts);

    // Winner = highest-vote option(s). Empty when no votes cast yet
    // OR when there's a tie. Useful to show "winning: Yes" in QML.
    QStringList winners;
    if (maxVotes > 0) {
        for (const PollOption& o : opts) {
            int n = 0;
            for (const PollVote& v : votes)
                if (v.optionId == o.id)
                    ++n;
            if (n == maxVotes)
                winners.append(o.id);
        }
    }
    m.insert(QStringLiteral("winning_option_ids"), winners);
    m.insert(QStringLiteral("total_votes"), votes.size());

    QStringList voterAliases;
    for (const PollVote& v : votes) {
        if (!voterAliases.contains(v.voterAlias))
            voterAliases.append(v.voterAlias);
    }
    m.insert(QStringLiteral("voter_count"), voterAliases.size());
    return m;
}

bool PollService::maybeLazyAutoClose(const QString& pollId, QString& outConvId) {
    if (pollId.isEmpty())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT conversation_id, closes_at, status FROM polls WHERE id=?"));
    q.addBindValue(pollId);
    if (!q.exec() || !q.next())
        return false;

    const QString convId = q.value(0).toString();
    const QString closesIso = q.value(1).toString();
    const QString status = q.value(2).toString();
    if (status != QStringLiteral("open"))
        return false;
    if (closesIso.isEmpty())
        return false;

    const QDateTime closesAt = parseIso(closesIso);
    if (!closesAt.isValid())
        return false;
    if (QDateTime::currentDateTimeUtc() < closesAt)
        return false;

    QSqlQuery upd(m_db.db());
    upd.prepare(QStringLiteral("UPDATE polls SET status='closed', closed_at=? WHERE id=? "
                               "AND status='open'"));
    upd.addBindValue(isoNow());
    upd.addBindValue(pollId);
    if (!upd.exec()) {
        qCWarning(verzetaDb) << "PollService::maybeLazyAutoClose: update failed:"
                             << upd.lastError().text();
        return false;
    }
    if (upd.numRowsAffected() <= 0)
        return false;
    outConvId = convId;

    if (m_auditService) {
        m_auditService->record(ActivityEvent::forPollClosed(
            QString(), convId, pollId, QStringLiteral("system"), QString(), QString(), QString()));
    }
    return true;
}

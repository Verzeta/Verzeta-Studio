// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file poll-service.h
 * @brief Group-chat coordination polls + voting.
 *
 *        Schema supports two vote modes: 'single' (one vote per
 *        voter) and 'multi' (multiple options per voter). Ranked-
 *        choice is schema-reserved but rejected at createPoll()
 *        until tabulation lands.
 *
 *        Lazy auto-close: results() and pollsForConversation()
 *        check closes_at on every read and flip 'open' to 'closed'
 *        when now > closes_at. No background timer / sweep job.
 *
 *        Agents and the user both vote: voterKind ∈ {'agent','user'}.
 *        The cascade controller resolves agent identity (alias is
 *        unique per group chat). For the human, the alias is the
 *        literal string "user".
 *
 * @layer Service
 * @dependencies DbManager (reference). Strictly main-thread.
 */
#pragma once

#include "../models/poll.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class AuditService;
class DbManager;
class MessageService;

/**
 * @brief Polls and voting feature for group conversations. Owns
 *        polls / poll_options / poll_votes table CRUD, the lazy
 *        auto-close on read, and the optional message-card +
 *        audit-log side effects.
 */
class PollService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the poll service.
     * @param db     Non-owning reference to DbManager.
     * @param parent Qt parent (AppController).
     */
    explicit PollService(DbManager& db, QObject* parent = nullptr);
    ~PollService() override = default;

    /**
     * @brief Attach a MessageService so newly-created polls
     *        auto-persist as an inline poll-card message in the
     *        conversation. Without this attached, createPoll still
     *        works (DB row + signal) but no message is written;
     *        the poll only surfaces via the ACTIVE POLLS prompt
     *        layer / explicit get_poll_results.
     *
     *        Mirrors ImageService::setMessageService. AppController
     *        wires this once during initialize().
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setMessageService(MessageService* svc);

    /**
     * @brief Attach an AuditService so poll lifecycle events
     *        (poll_created / poll_vote / poll_closed) are recorded
     *        to the activity_log table.
     *
     *        Non-owning. AppController wires this once during
     *        initialize(). When unset, the audit hook is a no-op
     *        (the poll itself still creates / votes / closes).
     * @param svc Non-owning pointer; pass nullptr to detach.
     */
    void setAuditService(AuditService* svc);

    // ------------------------------------------------------------------
    // CRUD
    // ------------------------------------------------------------------

    /**
     * @brief Creates a new poll in @p conversationId.
     * @param conversationId   Owning conversation UUID.
     * @param creatorAlias     Cascade alias for an agent caller, or
     *                         the literal "user" when the user
     *                         created the poll from QML / Android.
     * @param creatorKind      "agent" or "user".
     * @param question         Plain text question (non-empty).
     * @param options          2..10 option text strings.
     * @param mode             "single" or "multi". Today the service
     *                         rejects "ranked" with an explicit
     *                         error.
     * @param closesInMinutes  Optional deadline in minutes from now;
     *                         0 (or negative) disables auto-close.
     * @returns Newly-created poll id, or empty QString on validation
     *          failure (creator/kind/options empty, mode unsupported,
     *          too few/many options, conv id missing).
     */
    Q_INVOKABLE QString createPoll(const QString& conversationId,
                                   const QString& creatorAlias,
                                   const QString& creatorKind,
                                   const QString& question,
                                   const QStringList& options,
                                   const QString& mode,
                                   int closesInMinutes);

    /**
     * @brief Cast a vote on @p pollId. Accepts EITHER an explicit
     *        option_id OR an option_text; the service resolves
     *        text to id with a case-insensitive match against the
     *        poll's options. Small models find writing the literal
     *        option text easier than copying two UUIDs in a row.
     * @param pollId      Poll UUID to vote on.
     * @param voterAlias  Identity of the voter (agent alias or the
     *                    literal "user").
     * @param voterKind   "agent" or "user".
     * @param optionId    Explicit option id (preferred when known).
     * @param optionText  Text to resolve when @p optionId is empty.
     *                    At least one of the two must be non-empty.
     * @returns true on success; false if poll is closed, voter
     *          already voted in 'single' mode, or option couldn't be
     *          resolved.
     */
    bool castVote(const QString& pollId,
                  const QString& voterAlias,
                  const QString& voterKind,
                  const QString& optionId,
                  const QString& optionText);

    /**
     * @brief Convenience wrapper: cast on (poll, alias, kind, id).
     * @param pollId     Poll UUID.
     * @param voterAlias Voter alias.
     * @param voterKind  "agent" or "user".
     * @param optionId   Explicit option id.
     * @returns true on success; same failure semantics as castVote.
     */
    Q_INVOKABLE bool castVoteByOptionId(const QString& pollId,
                                        const QString& voterAlias,
                                        const QString& voterKind,
                                        const QString& optionId) {
        return castVote(pollId, voterAlias, voterKind, optionId, QString());
    }

    /**
     * @brief Convenience wrapper: cast on (poll, alias, kind, text).
     * @param pollId     Poll UUID.
     * @param voterAlias Voter alias.
     * @param voterKind  "agent" or "user".
     * @param optionText Option text to resolve (case-insensitive).
     * @returns true on success; same failure semantics as castVote.
     */
    Q_INVOKABLE bool castVoteByOptionText(const QString& pollId,
                                          const QString& voterAlias,
                                          const QString& voterKind,
                                          const QString& optionText) {
        return castVote(pollId, voterAlias, voterKind, QString(), optionText);
    }

    /**
     * @brief Explicitly close a poll. Idempotent: closing an
     *        already-closed poll is a no-op success.
     * @param pollId Poll UUID to close.
     * @returns true on success; false on unknown pollId or DB
     *          failure.
     */
    Q_INVOKABLE bool closePoll(const QString& pollId);

    // ------------------------------------------------------------------
    // Read paths (also fire lazy auto-close)
    // ------------------------------------------------------------------

    /**
     * @brief Returns the poll's full state plus the current tally
     *        for every option. Fires lazy auto-close + emits
     *        pollClosed if the poll just rolled past closes_at.
     *
     *        Output shape (QVariantMap projection):
     *          {
     *            id, conversation_id, creator_kind, creator_alias,
     *            question, mode, status, created_at, closes_at,
     *            closed_at,
     *            options: [{ id, text, ordering, votes }],
     *            total_votes, voter_count
     *          }
     * @param pollId Poll UUID to query.
     * @returns Projection map as described; empty map when @p pollId
     *          is unknown.
     */
    Q_INVOKABLE QVariantMap pollResults(const QString& pollId);

    /**
     * @brief Compact list of polls in a conversation. Each entry has
     *        the same shape as pollResults() but with an `options`
     *        array elided to just `{id, text, votes}` for brevity.
     * @param conversationId Conversation UUID to enumerate.
     * @param openOnly       true → only status='open'; false → all
     *                       polls.
     * @returns QVariantList of compact poll projections in created_at
     *          order; empty when the conversation has no polls.
     */
    Q_INVOKABLE QVariantList pollsForConversation(const QString& conversationId, bool openOnly);

    // ------------------------------------------------------------------
    // C++ accessors (test-friendly, signature-stable)
    // ------------------------------------------------------------------

    /**
     * @brief C++ accessor: fetch a poll row by id.
     * @param pollId Poll UUID.
     * @returns Poll struct; default-constructed (empty `id`) when
     *          the poll is absent.
     */
    Poll pollById(const QString& pollId) const;

    /**
     * @brief C++ accessor: list the options for a poll.
     * @param pollId Poll UUID.
     * @returns Options in `ordering` ASC; empty list when the poll
     *          is absent.
     */
    QList<PollOption> optionsForPoll(const QString& pollId) const;

    /**
     * @brief C++ accessor: list the votes cast on a poll.
     * @param pollId Poll UUID.
     * @returns Votes in cast-time ASC; empty list when the poll has
     *          no votes (or is absent).
     */
    QList<PollVote> votesForPoll(const QString& pollId) const;

  signals:
    /**
     * @brief Fired when a new poll is created.
     * @param convId Conversation UUID the poll belongs to.
     * @param pollId UUID of the newly-created poll.
     */
    void pollCreated(const QString& convId, const QString& pollId);

    /**
     * @brief Fired when a vote is cast or the poll status changes.
     * @param convId Conversation UUID.
     * @param pollId UUID of the updated poll.
     */
    void pollUpdated(const QString& convId, const QString& pollId);

    /**
     * @brief Fired when a poll transitions to status='closed' (auto
     *        or explicit). Always preceded by pollUpdated for the
     *        same id.
     * @param convId Conversation UUID.
     * @param pollId UUID of the closed poll.
     */
    void pollClosed(const QString& convId, const QString& pollId);

    /**
     * @brief Fired with the individual vote details. The bridge
     *        forwards this so wire clients can render an inline
     *        "Alice voted Yes" trace without re-fetching
     *        pollResults().
     * @param convId      Conversation UUID.
     * @param pollId      Poll UUID the vote was cast on.
     * @param voterAlias  Alias of the voter (agent alias or the
     *                    literal "user").
     * @param optionId    Option UUID that was voted for.
     */
    void voteCast(const QString& convId,
                  const QString& pollId,
                  const QString& voterAlias,
                  const QString& optionId);

  private:
    QVariantMap projectPoll(const Poll& p,
                            const QList<PollOption>& opts,
                            const QList<PollVote>& votes,
                            bool elideOptionText) const;

    /** Promote 'open' to 'closed' when now > closes_at. Returns true
     *  if the row flipped (caller emits pollClosed). No-op when the
     *  row is missing or already closed. */
    bool maybeLazyAutoClose(const QString& pollId, QString& outConvId);

    /** Auto-persist a poll-card message for a newly-created poll.
     *  No-op when m_msgService is null. The message has
     *  role=assistant, metadata.poll_id set so MessageBubble renders
     *  the PollCard inline, and metadata.produced_by="poll_service"
     *  so artifact / task observers can distinguish it from a normal
     *  LLM reply. */
    void autoPersistPollMessage(const QString& convId,
                                const QString& pollId,
                                const QString& creatorKind,
                                const QString& creatorAlias,
                                const QString& question);

    DbManager& m_db;
    MessageService* m_msgService = nullptr;  // non-owning; optional
    AuditService* m_auditService = nullptr;  // non-owning; optional
};

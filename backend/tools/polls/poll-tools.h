// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file poll-tools.h
 * @brief Four agent-callable poll tools.
 *
 *          - `start_poll(question, options, mode?, closes_in_minutes?)`
 *          - `cast_vote(poll_id, option_id? OR option_text?)`
 *          - `get_poll_results(poll_id? OR latest?)`
 *          - `close_poll(poll_id)`
 *
 *        All four resolve the caller's identity (alias + kind="agent")
 *        through CascadeController, mirroring the skill / membership /
 *        heartbeat tool families. Each tool runs on the main thread
 *        because PollService is main-thread-only.
 * @layer Service (Tool subsystem)
 * @dependencies PollService, ConversationService, CascadeController
 *               (via PollToolDeps).
 */


#pragma once

#include "../itool.h"
#include "poll-tool-deps.h"

namespace Tools {

/**
 * @brief ITool implementation for the `start_poll` built-in. Creates a
 *        new poll and persists its options.
 */
class StartPollTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of poll-tool dependencies.
     */
    explicit StartPollTool(const PollToolDeps& deps) : m_deps(deps) {}

    /**
     * @brief Canonical tool name.
     * @returns "start_poll".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the start-poll behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `question` (required), `options`
     *          (required string list), `mode` (optional), and
     *          `closes_in_minutes` (optional int).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because PollService is main-thread-only.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Conversation-capability scope.
     * @returns GroupOnly, because polling is a group-coordination feature with no
     *          meaning in a single-agent chat.
     */
    ToolScope scope() const override { return ToolScope::GroupOnly; }

    /**
     * @brief Persist the new poll and its options.
     * @param args  JSON object with the schema-defined fields.
     * @returns `{"poll_id": ..., "status": "open"}` on success;
     *          structured error JSON otherwise.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    PollToolDeps m_deps;
};

/**
 * @brief ITool implementation for the `cast_vote` built-in. Records
 *        the caller's vote against a poll option.
 */
class CastVoteTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of poll-tool dependencies.
     */
    explicit CastVoteTool(const PollToolDeps& deps) : m_deps(deps) {}

    /**
     * @brief Canonical tool name.
     * @returns "cast_vote".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the cast-vote behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `poll_id` (required) and one of
     *          `option_id` / `option_text`.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because PollService is main-thread-only.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Conversation-capability scope.
     * @returns GroupOnly, because polling is a group-coordination feature with no
     *          meaning in a single-agent chat.
     */
    ToolScope scope() const override { return ToolScope::GroupOnly; }

    /**
     * @brief Persist the caller's vote.
     * @param args  JSON object with the schema-defined fields.
     * @returns `{"ok": true}` on success; structured error JSON when
     *          the poll is closed, the option is unknown, or the
     *          caller cannot vote.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    PollToolDeps m_deps;
};

/**
 * @brief ITool implementation for the `get_poll_results` built-in.
 *        Returns the current vote tally for a poll.
 */
class GetPollResultsTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of poll-tool dependencies.
     */
    explicit GetPollResultsTool(const PollToolDeps& deps) : m_deps(deps) {}

    /**
     * @brief Canonical tool name.
     * @returns "get_poll_results".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the results-query behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `poll_id` (optional) and `latest`
     *          (optional bool).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because PollService is main-thread-only.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Conversation-capability scope.
     * @returns GroupOnly, because polling is a group-coordination feature with no
     *          meaning in a single-agent chat.
     */
    ToolScope scope() const override { return ToolScope::GroupOnly; }

    /**
     * @brief Return current vote counts for the requested poll.
     * @param args  JSON object with either `poll_id` or `latest=true`.
     * @returns `{"poll_id": ..., "options": [{"text": ..., "votes":
     *          ...}], "status": ...}`; structured error JSON otherwise.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    PollToolDeps m_deps;
};

/**
 * @brief ITool implementation for the `close_poll` built-in. Closes an
 *        open poll and records the winning option.
 */
class ClosePollTool : public ITool {
  public:
    /**
     * @brief Constructs the tool with the injected service dependencies.
     * @param deps  Bundle of poll-tool dependencies.
     */
    explicit ClosePollTool(const PollToolDeps& deps) : m_deps(deps) {}

    /**
     * @brief Canonical tool name.
     * @returns "close_poll".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the close-poll behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `poll_id` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns true, because PollService is main-thread-only.
     */
    bool runsOnMainThread() const override { return true; }

    /**
     * @brief Conversation-capability scope.
     * @returns GroupOnly, because polling is a group-coordination feature with no
     *          meaning in a single-agent chat.
     */
    ToolScope scope() const override { return ToolScope::GroupOnly; }

    /**
     * @brief Close the poll and persist the winning option.
     * @param args  JSON object with required `poll_id`.
     * @returns `{"ok": true, "winner": ...}` on success; structured
     *          error JSON otherwise.
     */
    QJsonValue invoke(const QJsonObject& args) override;

  private:
    PollToolDeps m_deps;
};

}  // namespace Tools

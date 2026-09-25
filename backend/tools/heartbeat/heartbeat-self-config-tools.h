// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-self-config-tools.h
 * @brief Five LLM-callable tools that let an agent manage its own
 *        heartbeat configuration in the (agent, scope, alias) tuple
 *        it is currently invoked under.
 *
 *        Subject to hard caps the agent CANNOT override:
 *
 *          - The schedule must parse to one of the four named formats
 *            (`\@hourly`, `\@daily at HH:MM`, `\@weekly[ on DAY at HH:MM]`,
 *            `\@interval N≥5`); shorthand and cron are rejected.
 *          - The schedule's effective fire rate (firesIn24h) must not
 *            exceed the user-set `max_runs_per_day`.
 *          - `enable_heartbeat` requires the heartbeat_configs row's
 *            `self_config_allowed=1` grant (user-only flag) AND
 *            respects the global pause kill switch, with the same precedence
 *            as the schedule tick.
 *          - The per-conversation `allow_heartbeat_auto_surface` gate,
 *            the per-conversation daily cap, and `self_config_allowed`
 *            itself are user-only; none of these tools touches them.
 *
 *        Every successful mutation writes a row to
 *        `heartbeat_config_changes` with `source='agent'` so the
 *        Diagnostics → Self-config audit panel can surface the agent's
 *        actions to the user.
 * @layer Service (Tool subsystem)
 * @dependencies HeartbeatConfigService, HeartbeatSubagentService,
 *               AgentRegistry, ConversationService, CascadeController,
 *               ChatController.
 */


#pragma once

#include "../itool.h"

#include <functional>
#include <memory>
#include <QString>

class AgentRegistry;
class ConversationService;
class HeartbeatConfigService;
class HeartbeatSubagentService;

namespace Chat {
class CascadeController;
}

namespace Tools {

/**
 * @brief Bundle of non-owning service references needed by every
 *        heartbeat self-config tool. AppController constructs this
 *        once at registration time and hands it into each tool's
 *        constructor.
 *
 * The `activeConvIdGetter` pattern (rather than a ChatController*
 * reference) mirrors the memory-tool family in tool-registration.cpp:
 * tools should depend on the smallest possible interface. Tests pass a
 * captured lambda; production code captures ChatController::
 * activeConversationId. Decoupling keeps the integration tests for
 * these tools fast and standalone.
 */
struct HeartbeatToolDeps {
    HeartbeatConfigService* configSvc = nullptr;  ///< Reads and writes heartbeat config rows.
    HeartbeatSubagentService* subagentSvc =
        nullptr;                           ///< Rebuilds schedules; reports the global pause.
    AgentRegistry* agents = nullptr;       ///< Agent templates; required by isValid().
    ConversationService* convs = nullptr;  ///< Resolves the calling conversation.
    /// Resolver for the in-flight run's cascade (fallback
    /// responder identity when args-injected identity is absent), resolved
    /// per invocation rather than pinned once at registration so a tool
    /// firing on a backgrounded run reads the right responder.
    std::function<Chat::CascadeController*()> cascadeResolver;
    /// Returns the UI-active conversation id; used when the call carries
    /// no `__caller_conv_id`.
    std::function<QString()> activeConvIdGetter;

    /**
     * @brief Reports whether every required dependency is populated.
     * @returns True iff all fields (including the lambda) are set.
     */
    bool isValid() const;
};

/**
 * @brief Common base for the five self-config tools.
 *
 * Resolves the (agent, scope, alias, configId) tuple from the LLM
 * dispatch context. Each subclass invokes resolveContext() at the top
 * of its own invoke() body and bails with a structured error if the
 * heartbeat config row for the caller does not exist.
 */
class HeartbeatSelfConfigToolBase : public ITool {
  public:
    /**
     * @brief Constructs the base with the injected dependency bundle.
     * @param deps  Bundle of HeartbeatToolDeps. Must be `isValid()`.
     */
    explicit HeartbeatSelfConfigToolBase(const HeartbeatToolDeps& deps);

    /**
     * @brief Thread-residency declaration.
     * @returns true, because every service in HeartbeatToolDeps is main-thread-only.
     */
    bool runsOnMainThread() const override;

  protected:
    /**
     * @brief Resolves the heartbeat config row that owns the caller.
     * @param args         Tool args including (optionally) the `__caller_*`
     *                     keys.
     * @param outConfigId  Resolved heartbeat config UUID (output). Set to
     *                     empty when resolution fails.
     * @returns Empty QJsonValue on success; structured error JSON when
     *          the call was made outside a cascade turn or the
     *          heartbeat row does not exist.
     *
     * Reads the caller's identity via the args-injection precedence
     * chain: `__caller_agent_id` / `__caller_agent_alias` from args
     * first (host-injected by ToolDispatcher from the calling CC's
     * batch inputs), `cascade->currentResponder*()` fallback when
     * args are absent.  Conv id resolves the same way:
     * `__caller_conv_id` from args first, `activeConvIdGetter()`
     * fallback.  This is what makes wire-side (Android-paired) per-
     * client cascades correctly update THEIR own agent's heartbeat
     * config instead of the LOCAL desktop responder's.
     */
    QJsonValue resolveContext(const QJsonObject& args, QString& outConfigId);

    HeartbeatToolDeps m_deps;  ///< Injected dependencies.
};

/**
 * @brief `set_heartbeat_goal`: replace the standing instruction (the
 *        routine's WHAT) for the caller's heartbeat config row.
 */
class SetHeartbeatGoalTool : public HeartbeatSelfConfigToolBase {
  public:
    using HeartbeatSelfConfigToolBase::HeartbeatSelfConfigToolBase;

    /**
     * @brief Canonical tool name.
     * @returns "set_heartbeat_goal".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the goal-update behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `goal` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Persist the new goal text after resolving the caller's
     *        config row.
     * @param args  JSON object with required `goal`.
     * @returns `{"ok": true, "config_id": ...}` on success; structured
     *          error JSON otherwise.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

/**
 * @brief `set_heartbeat_schedule`: replace the named-format schedule
 *        on the caller's heartbeat config row. Refuses cron / shorthand
 *        forms and rejects schedules that exceed `max_runs_per_day`.
 */
class SetHeartbeatScheduleTool : public HeartbeatSelfConfigToolBase {
  public:
    using HeartbeatSelfConfigToolBase::HeartbeatSelfConfigToolBase;

    /**
     * @brief Canonical tool name.
     * @returns "set_heartbeat_schedule".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the schedule-update behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `schedule` string (named
     *          format).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Persist the new schedule after validating the named format
     *        and the firesIn24h-vs-max_runs_per_day cap.
     * @param args  JSON object with required `schedule`.
     * @returns `{"ok": true, "config_id": ..., "fires_in_24h": ...}` on
     *          success; structured error JSON otherwise.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

/**
 * @brief `set_heartbeat_surface_criteria`: replace the surface
 *        criteria (the routine's WHEN-to-share) on the caller's
 *        heartbeat config row.
 */
class SetHeartbeatSurfaceCriteriaTool : public HeartbeatSelfConfigToolBase {
  public:
    using HeartbeatSelfConfigToolBase::HeartbeatSelfConfigToolBase;

    /**
     * @brief Canonical tool name.
     * @returns "set_heartbeat_surface_criteria".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the surface-criteria update.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptor for the required `surface_criteria` string.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Persist the new surface criteria.
     * @param args  JSON object with required `surface_criteria`.
     * @returns `{"ok": true, "config_id": ...}` on success; structured
     *          error JSON otherwise.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

/**
 * @brief `enable_heartbeat`: flip the caller's heartbeat config row
 *        to enabled=1. Requires `self_config_allowed=1`.
 */
class EnableHeartbeatTool : public HeartbeatSelfConfigToolBase {
  public:
    using HeartbeatSelfConfigToolBase::HeartbeatSelfConfigToolBase;

    /**
     * @brief Canonical tool name.
     * @returns "enable_heartbeat".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the enable behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Empty list, because the tool takes no parameters.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Enable the caller's heartbeat after checking the
     *        self-config grant and global pause kill switch.
     * @param args  Ignored.
     * @returns `{"ok": true, "config_id": ...}` on success; structured
     *          error JSON when the grant is missing or paused.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

/**
 * @brief `disable_heartbeat`: flip the caller's heartbeat config row
 *        to enabled=0. No grant required (an agent can always stop
 *        its own heartbeat).
 */
class DisableHeartbeatTool : public HeartbeatSelfConfigToolBase {
  public:
    using HeartbeatSelfConfigToolBase::HeartbeatSelfConfigToolBase;

    /**
     * @brief Canonical tool name.
     * @returns "disable_heartbeat".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the disable behaviour.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Empty list, because the tool takes no parameters.
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Disable the caller's heartbeat.
     * @param args  Ignored.
     * @returns `{"ok": true, "config_id": ...}` on success; structured
     *          error JSON otherwise.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

}  // namespace Tools

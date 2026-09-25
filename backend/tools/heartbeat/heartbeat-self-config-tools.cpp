// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-self-config-tools.cpp
 * @brief Implementations of the heartbeat self-config tool bodies.
 *
 *        Hard-cap enforcement and audit-row creation are colocated so
 *        every tool's invoke() drains through the same write path.
 * @layer Service (Tool subsystem)
 * @dependencies HeartbeatConfigService, HeartbeatSubagentService,
 *               AgentRegistry, ConversationService, Chat::CascadeController.
 */


#include "heartbeat-self-config-tools.h"

#include "../../models/agent.h"
#include "../../models/conversation.h"
#include "../../models/heartbeat-config.h"
#include "../../services/agent-registry.h"
#include "../../services/chat/cascade-controller.h"
#include "../../services/conversation-service.h"
#include "../../services/heartbeat-config-service.h"
#include "../../services/heartbeat-subagent-service.h"
#include "../../utils/heartbeat-schedule.h"
#include "../../utils/logger.h"

#include <QJsonObject>
#include <QJsonValue>

namespace Tools {

bool HeartbeatToolDeps::isValid() const {
    return configSvc && subagentSvc && agents && convs && cascadeResolver != nullptr &&
           activeConvIdGetter != nullptr;
}

// ---------------------------------------------------------------------------
// HeartbeatSelfConfigToolBase
// ---------------------------------------------------------------------------

HeartbeatSelfConfigToolBase::HeartbeatSelfConfigToolBase(const HeartbeatToolDeps& deps)
    : m_deps(deps) {}

bool HeartbeatSelfConfigToolBase::runsOnMainThread() const {
    // Every body touches HeartbeatConfigService SQL + reads
    // CascadeController / ChatController main-thread state. Per
    // ToolDispatcher contract, runsOnMainThread=true forces inline
    // dispatch.
    return true;
}

namespace {

QJsonObject makeError(const QString& message) {
    QJsonObject e;
    e[QStringLiteral("error")] = message;
    return e;
}

}  // namespace

QJsonValue HeartbeatSelfConfigToolBase::resolveContext(const QJsonObject& args,
                                                       QString& outConfigId) {
    outConfigId.clear();
    if (!m_deps.isValid()) {
        return makeError(QStringLiteral("Heartbeat self-config is unavailable in this build."));
    }

    // Args-injected caller identity wins over the captured-LOCAL
    // CascadeController pointer.  Wire-side per-client cascades
    // resolve to THEIR own agent's heartbeat config; direct callers
    // / tests that bypass ToolService fall back to the cascade
    // pointer.
    QString agentId = args.value(QStringLiteral("__caller_agent_id")).toString();
    QString alias = args.value(QStringLiteral("__caller_agent_alias")).toString();
    // resolve the in-flight run's cascade per call (the
    // fallback when the dispatch path did not inject caller identity).
    Chat::CascadeController* cascade = m_deps.cascadeResolver ? m_deps.cascadeResolver() : nullptr;
    if (agentId.isEmpty() && cascade) {
        agentId = cascade->currentResponderAgentId();
    }
    if (alias.isEmpty() && cascade) {
        alias = cascade->currentResponderAlias();
    }
    if (agentId.isEmpty()) {
        return makeError(
            QStringLiteral("Heartbeat self-config tools can only be called from inside a "
                           "cascade turn; no responder identity is currently set."));
    }

    // Args-injected conv id wins over the captured-LOCAL getter.
    QString convId = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (convId.isEmpty() && m_deps.activeConvIdGetter) {
        convId = m_deps.activeConvIdGetter();
    }
    if (convId.isEmpty()) {
        return makeError(
            QStringLiteral("Heartbeat self-config tools require an active conversation."));
    }

    const auto convOpt = m_deps.convs->getConversation(convId);
    if (!convOpt) {
        return makeError(QStringLiteral("Active conversation no longer exists."));
    }

    const HeartbeatScopeType scope = convOpt->isGroup ? HeartbeatScopeType::ConversationGroup
                                                      : HeartbeatScopeType::Conversation1to1;
    // 1:1 stores empty alias by convention; group uses the membership alias.
    const QString aliasKey = scope == HeartbeatScopeType::ConversationGroup ? alias : QString();

    const HeartbeatConfig cfg = m_deps.configSvc->configFor(agentId, scope, convId, aliasKey);
    if (cfg.id.isEmpty()) {
        return makeError(
            QStringLiteral("No heartbeat config exists for this membership. Ask the user to "
                           "create one from the chat or folder settings before calling "
                           "self-config tools."));
    }

    outConfigId = cfg.id;
    return QJsonValue();  // empty == success
}

// ---------------------------------------------------------------------------
// set_heartbeat_goal
// ---------------------------------------------------------------------------

QString SetHeartbeatGoalTool::name() const {
    return QStringLiteral("set_heartbeat_goal");
}
QString SetHeartbeatGoalTool::description() const {
    return QStringLiteral("Update your own background-activity goal — the standing "
                          "instruction your subagent runs follow on every fire. Only "
                          "available when the user has granted self-configuration on "
                          "your heartbeat row. Cannot erase to empty.");
}
QList<ToolParameterSchema> SetHeartbeatGoalTool::parameters() const {
    ToolParameterSchema p;
    p.name = QStringLiteral("text");
    p.type = QStringLiteral("string");
    p.description =
        QStringLiteral("New goal text. Required, non-empty. Replaces the existing goal.");
    p.required = true;
    return {p};
}
QJsonValue SetHeartbeatGoalTool::invoke(const QJsonObject& args) {
    QString configId;
    const auto err = resolveContext(args, configId);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }
    const QString newGoal = args[QStringLiteral("text")].toString().trimmed();
    if (newGoal.isEmpty()) {
        return makeError(
            QStringLiteral("Goal must be non-empty. Use disable_heartbeat to stop runs."));
    }
    HeartbeatConfig cfg = m_deps.configSvc->configById(configId);
    if (!cfg.isValid()) {
        return makeError(QStringLiteral("Config disappeared mid-call."));
    }
    if (cfg.goal == newGoal) {
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("unchanged")},
                           {QStringLiteral("configId"), configId}};
    }

    const QString oldGoal = cfg.goal;
    cfg.goal = newGoal;
    if (m_deps.configSvc->upsertConfig(cfg).isEmpty()) {
        return makeError(QStringLiteral("Failed to persist new goal."));
    }
    m_deps.configSvc->recordChange(
        configId, QStringLiteral("goal"), oldGoal, newGoal, QStringLiteral("agent"));
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                       {QStringLiteral("configId"), configId},
                       {QStringLiteral("field"), QStringLiteral("goal")}};
}

// ---------------------------------------------------------------------------
// set_heartbeat_schedule
// ---------------------------------------------------------------------------

QString SetHeartbeatScheduleTool::name() const {
    return QStringLiteral("set_heartbeat_schedule");
}
QString SetHeartbeatScheduleTool::description() const {
    return QStringLiteral("Update your own background-activity schedule. Format: one of "
                          "@hourly | @daily at HH:MM | @weekly[ on DAY at HH:MM] | "
                          "@interval N (N >= 5 minutes). The schedule's effective "
                          "firing rate must not exceed the user-set max_runs_per_day.");
}
QList<ToolParameterSchema> SetHeartbeatScheduleTool::parameters() const {
    ToolParameterSchema p;
    p.name = QStringLiteral("schedule");
    p.type = QStringLiteral("string");
    p.description = QStringLiteral("Schedule string in one of the four named formats. Empty to "
                                   "switch to manual-fire-only.");
    p.required = true;
    return {p};
}
QJsonValue SetHeartbeatScheduleTool::invoke(const QJsonObject& args) {
    QString configId;
    const auto err = resolveContext(args, configId);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }
    const QString newSched = args[QStringLiteral("schedule")].toString().trimmed();

    // Parse-and-validate against the four named formats. Empty
    // schedules are valid (manual-fire-only).
    const HeartbeatSchedule parsed = parseHeartbeatSchedule(newSched);
    if (!parsed.valid) {
        return makeError(QStringLiteral("Invalid schedule: %1").arg(parsed.errorMessage));
    }

    HeartbeatConfig cfg = m_deps.configSvc->configById(configId);
    if (!cfg.isValid()) {
        return makeError(QStringLiteral("Config disappeared mid-call."));
    }

    // Enforce max_runs_per_day. firesIn24h gives the upper bound on
    // dispatches per 24h for this schedule; clamp self-config to it.
    if (cfg.maxRunsPerDay > 0 && firesIn24h(parsed) > cfg.maxRunsPerDay) {
        return makeError(QStringLiteral("Schedule would fire %1 times/day, which exceeds the "
                                        "user-set max_runs_per_day=%2. Choose a longer interval "
                                        "or ask the user to raise the cap.")
                             .arg(firesIn24h(parsed))
                             .arg(cfg.maxRunsPerDay));
    }

    if (cfg.schedule == newSched) {
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("unchanged")},
                           {QStringLiteral("configId"), configId}};
    }

    const QString oldSched = cfg.schedule;
    cfg.schedule = newSched;
    if (m_deps.configSvc->upsertConfig(cfg).isEmpty()) {
        return makeError(QStringLiteral("Failed to persist new schedule."));
    }
    m_deps.configSvc->recordChange(
        configId, QStringLiteral("schedule"), oldSched, newSched, QStringLiteral("agent"));
    if (m_deps.subagentSvc) {
        m_deps.subagentSvc->rebuildSchedule(configId);
    }
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                       {QStringLiteral("configId"), configId},
                       {QStringLiteral("field"), QStringLiteral("schedule")}};
}

// ---------------------------------------------------------------------------
// set_heartbeat_surface_criteria
// ---------------------------------------------------------------------------

QString SetHeartbeatSurfaceCriteriaTool::name() const {
    return QStringLiteral("set_heartbeat_surface_criteria");
}
QString SetHeartbeatSurfaceCriteriaTool::description() const {
    return QStringLiteral("Update the criterion the parent-review step (Tier 2) uses to "
                          "decide whether to surface a background report to the chat. "
                          "Empty falls back to your goal text.");
}
QList<ToolParameterSchema> SetHeartbeatSurfaceCriteriaTool::parameters() const {
    ToolParameterSchema p;
    p.name = QStringLiteral("text");
    p.type = QStringLiteral("string");
    p.description =
        QStringLiteral("New surface-criteria text. Empty allowed (falls back to goal).");
    p.required = true;
    return {p};
}
QJsonValue SetHeartbeatSurfaceCriteriaTool::invoke(const QJsonObject& args) {
    QString configId;
    const auto err = resolveContext(args, configId);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }
    const QString newCrit = args[QStringLiteral("text")].toString();
    HeartbeatConfig cfg = m_deps.configSvc->configById(configId);
    if (!cfg.isValid()) {
        return makeError(QStringLiteral("Config disappeared mid-call."));
    }
    if (cfg.surfaceCriteria == newCrit) {
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("unchanged")},
                           {QStringLiteral("configId"), configId}};
    }
    const QString oldCrit = cfg.surfaceCriteria;
    cfg.surfaceCriteria = newCrit;
    if (m_deps.configSvc->upsertConfig(cfg).isEmpty()) {
        return makeError(QStringLiteral("Failed to persist surface criteria."));
    }
    m_deps.configSvc->recordChange(
        configId, QStringLiteral("surface_criteria"), oldCrit, newCrit, QStringLiteral("agent"));
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                       {QStringLiteral("configId"), configId},
                       {QStringLiteral("field"), QStringLiteral("surface_criteria")}};
}

// ---------------------------------------------------------------------------
// enable_heartbeat
// ---------------------------------------------------------------------------

QString EnableHeartbeatTool::name() const {
    return QStringLiteral("enable_heartbeat");
}
QString EnableHeartbeatTool::description() const {
    return QStringLiteral("Turn your own heartbeat on. Requires the user to have granted "
                          "self-configuration on your heartbeat row. The global pause kill "
                          "switch overrides this — if the user has paused all heartbeats, "
                          "your enable will persist but no fires happen until pause clears.");
}
QList<ToolParameterSchema> EnableHeartbeatTool::parameters() const {
    return {};  // no args
}
QJsonValue EnableHeartbeatTool::invoke(const QJsonObject& args) {
    QString configId;
    const auto err = resolveContext(args, configId);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }
    HeartbeatConfig cfg = m_deps.configSvc->configById(configId);
    if (!cfg.isValid()) {
        return makeError(QStringLiteral("Config disappeared mid-call."));
    }
    if (!cfg.selfConfigAllowed) {
        return makeError(QStringLiteral("Self-configuration is not allowed on this heartbeat row. "
                                        "Ask the user to flip 'Allow agent to manage its own "
                                        "heartbeat config' in chat / folder settings."));
    }
    if (cfg.enabled) {
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("unchanged")},
                           {QStringLiteral("configId"), configId}};
    }

    const QString oldVal = QStringLiteral("0");
    cfg.enabled = true;
    if (m_deps.configSvc->upsertConfig(cfg).isEmpty()) {
        return makeError(QStringLiteral("Failed to enable heartbeat."));
    }
    m_deps.configSvc->recordChange(
        configId, QStringLiteral("enabled"), oldVal, QStringLiteral("1"), QStringLiteral("agent"));
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                       {QStringLiteral("configId"), configId},
                       {QStringLiteral("globally_paused"),
                        m_deps.subagentSvc && m_deps.subagentSvc->globallyPaused()}};
}

// ---------------------------------------------------------------------------
// disable_heartbeat
// ---------------------------------------------------------------------------

QString DisableHeartbeatTool::name() const {
    return QStringLiteral("disable_heartbeat");
}
QString DisableHeartbeatTool::description() const {
    return QStringLiteral("Turn your own heartbeat off. Always allowed — disabling does "
                          "not require self_config_allowed. Useful when the agent decides "
                          "the routine is no longer relevant.");
}
QList<ToolParameterSchema> DisableHeartbeatTool::parameters() const {
    return {};
}
QJsonValue DisableHeartbeatTool::invoke(const QJsonObject& args) {
    QString configId;
    const auto err = resolveContext(args, configId);
    if (err.isObject() && err.toObject().contains(QStringLiteral("error"))) {
        return err;
    }
    HeartbeatConfig cfg = m_deps.configSvc->configById(configId);
    if (!cfg.isValid()) {
        return makeError(QStringLiteral("Config disappeared mid-call."));
    }
    if (!cfg.enabled) {
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("unchanged")},
                           {QStringLiteral("configId"), configId}};
    }
    const QString oldVal = QStringLiteral("1");
    cfg.enabled = false;
    if (m_deps.configSvc->upsertConfig(cfg).isEmpty()) {
        return makeError(QStringLiteral("Failed to disable heartbeat."));
    }
    m_deps.configSvc->recordChange(
        configId, QStringLiteral("enabled"), oldVal, QStringLiteral("0"), QStringLiteral("agent"));
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                       {QStringLiteral("configId"), configId}};
}

}  // namespace Tools

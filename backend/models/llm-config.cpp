// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file llm-config.cpp
 * @brief JSON serialization for the LlmConfig data model.
 * @layer Data Access
 * @dependencies Qt6::Core
 */

#include "llm-config.h"

/**
 * @brief Serializes this LlmConfig to a JSON object.
 * @return QJsonObject with all configuration fields.
 */
QJsonObject LlmConfig::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("provider_id")] = providerId;
    obj[QStringLiteral("model_name")] = modelName;
    obj[QStringLiteral("temperature")] = temperature;
    obj[QStringLiteral("max_tokens")] = maxTokens;
    obj[QStringLiteral("context_window")] = contextWindow;
    obj[QStringLiteral("stream")] = stream;
    obj[QStringLiteral("thinking_mode")] = thinkingMode;
    obj[QStringLiteral("top_k")] = topK;
    obj[QStringLiteral("top_p")] = topP;
    obj[QStringLiteral("repeat_penalty")] = repeatPenalty;
    obj[QStringLiteral("presence_penalty")] = presencePenalty;
    obj[QStringLiteral("frequency_penalty")] = frequencyPenalty;
    obj[QStringLiteral("force_app_sampling")] = forceAppSampling;
    obj[QStringLiteral("allow_heartbeat_auto_surface")] = allowHeartbeatAutoSurface;
    obj[QStringLiteral("auto_surface_max_per_day")] = autoSurfaceMaxPerDay;
    obj[QStringLiteral("agent_pattern")] = agentPattern;
    obj[QStringLiteral("tools_enabled")] = toolsEnabled;
    obj[QStringLiteral("tools_in_system_prompt")] = toolsInSystemPrompt;
    obj[QStringLiteral("dynamic_compact_enabled")] = dynamicCompactEnabled;
    obj[QStringLiteral("implicit_task_completion")] = implicitTaskCompletion;
    obj[QStringLiteral("compact_every_turns")] = compactEveryTurns;
    obj[QStringLiteral("require_confirmation")] = requireConfirmation;
    obj[QStringLiteral("rag_enabled")] = ragEnabled;
    // default true (agent-memory recall on for everyone unless turned off).
    obj[QStringLiteral("aim_enabled")] = aimEnabled;
    // default true (team memory; only active inside an ACN-enabled project/org).
    obj[QStringLiteral("acn_enabled")] = acnEnabled;
    // Per-conversation autonomous-round cap (0 = unbounded).
    obj[QStringLiteral("max_auto_rounds")] = maxAutoRounds;
    return obj;
}

/*
 * @brief Deserializes a LlmConfig from a JSON object.
 * @param json JSON object with config fields.
 * @return Populated LlmConfig. Missing fields use struct defaults.
 */
LlmConfig LlmConfig::fromJson(const QJsonObject& json) {
    LlmConfig cfg;
    cfg.providerId = json[QStringLiteral("provider_id")].toString();
    cfg.modelName = json[QStringLiteral("model_name")].toString();

    // Use defaults for optional fields if absent
    if (json.contains(QStringLiteral("temperature"))) {
        cfg.temperature = json[QStringLiteral("temperature")].toDouble(-1);
    }
    if (json.contains(QStringLiteral("max_tokens"))) {
        // Conversion-failure fallback matches the struct default (-1 =
        // Auto). Rows persisted by older builds carry explicit numeric
        // values (e.g. 4096) and keep them via normal conversion.
        cfg.maxTokens = json[QStringLiteral("max_tokens")].toInt(-1);
    }
    if (json.contains(QStringLiteral("context_window"))) {
        cfg.contextWindow = json[QStringLiteral("context_window")].toInt(8192);
    }
    if (json.contains(QStringLiteral("stream"))) {
        cfg.stream = json[QStringLiteral("stream")].toBool(true);
    }
    if (json.contains(QStringLiteral("thinking_mode"))) {
        cfg.thinkingMode = json[QStringLiteral("thinking_mode")].toBool(false);
    }
    if (json.contains(QStringLiteral("top_k"))) {
        cfg.topK = json[QStringLiteral("top_k")].toDouble(-1);
    }
    if (json.contains(QStringLiteral("top_p"))) {
        cfg.topP = json[QStringLiteral("top_p")].toDouble(-1);
    }
    if (json.contains(QStringLiteral("repeat_penalty"))) {
        cfg.repeatPenalty = json[QStringLiteral("repeat_penalty")].toDouble(-1);
    }
    if (json.contains(QStringLiteral("presence_penalty"))) {
        cfg.presencePenalty = json[QStringLiteral("presence_penalty")].toDouble(-1);
    }
    if (json.contains(QStringLiteral("frequency_penalty"))) {
        cfg.frequencyPenalty = json[QStringLiteral("frequency_penalty")].toDouble(-1);
    }
    // Pre-existing rows lack `force_app_sampling`; default true so qwen3.5/3.6
    // conversations created before this field shipped get the auto-applied
    // workaround on next request build.  Users opt out via the conv-settings
    // toggle; that flip persists the explicit `false`.
    if (json.contains(QStringLiteral("force_app_sampling"))) {
        cfg.forceAppSampling = json[QStringLiteral("force_app_sampling")].toBool(true);
    }
    // Heartbeat gate fields. Absent on older
    // rows; the struct defaults (false / 1) keep behaviour identical to
    // pre-H2 builds when the keys are missing.
    if (json.contains(QStringLiteral("allow_heartbeat_auto_surface"))) {
        cfg.allowHeartbeatAutoSurface =
            json[QStringLiteral("allow_heartbeat_auto_surface")].toBool(false);
    }
    if (json.contains(QStringLiteral("auto_surface_max_per_day"))) {
        cfg.autoSurfaceMaxPerDay = json[QStringLiteral("auto_surface_max_per_day")].toInt(1);
    }
    // Per-conversation agent settings. Absent on older
    // rows; struct defaults (empty / true / false) keep behaviour
    // identical to earlier builds when the keys are missing:
    // empty agentPattern resolves to "direct" via the resolver,
    // toolsEnabled defaults to true, requireConfirmation defaults to
    // false. All three match the former global defaults so
    // existing rows behave the same after migration.
    if (json.contains(QStringLiteral("agent_pattern"))) {
        cfg.agentPattern = json[QStringLiteral("agent_pattern")].toString();
    }
    if (json.contains(QStringLiteral("tools_enabled"))) {
        cfg.toolsEnabled = json[QStringLiteral("tools_enabled")].toBool(true);
    }
    // Absent on older rows → field default (false: the prose list
    // duplicates the structured tools channel and starves history on
    // small context windows). Stored values are honoured — rows saved
    // while the old default was true keep prose until the user flips
    // the conversation's "Describe tools in system prompt" toggle.
    if (json.contains(QStringLiteral("tools_in_system_prompt"))) {
        cfg.toolsInSystemPrompt = json[QStringLiteral("tools_in_system_prompt")].toBool(false);
    }
    if (json.contains(QStringLiteral("dynamic_compact_enabled"))) {
        cfg.dynamicCompactEnabled = json[QStringLiteral("dynamic_compact_enabled")].toBool(true);
    }
    // Implicit task completion — absent on older rows; default FALSE (a task
    // closes only via explicit complete_task / stop_task unless turned on).
    if (json.contains(QStringLiteral("implicit_task_completion"))) {
        cfg.implicitTaskCompletion = json[QStringLiteral("implicit_task_completion")].toBool(false);
    }
    // Proactive compaction cadence — absent on older rows; default 20
    // assistant turns (0 disables the cadence path).
    if (json.contains(QStringLiteral("compact_every_turns"))) {
        cfg.compactEveryTurns = json[QStringLiteral("compact_every_turns")].toInt(20);
    }
    if (json.contains(QStringLiteral("require_confirmation"))) {
        cfg.requireConfirmation = json[QStringLiteral("require_confirmation")].toBool(false);
    }
    if (json.contains(QStringLiteral("rag_enabled"))) {
        cfg.ragEnabled = json[QStringLiteral("rag_enabled")].toBool(false);
    }
    // Absent on older rows → struct default true (memory recall on for everyone
    // unless explicitly turned off).
    if (json.contains(QStringLiteral("aim_enabled"))) {
        cfg.aimEnabled = json[QStringLiteral("aim_enabled")].toBool(true);
    }
    // Absent on older rows → struct default true.
    if (json.contains(QStringLiteral("acn_enabled"))) {
        cfg.acnEnabled = json[QStringLiteral("acn_enabled")].toBool(true);
    }
    // Absent on older rows → struct default 6. 0 = unbounded.
    if (json.contains(QStringLiteral("max_auto_rounds"))) {
        cfg.maxAutoRounds = json[QStringLiteral("max_auto_rounds")].toInt(6);
    }
    return cfg;
}

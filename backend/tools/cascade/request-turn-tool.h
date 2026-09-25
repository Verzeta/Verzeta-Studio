// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file request-turn-tool.h
 * @brief `request_turn` tool: a structured routing signal that one
 *        agent emits to hand the next group-chat turn to another
 *        teammate (or broadcast to "all").
 * @layer Service (Tool subsystem)
 * @dependencies backend/tools/itool.h.
 *
 * This tool's invocation body produces a descriptor object; the
 * actual cascade routing is performed by
 * Chat::ToolDispatcher's completion callback, which reads the
 * descriptor and emits CascadeController signals. The tool itself
 * is stateless and purely declarative.
 *
 * Threading: runsOnMainThread() returns false. The tool reads JSON
 * input and returns JSON output with no shared state, so it is
 * safe to invoke from any thread.
 */
#pragma once

#include "../itool.h"

namespace Tools {

/**
 * @brief ITool implementation for the `request_turn` built-in tool.
 */
class RequestTurnTool : public ITool {
  public:
    /**
     * @brief Constructs the stateless tool.
     */
    RequestTurnTool();

    /**
     * @brief Canonical tool name.
     * @returns "request_turn".
     */
    QString name() const override;

    /**
     * @brief Human-readable description shown to the LLM.
     * @returns One-line description of the turn-handoff signal.
     */
    QString description() const override;

    /**
     * @brief Parameter schema.
     * @returns Descriptors for `alias` (required) and `context` (optional).
     */
    QList<ToolParameterSchema> parameters() const override;

    /**
     * @brief Thread-residency declaration.
     * @returns false, because it is purely JSON-in / JSON-out with no shared state.
     */
    bool runsOnMainThread() const override;

    /**
     * @brief Conversation-capability scope.
     * @returns GroupOnly, because requesting a teammate's turn is meaningless
     *          in a single-agent chat (there is no other member).
     */
    ToolScope scope() const override { return ToolScope::GroupOnly; }

    /**
     * @brief Produce the routing descriptor.
     * @param args JSON object with:
     *               - "alias"  (required, string): teammate alias or
     *                          the special broadcast token "all".
     *               - "context" (optional, string): short reason;
     *                          accepted but not used by the
     *                          dispatcher.
     * @return On success, {"requested": \<alias\>, "status": "queued"}.
     *         On empty alias, {"error": "alias is required"}.
     * @complexity O(1).
     * @sideeffects None. The caller (Chat::ToolDispatcher) reads
     *              the returned "requested" field and emits a
     *              cascade routing signal, but that side effect is
     *              outside this tool.
     */
    QJsonValue invoke(const QJsonObject& args) override;
};

}  // namespace Tools

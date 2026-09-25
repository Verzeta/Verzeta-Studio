// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file itool.h
 * @brief Abstract interface implemented by every built-in tool under
 *        backend/tools/. A tool is a named, parameterised, invokable
 *        callback that the LLM can request through the tool-call
 *        protocol. Classes implementing ITool carry their own
 *        non-owning service references and expose both the declarative
 *        schema and the invocation body.
 * @layer Service (Tool subsystem)
 * @dependencies api/tool-calling-schema.h for ToolParameterSchema.
 *
 * Each ITool implementation is instantiated by the registration helper
 * in tool-registration.h. The helper queries name() / description() /
 * parameters() to build a ToolSchema, then wires the instance's
 * invoke() method into ToolService via registerTool(..., ToolKind::BuiltIn).
 *
 * Thread-residency contract:
 *   runsOnMainThread() returns true iff invoke() requires the Qt main
 *   thread (touches SQLite through a service, reads UI-owned state,
 *   or interacts with objects that live on the main thread). When
 *   false, the tool body must be safe to execute on a QtConcurrent
 *   worker thread and must not reach into any service whose
 *   documented threading rule demands the main thread.
 */
#pragma once

#include "../api/tool-calling-schema.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>

namespace Tools {

/**
 * @brief Interface for a single LLM-callable tool.
 *
 * Implementers hold their own non-owning dependencies (services,
 * collaborators) via constructor injection and must outlive the
 * ToolService they are registered on. The registration helper in
 * tool-registration.h owns every ITool instance for the lifetime of
 * the ToolService.
 */
class ITool {
  public:
    virtual ~ITool() = default;

    /**
     * @brief Stable identifier used by the LLM tool-call protocol.
     * @return Unique name; must match the OpenAI allowed-name grammar
     *         and be unique across the ToolService registry.
     */
    virtual QString name() const = 0;

    /**
     * @brief Human-readable description shown to the LLM in the tool
     *        catalog. Describes when to call the tool and what it does.
     * @return Description string; non-empty.
     */
    virtual QString description() const = 0;

    /**
     * @brief Declarative parameter list used to generate the OpenAI
     *        function schema and validate incoming arguments.
     * @return Ordered parameter descriptors. May be empty for tools
     *         that take no arguments.
     */
    virtual QList<ToolParameterSchema> parameters() const = 0;

    /**
     * @brief Thread-residency declaration.
     * @return true iff invoke() MUST run on the Qt main thread.
     *         false iff invoke() is safe on a worker thread.
     *
     * ToolDispatcher reads this per invocation to choose the
     * dispatch path: main-thread-only tools run inline on the caller
     * thread; worker-safe tools go through QtConcurrent::run.
     */
    virtual bool runsOnMainThread() const = 0;

    /**
     * @brief Conversation-capability scope declaration.
     * @return The conversation kinds this tool is offered in. Defaults to
     *         ToolScope::Universal (offered everywhere). Group-coordination
     *         tools (cascade / polls) return GroupOnly; project-membership
     *         tools return ProjectOnly. RequestBuilder filters the offered
     *         tool set by the conversation's actual capability; the filter
     *         only NARROWS, never grants.
     *
     * Non-pure so existing tools (and custom / MCP / task tools) keep the
     * Universal default with no change; only the handful of group/project
     * tools override it. Propagated into ToolSchema::scope by
     * Tools::registerITool, mirroring the runsOnMainThread() propagation.
     */
    virtual ToolScope scope() const { return ToolScope::Universal; }

    /**
     * @brief Execute the tool.
     * @param args Shape-checked JSON argument object; keys match
     *             parameters() names.
     * @return Result payload. Conventions:
     *           - On success, return a QJsonObject with named result
     *             fields (preferred) or a scalar JSON value.
     *           - On error, return a QJsonObject containing an
     *             "error" key with a human-readable message so the
     *             LLM observes a structured failure.
     * @complexity Implementation-defined; tools should document their
     *             own expected cost.
     * @sideeffects Implementation-defined; tools must document their
     *              observable side effects (DB writes, file I/O,
     *              network calls, signal emissions).
     */
    virtual QJsonValue invoke(const QJsonObject& args) = 0;
};

}  // namespace Tools

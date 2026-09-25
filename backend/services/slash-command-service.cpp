// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file slash-command-service.cpp
 * @brief Implementation of SlashCommandService, promoted out of
 *        `Chat::SlashCommandHandler` (under
 *        `backend/services/chat/`) so slash commands are a self-
 *        contained subsystem with their own ownership by AppController.
 *        Service refs (ConversationService / MessageService /
 *        FileService / ToolService / MessageListModel) attach via
 *        setters; production `tryHandle(QString)` builds the per-call
 *        SlashCommandContext from internal state and dispatches.
 *
 * @layer Service
 * @dependencies Qt6::Core, ConversationService (folder-chain lookup
 *               for `/artifacts` project-name detection),
 *               MessageService (ephemeral system-message posting for
 *               every command's reply), FileService (artifact-path
 *               resolution), ToolService (`/showtools` queries),
 *               MessageListModel (`/clear` model reload).
 */

#include "slash-command-service.h"

#include "../api/llm-interface.h"
#include "../models/conversation.h"
#include "../models/message-list-model.h"
#include "../models/message.h"
#include "../utils/http-client.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "conversation-service.h"
#include "conversation-summarizer.h"
#include "file-service.h"
#include "message-service.h"
#include "tool-service.h"

#include <algorithm>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace {

/**
 * @brief One row of the user-facing slash-command catalogue.
 *
 * `name` is the canonical command token (leading slash). `summary` is
 * the one-line description the picker shows. `usage` is the full
 * invocation form (equal to `name` for argument-less commands).
 */
struct CommandEntry {
    const char* name;
    const char* summary;
    const char* usage;
};

// Single source of truth for the user-typeable slash commands. Every
// entry here is dispatched by tryHandle() below, and tryHandle()
// recognises no user-facing command that is absent here. The
// argument-only filter forms of /showtools (builtin / custom / mcp /
// <server> / <text>) are documented in the /showtools and /help output
// rather than listed as separate picker rows — the picker completes the
// base command and the user types the filter. availableCommands()
// returns this table verbatim; keep the two in lock-step.
constexpr CommandEntry kCommandCatalogue[] = {
    {"/showtools", "Show a tool summary and how to filter the list", "/showtools"},
    {"/showmcptools", "List MCP server tools (alias for /showtools mcp)", "/showmcptools"},
    {"/artifacts", "Show this conversation's artifact directory path", "/artifacts"},
    {"/compact", "Summarise older messages to free context space", "/compact"},
    {"/flashmemory", "Permanently wipe this conversation's history", "/flashmemory confirm"},
    {"/clear", "Clear slash-command output bubbles from the view", "/clear"},
    {"/help", "Show the list of available commands", "/help"},
};

}  // namespace

SlashCommandService::SlashCommandService(QObject* parent) : QObject(parent) {}

SlashCommandService::~SlashCommandService() = default;

// ---------------------------------------------------------------------------
// Service attachments (AppController wires these during initialize())
// ---------------------------------------------------------------------------

void SlashCommandService::setConversationService(ConversationService* svc) {
    m_convSvc = svc;
}

void SlashCommandService::setMessageService(MessageService* svc) {
    m_msgSvc = svc;
}

void SlashCommandService::setFileService(FileService* svc) {
    m_fileSvc = svc;
}

void SlashCommandService::setToolService(ToolService* svc) {
    m_toolSvc = svc;
}

void SlashCommandService::setMessageListModel(MessageListModel* model) {
    m_msgModel = model;
}

void SlashCommandService::setActiveConversationIdGetter(std::function<QString()> getter) {
    m_activeConvIdGetter = std::move(getter);
}

// ---------------------------------------------------------------------------
// Production entry — builds context from internal state, dispatches.
// ---------------------------------------------------------------------------

bool SlashCommandService::tryHandle(const QString& text) {
    return tryHandle(buildContext(text));
}

// ---------------------------------------------------------------------------
// Build the per-call SlashCommandContext from setter state. Mirrors
// the closure-build that previously lived in
// ChatController::handleSlashCommand.
// ---------------------------------------------------------------------------

SlashCommandContext SlashCommandService::buildContext(const QString& text) const {
    SlashCommandContext ctx;
    ctx.text = text;
    ctx.activeConvId = m_activeConvIdGetter ? m_activeConvIdGetter() : QString();
    ctx.toolService = m_toolSvc;

    // /artifacts — resolve absolute path + project-name (if the active
    // conversation lives inside a project/organization folder).
    ConversationService* convSvc = m_convSvc;
    FileService* fileSvc = m_fileSvc;
    const QString convId = ctx.activeConvId;
    ctx.resolveArtifactsLocation = [convSvc, fileSvc, convId]() {
        SlashCommandContext::ArtifactsLocation loc;
        if (!convSvc || !fileSvc || convId.isEmpty())
            return loc;

        // Project-scoped vs per-conversation — same logic as the
        // pre-extraction ChatController::activeArtifactsPath body.
        const QList<Folder> chain = convSvc->folderChainForConversation(convId);
        for (const Folder& f : chain) {
            if (f.isProject()) {
                fileSvc->setActiveProjectContext(f.id, f.name);
                loc.path = fileSvc->activeProjectDir();
                loc.projectName = f.name;
                return loc;
            }
        }
        fileSvc->setActiveConversation(convId);
        loc.path = fileSvc->activeProjectDir();
        return loc;
    };

    // Ephemeral system-message posting (used by /help, /showtools,
    // /artifacts replies). Same Message shape the pre-extraction code
    // used: role=assistant, finishReason="command", modelUsed="system".
    MessageService* msgSvc = m_msgSvc;
    ctx.postMessage = [msgSvc, convId](const QString& content) {
        if (!msgSvc || convId.isEmpty())
            return;
        Message msg;
        msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        msg.conversationId = convId;
        msg.role = QStringLiteral("assistant");
        msg.content = content;
        msg.createdAt = QDateTime::currentDateTimeUtc();
        msg.finishReason = QStringLiteral("command");
        msg.modelUsed = QStringLiteral("system");
        msgSvc->postEphemeralMessage(msg);
    };

    // /clear — drop ephemeral slash-command output bubbles, then
    // reload. The ephemeral-clear path emits messageEphemeralCleared
    // which MessageListModel listens to and reloads itself; the
    // explicit setActiveConversation reload below is belt-and-braces
    // for any case where the model isn't subscribed (the legacy
    // unit-test path uses pure closures with no MessageService).
    // Without the ephemeral clear, slash-command bubbles would
    // re-appear after the reload because MessageListModel's reload
    // path explicitly re-includes them from
    // ephemeralMessagesForConversation().
    MessageListModel* msgModel = m_msgModel;
    MessageService* msgSvcRef = m_msgSvc;
    ctx.reloadActiveConversation = [msgModel, msgSvcRef, convId]() {
        if (msgSvcRef && !convId.isEmpty()) {
            msgSvcRef->clearEphemeralForConversation(convId);
        }
        if (!msgModel)
            return;
        msgModel->setActiveConversation(QString());
        msgModel->setActiveConversation(convId);
    };

    ctx.summarizer = m_summarizer;
    // /flashmemory wipe closure — bundles every destructive step so
    // the handler stays declarative. Summary + canvas rows + token
    // total reset ride along with the message wipe.
    ConversationService* convSvcW = m_convSvc;
    MessageService* msgSvcW = m_msgSvc;
    ConversationSummarizer* sumW = m_summarizer;
    ctx.wipeActiveConversation = [convSvcW, msgSvcW, sumW, convId]() -> int {
        if (!msgSvcW || convId.isEmpty())
            return -1;
        const int deleted = msgSvcW->deleteAllForConversation(convId);
        if (deleted < 0)
            return -1;
        if (sumW)
            sumW->clearFor(convId);
        if (convSvcW && !convSvcW->clearAuxiliaryContent(convId)) {
            qCWarning(verzetaUi) << "/flashmemory: auxiliary wipe partially failed for"
                                 << convId.left(8);
        }
        return deleted;
    };

    return ctx;
}

bool SlashCommandService::tryHandle(const SlashCommandContext& ctx) {
    // Only process lines starting with /
    if (!ctx.text.startsWith(QLatin1Char('/'))) {
        return false;
    }

    const QString cmd = ctx.text.section(QLatin1Char(' '), 0, 0).toLower();

    // ---------------------------------------------------------------
    // /showtools (+ /tools alias, + /showmcptools alias, + filters)
    // ---------------------------------------------------------------
    if (cmd == QStringLiteral("/showtools") || cmd == QStringLiteral("/tools") ||
        cmd == QStringLiteral("/showmcptools")) {
        QString filterArg;
        if (cmd == QStringLiteral("/showmcptools")) {
            filterArg = QStringLiteral("mcp");
        } else {
            filterArg = ctx.text.section(QLatin1Char(' '), 1).trimmed().toLower();
        }

        QString output;

        if (!ctx.toolService) {
            output = QStringLiteral("## Available Tools\n\n*No tool service configured.*\n");
        } else {
            const QVariantList allInfo = ctx.toolService->registeredToolsList();

            // Categorize
            QStringList builtIn, custom, mcp;
            QMap<QString, QVariantMap> infoMap;
            QMap<QString, QStringList> mcpByServer;
            for (const QVariant& v : allInfo) {
                const QVariantMap m = v.toMap();
                const QString name = m[QStringLiteral("name")].toString();
                infoMap[name] = m;

                if (name.contains(QLatin1Char(':'))) {
                    mcp.append(name);
                    const QString server = name.left(name.indexOf(QLatin1Char(':')));
                    mcpByServer[server].append(name);
                } else if (m[QStringLiteral("isBuiltIn")].toBool()) {
                    builtIn.append(name);
                } else {
                    custom.append(name);
                }
            }
            const int enabledCount = static_cast<int>(
                std::count_if(allInfo.cbegin(), allInfo.cend(), [](const QVariant& v) {
                    return v.toMap()[QStringLiteral("enabled")].toBool();
                }));

            auto renderTool = [&infoMap](QString& out, const QString& name, bool withParams) {
                const QVariantMap& info = infoMap[name];
                const bool enabled = info[QStringLiteral("enabled")].toBool();
                out += QStringLiteral("- **%1** %2: %3\n")
                           .arg(name,
                                enabled ? QStringLiteral("✓") : QStringLiteral("✗ (disabled)"),
                                info[QStringLiteral("description")].toString());
                if (!withParams)
                    return;
                const QVariantList params = info[QStringLiteral("parameters")].toList();
                for (const QVariant& pv : params) {
                    const QVariantMap p = pv.toMap();
                    out += QStringLiteral("  - `%1` (%2%3): %4\n")
                               .arg(p[QStringLiteral("name")].toString(),
                                    p[QStringLiteral("type")].toString(),
                                    p[QStringLiteral("required")].toBool()
                                        ? QStringLiteral(", required")
                                        : QString(),
                                    p[QStringLiteral("description")].toString());
                }
            };

            // -- No filter: SHORT summary + tips ---------------------------
            if (filterArg.isEmpty()) {
                output += QStringLiteral("## Tools Summary\n\n");
                output += QStringLiteral("- **Built-in:** %1 (%2 enabled)\n"
                                         "- **Custom:** %3\n"
                                         "- **MCP:** %4 tool(s) across %5 server(s)\n"
                                         "- **Total:** %6 (%7 enabled)\n\n")
                              .arg(builtIn.size())
                              .arg(static_cast<int>(std::count_if(
                                  builtIn.cbegin(),
                                  builtIn.cend(),
                                  [&infoMap](const QString& n) {
                                      return infoMap[n][QStringLiteral("enabled")].toBool();
                                  })))
                              .arg(custom.size())
                              .arg(mcp.size())
                              .arg(mcpByServer.size())
                              .arg(allInfo.size())
                              .arg(enabledCount);
                output += QStringLiteral("### How to list tools\n\n"
                                         "- `/showtools builtin`: list built-in tools\n"
                                         "- `/showtools custom`: list custom tools\n"
                                         "- `/showtools mcp`: list MCP tools grouped by server\n"
                                         "- `/showmcptools`: same as `/showtools mcp`\n");
                if (!mcpByServer.isEmpty()) {
                    output += QStringLiteral("- `/showtools <server>`: list the tools from one MCP "
                                             "server, for example:\n");
                    for (auto it = mcpByServer.cbegin(); it != mcpByServer.cend(); ++it) {
                        output += QStringLiteral("  - `/showtools %1`\n").arg(it.key());
                    }
                }
                output += QStringLiteral(
                    "- `/showtools <text>`: list tools whose names contain `<text>`\n");
            }
            // -- Filter: builtin --------------------------------------------
            else if (filterArg == QStringLiteral("builtin") ||
                     filterArg == QStringLiteral("built-in")) {
                output += QStringLiteral("## Built-in Tools (%1)\n\n").arg(builtIn.size());
                if (builtIn.isEmpty())
                    output += QStringLiteral("*None.*\n");
                for (const QString& name : builtIn)
                    renderTool(output, name, true);
            }
            // -- Filter: custom ---------------------------------------------
            else if (filterArg == QStringLiteral("custom")) {
                output += QStringLiteral("## Custom Tools (%1)\n\n").arg(custom.size());
                if (custom.isEmpty())
                    output += QStringLiteral("*None.*\n");
                for (const QString& name : custom)
                    renderTool(output, name, false);
            }
            // -- Filter: mcp (all servers) ----------------------------------
            else if (filterArg == QStringLiteral("mcp")) {
                output += QStringLiteral("## MCP Tools (%1 from %2 server(s))\n\n")
                              .arg(mcp.size())
                              .arg(mcpByServer.size());
                if (mcpByServer.isEmpty()) {
                    output += QStringLiteral("*No MCP servers connected.* Configure servers on the "
                                             "Tools page.\n");
                }
                for (auto it = mcpByServer.cbegin(); it != mcpByServer.cend(); ++it) {
                    output += QStringLiteral("### %1 (%2 tool(s))\n\n")
                                  .arg(it.key())
                                  .arg(it.value().size());
                    for (const QString& name : it.value()) {
                        const QVariantMap& info = infoMap[name];
                        const bool enabled = info[QStringLiteral("enabled")].toBool();
                        const QString toolName = name.mid(name.indexOf(QLatin1Char(':')) + 1);
                        output += QStringLiteral("- `%1` %2: %3\n")
                                      .arg(toolName,
                                           enabled ? QStringLiteral("✓") : QStringLiteral("✗"),
                                           info[QStringLiteral("description")].toString());
                    }
                    output += QLatin1Char('\n');
                }
            }
            // -- Filter: specific MCP server name ---------------------------
            else if (mcpByServer.contains(filterArg)) {
                const QStringList& names = mcpByServer[filterArg];
                output += QStringLiteral("## MCP Server: %1 (%2 tool(s))\n\n")
                              .arg(filterArg)
                              .arg(names.size());
                for (const QString& name : names) {
                    const QVariantMap& info = infoMap[name];
                    const bool enabled = info[QStringLiteral("enabled")].toBool();
                    const QString toolName = name.mid(name.indexOf(QLatin1Char(':')) + 1);
                    output += QStringLiteral("- `%1` %2: %3\n")
                                  .arg(toolName,
                                       enabled ? QStringLiteral("✓") : QStringLiteral("✗"),
                                       info[QStringLiteral("description")].toString());
                }
            }
            // -- Fallback: fuzzy substring match ---------------------------
            else {
                QStringList matches;
                for (const QVariant& v : allInfo) {
                    const QString name = v.toMap()[QStringLiteral("name")].toString();
                    if (name.contains(filterArg, Qt::CaseInsensitive)) {
                        matches.append(name);
                    }
                }
                output += QStringLiteral("## Tools matching \"%1\" (%2)\n\n")
                              .arg(filterArg)
                              .arg(matches.size());
                if (matches.isEmpty()) {
                    output += QStringLiteral("*No tools found.* Try `/showtools` "
                                             "with no argument for filter options.\n");
                }
                for (const QString& name : matches)
                    renderTool(output, name, false);
            }
        }

        // Display as a system message in the chat
        if (ctx.postMessage)
            ctx.postMessage(output);
        return true;
    }

    // ---------------------------------------------------------------
    // /help
    // ---------------------------------------------------------------
    if (cmd == QStringLiteral("/help")) {
        // Note: angle-bracket placeholders (`<server>` / `<text>`) used
        // to live here. The Markdown → HTML pass treated them as
        // unknown HTML tags and ate everything after the first `<`,
        // producing empty bullets in the rendered help. Square-bracket
        // placeholders render as literal `[NAME]` and are unambiguous.
        const QString help = QStringLiteral(
            "## Available Commands\n\n"
            "- **/showtools**: Show a tool summary and how to filter the list\n"
            "- **/showtools builtin**: List built-in tools\n"
            "- **/showtools custom**: List your custom tools\n"
            "- **/showtools mcp**: List MCP server tools (also `/showmcptools`)\n"
            "- **/showtools [SERVER]**: List the tools from one MCP server "
            "(use the server's name)\n"
            "- **/showtools [TEXT]**: List tools whose names contain the text\n"
            "- **/artifacts**: Show this conversation's artifact directory path\n"
            "- **/compact**: Summarise older messages to free context space\n"
            "- **/flashmemory**: Permanently wipe this conversation's history "
            "(asks you to confirm first)\n"
            "- **/help**: Show this help message\n"
            "- **/clear**: Clear slash-command output from the view "
            "(your saved chat history is kept)\n"
            "\n"
            "## Group Chat Mentions\n\n"
            "- **@AliasName**: Send this message to one member\n"
            "- **@everyone** / **@all**: Send to every member in the group "
            "(the coordinator answers first, then the other members in turn)\n"
            "- Agents can also @mention each other to delegate work automatically\n"
            "- Agents can @mention the user with **@owner**, **@user**, **@you**, "
            "or **@leader** to pause the team's turns and send you a desktop notification\n");

        if (ctx.postMessage)
            ctx.postMessage(help);
        return true;
    }

    if (cmd == QStringLiteral("/compact")) {
        if (!ctx.summarizer) {
            if (ctx.postMessage) {
                ctx.postMessage(QStringLiteral("*/compact is unavailable because the compaction "
                                               "service is not attached.*"));
            }
            return true;
        }
        if (ctx.activeConvId.isEmpty()) {
            if (ctx.postMessage) {
                ctx.postMessage(QStringLiteral("*No active conversation to compact.*"));
            }
            return true;
        }
        ctx.summarizer->invalidate(ctx.activeConvId, QStringLiteral("user_regenerated"));
        ctx.summarizer->generateAsync(ctx.activeConvId, QStringLiteral("manual"));
        if (ctx.postMessage) {
            ctx.postMessage(QStringLiteral("**Compaction started.** A `~Dynamic Compact "
                                           "Performed, Reason: Manual~` entry will appear when "
                                           "the summary is ready. (Very short conversations have "
                                           "nothing to compact and finish silently.)"));
        }
        return true;
    }

    if (cmd == QStringLiteral("/flashmemory")) {
        const QString arg = ctx.text.section(QLatin1Char(' '), 1, 1).toLower();
        if (arg != QStringLiteral("confirm")) {
            if (ctx.postMessage) {
                ctx.postMessage(
                    QStringLiteral("⚠️ **This will permanently delete every message, "
                                   "the compaction summary, and canvas state for this "
                                   "conversation.** The conversation itself (title, "
                                   "members, settings) is preserved.\n\nType "
                                   "`/flashmemory confirm` to proceed."));
            }
            return true;
        }
        if (!ctx.wipeActiveConversation) {
            if (ctx.postMessage) {
                ctx.postMessage(QStringLiteral("*/flashmemory is unavailable because the required "
                                               "services are not attached.*"));
            }
            return true;
        }
        const int deleted = ctx.wipeActiveConversation();
        if (deleted < 0) {
            if (ctx.postMessage) {
                ctx.postMessage(
                    QStringLiteral("**/flashmemory failed.** See the logs for details."));
            }
            return true;
        }
        if (ctx.postMessage) {
            ctx.postMessage(QStringLiteral("~Memory flashed by user; conversation history reset~ "
                                           "(messages removed: %1)")
                                .arg(deleted));
        }
        if (ctx.reloadActiveConversation)
            ctx.reloadActiveConversation();
        return true;
    }

    // ---------------------------------------------------------------
    // /clear — reload active conversation (clears + refetches from DB)
    // ---------------------------------------------------------------
    if (cmd == QStringLiteral("/clear")) {
        if (ctx.reloadActiveConversation)
            ctx.reloadActiveConversation();
        return true;
    }

    // ---------------------------------------------------------------
    // /artifacts
    // ---------------------------------------------------------------
    if (cmd == QStringLiteral("/artifacts")) {
        SlashCommandContext::ArtifactsLocation loc;
        if (ctx.resolveArtifactsLocation) {
            loc = ctx.resolveArtifactsLocation();
        }
        QString output;
        output += QStringLiteral("## Artifacts Directory\n\n");
        if (loc.path.isEmpty()) {
            output += QStringLiteral("*No active conversation.*\n");
        } else {
            if (!loc.projectName.isEmpty()) {
                output += QStringLiteral("This conversation is inside the project **%1**, so files "
                                         "generated by tool calls (e.g. `write_file`) are saved to "
                                         "a shared project directory:\n\n")
                              .arg(loc.projectName);
            } else {
                output +=
                    QStringLiteral("This conversation is not inside a project. Files generated "
                                   "by tool calls are saved to a per-conversation directory:\n\n");
            }
            output += QStringLiteral("```\n%1\n```\n\n").arg(loc.path);
            output += QStringLiteral("Click the **Artifacts** button in the top bar "
                                     "to browse, or the **Open** button in the "
                                     "Artifacts dialog to open this folder in your "
                                     "file manager.\n");
        }
        if (ctx.postMessage)
            ctx.postMessage(output);
        return true;
    }

    // Unknown slash command — let it pass through to the LLM
    return false;
}

// ---------------------------------------------------------------------------
// Catalogue accessor — feeds the composer's autocomplete picker. Reads
// the same kCommandCatalogue table tryHandle() dispatches from.
// ---------------------------------------------------------------------------

QVariantList SlashCommandService::availableCommands() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    out.reserve(static_cast<int>(std::size(kCommandCatalogue)));
    for (const CommandEntry& e : kCommandCatalogue) {
        QVariantMap m;
        m[QStringLiteral("name")] = QString::fromLatin1(e.name);
        m[QStringLiteral("summary")] = QString::fromLatin1(e.summary);
        m[QStringLiteral("usage")] = QString::fromLatin1(e.usage);
        out.append(m);
    }
    return out;
}

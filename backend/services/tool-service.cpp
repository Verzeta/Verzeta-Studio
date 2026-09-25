// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-service.cpp
 * @brief Implementation of ToolService: tool registry, dispatch, and built-in tools.
 * @layer Service
 * @dependencies ProcessSandbox (Utility), FileService (Service), Qt6::Core
 */


#include "tool-service.h"

// QtGlobal must come before any `#ifdef Q_OS_*` so the platform
// macros are defined. Other Qt includes pull this in transitively
// today; explicit include is the documented safe pattern after the
// windows-theme-bridge incident where the absent-Qt-header path
// silently zeroed out a Q_OS_WIN gate.
#include "services/file-service.h"
#include "services/search/web-search-service.h"
#include "tools/cascade/request-turn-tool.h"
#include "tools/file/list-files-tool.h"
#include "tools/file/read-file-tool.h"
#include "tools/file/write-file-tool.h"
#include "tools/itool-adapter.h"
#include "tools/shell/run-shell-tool.h"
#include "tools/time/current-time-tool.h"
#include "tools/web/search-web-tool.h"
#include "utils/logger.h"
#include "utils/process-sandbox.h"

#include <QtGlobal>
#include <QTimer>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

/**
 * @brief Constructs an empty ToolService.
 * @param parent Optional Qt parent.
 */
ToolService::ToolService(QObject* parent) : QObject(parent) {}

ToolService::~ToolService() = default;

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

/*
 * @brief Registers a tool.
 * @param schema  Tool definition.
 * @param handler Callable executed on invocation.
 */
void ToolService::registerTool(const ToolSchema& schema, ToolHandler handler) {
    registerTool(schema, std::move(handler), ToolKind::Custom);
}

void ToolService::registerTool(const ToolSchema& schema, ToolHandler handler, ToolKind kind) {
    const bool existed = m_tools.contains(schema.name);
    RegisteredTool row;
    row.schema = schema;
    row.handler = std::move(handler);
    row.kind = kind;
    row.enabled = true;
    m_tools[schema.name] = std::move(row);

    if (kind == ToolKind::BuiltIn) {
        m_builtInNames.insert(schema.name);
    }

    // Per-tool log only at debug level during batches (MCP servers can
    // register 50+ tools at once — that used to produce 50+ info-level
    // log lines plus 50+ tools-changed signals and freeze the UI).
    if (m_batchDepth > 0) {
        qCDebug(verzetaTools) << "ToolService: registered tool" << schema.name;
    } else {
        qCInfo(verzetaTools) << "ToolService: registered tool" << schema.name;
    }
    if (!existed) {
        if (m_batchDepth > 0) {
            m_batchDirty = true;
        } else {
            emit toolsChanged();
        }
    }
}

void ToolService::unregisterTool(const QString& name) {
    if (m_tools.remove(name)) {
        if (m_batchDepth > 0) {
            m_batchDirty = true;
        } else {
            emit toolsChanged();
        }
    }
}

bool ToolService::replaceHandler(const QString& name, ToolHandler handler) {
    auto it = m_tools.find(name);
    if (it == m_tools.end())
        return false;
    it->handler = std::move(handler);
    return true;
}

void ToolService::beginBatch() {
    ++m_batchDepth;
}

void ToolService::endBatch() {
    if (m_batchDepth <= 0) {
        qCWarning(verzetaTools) << "ToolService::endBatch called without matching beginBatch";
        return;
    }
    --m_batchDepth;
    if (m_batchDepth == 0 && m_batchDirty) {
        m_batchDirty = false;
        emit toolsChanged();
    }
}

QList<ToolSchema> ToolService::availableTools() const {
    QList<ToolSchema> result;
    result.reserve(m_tools.size());
    for (const RegisteredTool& t : m_tools) {
        if (t.enabled)
            result.append(t.schema);
    }
    return result;
}

bool ToolService::hasTool(const QString& name) const {
    return m_tools.contains(name);
}


bool ToolService::runsOnMainThread(const QString& name) const {
    const auto it = m_tools.constFind(name);
    if (it == m_tools.constEnd())
        return false;
    return it->runsOnMainThread;
}

void ToolService::setRunsOnMainThread(const QString& name, bool runsOnMainThread) {
    auto it = m_tools.find(name);
    if (it == m_tools.end())
        return;
    it->runsOnMainThread = runsOnMainThread;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

QJsonValue ToolService::invokeTool(const QString& name,
                                   const QJsonObject& args,
                                   const QString& callerConvId,
                                   const QString& callerAgentId,
                                   const QString& callerAgentAlias,
                                   const QString& callerFolderId,
                                   const QString& callerClientId) {
    auto it = m_tools.find(name);
    if (it == m_tools.end()) {
        const QString msg = QStringLiteral("Tool not registered: %1").arg(name);
        qCWarning(verzetaTools) << "ToolService:" << msg;
        emit toolError(name, msg);
        return QJsonObject{{QStringLiteral("error"), msg}};
    }

    if (!it->enabled) {
        const QString msg = QStringLiteral("Tool is disabled: %1").arg(name);
        qCWarning(verzetaTools) << "ToolService:" << msg;
        emit toolError(name, msg);
        return QJsonObject{{QStringLiteral("error"), msg}};
    }

    QJsonObject mergedArgs = args;
    if (!callerConvId.isEmpty()) {
        mergedArgs[QStringLiteral("__caller_conv_id")] = callerConvId;
    }
    if (!callerAgentId.isEmpty()) {
        mergedArgs[QStringLiteral("__caller_agent_id")] = callerAgentId;
    }
    if (!callerAgentAlias.isEmpty()) {
        mergedArgs[QStringLiteral("__caller_agent_alias")] = callerAgentAlias;
    }
    if (!callerFolderId.isEmpty()) {
        mergedArgs[QStringLiteral("__caller_folder_id")] = callerFolderId;
    }
    if (!callerClientId.isEmpty()) {
        mergedArgs[QStringLiteral("__caller_client_id")] = callerClientId;
    }

    emit toolInvoked(name, mergedArgs);
    qCDebug(verzetaTools) << "ToolService: invoking" << name << "callerConv=" << callerConvId
                          << "callerAgent=" << callerAgentAlias;

    QJsonValue result = it->handler(mergedArgs);
    emit toolResult(name, result);
    return result;
}

// ---------------------------------------------------------------------------
// QML-accessible tool management
// ---------------------------------------------------------------------------

QVariantList ToolService::registeredToolsList() const {
    QVariantList list;
    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
        QVariantMap entry;
        entry[QStringLiteral("name")] = it->schema.name;
        entry[QStringLiteral("description")] = it->schema.description;
        entry[QStringLiteral("enabled")] = it->enabled;
        entry[QStringLiteral("isBuiltIn")] = (it->kind == ToolKind::BuiltIn);
        QString kindStr;
        switch (it->kind) {
            case ToolKind::BuiltIn:
                kindStr = QStringLiteral("builtin");
                break;
            case ToolKind::Custom:
                kindStr = QStringLiteral("custom");
                break;
            case ToolKind::Mcp:
                kindStr = QStringLiteral("mcp");
                break;
        }
        entry[QStringLiteral("kind")] = kindStr;

        // For MCP tools, also split out the server name + short
        // tool name so QML doesn't have to parse "server:toolname".
        if (it->kind == ToolKind::Mcp) {
            const int sep = it->schema.name.indexOf(QLatin1Char(':'));
            if (sep > 0) {
                entry[QStringLiteral("mcpServer")] = it->schema.name.left(sep);
                entry[QStringLiteral("shortName")] = it->schema.name.mid(sep + 1);
            }
        }

        QVariantList params;
        for (const ToolParameterSchema& p : it->schema.parameters) {
            QVariantMap pm;
            pm[QStringLiteral("name")] = p.name;
            pm[QStringLiteral("type")] = p.type;
            pm[QStringLiteral("description")] = p.description;
            pm[QStringLiteral("required")] = p.required;
            params.append(pm);
        }
        entry[QStringLiteral("parameters")] = params;
        list.append(entry);
    }
    return list;
}

void ToolService::setToolEnabled(const QString& name, bool enabled) {
    auto it = m_tools.find(name);
    if (it != m_tools.end() && it->enabled != enabled) {
        it->enabled = enabled;
        emit toolsChanged();
        saveCustomTools();  // persist enabled state
    }
}

bool ToolService::isToolEnabled(const QString& name) const {
    auto it = m_tools.find(name);
    return it != m_tools.end() && it->enabled;
}

bool ToolService::addCustomTool(const QVariantMap& toolDef) {
    const QString name = toolDef[QStringLiteral("name")].toString().trimmed();
    if (name.isEmpty())
        return false;

    // Prevent overwriting built-in tools
    if (m_builtInNames.contains(name)) {
        qCWarning(verzetaTools) << "Cannot overwrite built-in tool:" << name;
        return false;
    }

    const QString description = toolDef[QStringLiteral("description")].toString();
    const QString commandTemplate = toolDef[QStringLiteral("commandTemplate")].toString();

    ToolSchema schema;
    schema.name = name;
    schema.description = description;

    // Parse parameters
    const QVariantList paramsList = toolDef[QStringLiteral("parameters")].toList();
    for (const QVariant& pv : paramsList) {
        const QVariantMap pm = pv.toMap();
        ToolParameterSchema p;
        p.name = pm[QStringLiteral("name")].toString();
        const QString pType = pm[QStringLiteral("type")].toString();
        p.type = pType.isEmpty() ? QStringLiteral("string") : pType;
        p.description = pm[QStringLiteral("description")].toString();
        p.required = pm[QStringLiteral("required")].toBool();
        if (!p.name.isEmpty())
            schema.parameters.append(p);
    }

    // If a command template is provided, create a handler that runs it
    // through ProcessSandbox with argument substitution.
    // Otherwise create a no-op handler that returns the args.
    ToolHandler handler;
    if (!commandTemplate.isEmpty()) {
        // Capture by value for the lambda
        handler = [commandTemplate](const QJsonObject& args) -> QJsonValue {
            // Substitute {{paramName}} placeholders with argument values
            QString cmd = commandTemplate;
            for (auto it = args.begin(); it != args.end(); ++it) {
                const QString placeholder = QStringLiteral("{{%1}}").arg(it.key());
                cmd.replace(placeholder, it.value().toVariant().toString());
            }

            QProcess proc;
            proc.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_ANDROID
            QJsonObject result;
            result[QStringLiteral("error")] =
                QStringLiteral("Custom shell-template tools are not available on "
                               "Android (no /bin/sh, no fork/exec on app sandbox).");
            return result;
#elif defined(Q_OS_WIN)
            proc.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), cmd});
#else
            proc.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
#endif
            proc.waitForFinished(30000);

            QJsonObject result;
            result[QStringLiteral("stdout")] = QString::fromUtf8(proc.readAllStandardOutput());
            result[QStringLiteral("stderr")] = QString::fromUtf8(proc.readAllStandardError());
            result[QStringLiteral("exitCode")] = proc.exitCode();
            result[QStringLiteral("timedOut")] = (proc.state() != QProcess::NotRunning);
            return result;
        };
    } else {
        handler = [](const QJsonObject& args) -> QJsonValue {
            QJsonObject result;
            result[QStringLiteral("info")] =
                QStringLiteral("Custom tool executed (no command template)");
            result[QStringLiteral("args_received")] =
                QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
            return result;
        };
    }

    RegisteredTool row;
    row.schema = schema;
    row.handler = std::move(handler);
    row.kind = ToolKind::Custom;
    row.enabled = true;
    m_tools[name] = std::move(row);
    qCInfo(verzetaTools) << "ToolService: added custom tool" << name;
    emit toolsChanged();
    saveCustomTools();
    return true;
}

bool ToolService::removeCustomTool(const QString& name) {
    if (m_builtInNames.contains(name)) {
        qCWarning(verzetaTools) << "Cannot remove built-in tool:" << name;
        return false;
    }
    if (m_tools.remove(name)) {
        qCInfo(verzetaTools) << "ToolService: removed custom tool" << name;
        emit toolsChanged();
        saveCustomTools();
        return true;
    }
    return false;
}

QString ToolService::customToolsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/custom-tools.json");
}

void ToolService::saveCustomTools() {
    QJsonObject root;

    // Save enabled state for ALL tools (built-in + custom)
    QJsonObject enabledState;
    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
        enabledState[it.key()] = it->enabled;
    }
    root[QStringLiteral("enabled")] = enabledState;

    // Save custom tool definitions
    QJsonArray customs;
    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
        if (it->kind == ToolKind::BuiltIn)
            continue;

        QJsonObject def;
        def[QStringLiteral("name")] = it->schema.name;
        def[QStringLiteral("description")] = it->schema.description;

        QJsonArray params;
        for (const ToolParameterSchema& p : it->schema.parameters) {
            QJsonObject pm;
            pm[QStringLiteral("name")] = p.name;
            pm[QStringLiteral("type")] = p.type;
            pm[QStringLiteral("description")] = p.description;
            pm[QStringLiteral("required")] = p.required;
            params.append(pm);
        }
        def[QStringLiteral("parameters")] = params;
        // Note: commandTemplate is embedded in the lambda closure,
        // we can't extract it. Store it in schema.description or metadata.
        // For now, save it if it was stored.
        customs.append(def);
    }
    root[QStringLiteral("customTools")] = customs;

    const QString path = customToolsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
        qCInfo(verzetaTools) << "ToolService: saved custom tools to" << path;
    }
}

void ToolService::loadCustomTools(ProcessSandbox& /*sandbox*/) {
    const QString path = customToolsPath();
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return;

    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    // Restore enabled state for built-in tools
    const QJsonObject enabledState = root[QStringLiteral("enabled")].toObject();
    for (auto it = enabledState.begin(); it != enabledState.end(); ++it) {
        auto toolIt = m_tools.find(it.key());
        if (toolIt != m_tools.end()) {
            toolIt->enabled = it.value().toBool(true);
        }
    }

    // Load custom tool definitions
    const QJsonArray customs = root[QStringLiteral("customTools")].toArray();
    for (const QJsonValue& v : customs) {
        const QJsonObject def = v.toObject();
        QVariantMap toolDef;
        toolDef[QStringLiteral("name")] = def[QStringLiteral("name")].toString();
        toolDef[QStringLiteral("description")] = def[QStringLiteral("description")].toString();

        QVariantList params;
        const QJsonArray paramsArr = def[QStringLiteral("parameters")].toArray();
        for (const QJsonValue& pv : paramsArr) {
            params.append(pv.toObject().toVariantMap());
        }
        toolDef[QStringLiteral("parameters")] = params;
        addCustomTool(toolDef);
    }

    qCInfo(verzetaTools) << "ToolService: loaded custom tools from" << path;
    emit toolsChanged();
}

// ---------------------------------------------------------------------------
// Built-in tools
// ---------------------------------------------------------------------------

void ToolService::setWebSearchService(Search::WebSearchService* svc) {
    m_webSearchService = svc;
}

/*
 * @brief Registers the full set of built-in tools.
 * @param sandbox     ProcessSandbox instance.
 * @param fileService FileService instance.
 */
void ToolService::registerBuiltInTools(ProcessSandbox& sandbox,
                                       FileService& fileService,
                                       BackgroundProcessService* bgService) {
    // run_shell — delegated to Tools::RunShellTool (backend/tools/shell/).
    // The sandbox reference threads through unchanged; allow-list and
    // timeout enforcement live inside ProcessSandbox. The optional background
    // registry backs run_shell(background:true) for long-running processes.
    Tools::registerITool(*this,
                         std::make_unique<Tools::RunShellTool>(sandbox, fileService, bgService),
                         ToolKind::BuiltIn);

    // list_files / read_file / write_file — all three capture the
    // FileService reference so relative paths anchor against the
    // active project directory consistently.
    Tools::registerITool(
        *this, std::make_unique<Tools::ListFilesTool>(fileService), ToolKind::BuiltIn);
    Tools::registerITool(
        *this, std::make_unique<Tools::ReadFileTool>(fileService), ToolKind::BuiltIn);
    Tools::registerITool(
        *this, std::make_unique<Tools::WriteFileTool>(fileService), ToolKind::BuiltIn);

    // get_current_time — delegated to Tools::CurrentTimeTool.
    Tools::registerITool(*this, std::make_unique<Tools::CurrentTimeTool>(), ToolKind::BuiltIn);

    // search_web — delegated to Tools::SearchWebTool (backend/tools/web/).
    // The tool binds to the WebSearchService, which selects the active
    // backend (DDG scrape / Instant Answer / configured API provider) and
    // guarantees a graceful result. AppController injects the configured
    // service via setWebSearchService(); when none is injected (tests),
    // bind to an internally-owned default so search_web always works.
    if (!m_webSearchService) {
        if (!m_ownedWebSearch) {
            m_ownedWebSearch = std::make_unique<Search::WebSearchService>();
        }
        m_webSearchService = m_ownedWebSearch.get();
    }
    Tools::registerITool(
        *this, std::make_unique<Tools::SearchWebTool>(*m_webSearchService), ToolKind::BuiltIn);

    // request_turn — delegated to Tools::RequestTurnTool. The routing
    // descriptor returned by the tool is consumed by ToolDispatcher's
    // completion callback, which emits the cascade signal; nothing in
    // that pathway changed.
    Tools::registerITool(*this, std::make_unique<Tools::RequestTurnTool>(), ToolKind::BuiltIn);

    // Mark all tools registered so far as built-in
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it) {
        it->kind = ToolKind::BuiltIn;
        m_builtInNames.insert(it.key());
    }

    qCInfo(verzetaTools) << "ToolService: registered" << m_builtInNames.size() << "built-in tools";
}

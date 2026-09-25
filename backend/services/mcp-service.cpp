// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file mcp-service.cpp
 * @brief MCP service implementation: multi-server management.
 * @layer Service
 * @dependencies McpClient, ToolService, Qt6::Core, Qt6::Network.
 */


#include "mcp-service.h"

#include "../utils/logger.h"
#include "tool-service.h"

#include <QThread>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QStandardPaths>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

McpService::McpService(ToolService* toolService, QObject* parent)
    : QObject(parent), m_toolService(toolService) {}

McpService::~McpService() {
    for (auto* client : m_clients) {
        if (client)
            client->disconnect();
    }
    // QObject children cleaned up automatically
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void McpService::loadAndConnect() {
    const QJsonObject root = loadConfigFile();
    const QJsonObject servers = root[QStringLiteral("mcpServers")].toObject();

    for (auto it = servers.begin(); it != servers.end(); ++it) {
        const QString name = it.key();
        const QJsonObject cfg = it.value().toObject();

        McpClient::ServerConfig sc;
        sc.name = name;
        sc.disabled = cfg[QStringLiteral("disabled")].toBool(false);

        // Determine transport type
        const QString typeStr = cfg[QStringLiteral("type")].toString();
        if (typeStr == QStringLiteral("sse")) {
            sc.transport = McpClient::Transport::Sse;
        } else if (typeStr == QStringLiteral("streamable_http")) {
            sc.transport = McpClient::Transport::StreamableHttp;
        } else {
            sc.transport = McpClient::Transport::Stdio;
        }

        // stdio fields
        sc.command = cfg[QStringLiteral("command")].toString();
        const QJsonArray argsArr = cfg[QStringLiteral("args")].toArray();
        for (const QJsonValue& a : argsArr) {
            sc.args.append(a.toString());
        }
        const QJsonObject envObj = cfg[QStringLiteral("env")].toObject();
        for (auto eit = envObj.begin(); eit != envObj.end(); ++eit) {
            sc.env[eit.key()] = eit.value().toString();
        }

        // HTTP fields
        sc.url = cfg[QStringLiteral("url")].toString();
        const QJsonObject hdrs = cfg[QStringLiteral("headers")].toObject();
        for (auto hit = hdrs.begin(); hit != hdrs.end(); ++hit) {
            sc.headers[hit.key()] = hit.value().toString();
        }

        sc.timeoutMs = cfg[QStringLiteral("timeout")].toInt(30000);

        m_configs[name] = sc;

        if (!sc.disabled) {
            connectServer(name);
        }
    }

    qCInfo(verzetaTools) << "McpService: loaded" << m_configs.size() << "server configs,"
                         << m_clients.size() << "active";
    emit serversChanged();
}

QVariantList McpService::serverList() const {
    QVariantList list;
    for (auto it = m_configs.cbegin(); it != m_configs.cend(); ++it) {
        QVariantMap entry;
        entry[QStringLiteral("name")] = it->name;
        entry[QStringLiteral("disabled")] = it->disabled;

        switch (it->transport) {
            case McpClient::Transport::Stdio:
                entry[QStringLiteral("type")] = QStringLiteral("stdio");
                entry[QStringLiteral("command")] = it->command;
                entry[QStringLiteral("args")] = it->args.join(QLatin1Char(' '));
                break;
            case McpClient::Transport::Sse:
                entry[QStringLiteral("type")] = QStringLiteral("sse");
                entry[QStringLiteral("url")] = it->url;
                break;
            case McpClient::Transport::StreamableHttp:
                entry[QStringLiteral("type")] = QStringLiteral("streamable_http");
                entry[QStringLiteral("url")] = it->url;
                break;
        }

        // Connection status
        auto clientIt = m_clients.find(it.key());
        if (clientIt != m_clients.end() && *clientIt) {
            const auto state = (*clientIt)->state();
            switch (state) {
                case McpClient::State::Ready:
                    entry[QStringLiteral("status")] = QStringLiteral("connected");
                    break;
                case McpClient::State::Connecting:
                case McpClient::State::Initializing:
                    entry[QStringLiteral("status")] = QStringLiteral("connecting");
                    break;
                case McpClient::State::Error:
                    entry[QStringLiteral("status")] = QStringLiteral("error");
                    entry[QStringLiteral("error")] = (*clientIt)->lastError();
                    break;
                default:
                    entry[QStringLiteral("status")] = QStringLiteral("disconnected");
                    break;
            }
        } else {
            entry[QStringLiteral("status")] =
                it->disabled ? QStringLiteral("disabled") : QStringLiteral("disconnected");
        }

        // Tool count
        entry[QStringLiteral("toolCount")] = m_serverToolNames.value(it.key()).size();

        list.append(entry);
    }
    return list;
}

bool McpService::addServer(const QVariantMap& config) {
    const QString name = config[QStringLiteral("name")].toString().trimmed();
    if (name.isEmpty())
        return false;

    McpClient::ServerConfig sc;
    sc.name = name;
    sc.disabled = config[QStringLiteral("disabled")].toBool();

    const QString typeStr = config[QStringLiteral("type")].toString();
    if (typeStr == QStringLiteral("sse")) {
        sc.transport = McpClient::Transport::Sse;
    } else if (typeStr == QStringLiteral("streamable_http")) {
        sc.transport = McpClient::Transport::StreamableHttp;
    } else {
        sc.transport = McpClient::Transport::Stdio;
    }

    sc.command = config[QStringLiteral("command")].toString();
    sc.args = config[QStringLiteral("args")].toStringList();
    sc.url = config[QStringLiteral("url")].toString();

    // Parse env from QVariantMap
    const QVariantMap envMap = config[QStringLiteral("env")].toMap();
    for (auto it = envMap.cbegin(); it != envMap.cend(); ++it) {
        sc.env[it.key()] = it.value().toString();
    }

    // Parse headers from QVariantMap
    const QVariantMap hdrMap = config[QStringLiteral("headers")].toMap();
    for (auto it = hdrMap.cbegin(); it != hdrMap.cend(); ++it) {
        sc.headers[it.key()] = it.value().toString();
    }

    const QVariant timeoutVar = config[QStringLiteral("timeout")];
    sc.timeoutMs = timeoutVar.isValid() ? timeoutVar.toInt() : 30000;

    m_configs[name] = sc;
    saveConfig();

    if (!sc.disabled) {
        connectServer(name);
    }

    emit serversChanged();
    return true;
}

bool McpService::removeServer(const QString& name) {
    if (!m_configs.contains(name))
        return false;

    disconnectServer(name);
    m_configs.remove(name);
    saveConfig();
    emit serversChanged();
    return true;
}

void McpService::setServerEnabled(const QString& name, bool enabled) {
    auto it = m_configs.find(name);
    if (it == m_configs.end())
        return;

    it->disabled = !enabled;
    if (enabled) {
        connectServer(name);
    } else {
        disconnectServer(name);
    }
    saveConfig();
    emit serversChanged();
}

void McpService::reconnectServer(const QString& name) {
    disconnectServer(name);
    connectServer(name);
    emit serversChanged();
}

QVariantList McpService::serverTools(const QString& serverName) const {
    QVariantList list;
    auto clientIt = m_clients.find(serverName);
    if (clientIt == m_clients.end() || !*clientIt)
        return list;

    const QJsonArray tools = (*clientIt)->discoveredTools();
    for (const QJsonValue& tv : tools) {
        const QJsonObject t = tv.toObject();
        QVariantMap entry;
        entry[QStringLiteral("name")] = t[QStringLiteral("name")].toString();
        entry[QStringLiteral("description")] = t[QStringLiteral("description")].toString();
        entry[QStringLiteral("server")] = serverName;
        list.append(entry);
    }
    return list;
}

void McpService::saveConfig() {
    QJsonObject servers;
    for (auto it = m_configs.cbegin(); it != m_configs.cend(); ++it) {
        QJsonObject cfg;

        switch (it->transport) {
            case McpClient::Transport::Stdio:
                cfg[QStringLiteral("command")] = it->command;
                {
                    QJsonArray argsArr;
                    for (const QString& a : it->args)
                        argsArr.append(a);
                    cfg[QStringLiteral("args")] = argsArr;
                }
                if (!it->env.isEmpty()) {
                    QJsonObject envObj;
                    for (auto eit = it->env.cbegin(); eit != it->env.cend(); ++eit)
                        envObj[eit.key()] = eit.value();
                    cfg[QStringLiteral("env")] = envObj;
                }
                break;

            case McpClient::Transport::Sse:
                cfg[QStringLiteral("type")] = QStringLiteral("sse");
                cfg[QStringLiteral("url")] = it->url;
                break;

            case McpClient::Transport::StreamableHttp:
                cfg[QStringLiteral("type")] = QStringLiteral("streamable_http");
                cfg[QStringLiteral("url")] = it->url;
                break;
        }

        if (!it->headers.isEmpty()) {
            QJsonObject hdrs;
            for (auto hit = it->headers.cbegin(); hit != it->headers.cend(); ++hit)
                hdrs[hit.key()] = hit.value();
            cfg[QStringLiteral("headers")] = hdrs;
        }

        cfg[QStringLiteral("disabled")] = it->disabled;
        if (it->timeoutMs != 30000)
            cfg[QStringLiteral("timeout")] = it->timeoutMs;

        servers[it.key()] = cfg;
    }

    QJsonObject root;
    root[QStringLiteral("mcpServers")] = servers;

    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
        qCInfo(verzetaTools) << "McpService: saved config to" << path;
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void McpService::connectServer(const QString& name) {
    auto cfgIt = m_configs.find(name);
    if (cfgIt == m_configs.end())
        return;

    // Disconnect existing
    disconnectServer(name);

    auto* client = new McpClient(this);

    connect(client, &McpClient::toolsDiscovered, this, [this, name](const QJsonArray& tools) {
        registerMcpToolsFromServer(name, tools);
        emit serversChanged();
        emit mcpToolsChanged();
    });

    connect(client, &McpClient::error, this, [this, name](const QString& msg) {
        qCWarning(verzetaTools) << "MCP server" << name << "error:" << msg;
        emit serversChanged();
    });

    connect(client, &McpClient::stateChanged, this, [this](McpClient::State) {
        emit serversChanged();
    });

    client->connectToServer(*cfgIt);
    m_clients[name] = client;
}

void McpService::disconnectServer(const QString& name) {
    unregisterMcpToolsFromServer(name);
    auto it = m_clients.find(name);
    if (it != m_clients.end()) {
        if (*it) {
            (*it)->disconnect();
            (*it)->deleteLater();
        }
        m_clients.erase(it);
    }
}

void McpService::registerMcpToolsFromServer(const QString& serverName, const QJsonArray& tools) {
    if (!m_toolService)
        return;

    // Batch the whole (unregister-old + register-new) sequence. Every
    // mutation inside the batch is coalesced into a single toolsChanged
    // signal at endBatch(), which prevents the O(N^2) UI freeze from
    // the Repeater rebuilding on every tool registration.
    m_toolService->beginBatch();
    auto batchGuard = qScopeGuard([this]() { m_toolService->endBatch(); });

    // Unregister old tools from this server (still inside the batch)
    unregisterMcpToolsFromServer(serverName);

    QStringList registeredNames;
    McpClient* client = m_clients.value(serverName, nullptr);
    if (!client)
        return;

    for (const QJsonValue& tv : tools) {
        const QJsonObject t = tv.toObject();
        const QString toolName = t[QStringLiteral("name")].toString();
        if (toolName.isEmpty())
            continue;

        // Prefix with server name to avoid collisions: "server:tool"
        const QString qualifiedName = QStringLiteral("%1:%2").arg(serverName, toolName);

        // Build ToolSchema from MCP tool definition
        ToolSchema schema;
        schema.name = qualifiedName;
        schema.description = QStringLiteral("[MCP: %1] %2")
                                 .arg(serverName, t[QStringLiteral("description")].toString());

        // Parse inputSchema → ToolParameterSchema
        const QJsonObject inputSchema = t[QStringLiteral("inputSchema")].toObject();
        const QJsonObject properties = inputSchema[QStringLiteral("properties")].toObject();
        const QJsonArray required = inputSchema[QStringLiteral("required")].toArray();

        QSet<QString> requiredSet;
        for (const QJsonValue& rv : required)
            requiredSet.insert(rv.toString());

        for (auto pit = properties.begin(); pit != properties.end(); ++pit) {
            ToolParameterSchema p;
            p.name = pit.key();
            p.type = pit.value().toObject()[QStringLiteral("type")].toString();
            p.description = pit.value().toObject()[QStringLiteral("description")].toString();
            p.required = requiredSet.contains(pit.key());
            schema.parameters.append(p);
        }

        // Create handler that delegates to McpClient::callTool
        // Capture raw toolName (without prefix) for the MCP call
        ToolHandler handler = [client, toolName](const QJsonObject& args) -> QJsonValue {
            // Synchronous wrapper around async callTool
            // (invokeTool runs on worker thread so blocking is OK)
            QJsonObject result;
            bool done = false;

            QMetaObject::invokeMethod(
                client,
                [&]() {
                    client->callTool(toolName, args, [&](const QJsonObject& r) {
                        result = r;
                        done = true;
                    });
                },
                Qt::BlockingQueuedConnection);

            // Wait for response (with timeout)
            const auto deadline = QDeadlineTimer(30000);
            while (!done && !deadline.hasExpired()) {
                QThread::msleep(10);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }

            if (!done) {
                return QJsonObject{
                    {QStringLiteral("error"), QStringLiteral("MCP tool call timed out")}};
            }

            // Extract text content from MCP result
            const QJsonArray content = result[QStringLiteral("content")].toArray();
            if (content.isEmpty())
                return result;

            // Combine text content blocks
            QString text;
            for (const QJsonValue& cv : content) {
                const QJsonObject co = cv.toObject();
                if (co[QStringLiteral("type")].toString() == QStringLiteral("text")) {
                    if (!text.isEmpty())
                        text += QLatin1Char('\n');
                    text += co[QStringLiteral("text")].toString();
                }
            }

            if (result[QStringLiteral("isError")].toBool()) {
                return QJsonObject{{QStringLiteral("error"), text}};
            }

            return QJsonObject{{QStringLiteral("result"), text}};
        };

        m_toolService->registerTool(schema, std::move(handler), ToolKind::Mcp);
        registeredNames.append(qualifiedName);
    }

    m_serverToolNames[serverName] = registeredNames;
    qCInfo(verzetaTools) << "McpService: registered" << registeredNames.size()
                         << "tools from MCP server" << serverName;
}

void McpService::unregisterMcpToolsFromServer(const QString& serverName) {
    if (!m_toolService)
        return;

    const QStringList names = m_serverToolNames.value(serverName);
    for (const QString& n : names) {
        m_toolService->unregisterTool(n);
    }
    m_serverToolNames.remove(serverName);
}

QString McpService::configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/mcp-servers.json");
}

QJsonObject McpService::loadConfigFile() const {
    const QString path = configPath();
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        qCInfo(verzetaTools) << "McpService: no config at" << path;
        return {};
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();

    if (err.error != QJsonParseError::NoError) {
        qCWarning(verzetaTools) << "McpService: config parse error:" << err.errorString();
        return {};
    }

    return doc.object();
}

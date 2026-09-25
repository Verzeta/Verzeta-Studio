// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file mcp-service.h
 * @brief Manages multiple MCP server connections. Loads mcp-servers.json
 *        config, spawns/connects MCP clients, aggregates discovered tools,
 *        and registers them into ToolService for LLM access.
 * @layer Service
 * @dependencies McpClient, ToolService, Qt6::Core
 */


#pragma once

#include "mcp-client.h"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariantList>

class ToolService;

/**
 * @brief Manages the lifecycle of multiple MCP server connections.
 *
 * Exposed to QML as "McpService" context property for configuration UI.
 *
 * ## Config file format (mcp-servers.json)
 * ```json
 * {
 *   "mcpServers": {
 *     "server-name": {
 *       "command": "npx",
 *       "args": ["-y", "@modelcontextprotocol/server-filesystem"],
 *       "env": { "KEY": "value" },
 *       "disabled": false
 *     },
 *     "remote-server": {
 *       "type": "sse",
 *       "url": "http://localhost:8000/sse",
 *       "headers": { "Authorization": "Bearer token" },
 *       "disabled": false
 *     }
 *   }
 * }
 * ```
 */
class McpService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the service.
     * @param toolService Registry that receives each server's tools.
     * @param parent      Optional Qt parent.
     */
    explicit McpService(ToolService* toolService, QObject* parent = nullptr);
    ~McpService() override;

    /**
     * @brief Loads mcp-servers.json and connects to all enabled servers.
     * @sideeffects Spawns subprocesses, registers tools into ToolService.
     */
    void loadAndConnect();

    /**
     * @brief Returns the list of configured MCP servers for QML.
     * @return QVariantList of {name, type, command/url, disabled, status, toolCount} maps.
     */
    Q_INVOKABLE QVariantList serverList() const;

    /**
     * @brief Adds a new MCP server configuration.
     * @param config QVariantMap matching the JSON config format.
     * @return true on success.
     */
    Q_INVOKABLE bool addServer(const QVariantMap& config);

    /**
     * @brief Removes an MCP server by name.
     * @param name Server name (key in mcpServers).
     * @return true if removed.
     */
    Q_INVOKABLE bool removeServer(const QString& name);

    /**
     * @brief Enables or disables an MCP server.
     * @param name Server name.
     * @param enabled Whether to enable (connect) or disable (disconnect).
     */
    Q_INVOKABLE void setServerEnabled(const QString& name, bool enabled);

    /**
     * @brief Reconnects a specific server (useful after config change).
     * @param name Server name.
     */
    Q_INVOKABLE void reconnectServer(const QString& name);

    /**
     * @brief Returns tools discovered from a specific MCP server.
     * @param serverName Server name.
     * @return QVariantList of tool schemas.
     */
    Q_INVOKABLE QVariantList serverTools(const QString& serverName) const;

    /**
     * @brief Saves the current configuration to mcp-servers.json.
     */
    Q_INVOKABLE void saveConfig();

  signals:
    /** @brief Emitted when the server list or any server status changes. */
    void serversChanged();

    /** @brief Emitted when tools from any server change. */
    void mcpToolsChanged();

  private:
    void connectServer(const QString& name);
    void disconnectServer(const QString& name);
    void registerMcpToolsFromServer(const QString& serverName, const QJsonArray& tools);
    void unregisterMcpToolsFromServer(const QString& serverName);

    static QString configPath();
    QJsonObject loadConfigFile() const;

    ToolService* m_toolService;

    /// Server configs keyed by name
    QMap<QString, McpClient::ServerConfig> m_configs;

    /// Active client connections keyed by name (owned via QObject parent)
    QMap<QString, McpClient*> m_clients;

    /// Track which tool names were registered from which server
    QMap<QString, QStringList> m_serverToolNames;
};

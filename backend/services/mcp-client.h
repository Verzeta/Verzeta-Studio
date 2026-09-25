// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file mcp-client.h
 * @brief MCP (Model Context Protocol) client for communicating with
 *        a single MCP server. Supports stdio (QProcess), SSE, and streamable
 *        HTTP transports. Implements the JSON-RPC 2.0 message flow:
 *        initialize → tools/list → tools/call.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Network, HttpClient
 */


#pragma once

#include <QTimer>

#include <functional>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * @brief Communicates with a single MCP server via JSON-RPC 2.0.
 *
 * ## Lifecycle
 * 1. Call connectToServer() with config (command+args or url).
 * 2. Client sends initialize → server responds with capabilities.
 * 3. Client sends notifications/initialized.
 * 4. Client calls discoverTools() → server responds with tools/list.
 * 5. Client calls callTool(name, args) → server responds with result.
 * 6. Call disconnect() to shut down.
 *
 * ## Transport
 * - stdio: QProcess; write JSON-RPC to stdin, read from stdout
 * - sse: HTTP GET for server→client events, POST for client→server
 * - streamable_http: single HTTP endpoint, POST+SSE
 */
class McpClient : public QObject {
    Q_OBJECT

  public:
    /** @brief How the client talks to the server (stdio, SSE or streamable HTTP). */
    enum class Transport { Stdio, Sse, StreamableHttp };
    Q_ENUM(Transport)

    /**
     * @brief Connection state. Initializing covers the MCP initialize
     *        handshake; Ready means tools can be listed and called.
     */
    enum class State { Disconnected, Connecting, Initializing, Ready, Error };
    Q_ENUM(State)

    /**
     * @brief Configuration for connecting to an MCP server.
     */
    struct ServerConfig {
        QString name;                            ///< User-defined server name (key in mcpServers)
        Transport transport = Transport::Stdio;  ///< How to reach the server.
        QString command;                         ///< Executable for stdio (e.g., "npx")
        QStringList args;                        ///< Arguments for stdio
        QMap<QString, QString> env;              ///< Environment variables for stdio
        QString url;                             ///< URL for SSE / streamable HTTP
        QMap<QString, QString> headers;          ///< HTTP headers (auth, etc.)
        bool disabled = false;                   ///< When true, the server is not connected.
        /// Limit for connecting and the initialize handshake; the
        /// client enters Error when it expires.
        int timeoutMs = 30000;
    };

    /**
     * @brief Constructs a disconnected client.
     * @param parent Optional Qt parent.
     */
    explicit McpClient(QObject* parent = nullptr);
    ~McpClient() override;

    /**
     * @brief Connects to an MCP server using the given config.
     * @param config Server configuration.
     * @sideeffects Spawns subprocess (stdio) or opens HTTP connection.
     */
    void connectToServer(const ServerConfig& config);

    /**
     * @brief Disconnects from the MCP server.
     * @sideeffects Kills subprocess or closes HTTP connection.
     */
    void disconnect();

    /**
     * @brief Requests the tool list from the server.
     * @sideeffects Sends tools/list JSON-RPC. Emits toolsDiscovered() on response.
     */
    void discoverTools();

    /**
     * @brief Invokes a tool on the MCP server.
     * @param toolName Tool name.
     * @param arguments JSON arguments.
     * @param callback Called with the result JSON on completion.
     */
    void callTool(const QString& toolName,
                  const QJsonObject& arguments,
                  std::function<void(const QJsonObject&)> callback);

    /**
     * @brief Current connection state.
     * @returns The State enum value.
     */
    State state() const { return m_state; }

    /**
     * @brief Configured server name.
     * @returns The name from ServerConfig::name.
     */
    QString serverName() const { return m_config.name; }

    /**
     * @brief Cached tools/list response from the server.
     * @returns JSON array of tool descriptors.
     */
    QJsonArray discoveredTools() const { return m_tools; }

    /**
     * @brief Last error message captured.
     * @returns Last error string, or empty when none.
     */
    QString lastError() const { return m_lastError; }

  signals:
    /**
     * @brief Emitted when state() transitions.
     * @param newState New state value.
     */
    void stateChanged(McpClient::State newState);

    /**
     * @brief Emitted with the parsed tools/list response.
     * @param tools JSON array of tool descriptors.
     */
    void toolsDiscovered(const QJsonArray& tools);

    /**
     * @brief Emitted on connection / RPC errors.
     * @param message Human-readable error description.
     */
    void error(const QString& message);

    /**
     * @brief Emitted with a line of server-side log output.
     * @param message Log line from the server.
     */
    void serverLog(const QString& message);

  private slots:
    /** @brief Drain stdio ready-read into the JSON-RPC parser. */
    void onProcessReadyRead();
    /**
     * @brief Handle QProcess::errorOccurred.
     * @param err Qt process-error code.
     */
    void onProcessError(QProcess::ProcessError err);

    /**
     * @brief Handle QProcess::finished.
     * @param exitCode Process exit code.
     * @param status   Normal vs CrashExit indicator.
     */
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

  private:
    void setState(State s);
    void sendJsonRpc(const QJsonObject& message);
    void handleJsonRpcMessage(const QJsonObject& msg);
    void sendInitialize();
    int nextId();

    // stdio transport
    QProcess* m_process = nullptr;
    QByteArray m_readBuffer;

    // HTTP transport
    QNetworkAccessManager* m_nam = nullptr;

    // State
    ServerConfig m_config;
    State m_state = State::Disconnected;
    int m_nextId = 1;
    QJsonArray m_tools;
    QString m_lastError;

    // Pending request callbacks: id → handler
    QMap<int, std::function<void(const QJsonObject&)>> m_pendingRequests;

    QTimer m_timeoutTimer;
};

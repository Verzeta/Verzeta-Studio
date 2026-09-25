// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file mcp-client.cpp
 * @brief MCP client implementation with stdio and HTTP transports.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Network
 */


#include "mcp-client.h"

#include "../utils/logger.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>

/// MCP protocol revision sent in the initialize request.
static constexpr const char* kMcpProtocolVersion = "2024-11-05";

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

McpClient::McpClient(QObject* parent) : QObject(parent) {
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, [this]() {
        if (m_state == State::Connecting || m_state == State::Initializing) {
            setState(State::Error);
            m_lastError = QStringLiteral("Connection timed out");
            emit error(m_lastError);
        }
    });
}

McpClient::~McpClient() {
    disconnect();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void McpClient::connectToServer(const ServerConfig& config) {
    if (m_state != State::Disconnected) {
        disconnect();
    }

    m_config = config;
    m_tools = {};
    m_readBuffer.clear();
    m_pendingRequests.clear();
    m_nextId = 1;

    if (config.transport == Transport::Stdio) {
        // Spawn subprocess
        m_process = new QProcess(this);
        connect(
            m_process, &QProcess::readyReadStandardOutput, this, &McpClient::onProcessReadyRead);
        connect(m_process, &QProcess::errorOccurred, this, &McpClient::onProcessError);
        connect(m_process,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                &McpClient::onProcessFinished);

        // Forward stderr as log messages.
        //
        // Local var named `stderrOutput` rather than `stderr` because
        // the Windows C runtime defines `stderr` as a macro expanding
        // to a FILE* expression — a `QString stderr` declaration
        // collides with it and won't compile on MSYS2. The rename is
        // a no-op on Linux (no such macro) but keeps the source
        // portable.
        connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
            const QString stderrOutput = QString::fromUtf8(m_process->readAllStandardError());
            emit serverLog(stderrOutput.trimmed());
        });

        // Set environment variables
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (auto it = config.env.cbegin(); it != config.env.cend(); ++it) {
            // Support ${VAR_NAME} substitution from system env
            QString val = it.value();
            if (val.startsWith(QStringLiteral("${")) && val.endsWith(QLatin1Char('}'))) {
                const QString varName = val.mid(2, val.length() - 3);
                val = env.value(varName);
            }
            env.insert(it.key(), val);
        }
        m_process->setProcessEnvironment(env);

        setState(State::Connecting);
        m_timeoutTimer.start(config.timeoutMs);

        qCInfo(verzetaTools) << "MCP: spawning" << config.command << config.args << "for server"
                             << config.name;
        m_process->start(config.command, config.args);

        if (!m_process->waitForStarted(5000)) {
            setState(State::Error);
            m_lastError =
                QStringLiteral("Failed to start MCP server: %1").arg(m_process->errorString());
            emit error(m_lastError);
            return;
        }

        // Process started — send initialize
        sendInitialize();

    } else if (config.transport == Transport::Sse ||
               config.transport == Transport::StreamableHttp) {
        // HTTP-based transport
        if (!m_nam) {
            m_nam = new QNetworkAccessManager(this);
        }

        setState(State::Connecting);
        m_timeoutTimer.start(config.timeoutMs);
        sendInitialize();
    }
}

void McpClient::disconnect() {
    m_timeoutTimer.stop();
    m_pendingRequests.clear();

    if (m_process) {
        if (m_process->state() != QProcess::NotRunning) {
            m_process->closeWriteChannel();
            if (!m_process->waitForFinished(3000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
    }

    setState(State::Disconnected);
}

void McpClient::discoverTools() {
    if (m_state != State::Ready) {
        emit error(QStringLiteral("Cannot discover tools: not connected"));
        return;
    }

    // MCP spec: for the initial tools/list call, `cursor` is OPTIONAL
    // and must be either an opaque string token (from a previous
    // nextCursor) or absent. We must NOT send `cursor: null` — strict
    // schema validators (e.g. mcp-swiss which uses zod) reject that as
    // "expected string, received null" and the tool list fails to load.
    // Send an empty params object on first call.
    QJsonObject msg;
    msg[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    msg[QStringLiteral("id")] = nextId();
    msg[QStringLiteral("method")] = QStringLiteral("tools/list");
    msg[QStringLiteral("params")] = QJsonObject{};

    const int id = msg[QStringLiteral("id")].toInt();
    m_pendingRequests[id] = [this](const QJsonObject& result) {
        m_tools = result[QStringLiteral("tools")].toArray();
        qCInfo(verzetaTools) << "MCP:" << m_config.name << "discovered" << m_tools.size()
                             << "tools";
        emit toolsDiscovered(m_tools);
    };

    sendJsonRpc(msg);
}

void McpClient::callTool(const QString& toolName,
                         const QJsonObject& arguments,
                         std::function<void(const QJsonObject&)> callback) {
    if (m_state != State::Ready) {
        QJsonObject err;
        err[QStringLiteral("isError")] = true;
        err[QStringLiteral("content")] = QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("MCP server not connected")}}};
        callback(err);
        return;
    }

    QJsonObject params;
    params[QStringLiteral("name")] = toolName;
    params[QStringLiteral("arguments")] = arguments;

    QJsonObject msg;
    msg[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    msg[QStringLiteral("id")] = nextId();
    msg[QStringLiteral("method")] = QStringLiteral("tools/call");
    msg[QStringLiteral("params")] = params;

    const int id = msg[QStringLiteral("id")].toInt();
    m_pendingRequests[id] = callback;

    sendJsonRpc(msg);
}

// ---------------------------------------------------------------------------
// Private: JSON-RPC transport
// ---------------------------------------------------------------------------

void McpClient::sendJsonRpc(const QJsonObject& message) {
    const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';

    if (m_config.transport == Transport::Stdio && m_process) {
        m_process->write(json);
        m_process->waitForBytesWritten(1000);
    } else if (m_nam && !m_config.url.isEmpty()) {
        // HTTP POST for SSE and StreamableHttp
        QNetworkRequest req(QUrl(m_config.url));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        // Add custom headers (auth, etc.)
        for (auto it = m_config.headers.cbegin(); it != m_config.headers.cend(); ++it) {
            req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
        }

        QNetworkReply* reply = m_nam->post(req, json);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                m_lastError = reply->errorString();
                emit error(m_lastError);
                return;
            }
            const QByteArray body = reply->readAll();
            QJsonParseError parseErr;
            const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
            if (parseErr.error != QJsonParseError::NoError) {
                emit error(QStringLiteral("MCP: invalid JSON response"));
                return;
            }
            handleJsonRpcMessage(doc.object());
        });
    }
}

void McpClient::sendInitialize() {
    setState(State::Initializing);

    QJsonObject clientInfo;
    clientInfo[QStringLiteral("name")] = QStringLiteral("Verzeta");
    clientInfo[QStringLiteral("version")] = QStringLiteral("1.0.0");

    QJsonObject capabilities;
    // We support tool calling
    capabilities[QStringLiteral("roots")] = QJsonObject{};

    QJsonObject params;
    params[QStringLiteral("protocolVersion")] = QLatin1String(kMcpProtocolVersion);
    params[QStringLiteral("capabilities")] = capabilities;
    params[QStringLiteral("clientInfo")] = clientInfo;

    QJsonObject msg;
    msg[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    msg[QStringLiteral("id")] = nextId();
    msg[QStringLiteral("method")] = QStringLiteral("initialize");
    msg[QStringLiteral("params")] = params;

    const int id = msg[QStringLiteral("id")].toInt();
    m_pendingRequests[id] = [this](const QJsonObject& result) {
        m_timeoutTimer.stop();

        const QString serverName =
            result[QStringLiteral("serverInfo")].toObject()[QStringLiteral("name")].toString();
        qCInfo(verzetaTools) << "MCP:" << m_config.name << "initialized (server:" << serverName
                             << ")";

        // Send initialized notification (no id, no response expected)
        QJsonObject notif;
        notif[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        notif[QStringLiteral("method")] = QStringLiteral("notifications/initialized");
        sendJsonRpc(notif);

        setState(State::Ready);

        // Auto-discover tools after initialization
        discoverTools();
    };

    sendJsonRpc(msg);
}

void McpClient::handleJsonRpcMessage(const QJsonObject& msg) {
    // Check if it's a response (has "id" and "result" or "error")
    if (msg.contains(QStringLiteral("id")) &&
        (msg.contains(QStringLiteral("result")) || msg.contains(QStringLiteral("error")))) {
        const int id = msg[QStringLiteral("id")].toInt();
        auto it = m_pendingRequests.find(id);
        if (it != m_pendingRequests.end()) {
            if (msg.contains(QStringLiteral("error"))) {
                const QJsonObject err = msg[QStringLiteral("error")].toObject();
                const QString errMsg = err[QStringLiteral("message")].toString();
                qCWarning(verzetaTools) << "MCP:" << m_config.name << "error:" << errMsg;
                // Pass error as result with isError flag
                QJsonObject errorResult;
                errorResult[QStringLiteral("isError")] = true;
                errorResult[QStringLiteral("content")] =
                    QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                           {QStringLiteral("text"), errMsg}}};
                it.value()(errorResult);
            } else {
                it.value()(msg[QStringLiteral("result")].toObject());
            }
            m_pendingRequests.erase(it);
        }
        return;
    }

    // Check if it's a notification (has "method" but no "id")
    if (msg.contains(QStringLiteral("method")) && !msg.contains(QStringLiteral("id"))) {
        const QString method = msg[QStringLiteral("method")].toString();
        if (method == QStringLiteral("notifications/tools/list_changed")) {
            // Server says tools changed — re-discover
            qCInfo(verzetaTools) << "MCP:" << m_config.name << "tools changed, re-discovering";
            discoverTools();
        } else if (method == QStringLiteral("notifications/message")) {
            const QString logMsg =
                msg[QStringLiteral("params")].toObject()[QStringLiteral("data")].toString();
            emit serverLog(logMsg);
        }
    }
}

int McpClient::nextId() {
    return m_nextId++;
}

void McpClient::setState(State s) {
    if (m_state != s) {
        m_state = s;
        emit stateChanged(s);
    }
}

// ---------------------------------------------------------------------------
// QProcess slots (stdio transport)
// ---------------------------------------------------------------------------

void McpClient::onProcessReadyRead() {
    m_readBuffer += m_process->readAllStandardOutput();

    // Parse newline-delimited JSON messages
    while (true) {
        const int nlPos = m_readBuffer.indexOf('\n');
        if (nlPos < 0)
            break;

        const QByteArray line = m_readBuffer.left(nlPos).trimmed();
        m_readBuffer = m_readBuffer.mid(nlPos + 1);

        if (line.isEmpty())
            continue;

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError) {
            qCWarning(verzetaTools)
                << "MCP:" << m_config.name << "invalid JSON:" << err.errorString();
            continue;
        }

        handleJsonRpcMessage(doc.object());
    }
}

void McpClient::onProcessError(QProcess::ProcessError err) {
    Q_UNUSED(err)
    m_lastError = QStringLiteral("MCP process error: %1").arg(m_process->errorString());
    qCWarning(verzetaTools) << m_lastError;
    setState(State::Error);
    emit error(m_lastError);
}

void McpClient::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status)
    qCInfo(verzetaTools) << "MCP:" << m_config.name << "process exited with code" << exitCode;
    if (m_state != State::Disconnected) {
        setState(State::Disconnected);
    }
}

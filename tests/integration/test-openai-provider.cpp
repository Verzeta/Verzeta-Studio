// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/openai-provider.h"
#include "../../backend/utils/http-client.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <QByteArray>
#include <QEventLoop>
#include <QList>
#include <QSignalSpy>
#include <QString>


class MockSseServer : public QObject {
    Q_OBJECT
  public:
    explicit MockSseServer(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, &MockSseServer::onNewConnection);
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }

    quint16 port() const { return m_server.serverPort(); }

    void setResponse(const QByteArray& response) { m_response = response; }

  private slots:
    void onNewConnection() {
        QTcpSocket* socket = m_server.nextPendingConnection();
        if (!socket) {
            return;
        }
        auto* responded = new bool(false);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, responded]() {
            socket->readAll();
            if (*responded) {
                return;
            }
            *responded = true;
            socket->write(m_response);
            socket->flush();
            QTimer::singleShot(500, socket, &QTcpSocket::disconnectFromHost);
        });
        connect(socket, &QTcpSocket::disconnected, socket, [socket, responded]() {
            delete responded;
            socket->deleteLater();
        });
    }

  private:
    QTcpServer m_server;
    QByteArray m_response;
};


static QByteArray buildSseResponse(const QList<QByteArray>& dataLines) {
    QByteArray body;
    for (const QByteArray& line : dataLines) {
        body += "data: " + line + "\n\n";
    }
    body += "data: [DONE]\n\n";

    QByteArray response;
    response += "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/event-stream\r\n";
    response += "Cache-Control: no-cache\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;
    return response;
}

static QByteArray buildErrorResponse(int statusCode, const QByteArray& body) {
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(statusCode) + " Error\r\n";
    response += "Content-Type: application/json\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "\r\n";
    response += body;
    return response;
}

static const QByteArray kTextChunk1 = "{\"id\":\"chatcmpl-1\",\"choices\":[{\"delta\":{\"content\":"
                                      "\"Hello\"},\"finish_reason\":null,\"index\":0}]}";
static const QByteArray kTextChunk2 = "{\"id\":\"chatcmpl-1\",\"choices\":[{\"delta\":{\"content\":"
                                      "\" world\"},\"finish_reason\":null,\"index\":0}]}";
static const QByteArray kStopChunk =
    "{\"id\":\"chatcmpl-1\",\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\",\"index\":0}]}";
static const QByteArray kToolCallChunk1 =
    "{\"id\":\"chatcmpl-2\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_"
    "abc\","
    "\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"\"}}]},"
    "\"finish_reason\":null,\"index\":0}]}";
static const QByteArray kToolCallChunk2 =
    "{\"id\":\"chatcmpl-2\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"function\":{\"arguments\":\"{\\\"location\\\":\"}}]},\"finish_reason\":null,\"index\":0}]}";
static const QByteArray kToolCallChunk3 =
    "{\"id\":\"chatcmpl-2\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"function\":{\"arguments\":\"\\\"London\\\"}\"}}]},\"finish_reason\":null,\"index\":0}]}";
static const QByteArray kToolCallStopChunk = "{\"id\":\"chatcmpl-2\",\"choices\":[{\"delta\":{},"
                                             "\"finish_reason\":\"tool_calls\",\"index\":0}]}";


class TestOpenAIProvider : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<OpenAIProvider> m_provider;
    std::unique_ptr<MockSseServer> m_server;

  private slots:

    void init() {
        m_server = std::make_unique<MockSseServer>();
        QVERIFY(m_server->listen());

        m_http = std::make_unique<HttpClient>();
        m_provider = std::make_unique<OpenAIProvider>(*m_http);
        m_provider->setApiKey(QStringLiteral("test-key-placeholder"));
        m_provider->setBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server->port()));
    }

    void cleanup() {
        m_provider.reset();
        m_http.reset();
        m_server.reset();
    }

    void test_sseParsing_allChunksReceived() {
        m_server->setResponse(buildSseResponse({kTextChunk1, kTextChunk2, kStopChunk}));

        QSignalSpy chunkSpy(m_provider.get(), &ILLMProvider::chunkReceived);
        QSignalSpy finishedSpy(m_provider.get(), &ILLMProvider::requestFinished);

        LlmRequest req;
        req.config.modelName = QStringLiteral("gpt-4o");
        req.config.stream = true;
        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Hi");
        req.messages.append(msg);

        m_provider->sendRequest(req);
        QVERIFY(finishedSpy.wait(5000));

        QVERIFY(chunkSpy.count() >= 2);

        const LlmChunk c0 = chunkSpy[0][0].value<LlmChunk>();
        const LlmChunk c1 = chunkSpy[1][0].value<LlmChunk>();
        QCOMPARE(c0.delta, QStringLiteral("Hello"));
        QCOMPARE(c1.delta, QStringLiteral(" world"));
    }

    void test_toolCallParsing_chunkContainsToolCall() {
        m_server->setResponse(buildSseResponse(
            {kToolCallChunk1, kToolCallChunk2, kToolCallChunk3, kToolCallStopChunk}));

        QSignalSpy chunkSpy(m_provider.get(), &ILLMProvider::chunkReceived);
        QSignalSpy finishedSpy(m_provider.get(), &ILLMProvider::requestFinished);

        LlmRequest req;
        req.config.modelName = QStringLiteral("gpt-4o");
        req.config.stream = true;
        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("What is the weather in London?");
        req.messages.append(msg);

        m_provider->sendRequest(req);
        QVERIFY(finishedSpy.wait(5000));

        bool foundToolCall = false;
        for (int i = 0; i < chunkSpy.count(); ++i) {
            const LlmChunk chunk = chunkSpy[i][0].value<LlmChunk>();
            if (!chunk.toolCallJson.isEmpty()) {
                foundToolCall = true;
                QCOMPARE(chunk.toolCallJson[QStringLiteral("id")].toString(),
                         QStringLiteral("call_abc"));
                QCOMPARE(chunk.toolCallJson[QStringLiteral("name")].toString(),
                         QStringLiteral("get_weather"));
                const QJsonObject args = chunk.toolCallJson[QStringLiteral("arguments")].toObject();
                QCOMPARE(args[QStringLiteral("location")].toString(), QStringLiteral("London"));
                break;
            }
        }
        QVERIFY2(foundToolCall, "Expected a tool call chunk but none found");
    }

    void test_finishReason_stopIsForwarded() {
        m_server->setResponse(buildSseResponse({kTextChunk1, kStopChunk}));

        QSignalSpy finishedSpy(m_provider.get(), &ILLMProvider::requestFinished);

        LlmRequest req;
        req.config.modelName = QStringLiteral("gpt-4o");
        req.config.stream = true;
        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Test");
        req.messages.append(msg);

        m_provider->sendRequest(req);
        QVERIFY(finishedSpy.wait(5000));
        QCOMPARE(finishedSpy[0][0].toString(), QStringLiteral("stop"));
    }

    void test_errorResponse_emitsRequestError() {
        const QByteArray errorBody =
            R"({"error":{"message":"Invalid API key","type":"invalid_request_error","code":"invalid_api_key"}})";
        m_server->setResponse(buildErrorResponse(401, errorBody));

        QSignalSpy errorSpy(m_provider.get(), &ILLMProvider::requestError);

        LlmRequest req;
        req.config.modelName = QStringLiteral("gpt-4o");
        req.config.stream = true;
        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Test");
        req.messages.append(msg);

        m_provider->sendRequest(req);

        QEventLoop loop;
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        connect(m_provider.get(), &ILLMProvider::requestError, &loop, &QEventLoop::quit);
        loop.exec();

        qInfo() << "Error spy count:" << errorSpy.count();
    }

    void test_emptyApiKey_emitsErrorWithoutNetworkCall() {
        m_provider->setApiKey(QString{});

        QSignalSpy errorSpy(m_provider.get(), &ILLMProvider::requestError);

        LlmRequest req;
        req.config.modelName = QStringLiteral("gpt-4o");
        req.config.stream = true;
        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Test");
        req.messages.append(msg);

        m_provider->sendRequest(req);

        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy[0][0].toString().contains(QStringLiteral("API key")));
    }
};

QTEST_MAIN(TestOpenAIProvider)
#include "test-openai-provider.moc"

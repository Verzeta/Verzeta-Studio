// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llamacpp-remote-provider.h"
#include "../../backend/api/llm-interface.h"
#include "../../backend/api/openai-provider.h"
#include "../../backend/utils/http-client.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <QByteArray>
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

    int connectionCount() const { return m_connectionCount; }

  private slots:
    void onNewConnection() {
        QTcpSocket* socket = m_server.nextPendingConnection();
        if (!socket) {
            return;
        }
        ++m_connectionCount;
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
    int m_connectionCount = 0;
};


static QByteArray buildMinimalSseResponse() {
    QByteArray body;
    body += "data: {\"id\":\"chatcmpl-mock\","
            "\"choices\":[{\"delta\":{\"content\":\"\"},"
            "\"finish_reason\":\"stop\",\"index\":0}]}\n\n";
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


static LlmRequest buildMinimalRequest() {
    LlmRequest req;
    req.config.modelName = QStringLiteral("current");
    req.config.stream = true;
    LlmMessage msg;
    msg.role = QStringLiteral("user");
    msg.content = QStringLiteral("Hello");
    req.messages.append(msg);
    return req;
}


class TestLlamaCppRemoteEmptyKeyDispatch : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<MockSseServer> m_server;

  private slots:

    void init() {
        m_server = std::make_unique<MockSseServer>();
        QVERIFY(m_server->listen());
        m_server->setResponse(buildMinimalSseResponse());

        m_http = std::make_unique<HttpClient>();
    }

    void cleanup() {
        m_http.reset();
        m_server.reset();
    }

    void test_llamaCppRemote_emptyKey_dispatchesToNetwork() {
        LlamaCppRemoteProvider provider(*m_http);
        provider.setBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server->port()));

        QSignalSpy errorSpy(&provider, &ILLMProvider::requestError);
        QSignalSpy finishedSpy(&provider, &ILLMProvider::requestFinished);

        provider.sendRequest(buildMinimalRequest());

        const bool gotTerminal = finishedSpy.wait(5000) || errorSpy.count() > 0;
        QVERIFY2(gotTerminal,
                 "Expected sendRequest to produce a terminal signal "
                 "after reaching the network; got neither requestFinished "
                 "nor requestError.");

        QVERIFY2(m_server->connectionCount() >= 1,
                 "Expected at least one incoming connection on the mock "
                 "server — the dispatch never reached the network, which "
                 "means OpenAIProvider::sendRequest short-circuited before "
                 "even building the request.");

        for (int i = 0; i < errorSpy.count(); ++i) {
            const QString msg = errorSpy[i][0].toString();
            QVERIFY2(!msg.contains(QStringLiteral("API key not configured")),
                     qPrintable(QStringLiteral("LlamaCppRemoteProvider with empty key emitted "
                                               "the OpenAI-style 'API key not configured' "
                                               "error; got: '%1'")
                                    .arg(msg)));
        }
    }

    void test_openAI_emptyKey_stillEmitsApiKeyError() {
        OpenAIProvider provider(*m_http);
        provider.setBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server->port()));

        QSignalSpy errorSpy(&provider, &ILLMProvider::requestError);
        QSignalSpy finishedSpy(&provider, &ILLMProvider::requestFinished);

        provider.sendRequest(buildMinimalRequest());

        QTest::qWait(100);

        QCOMPARE(finishedSpy.count(), 0);
        QVERIFY2(errorSpy.count() >= 1,
                 "Expected OpenAIProvider with empty key to emit "
                 "requestError; got none.");

        const QString msg = errorSpy[0][0].toString();
        QVERIFY2(
            msg.contains(QStringLiteral("API key not configured")),
            qPrintable(
                QStringLiteral("Expected 'API key not configured' error; got: '%1'").arg(msg)));

        QCOMPARE(m_server->connectionCount(), 0);
    }
};

QTEST_MAIN(TestLlamaCppRemoteEmptyKeyDispatch)
#include "test-llamacpp-remote-empty-key-dispatch.moc"

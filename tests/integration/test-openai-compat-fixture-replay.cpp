// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/openai-compat-provider.h"
#include "../../backend/utils/http-client.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>


class MockReplayServer : public QObject {
    Q_OBJECT
  public:
    explicit MockReplayServer(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, &MockReplayServer::onNewConnection);
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }

    quint16 port() const { return m_server.serverPort(); }
    void setResponse(const QByteArray& r) { m_response = r; }

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


namespace {

QString fixtureDirFor(const QString& stack) {
    const QString srcRel = QStringLiteral("tests/integration/fixtures/openai-compat/%1").arg(stack);
    QDir d(QDir::currentPath());
    for (int i = 0; i < 6; ++i) {
        const QString candidate = d.absoluteFilePath(srcRel);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
        if (!d.cdUp()) {
            break;
        }
    }
    return srcRel;
}

QByteArray asJsonResponse(const QByteArray& body) {
    QByteArray r;
    r += "HTTP/1.1 200 OK\r\n";
    r += "Content-Type: application/json\r\n";
    r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    r += "Connection: close\r\n";
    r += "\r\n";
    r += body;
    return r;
}

QByteArray asSseResponse(const QByteArray& body) {
    QByteArray r;
    r += "HTTP/1.1 200 OK\r\n";
    r += "Content-Type: text/event-stream\r\n";
    r += "Cache-Control: no-cache\r\n";
    r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    r += "Connection: close\r\n";
    r += "\r\n";
    r += body;
    return r;
}

}  // namespace


class TestOpenAICompatFixtureReplay : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<OpenAICompatProvider> m_provider;
    std::unique_ptr<MockReplayServer> m_server;

    QStringList m_skippedStacks;
    QStringList m_replayedStacks;

  private slots:

    void init() {
        m_server = std::make_unique<MockReplayServer>();
        QVERIFY(m_server->listen());

        m_http = std::make_unique<HttpClient>();
        m_provider = std::make_unique<OpenAICompatProvider>(*m_http);
        m_provider->setSlug(QStringLiteral("replay-test"));
        m_provider->setDisplayName(QStringLiteral("Replay"));
        m_provider->setBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server->port()));
    }

    void cleanup() {
        m_provider.reset();
        m_http.reset();
        m_server.reset();
    }

    void cleanupTestCase() {
        if (!m_skippedStacks.isEmpty()) {
            qInfo("Fixture-replay summary — SKIPPED stacks (no fixtures yet): %s",
                  qPrintable(m_skippedStacks.join(QStringLiteral(", "))));
        }
        if (!m_replayedStacks.isEmpty()) {
            qInfo("Fixture-replay summary — REPLAYED stacks: %s",
                  qPrintable(m_replayedStacks.join(QStringLiteral(", "))));
        }
    }


    QStringList replayModelsList(const QString& stack) {
        const QString path = fixtureDirFor(stack) + QStringLiteral("/models-list.json");
        if (!QFileInfo::exists(path)) {
            return {};
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning("Could not open %s", qPrintable(path));
            return {};
        }
        const QByteArray body = f.readAll();
        m_server->setResponse(asJsonResponse(body));

        QSignalSpy spy(m_provider.get(), &ILLMProvider::modelsRefreshed);
        m_provider->refreshModels();
        if (spy.count() == 0 && !spy.wait(3000)) {
            qWarning("[%s] modelsRefreshed never fired", qPrintable(stack));
            return {};
        }
        const QStringList models = spy.takeFirst().at(0).toStringList();
        if (models.isEmpty()) {
            qWarning("[%s] models-list.json parsed but empty", qPrintable(stack));
        }
        return models;
    }

    void
    replayChatStreaming(const QString& stack, const QString& scenario, const QString& modelId) {
        const QString path = fixtureDirFor(stack) + QStringLiteral("/") + scenario;
        if (!QFileInfo::exists(path)) {
            return;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning("Could not open %s", qPrintable(path));
            return;
        }
        const QByteArray body = f.readAll();
        m_server->setResponse(asSseResponse(body));

        QSignalSpy chunkSpy(m_provider.get(), &ILLMProvider::chunkReceived);
        QSignalSpy finishedSpy(m_provider.get(), &ILLMProvider::requestFinished);
        QSignalSpy errorSpy(m_provider.get(), &ILLMProvider::requestError);

        LlmRequest req;
        req.config.modelName = modelId;
        req.config.stream = true;
        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("test");
        req.messages.append(msg);

        m_provider->sendRequest(req);
        if (finishedSpy.count() == 0 && !finishedSpy.wait(5000)) {
            qWarning(
                "[%s] %s: requestFinished never fired", qPrintable(stack), qPrintable(scenario));
            return;
        }
        QVERIFY2(errorSpy.count() == 0,
                 qPrintable(QStringLiteral("[%1] %2: unexpected requestError emission")
                                .arg(stack)
                                .arg(scenario)));
        QVERIFY2(chunkSpy.count() > 0,
                 qPrintable(QStringLiteral("[%1] %2: expected chunks but none received")
                                .arg(stack)
                                .arg(scenario)));
        const QString reason = finishedSpy.takeFirst().at(0).toString();
        QVERIFY2(
            !reason.isEmpty(),
            qPrintable(QStringLiteral("[%1] %2: empty finish_reason").arg(stack).arg(scenario)));
    }

    void replayStack(const QString& stack) {
        const QString dir = fixtureDirFor(stack);
        if (!QFileInfo::exists(dir + QStringLiteral("/models-list.json"))) {
            m_skippedStacks.append(stack);
            QSKIP("No models-list.json captured for this stack yet — "
                  "see SETUP.md in the fixture directory.");
        }
        const QStringList models = replayModelsList(stack);
        QVERIFY2(!models.isEmpty(),
                 qPrintable(
                     QStringLiteral("[%1] models-list.json captured but parsed empty").arg(stack)));
        const QString modelId = models.first();
        replayChatStreaming(stack, QStringLiteral("chat-streaming-stop.sse"), modelId);
        replayChatStreaming(stack, QStringLiteral("chat-streaming-tool-call.sse"), modelId);
        replayChatStreaming(stack, QStringLiteral("chat-streaming-empty.sse"), modelId);
        m_replayedStacks.append(stack);
    }


    void test_vllm() { replayStack(QStringLiteral("vllm")); }

    void test_lm_studio() { replayStack(QStringLiteral("lm-studio")); }

    void test_llamacpp_server() { replayStack(QStringLiteral("llamacpp-server")); }

    void test_ollama_openai_compat() { replayStack(QStringLiteral("ollama-openai-compat")); }
};

QTEST_MAIN(TestOpenAICompatFixtureReplay)
#include "test-openai-compat-fixture-replay.moc"

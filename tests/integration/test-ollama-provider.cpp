// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/api/ollama-provider.h"
#include "../../backend/utils/http-client.h"

#include <QTest>
#include <QTimer>

#include <QEventLoop>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QString>

static constexpr int kOllamaTimeoutMs = 60000;

static bool waitForSignal(QSignalSpy& spy, int msec = kOllamaTimeoutMs) {
    if (!spy.isEmpty()) {
        return true;
    }
    return spy.wait(msec);
}


class TestOllamaProvider : public QObject {
    Q_OBJECT

  private:
    QString m_ollamaHost;
    QString m_testModel;

    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<OllamaProvider> m_provider;

  private slots:

    void initTestCase() {
        m_ollamaHost = QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("OLLAMA_TEST_HOST"), QString());
        if (m_ollamaHost.isEmpty()) {
            QSKIP("OLLAMA_TEST_HOST not set — skipping Ollama integration tests");
        }

        m_testModel = QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("OLLAMA_TEST_MODEL"), QStringLiteral("llama3.2:1b"));

        qInfo() << "Running Ollama integration tests against" << m_ollamaHost << "with model"
                << m_testModel;
    }

    void init() {
        m_http = std::make_unique<HttpClient>();
        m_provider = std::make_unique<OllamaProvider>(*m_http);
        m_provider->setBaseUrl(m_ollamaHost);
    }

    void cleanup() {
        m_provider.reset();
        m_http.reset();
    }

    void test_refreshModels_returnsNonEmptyList() {
        QSignalSpy spy(m_provider.get(), &ILLMProvider::modelsRefreshed);
        m_provider->refreshModels();

        QVERIFY(waitForSignal(spy));
        QVERIFY(!spy.isEmpty());

        const QStringList models = spy[0][0].toStringList();
        QVERIFY(!models.isEmpty());
        qInfo() << "Ollama models found:" << models;
    }

    void test_streamingRequest_receivesChunksAndFinishes() {
        QSignalSpy chunkSpy(m_provider.get(), &ILLMProvider::chunkReceived);
        QSignalSpy finishedSpy(m_provider.get(), &ILLMProvider::requestFinished);
        QSignalSpy errorSpy(m_provider.get(), &ILLMProvider::requestError);

        LlmRequest req;
        req.config.modelName = m_testModel;
        req.config.stream = true;
        req.config.maxTokens = 50;
        req.config.temperature = 0.0;

        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Say 'Hello' and nothing else.");
        req.messages.append(msg);

        m_provider->sendRequest(req);

        QVERIFY(waitForSignal(finishedSpy));
        QVERIFY(errorSpy.isEmpty());
        QVERIFY(!chunkSpy.isEmpty());

        QString fullText;
        for (int i = 0; i < chunkSpy.count(); ++i) {
            const LlmChunk chunk = chunkSpy[i][0].value<LlmChunk>();
            fullText += chunk.delta;
        }
        qInfo() << "Ollama response:" << fullText;
        QVERIFY(!fullText.isEmpty());

        QCOMPARE(finishedSpy[0][0].toString(), QStringLiteral("stop"));
    }

    void test_systemPrompt_includedInRequest() {
        QSignalSpy chunkSpy(m_provider.get(), &ILLMProvider::chunkReceived);
        QSignalSpy finishedSpy(m_provider.get(), &ILLMProvider::requestFinished);
        QSignalSpy errorSpy(m_provider.get(), &ILLMProvider::requestError);

        LlmRequest req;
        req.config.modelName = m_testModel;
        req.config.stream = true;
        req.config.maxTokens = 30;
        req.config.temperature = 0.0;
        req.systemPrompt = QStringLiteral("Always respond with exactly: SYSTEM_OK");

        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Confirm the system prompt works.");
        req.messages.append(msg);

        m_provider->sendRequest(req);
        QVERIFY(waitForSignal(finishedSpy));
        QVERIFY(errorSpy.isEmpty());
        QVERIFY(!chunkSpy.isEmpty());
    }

    void test_cancel_stopsStream() {
        QSignalSpy chunkSpy(m_provider.get(), &ILLMProvider::chunkReceived);
        QSignalSpy errorSpy(m_provider.get(), &ILLMProvider::requestError);

        LlmRequest req;
        req.config.modelName = m_testModel;
        req.config.stream = true;
        req.config.maxTokens = 500;
        req.config.temperature = 0.5;

        LlmMessage msg;
        msg.role = QStringLiteral("user");
        msg.content = QStringLiteral("Write a long story about a robot.");
        req.messages.append(msg);

        m_provider->sendRequest(req);

        QEventLoop loop;
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
        loop.exec();

        m_provider->cancelRequest();

        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();

        QVERIFY(errorSpy.isEmpty());
        qInfo() << "Chunks received before cancel:" << chunkSpy.count();
    }
};

QTEST_MAIN(TestOllamaProvider)
#include "test-ollama-provider.moc"

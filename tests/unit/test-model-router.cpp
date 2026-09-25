// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/services/model-router.h"

#include <QTest>

#include <memory>
#include <QSignalSpy>
#include <QString>
#include <QStringList>


class MockProvider : public ILLMProvider {
    Q_OBJECT
  public:
    explicit MockProvider(const QString& id, QObject* parent = nullptr)
        : ILLMProvider(parent), m_id(id) {
        m_models = {id + QStringLiteral("-model-a"), id + QStringLiteral("-model-b")};
    }

    QString providerId() const override { return m_id; }
    QString displayName() const override { return m_id; }
    QStringList availableModels() override { return m_models; }
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }

    void refreshModels() override { emit modelsRefreshed(m_models); }

    void sendRequest(const LlmRequest& req) override {
        ++m_sendCallCount;
        m_lastRequest = req;

        if (m_emitChunk) {
            LlmChunk c;
            c.delta = QStringLiteral("hello");
            emit chunkReceived(c);
            emit requestFinished(QStringLiteral("stop"), 5);
        }
    }

    void cancelRequest() override { ++m_cancelCallCount; }

    void setEmitChunk(bool shouldEmit) { m_emitChunk = shouldEmit; }
    int sendCallCount() const { return m_sendCallCount; }
    int cancelCallCount() const { return m_cancelCallCount; }
    const LlmRequest& lastRequest() const { return m_lastRequest; }

    void emitChunkSignal(const LlmChunk& chunk) { emit chunkReceived(chunk); }
    void emitFinishedSignal(const QString& r, int t) { emit requestFinished(r, t); }
    void emitErrorSignal(const QString& m) { emit requestError(m); }

  private:
    QString m_id;
    QStringList m_models;
    int m_sendCallCount = 0;
    int m_cancelCallCount = 0;
    bool m_emitChunk = false;
    LlmRequest m_lastRequest;
};


class TestModelRouter : public QObject {
    Q_OBJECT

  private slots:
    void init() { m_router = std::make_unique<ModelRouter>(); }

    void cleanup() { m_router.reset(); }

    void test_registerProvider_appearsInList() {
        m_router->registerProvider(std::make_unique<MockProvider>(QStringLiteral("ollama")));
        m_router->registerProvider(std::make_unique<MockProvider>(QStringLiteral("openai")));

        const QStringList ids = m_router->registeredProviderIds();
        QVERIFY(ids.contains(QStringLiteral("ollama")));
        QVERIFY(ids.contains(QStringLiteral("openai")));
        QCOMPARE(ids.size(), 2);
    }

    void test_setActiveProvider_returnsCorrectInstance() {
        auto* rawOllama = new MockProvider(QStringLiteral("ollama"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(rawOllama));
        m_router->registerProvider(std::make_unique<MockProvider>(QStringLiteral("openai")));

        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        QCOMPARE(m_router->activeProvider(), rawOllama);
        QCOMPARE(m_router->activeProviderId(), QStringLiteral("ollama"));
        QCOMPARE(m_router->activeModelName(), QStringLiteral("ollama-model-a"));
    }

    void test_routeRequest_callsSendRequestOnActiveProvider() {
        auto* mockPtr = new MockProvider(QStringLiteral("ollama"));
        mockPtr->setEmitChunk(false);
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(mockPtr));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        LlmRequest req;
        req.config.modelName = QStringLiteral("ollama-model-a");
        req.systemPrompt = QStringLiteral("Be helpful.");
        m_router->route(req);

        QCOMPARE(mockPtr->sendCallCount(), 1);
        QCOMPARE(mockPtr->lastRequest().config.modelName, QStringLiteral("ollama-model-a"));
    }

    void test_signalForwarding_chunkReceived() {
        auto* mockPtr = new MockProvider(QStringLiteral("openai"));
        mockPtr->setEmitChunk(true);
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(mockPtr));
        m_router->setActiveProvider(QStringLiteral("openai"), QStringLiteral("openai-model-a"));

        QSignalSpy chunkSpy(m_router.get(), &ModelRouter::chunkReceived);
        QSignalSpy finishedSpy(m_router.get(), &ModelRouter::requestFinished);

        LlmRequest req;
        req.config.modelName = QStringLiteral("openai-model-a");
        m_router->route(req);

        QCOMPARE(chunkSpy.count(), 1);
        QCOMPARE(finishedSpy.count(), 1);

        const LlmChunk& receivedChunk = chunkSpy[0][1].value<LlmChunk>();
        QCOMPARE(receivedChunk.delta, QStringLiteral("hello"));

        QCOMPARE(finishedSpy[0][1].toString(), QStringLiteral("stop"));
        QCOMPARE(finishedSpy[0][2].toInt(), 5);
    }

    void test_cancelCurrent_callsCancelOnActiveProvider() {
        auto* mockPtr = new MockProvider(QStringLiteral("ollama"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(mockPtr));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        m_router->cancelCurrent();

        QCOMPARE(mockPtr->cancelCallCount(), 1);
    }

    void test_providerSwitch_signalRoutedToNewProvider() {
        auto* ollamaPtr = new MockProvider(QStringLiteral("ollama"));
        auto* openaiPtr = new MockProvider(QStringLiteral("openai"));

        m_router->registerProvider(std::unique_ptr<ILLMProvider>(ollamaPtr));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(openaiPtr));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        QSignalSpy chunkSpy(m_router.get(), &ModelRouter::chunkReceived);

        LlmRequest req;
        req.requestId = 1;
        req.config.providerId = QStringLiteral("openai");
        m_router->route(req);

        ollamaPtr->emitChunkSignal(LlmChunk{QStringLiteral("from-ollama"), {}, {}, 0});
        QCOMPARE(chunkSpy.count(), 0);

        openaiPtr->emitChunkSignal(LlmChunk{QStringLiteral("from-openai"), {}, {}, 0});
        QCOMPARE(chunkSpy.count(), 1);
    }

    void test_setActiveProvider_unknownId_emitsRequestError() {
        QSignalSpy errorSpy(m_router.get(), &ModelRouter::requestError);

        m_router->setActiveProvider(QStringLiteral("nonexistent"), QStringLiteral("some-model"));

        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy[0][1].toString().contains(QStringLiteral("nonexistent")));
        QVERIFY(m_router->activeProvider() == nullptr);
    }

    void test_allAvailableModels_aggregatesAcrossProviders() {
        m_router->registerProvider(std::make_unique<MockProvider>(QStringLiteral("ollama")));
        m_router->registerProvider(std::make_unique<MockProvider>(QStringLiteral("openai")));

        const auto models = m_router->allAvailableModels();
        QCOMPARE(static_cast<int>(models.size()), 2);
        QVERIFY(models.count(QStringLiteral("ollama")) > 0);
        QVERIFY(models.count(QStringLiteral("openai")) > 0);
        QCOMPARE(models.at(QStringLiteral("ollama")).size(), 2);
    }

  private:
    std::unique_ptr<ModelRouter> m_router;
};

QTEST_MAIN(TestModelRouter)
#include "test-model-router.moc"

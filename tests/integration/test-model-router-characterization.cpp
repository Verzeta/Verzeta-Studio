// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/services/model-router.h"

#include <QTest>

#include <memory>
#include <QSignalSpy>
#include <QString>


class CharMockProvider : public ILLMProvider {
    Q_OBJECT
  public:
    explicit CharMockProvider(const QString& id, QObject* parent = nullptr)
        : ILLMProvider(parent), m_id(id) {}

    QString providerId() const override { return m_id; }
    QString displayName() const override { return m_id; }
    QStringList availableModels() override { return {m_id + QStringLiteral("-model")}; }
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }
    void refreshModels() override { emit modelsRefreshed(availableModels()); }

    void sendRequest(const LlmRequest& req) override {
        ++m_sendCallCount;
        m_lastRequest = req;
    }

    void cancelRequest() override { ++m_cancelCallCount; }

    void emitChunk(const QString& delta) {
        LlmChunk c;
        c.delta = delta;
        emit chunkReceived(c);
    }
    void emitFinished(const QString& reason, int tokens) { emit requestFinished(reason, tokens); }
    void emitError(const QString& message) { emit requestError(message); }

    int sendCallCount() const { return m_sendCallCount; }
    int cancelCallCount() const { return m_cancelCallCount; }
    const LlmRequest& lastRequest() const { return m_lastRequest; }

  private:
    QString m_id;
    int m_sendCallCount = 0;
    int m_cancelCallCount = 0;
    LlmRequest m_lastRequest;
};


class TestModelRouterCharacterization : public QObject {
    Q_OBJECT

  private slots:
    void init() { m_router = std::make_unique<ModelRouter>(); }

    void cleanup() { m_router.reset(); }


    void test_P1_route_dispatchesToActiveProvider() {
        auto* fg = new CharMockProvider(QStringLiteral("ollama"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(fg));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        LlmRequest req;
        req.requestId = 1;
        m_router->route(req);

        QCOMPARE(fg->sendCallCount(), 1);
        QCOMPARE(fg->lastRequest().requestId, quint64(1));
    }

    void test_P2_route_doesNotDispatchToInactiveProviders() {
        auto* ollama = new CharMockProvider(QStringLiteral("ollama"));
        auto* openai = new CharMockProvider(QStringLiteral("openai"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(ollama));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(openai));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        LlmRequest req;
        req.requestId = 2;
        m_router->route(req);

        QCOMPARE(ollama->sendCallCount(), 1);
        QCOMPARE(openai->sendCallCount(), 0);
    }

    void test_P3_route_noActiveProvider_emitsErrorWithStampedId() {
        auto* fg = new CharMockProvider(QStringLiteral("ollama"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(fg));

        QSignalSpy errorSpy(m_router.get(), &ModelRouter::requestError);

        LlmRequest req;
        req.requestId = 77;
        m_router->route(req);

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(errorSpy[0][0].value<quint64>(), quint64(77));
        QVERIFY(errorSpy[0][1].toString().contains(QStringLiteral("No LLM provider"),
                                                   Qt::CaseInsensitive));
        QCOMPARE(fg->sendCallCount(), 0);
    }

    void test_P4_route_doesNotSwapActiveProvider_butRespectsConfigProviderId() {
        auto* ollama = new CharMockProvider(QStringLiteral("ollama"));
        auto* openai = new CharMockProvider(QStringLiteral("openai"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(ollama));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(openai));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        LlmRequest req;
        req.requestId = 3;
        req.config.providerId = QStringLiteral("openai");
        m_router->route(req);

        QCOMPARE(m_router->activeProviderId(), QStringLiteral("ollama"));
        QCOMPARE(m_router->activeModelName(), QStringLiteral("ollama-model"));
        QCOMPARE(m_router->activeProvider(), static_cast<ILLMProvider*>(ollama));

        QCOMPARE(openai->sendCallCount(), 1);
        QCOMPARE(ollama->sendCallCount(), 0);
    }

    void test_P5_singleRequest_relaysCarryItsId() {
        auto* fg = new CharMockProvider(QStringLiteral("ollama"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(fg));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        QSignalSpy chunkSpy(m_router.get(), &ModelRouter::chunkReceived);
        QSignalSpy finishedSpy(m_router.get(), &ModelRouter::requestFinished);

        LlmRequest req;
        req.requestId = 555;
        m_router->route(req);
        fg->emitChunk(QStringLiteral("hello"));
        fg->emitFinished(QStringLiteral("stop"), 9);

        QCOMPARE(chunkSpy.count(), 1);
        QCOMPARE(chunkSpy[0][0].value<quint64>(), quint64(555));
        QCOMPARE(chunkSpy[0][1].value<LlmChunk>().delta, QStringLiteral("hello"));

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy[0][0].value<quint64>(), quint64(555));
        QCOMPARE(finishedSpy[0][1].toString(), QStringLiteral("stop"));
        QCOMPARE(finishedSpy[0][2].toInt(), 9);
    }

    void test_P6_providerError_relayedWithCurrentId() {
        auto* fg = new CharMockProvider(QStringLiteral("ollama"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(fg));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        QSignalSpy errorSpy(m_router.get(), &ModelRouter::requestError);

        LlmRequest req;
        req.requestId = 888;
        m_router->route(req);
        fg->emitError(QStringLiteral("upstream 500"));

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(errorSpy[0][0].value<quint64>(), quint64(888));
        QCOMPARE(errorSpy[0][1].toString(), QStringLiteral("upstream 500"));
    }

    void test_P7_cancelCurrent_cancelsActiveProviderOnly() {
        auto* ollama = new CharMockProvider(QStringLiteral("ollama"));
        auto* openai = new CharMockProvider(QStringLiteral("openai"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(ollama));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(openai));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        m_router->cancelCurrent();

        QCOMPARE(ollama->cancelCallCount(), 1);
        QCOMPARE(openai->cancelCallCount(), 0);
    }

    void test_P8_providerIsolation_onlyRouteTargetIsRelayed() {
        auto* ollama = new CharMockProvider(QStringLiteral("ollama"));
        auto* openai = new CharMockProvider(QStringLiteral("openai"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(ollama));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(openai));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        QSignalSpy chunkSpy(m_router.get(), &ModelRouter::chunkReceived);

        LlmRequest req;
        req.requestId = 8;
        req.config.providerId = QStringLiteral("ollama");
        m_router->route(req);

        openai->emitChunk(QStringLiteral("from-openai"));
        QCOMPARE(chunkSpy.count(), 0);

        ollama->emitChunk(QStringLiteral("from-ollama"));
        QCOMPARE(chunkSpy.count(), 1);
        QCOMPARE(chunkSpy[0][0].value<quint64>(), quint64(8));
        QCOMPARE(chunkSpy[0][1].value<LlmChunk>().delta, QStringLiteral("from-ollama"));
    }


    void test_C1_perRequestRelay_capturesOwnRequestId() {
        auto* provA = new CharMockProvider(QStringLiteral("provider-a"));
        auto* provB = new CharMockProvider(QStringLiteral("provider-b"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(provA));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(provB));
        m_router->setActiveProvider(QStringLiteral("provider-a"), QStringLiteral("model-a"));

        QSignalSpy chunkSpy(m_router.get(), &ModelRouter::chunkReceived);

        LlmRequest reqA;
        reqA.requestId = 10;
        reqA.config.providerId = QStringLiteral("provider-a");
        m_router->route(reqA);
        provA->emitChunk(QStringLiteral("from-a"));
        provA->emitFinished(QStringLiteral("stop"), 1);

        LlmRequest reqB;
        reqB.requestId = 20;
        reqB.config.providerId = QStringLiteral("provider-b");
        m_router->route(reqB);
        provB->emitChunk(QStringLiteral("from-b"));

        QCOMPARE(chunkSpy.count(), 2);
        QCOMPARE(chunkSpy[0][0].value<quint64>(), quint64(10));
        QCOMPARE(chunkSpy[0][1].value<LlmChunk>().delta, QStringLiteral("from-a"));
        QCOMPARE(chunkSpy[1][0].value<quint64>(), quint64(20));
        QCOMPARE(chunkSpy[1][1].value<LlmChunk>().delta, QStringLiteral("from-b"));
    }

    void test_C2_relayTornDownOnTerminal_stragglerDropped() {
        auto* fg = new CharMockProvider(QStringLiteral("ollama"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(fg));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        QSignalSpy chunkSpy(m_router.get(), &ModelRouter::chunkReceived);
        QSignalSpy finishedSpy(m_router.get(), &ModelRouter::requestFinished);

        LlmRequest req;
        req.requestId = 42;
        m_router->route(req);
        fg->emitFinished(QStringLiteral("stop"), 3);

        fg->emitChunk(QStringLiteral("straggler"));

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy[0][0].value<quint64>(), quint64(42));
        QCOMPARE(chunkSpy.count(), 0);
    }

    void test_C3_route_unregisteredConfigProviderId_emitsError() {
        auto* fg = new CharMockProvider(QStringLiteral("ollama"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(fg));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model"));

        QSignalSpy errorSpy(m_router.get(), &ModelRouter::requestError);

        LlmRequest req;
        req.requestId = 909;
        req.config.providerId = QStringLiteral("ghost-provider");
        m_router->route(req);

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(errorSpy[0][0].value<quint64>(), quint64(909));
        QVERIFY(errorSpy[0][1].toString().contains(QStringLiteral("ghost-provider")));
        QCOMPARE(fg->sendCallCount(), 0);
    }


    void test_P41_routesByConfigNotActive() {
        auto* provY = new CharMockProvider(QStringLiteral("provider-y"));
        auto* provX = new CharMockProvider(QStringLiteral("provider-x"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(provY));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(provX));

        m_router->setActiveProvider(QStringLiteral("provider-y"),
                                    QStringLiteral("provider-y-model"));

        LlmRequest reqB;
        reqB.requestId = 100;
        reqB.config.providerId = QStringLiteral("provider-x");
        m_router->route(reqB);

        QCOMPARE(provX->sendCallCount(), 1);
        QCOMPARE(provY->sendCallCount(), 0);
        QCOMPARE(m_router->activeProviderId(), QStringLiteral("provider-y"));
    }

    void test_P41_activeSwitchDoesNotRetargetStampedRequest() {
        auto* provY = new CharMockProvider(QStringLiteral("provider-y"));
        auto* provX = new CharMockProvider(QStringLiteral("provider-x"));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(provY));
        m_router->registerProvider(std::unique_ptr<ILLMProvider>(provX));

        m_router->setActiveProvider(QStringLiteral("provider-y"),
                                    QStringLiteral("provider-y-model"));
        m_router->setActiveProvider(QStringLiteral("provider-x"),
                                    QStringLiteral("provider-x-model"));

        LlmRequest reqA;
        reqA.requestId = 101;
        reqA.config.providerId = QStringLiteral("provider-y");
        m_router->route(reqA);

        QCOMPARE(provY->sendCallCount(), 1);
        QCOMPARE(provX->sendCallCount(), 0);
    }

  private:
    std::unique_ptr<ModelRouter> m_router;
};

QTEST_MAIN(TestModelRouterCharacterization)
#include "test-model-router-characterization.moc"

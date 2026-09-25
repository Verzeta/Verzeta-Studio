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
    explicit MockProvider(const QString& id, const QString& tag, QObject* parent = nullptr)
        : ILLMProvider(parent), m_id(id), m_tag(tag) {
        m_models = {id + QStringLiteral("-model-a"), id + QStringLiteral("-model-b")};
    }

    QString providerId() const override { return m_id; }
    QString displayName() const override {
        return m_id + QStringLiteral(" (") + m_tag + QStringLiteral(")");
    }
    QStringList availableModels() override { return m_models; }
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }

    void refreshModels() override { emit modelsRefreshed(m_models); }

    void sendRequest(const LlmRequest& req) override {
        ++m_sendCallCount;
        m_lastRequest = req;
    }

    void cancelRequest() override { ++m_cancelCallCount; }

    int sendCallCount() const { return m_sendCallCount; }
    int cancelCallCount() const { return m_cancelCallCount; }
    const LlmRequest& lastRequest() const { return m_lastRequest; }
    QString tag() const { return m_tag; }

    void emitChunkSignal(const LlmChunk& chunk) { emit chunkReceived(chunk); }
    void emitFinishedSignal(const QString& r, int t) { emit requestFinished(r, t); }
    void emitErrorSignal(const QString& m) { emit requestError(m); }

  private:
    QString m_id;
    QString m_tag;
    QStringList m_models;
    int m_sendCallCount = 0;
    int m_cancelCallCount = 0;
    LlmRequest m_lastRequest;
};


class TestModelRouterDualSlot : public QObject {
    Q_OBJECT

  private slots:
    void init() { m_router = std::make_unique<ModelRouter>(); }

    void cleanup() {
        m_router.reset();
        m_fgPtr = nullptr;
        m_bgPtr = nullptr;
    }

    void test_registerBackgroundProvider_isolatedFromForeground() {
        auto bg = std::make_unique<MockProvider>(QStringLiteral("ollama"), QStringLiteral("bg"));
        auto* bgRaw = bg.get();
        m_router->registerBackgroundProvider(std::move(bg));

        QCOMPARE(m_router->bgProviderForId(QStringLiteral("ollama")), bgRaw);
        QVERIFY(m_router->providerForId(QStringLiteral("ollama")) == nullptr);
        QVERIFY(m_router->activeProvider() == nullptr);
        QVERIFY(m_router->bgActiveProvider() == nullptr);
    }

    void test_setActiveProvider_mirrorsToBackground() {
        registerPair(QStringLiteral("ollama"));
        QSignalSpy bgChangedSpy(m_router.get(), &ModelRouter::activeBackgroundProviderChanged);

        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        QCOMPARE(m_router->activeProvider(), m_fgPtr);
        QCOMPARE(m_router->bgActiveProvider(), m_bgPtr);
        QCOMPARE(bgChangedSpy.count(), 1);
        QCOMPARE(bgChangedSpy[0][0].toString(), QStringLiteral("ollama"));
    }

    void test_setActiveProvider_clearsBackgroundWhenNoCounterpart() {
        registerPair(QStringLiteral("ollama"));
        auto fgOpenai =
            std::make_unique<MockProvider>(QStringLiteral("openai"), QStringLiteral("fg"));
        m_router->registerProvider(std::move(fgOpenai));

        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("m"));
        QCOMPARE(m_router->bgActiveProvider(), m_bgPtr);

        QSignalSpy bgChangedSpy(m_router.get(), &ModelRouter::activeBackgroundProviderChanged);
        m_router->setActiveProvider(QStringLiteral("openai"), QStringLiteral("m"));

        QVERIFY(m_router->bgActiveProvider() == nullptr);
        QCOMPARE(bgChangedSpy.count(), 1);
        QCOMPARE(bgChangedSpy[0][0].toString(), QString());
    }

    void test_routeBackground_dispatchesOnlyToBackgroundProvider() {
        registerPair(QStringLiteral("ollama"));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        LlmRequest req;
        req.requestId = 42;
        req.config.modelName = QStringLiteral("ollama-model-a");

        m_router->routeBackground(req);

        QCOMPARE(m_fgPtr->sendCallCount(), 0);
        QCOMPARE(m_bgPtr->sendCallCount(), 1);
        QCOMPARE(m_bgPtr->lastRequest().requestId, quint64(42));
    }

    void test_route_dispatchesOnlyToForegroundProvider() {
        registerPair(QStringLiteral("ollama"));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        LlmRequest req;
        req.requestId = 7;
        req.config.modelName = QStringLiteral("ollama-model-a");

        m_router->route(req);

        QCOMPARE(m_fgPtr->sendCallCount(), 1);
        QCOMPARE(m_fgPtr->lastRequest().requestId, quint64(7));
        QCOMPARE(m_bgPtr->sendCallCount(), 0);
    }

    void test_concurrentFgAndBgRoutes_independentSignalChannels() {
        registerPair(QStringLiteral("ollama"));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        QSignalSpy fgChunkSpy(m_router.get(), &ModelRouter::chunkReceived);
        QSignalSpy fgFinishedSpy(m_router.get(), &ModelRouter::requestFinished);
        QSignalSpy bgChunkSpy(m_router.get(), &ModelRouter::backgroundChunkReceived);
        QSignalSpy bgFinishedSpy(m_router.get(), &ModelRouter::backgroundRequestFinished);

        LlmRequest fgReq;
        fgReq.requestId = 100;
        LlmRequest bgReq;
        bgReq.requestId = 200;

        m_router->route(fgReq);
        m_router->routeBackground(bgReq);

        m_bgPtr->emitChunkSignal({QStringLiteral("bg-chunk"), {}, {}, 0});
        m_bgPtr->emitFinishedSignal(QStringLiteral("stop"), 5);

        m_fgPtr->emitChunkSignal({QStringLiteral("fg-chunk"), {}, {}, 0});
        m_fgPtr->emitFinishedSignal(QStringLiteral("stop"), 11);

        QCOMPARE(fgChunkSpy.count(), 1);
        QCOMPARE(fgFinishedSpy.count(), 1);
        QCOMPARE(bgChunkSpy.count(), 1);
        QCOMPARE(bgFinishedSpy.count(), 1);

        QCOMPARE(fgChunkSpy[0][0].value<quint64>(), quint64(100));
        QCOMPARE(fgChunkSpy[0][1].value<LlmChunk>().delta, QStringLiteral("fg-chunk"));
        QCOMPARE(fgFinishedSpy[0][0].value<quint64>(), quint64(100));
        QCOMPARE(fgFinishedSpy[0][2].toInt(), 11);

        QCOMPARE(bgChunkSpy[0][0].value<quint64>(), quint64(200));
        QCOMPARE(bgChunkSpy[0][1].value<LlmChunk>().delta, QStringLiteral("bg-chunk"));
        QCOMPARE(bgFinishedSpy[0][0].value<quint64>(), quint64(200));
        QCOMPARE(bgFinishedSpy[0][2].toInt(), 5);
    }

    void test_cancelCurrent_cancelsForegroundOnly() {
        registerPair(QStringLiteral("ollama"));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        m_router->cancelCurrent();

        QCOMPARE(m_fgPtr->cancelCallCount(), 1);
        QCOMPARE(m_bgPtr->cancelCallCount(), 0);
    }

    void test_cancelBackground_cancelsBackgroundOnly() {
        registerPair(QStringLiteral("ollama"));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        m_router->cancelBackground();

        QCOMPARE(m_fgPtr->cancelCallCount(), 0);
        QCOMPARE(m_bgPtr->cancelCallCount(), 1);
    }

    void test_routeBackground_withoutBgActive_emitsBackgroundError() {
        m_router->registerProvider(
            std::make_unique<MockProvider>(QStringLiteral("ollama"), QStringLiteral("fg")));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        QSignalSpy fgErrorSpy(m_router.get(), &ModelRouter::requestError);
        QSignalSpy bgErrorSpy(m_router.get(), &ModelRouter::backgroundRequestError);

        LlmRequest req;
        req.requestId = 99;
        m_router->routeBackground(req);

        QCOMPARE(fgErrorSpy.count(), 0);
        QCOMPARE(bgErrorSpy.count(), 1);
        QCOMPARE(bgErrorSpy[0][0].value<quint64>(), quint64(99));
        QVERIFY(bgErrorSpy[0][1].toString().contains(QStringLiteral("background")));
    }

    void test_foregroundOnlyMode_byteIdenticalBehavior() {
        m_router->registerProvider(
            std::make_unique<MockProvider>(QStringLiteral("ollama"), QStringLiteral("fg")));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));
        auto* fgRaw = qobject_cast<MockProvider*>(m_router->activeProvider());
        QVERIFY(fgRaw != nullptr);

        QSignalSpy fgChunkSpy(m_router.get(), &ModelRouter::chunkReceived);
        QSignalSpy bgChunkSpy(m_router.get(), &ModelRouter::backgroundChunkReceived);

        LlmRequest req;
        req.requestId = 33;
        m_router->route(req);
        fgRaw->emitChunkSignal({QStringLiteral("only-fg"), {}, {}, 0});

        QCOMPARE(fgChunkSpy.count(), 1);
        QCOMPARE(fgChunkSpy[0][0].value<quint64>(), quint64(33));
        QCOMPARE(bgChunkSpy.count(), 0);
        QVERIFY(m_router->bgActiveProvider() == nullptr);
    }

    void test_routeBackground_respectsConfigProviderId() {
        registerPair(QStringLiteral("ollama"));
        auto bgOpenai =
            std::make_unique<MockProvider>(QStringLiteral("openai"), QStringLiteral("bg"));
        auto* bgOpenaiRaw = bgOpenai.get();
        m_router->registerBackgroundProvider(std::move(bgOpenai));
        m_router->setActiveProvider(QStringLiteral("ollama"), QStringLiteral("ollama-model-a"));

        LlmRequest req;
        req.requestId = 51;
        req.config.providerId = QStringLiteral("openai");
        m_router->routeBackground(req);

        QCOMPARE(bgOpenaiRaw->sendCallCount(), 1);
        QCOMPARE(m_bgPtr->sendCallCount(), 0);

        QSignalSpy bgChunkSpy(m_router.get(), &ModelRouter::backgroundChunkReceived);
        bgOpenaiRaw->emitChunkSignal({QStringLiteral("bg-override"), {}, {}, 0});
        QCOMPARE(bgChunkSpy.count(), 1);
        QCOMPARE(bgChunkSpy[0][0].value<quint64>(), quint64(51));
    }

  private:
    void registerPair(const QString& providerId) {
        auto fg = std::make_unique<MockProvider>(providerId, QStringLiteral("fg"));
        m_fgPtr = fg.get();
        m_router->registerProvider(std::move(fg));

        auto bg = std::make_unique<MockProvider>(providerId, QStringLiteral("bg"));
        m_bgPtr = bg.get();
        m_router->registerBackgroundProvider(std::move(bg));
    }

    std::unique_ptr<ModelRouter> m_router;
    MockProvider* m_fgPtr = nullptr;
    MockProvider* m_bgPtr = nullptr;
};

QTEST_MAIN(TestModelRouterDualSlot)
#include "test-model-router-dual-slot.moc"

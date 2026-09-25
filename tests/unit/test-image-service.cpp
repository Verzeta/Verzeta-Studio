// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/audio-service.h"
#include "services/file-service.h"
#include "services/image-service.h"
#include "services/model-router.h"
#include "workers/image-gen-worker.h"

#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <QDir>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>


class TestModelRouter : public ModelRouter {
    Q_OBJECT
  public:
    explicit TestModelRouter(QObject* parent = nullptr) : ModelRouter(parent) {}
};


class TestImageService : public QObject {
    Q_OBJECT

  private:
    FileService* m_fileService = nullptr;
    TestModelRouter* m_router = nullptr;
    ImageService* m_imageService = nullptr;

  private slots:
    void init() {
        QStandardPaths::setTestModeEnabled(true);
        m_fileService = new FileService(this);
        m_router = new TestModelRouter(this);
        m_imageService = new ImageService(*m_router, *m_fileService, this);
    }

    void cleanup() {
        delete m_imageService;
        m_imageService = nullptr;
        delete m_router;
        m_router = nullptr;
        delete m_fileService;
        m_fileService = nullptr;
    }


    void test_generateImageEmitsStarted() {
        QSignalSpy spy(m_imageService, &ImageService::generationStarted);
        ImageGenConfig cfg;
        cfg.apiKey = QStringLiteral("test-key");
        m_imageService->generateImage(QStringLiteral("conv-1"), QStringLiteral("a dog"), cfg);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("conv-1"));
    }

    void test_generateImageErrorWhenNoApiKey() {
        QSignalSpy spy(m_imageService, &ImageService::error);
        ImageGenConfig cfg;
        cfg.backend = QStringLiteral("openai");
        cfg.apiKey = QString();
        m_imageService->generateImage(QStringLiteral("conv-1"), QStringLiteral("a dog"), cfg);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toString() == QStringLiteral("conv-1"));
        QVERIFY(!spy.at(0).at(1).toString().isEmpty());
    }

    void test_generateImageLocalSDErrorWhenNoPath() {
        QSignalSpy spy(m_imageService, &ImageService::error);
        ImageGenConfig cfg;
        cfg.backend = QStringLiteral("local_sd");
        cfg.sdPath = QString();
        m_imageService->generateImage(QStringLiteral("conv-2"), QStringLiteral("a cat"), cfg);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("conv-2"));
    }

    void test_conversationImagesEmptyForUnknownConv() {
        const QStringList result =
            m_imageService->conversationImages(QStringLiteral("no-such-conv"));
        QVERIFY(result.isEmpty());
    }

    void test_imageGenConfigDefaults() {
        ImageGenConfig cfg;
        QCOMPARE(cfg.backend, QStringLiteral("openai"));
        QCOMPARE(cfg.size, QStringLiteral("1024x1024"));
        QCOMPARE(cfg.quality, QStringLiteral("standard"));
        QVERIFY(cfg.sdPath.isEmpty());
    }

    void test_generateImageStartedBeforeError() {
        QSignalSpy startedSpy(m_imageService, &ImageService::generationStarted);
        QSignalSpy errorSpy(m_imageService, &ImageService::error);
        ImageGenConfig cfg;
        m_imageService->generateImage(QStringLiteral("conv-3"), QStringLiteral("mountains"), cfg);
        QCOMPARE(startedSpy.count(), 1);
        QCOMPARE(errorSpy.count(), 1);
    }

    void test_analyzeImageEmitsErrorOnBadPath() {
        QSignalSpy spy(m_imageService, &ImageService::error);
        m_imageService->analyzeImage(QStringLiteral("conv-4"),
                                     QStringLiteral("/nonexistent/path/image.png"),
                                     QStringLiteral("What is this?"));
        QCOMPARE(spy.count(), 1);
    }

    void test_base64EncodingRoundtrip() {
        const QByteArray original = QByteArray::fromHex("89504e47");
        const QString encoded = QString::fromLatin1(original.toBase64());
        QVERIFY(!encoded.isEmpty());
        const QByteArray decoded = QByteArray::fromBase64(encoded.toLatin1());
        QCOMPARE(decoded, original);
    }

    void test_multipleGenerationsEmitCorrectConvId() {
        QSignalSpy spy(m_imageService, &ImageService::error);
        ImageGenConfig cfg;
        m_imageService->generateImage(QStringLiteral("conv-A"), QStringLiteral("sunset"), cfg);
        m_imageService->generateImage(QStringLiteral("conv-B"), QStringLiteral("sunrise"), cfg);
        QVERIFY(spy.count() >= 1);
    }


    void test_shapeHttp_emptyBaseUrl_emitsError() {
        for (const QString shape : {QStringLiteral("openai_images"),
                                    QStringLiteral("a1111"),
                                    QStringLiteral("openai_chat_image")}) {
            QSignalSpy started(m_imageService, &ImageService::generationStarted);
            QSignalSpy err(m_imageService, &ImageService::error);
            ImageGenConfig cfg;
            cfg.endpointShape = shape;
            cfg.baseUrl = QString();
            m_imageService->generateImage(QStringLiteral("conv-shape"), QStringLiteral("x"), cfg);
            QCOMPARE(started.count(), 1);
            QCOMPARE(err.count(), 1);
            QVERIFY(err.at(0).at(1).toString().contains(QStringLiteral("base URL")));
        }
    }

    void test_shapeLocalCli_emptyPath_emitsError() {
        QSignalSpy err(m_imageService, &ImageService::error);
        ImageGenConfig cfg;
        cfg.endpointShape = QStringLiteral("local_cli");
        cfg.sdPath = QString();
        m_imageService->generateImage(QStringLiteral("conv-cli"), QStringLiteral("x"), cfg);
        QCOMPARE(err.count(), 1);
    }

    void test_shapeUnsupported_emitsError() {
        QSignalSpy err(m_imageService, &ImageService::error);
        ImageGenConfig cfg;
        cfg.endpointShape = QStringLiteral("not-a-shape");
        m_imageService->generateImage(QStringLiteral("conv-bad"), QStringLiteral("x"), cfg);
        QCOMPARE(err.count(), 1);
        QVERIFY(err.at(0).at(1).toString().contains(QStringLiteral("shape")));
    }


    void test_joinEndpoint_baseOnly_appendsSuffix() {
        QCOMPARE(ImageGenWorker::joinEndpoint(QStringLiteral("https://openrouter.ai/api/v1"),
                                              QStringLiteral("/chat/completions")),
                 QStringLiteral("https://openrouter.ai/api/v1/chat/completions"));
    }

    void test_joinEndpoint_fullUrl_notDoubled() {
        QCOMPARE(ImageGenWorker::joinEndpoint(
                     QStringLiteral("https://openrouter.ai/api/v1/chat/completions"),
                     QStringLiteral("/chat/completions")),
                 QStringLiteral("https://openrouter.ai/api/v1/chat/completions"));
    }

    void test_joinEndpoint_trailingSlashes() {
        QCOMPARE(ImageGenWorker::joinEndpoint(QStringLiteral("https://openrouter.ai/api/v1/"),
                                              QStringLiteral("/chat/completions")),
                 QStringLiteral("https://openrouter.ai/api/v1/chat/completions"));
        QCOMPARE(ImageGenWorker::joinEndpoint(
                     QStringLiteral("https://openrouter.ai/api/v1/chat/completions/"),
                     QStringLiteral("/chat/completions")),
                 QStringLiteral("https://openrouter.ai/api/v1/chat/completions"));
    }

    void test_joinEndpoint_otherSuffixes() {
        QCOMPARE(ImageGenWorker::joinEndpoint(QStringLiteral("https://api.openai.com/v1"),
                                              QStringLiteral("/images/generations")),
                 QStringLiteral("https://api.openai.com/v1/images/generations"));
        QCOMPARE(ImageGenWorker::joinEndpoint(
                     QStringLiteral("https://api.openai.com/v1/images/generations"),
                     QStringLiteral("/images/generations")),
                 QStringLiteral("https://api.openai.com/v1/images/generations"));
        QCOMPARE(ImageGenWorker::joinEndpoint(QStringLiteral("http://box:7860/sdapi/v1/txt2img"),
                                              QStringLiteral("/sdapi/v1/txt2img")),
                 QStringLiteral("http://box:7860/sdapi/v1/txt2img"));
    }

    void test_joinEndpoint_caseInsensitive() {
        QCOMPARE(ImageGenWorker::joinEndpoint(
                     QStringLiteral("https://openrouter.ai/api/v1/Chat/Completions"),
                     QStringLiteral("/chat/completions")),
                 QStringLiteral("https://openrouter.ai/api/v1/Chat/Completions"));
    }

    void test_activeProvider_noRegistry_emitsError() {
        QSignalSpy started(m_imageService, &ImageService::generationStarted);
        QSignalSpy err(m_imageService, &ImageService::error);
        m_imageService->generateImageFromActiveProvider(QStringLiteral("conv-noreg"),
                                                        QStringLiteral("x"));
        QCOMPARE(started.count(), 1);
        QCOMPARE(err.count(), 1);
    }
};

QTEST_MAIN(TestImageService)
#include "test-image-service.moc"

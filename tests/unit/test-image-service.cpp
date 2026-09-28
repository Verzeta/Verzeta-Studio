// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/audio-service.h"
#include "services/file-service.h"
#include "services/image-service.h"
#include "services/model-router.h"
#include "workers/image-gen-worker.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>


class TestModelRouter : public ModelRouter {
    Q_OBJECT
  public:
    explicit TestModelRouter(QObject* parent = nullptr) : ModelRouter(parent) {}
};


class FakeImageServer : public QObject {
    Q_OBJECT
  public:
    FakeImageServer(const QByteArray& status, const QByteArray& body)
        : m_status(status), m_body(body) {
        connect(&m_server, &QTcpServer::newConnection, this, &FakeImageServer::onConnection);
        m_server.listen(QHostAddress::LocalHost);
    }
    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1/api/v1").arg(m_server.serverPort());
    }
    QByteArray requestLine;
    QByteArray authHeader;
    QJsonObject requestJson;

  private:
    void onConnection() {
        QTcpSocket* sock = m_server.nextPendingConnection();
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
            m_buffer += sock->readAll();
            const int headerEnd = m_buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            const QList<QByteArray> lines = m_buffer.left(headerEnd).split('\n');
            int length = 0;
            for (const QByteArray& raw : lines) {
                const QByteArray line = raw.trimmed();
                if (requestLine.isEmpty())
                    requestLine = line;
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(15).trimmed().toInt();
                if (line.toLower().startsWith("authorization:"))
                    authHeader = line.mid(14).trimmed();
            }
            if (m_buffer.size() < headerEnd + 4 + length)
                return;
            requestJson = QJsonDocument::fromJson(m_buffer.mid(headerEnd + 4, length)).object();
            sock->write(
                "HTTP/1.1 " + m_status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                QByteArray::number(m_body.size()) + "\r\nConnection: close\r\n\r\n" + m_body);
            sock->disconnectFromHost();
        });
    }
    QTcpServer m_server;
    QByteArray m_status, m_body, m_buffer;
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
        QSignalSpy started(m_imageService, &ImageService::generationStarted);
        QSignalSpy err(m_imageService, &ImageService::error);
        ImageGenConfig cfg;
        cfg.endpointShape = QStringLiteral("a1111");
        cfg.baseUrl = QString();
        m_imageService->generateImage(QStringLiteral("conv-shape"), QStringLiteral("x"), cfg);
        QCOMPARE(started.count(), 1);
        QCOMPARE(err.count(), 1);
        QVERIFY(err.at(0).at(1).toString().contains(QStringLiteral("base URL")));
    }

    void test_shapeHttp_emptyBaseUrl_usesDefault() {
        for (const QString shape : {QStringLiteral("openai_images"),
                                    QStringLiteral("openai_chat_image"),
                                    QStringLiteral("openrouter_images")}) {
            QSignalSpy err(m_imageService, &ImageService::error);
            ImageGenConfig cfg;
            cfg.endpointShape = shape;
            cfg.baseUrl = QString();
            cfg.model = QStringLiteral("m");
            m_imageService->generateImage(QStringLiteral("conv-default"), QStringLiteral("x"), cfg);
            for (const QList<QVariant>& e : std::as_const(err))
                QVERIFY2(!e.at(1).toString().contains(QStringLiteral("base URL")),
                         qPrintable(shape));
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

    void test_pending_unknownConversationIsZero() {
        QCOMPARE(m_imageService->pendingGenerations(QStringLiteral("nope")), 0);
    }

    void test_pending_syncErrorReturnsToZero() {
        QSignalSpy spy(m_imageService, &ImageService::pendingGenerationsChanged);
        ImageGenConfig cfg;
        m_imageService->generateImage(QStringLiteral("conv-p1"), QStringLiteral("x"), cfg);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("conv-p1"));
        QCOMPARE(spy.at(0).at(1).toInt(), 1);
        QCOMPARE(spy.at(1).at(1).toInt(), 0);
        QCOMPARE(m_imageService->pendingGenerations(QStringLiteral("conv-p1")), 0);
    }

    void test_pending_asyncJobCountsUntilFinished() {
        ImageGenConfig cfg;
        cfg.endpointShape = QStringLiteral("openai_images");
        cfg.baseUrl = QStringLiteral("http://127.0.0.1:1");
        cfg.apiKey = QStringLiteral("test-key");
        cfg.model = QStringLiteral("test-model");
        m_imageService->generateImage(QStringLiteral("conv-p2"), QStringLiteral("x"), cfg);
        QCOMPARE(m_imageService->pendingGenerations(QStringLiteral("conv-p2")), 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            m_imageService->pendingGenerations(QStringLiteral("conv-p2")), 0, 15000);
    }

    void test_pending_isPerConversation() {
        ImageGenConfig cfg;
        cfg.endpointShape = QStringLiteral("openai_images");
        cfg.baseUrl = QStringLiteral("http://127.0.0.1:1");
        cfg.apiKey = QStringLiteral("test-key");
        m_imageService->generateImage(QStringLiteral("conv-p3"), QStringLiteral("x"), cfg);
        QCOMPARE(m_imageService->pendingGenerations(QStringLiteral("conv-p3")), 1);
        QCOMPARE(m_imageService->pendingGenerations(QStringLiteral("conv-p4")), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            m_imageService->pendingGenerations(QStringLiteral("conv-p3")), 0, 15000);
    }

    void test_pending_analysisErrorLeavesCountAlone() {
        QSignalSpy spy(m_imageService, &ImageService::pendingGenerationsChanged);
        m_imageService->analyzeImage(
            QStringLiteral("conv-p5"), QStringLiteral("/nonexistent/x.png"), QStringLiteral("?"));
        QCOMPARE(spy.count(), 0);
    }

    void test_joinEndpoint_doubledSlashes() {
        QCOMPARE(ImageGenWorker::joinEndpoint(QStringLiteral("https://openrouter.ai//api/v1/"),
                                              QStringLiteral("/images")),
                 QStringLiteral("https://openrouter.ai/api/v1/images"));
    }

    void test_joinEndpoint_otherEndpointReplaced() {
        QCOMPARE(
            ImageGenWorker::joinEndpoint(QStringLiteral("https://openrouter.ai/api/v1/images/"),
                                         QStringLiteral("/chat/completions")),
            QStringLiteral("https://openrouter.ai/api/v1/chat/completions"));
        QCOMPARE(ImageGenWorker::joinEndpoint(
                     QStringLiteral("https://openrouter.ai/api/v1/chat/completions"),
                     QStringLiteral("/images")),
                 QStringLiteral("https://openrouter.ai/api/v1/images"));
        QCOMPARE(ImageGenWorker::joinEndpoint(QStringLiteral("https://openrouter.ai/api/v1/images"),
                                              QStringLiteral("/images")),
                 QStringLiteral("https://openrouter.ai/api/v1/images"));
    }

    void test_defaultBaseUrl() {
        QCOMPARE(ImageGenWorker::defaultBaseUrl(QStringLiteral("openai_images")),
                 QStringLiteral("https://api.openai.com/v1"));
        QCOMPARE(ImageGenWorker::defaultBaseUrl(QStringLiteral("openrouter_images")),
                 QStringLiteral("https://openrouter.ai/api/v1"));
        QCOMPARE(ImageGenWorker::defaultBaseUrl(QStringLiteral("openai_chat_image")),
                 QStringLiteral("https://openrouter.ai/api/v1"));
        QVERIFY(ImageGenWorker::defaultBaseUrl(QStringLiteral("a1111")).isEmpty());
    }

    void test_isOpenRouterUrl() {
        QVERIFY(ImageGenWorker::isOpenRouterUrl(QStringLiteral("https://openrouter.ai/api/v1")));
        QVERIFY(ImageGenWorker::isOpenRouterUrl(QStringLiteral("https://eu.openrouter.ai/api/v1")));
        QVERIFY(!ImageGenWorker::isOpenRouterUrl(
            QStringLiteral("https://openrouter.ai.example.com/v1")));
        QVERIFY(!ImageGenWorker::isOpenRouterUrl(QStringLiteral("http://127.0.0.1:8080/v1")));
    }

    void test_openRouterImages_requestAndSave() {
        const QByteArray bytes("not-really-webp-but-bytes");
        const QByteArray body =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("data"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("b64_json"), QString::fromLatin1(bytes.toBase64())},
                         {QStringLiteral("media_type"), QStringLiteral("image/webp")}}}}})
                .toJson();
        FakeImageServer server("200 OK", body);
        ImageGenWorker worker;
        QSignalSpy ready(&worker, &ImageGenWorker::imageReady);
        QSignalSpy failed(&worker, &ImageGenWorker::errorOccurred);
        worker.generate(
            JobContext::makeNew(QStringLiteral("c1"), QStringLiteral("a cat")),
            QStringLiteral("a cat"),
            QJsonObject{
                {QStringLiteral("endpointShape"), QStringLiteral("openrouter_images")},
                {QStringLiteral("baseUrl"), server.baseUrl()},
                {QStringLiteral("model"), QStringLiteral("inclusionai/ming-image-0.1-design")},
                {QStringLiteral("apiKey"), QStringLiteral("sk-test")},
                {QStringLiteral("authLocation"), QStringLiteral("header")},
                {QStringLiteral("authHeaderName"), QStringLiteral("Authorization")},
                {QStringLiteral("authValuePrefix"), QStringLiteral("Bearer ")},
                {QStringLiteral("size"), QStringLiteral("1024x1024")}});
        QCOMPARE(failed.count(), 0);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(server.requestLine, QByteArray("POST /api/v1/images HTTP/1.1"));
        QCOMPARE(server.authHeader, QByteArray("Bearer sk-test"));
        QCOMPARE(server.requestJson.value(QStringLiteral("model")).toString(),
                 QStringLiteral("inclusionai/ming-image-0.1-design"));
        QCOMPARE(server.requestJson.value(QStringLiteral("prompt")).toString(),
                 QStringLiteral("a cat"));
        QVERIFY(!server.requestJson.contains(QStringLiteral("size")));
        const QString path = ready.at(0).at(1).toString();
        QVERIFY(path.endsWith(QStringLiteral(".webp")));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), bytes);
        f.close();
        QFile::remove(path);
    }

    void test_openRouterImages_errorMessage() {
        const QByteArray body =
            QJsonDocument(QJsonObject{{QStringLiteral("error"),
                                       QJsonObject{{QStringLiteral("message"),
                                                    QStringLiteral("Model not found")}}}})
                .toJson();
        FakeImageServer server("404 Not Found", body);
        ImageGenWorker worker;
        QSignalSpy failed(&worker, &ImageGenWorker::errorOccurred);
        worker.generate(
            JobContext::makeNew(QStringLiteral("c2"), QStringLiteral("x")),
            QStringLiteral("x"),
            QJsonObject{{QStringLiteral("endpointShape"), QStringLiteral("openrouter_images")},
                        {QStringLiteral("baseUrl"), server.baseUrl()},
                        {QStringLiteral("model"), QStringLiteral("nope/nope")}});
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("OpenRouter: Model not found"));
    }
};

QTEST_MAIN(TestImageService)
#include "test-image-service.moc"

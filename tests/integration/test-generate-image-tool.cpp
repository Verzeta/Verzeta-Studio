// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/models/message.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/file-service.h"
#include "../../backend/services/image-provider-registry.h"
#include "../../backend/services/image-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/settings-service.h"
#include "../../backend/tools/image/generate-image-tool.h"
#include "../../backend/tools/image/image-tool-deps.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QFile>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QVariantMap>

class TestGenerateImageTool : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<FileService> m_files;
    std::unique_ptr<ImageService> m_image;
    std::unique_ptr<SettingsService> m_settings;
    std::unique_ptr<ImageProviderRegistry> m_registry;
    std::unique_ptr<ConversationService> m_conv;
    std::unique_ptr<MessageService> m_msg;
    Tools::ImageToolDeps m_deps;
    QString m_convId = QStringLiteral("conv-test-1");

    QString addActive(const QString& name,
                      const QString& shape,
                      const QString& baseUrl,
                      const QString& model,
                      const QString& apiKey = QString(),
                      const QString& sdPath = QString()) {
        QVariantMap m;
        m.insert(QStringLiteral("displayName"), name);
        m.insert(QStringLiteral("endpointShape"), shape);
        if (!baseUrl.isEmpty())
            m.insert(QStringLiteral("baseUrl"), baseUrl);
        if (!model.isEmpty())
            m.insert(QStringLiteral("model"), model);
        if (!apiKey.isEmpty())
            m.insert(QStringLiteral("apiKey"), apiKey);
        if (!sdPath.isEmpty())
            m.insert(QStringLiteral("sdPath"), sdPath);
        const QString id = m_registry->upsert(m);
        m_registry->setActiveProviderId(id);
        return id;
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        QVERIFY(m_dbDir.isValid());
        m_dbPath =
            m_dbDir.path() + QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_router = std::make_unique<ModelRouter>();
        m_files = std::make_unique<FileService>();
        m_image = std::make_unique<ImageService>(*m_router, *m_files);
        m_settings = std::make_unique<SettingsService>(DbManager::instance());

        m_settings->setApiKey("openai", "");
        m_settings->setImageGenActiveBackend("");
        m_settings->setImageGenLocalSdPath("");
        m_settings->setImageGenOpenAICompatUrl("");
        m_settings->setImageGenOpenAICompatKey("");
        m_settings->setImageGenA1111Url("");
        m_settings->setImageGenA1111Key("");

        m_registry = std::make_unique<ImageProviderRegistry>(*m_settings);
        m_image->setImageProviderRegistry(m_registry.get());

        m_conv = std::make_unique<ConversationService>(DbManager::instance());
        m_msg = std::make_unique<MessageService>(DbManager::instance());
        m_image->setMessageService(m_msg.get());

        m_deps.image = m_image.get();
        m_deps.settings = m_settings.get();
        m_deps.registry = m_registry.get();
        m_deps.activeConvIdGetter = [this]() { return m_convId; };
    }

    void cleanup() {
        m_msg.reset();
        m_conv.reset();
        m_registry.reset();
        if (m_settings) {
            m_settings->setApiKey("openai", "");
            m_settings->setImageGenActiveBackend("");
            m_settings->setImageGenLocalSdPath("");
            m_settings->setImageGenOpenAICompatUrl("");
            m_settings->setImageGenOpenAICompatKey("");
            m_settings->setImageGenA1111Url("");
            m_settings->setImageGenA1111Key("");
        }
        m_settings.reset();
        m_image.reset();
        m_files.reset();
        m_router.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_NoActiveProvider_ReturnsError() {
        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "test image");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("not configured"));
    }

    void test_ProviderAddedButNotActive_ReturnsError() {
        QVariantMap m;
        m.insert("displayName", "Images");
        m.insert("endpointShape", "openai_images");
        m.insert("baseUrl", "https://api.openai.com/v1");
        const QString id = m_registry->upsert(m);
        QVERIFY(!id.isEmpty());
        m_registry->setActiveProviderId("");

        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "test image");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("not configured"));
    }

    void test_ActiveOpenAIImages_PicksShape() {
        addActive("OpenAI DALL-E",
                  "openai_images",
                  "https://api.openai.com/v1",
                  "dall-e-3",
                  "sk-test-key");
        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "test image");
        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("status").toString(), QStringLiteral("queued"));
        QCOMPARE(result.value("backend").toString(), QStringLiteral("openai_images"));
        QCOMPARE(result.value("provider").toString(), QStringLiteral("OpenAI DALL-E"));
    }

    void test_ActiveLocalCli_PicksShape() {
        addActive(
            "Local SD", "local_cli", QString(), QString(), QString(), "/usr/local/bin/sd-cli");
        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "test image");
        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("backend").toString(), QStringLiteral("local_cli"));
    }

    void test_ActiveA1111_PicksShape() {
        addActive("A1111", "a1111", "http://192.168.0.10:7860", QString());
        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "test image");
        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("backend").toString(), QStringLiteral("a1111"));
    }

    void test_ActiveChatImage_PicksShape() {
        addActive("OpenRouter",
                  "openai_chat_image",
                  "https://openrouter.ai/api/v1",
                  "google/gemini-2.5-flash-image",
                  "sk-or-key");
        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "test image");
        const auto result = tool.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("status").toString(), QStringLiteral("queued"));
        QCOMPARE(result.value("backend").toString(), QStringLiteral("openai_chat_image"));
    }

    void test_ActiveProviderUserPicksOverDefaults() {
        QVariantMap http;
        http.insert("displayName", "Cloud");
        http.insert("endpointShape", "openai_images");
        http.insert("baseUrl", "https://api.openai.com/v1");
        http.insert("apiKey", "sk-test-key");
        m_registry->upsert(http);

        const QString localId = addActive(
            "Local SD", "local_cli", QString(), QString(), QString(), "/usr/local/bin/sd-cli");
        QCOMPARE(m_registry->activeProviderId(), localId);

        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "test image");
        const auto result = tool.invoke(args).toObject();
        QCOMPARE(result.value("backend").toString(), QStringLiteral("local_cli"));
    }

    void test_EmptyPrompt_ReturnsError() {
        addActive("OpenAI DALL-E",
                  "openai_images",
                  "https://api.openai.com/v1",
                  "dall-e-3",
                  "sk-test-key");
        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("prompt"));
    }

    void test_NoActiveConv_ReturnsError() {
        addActive("OpenAI DALL-E",
                  "openai_images",
                  "https://api.openai.com/v1",
                  "dall-e-3",
                  "sk-test-key");
        m_deps.activeConvIdGetter = []() { return QString(); };
        Tools::GenerateImageTool tool(m_deps);
        QJsonObject args;
        args.insert("prompt", "test image");
        const auto result = tool.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("active conversation"));
    }

    void test_SyncGenerationError_PersistsInterveningSystemMessage() {
        const QString convId = m_conv->createConversation(QStringLiteral("img-err"));
        QVERIFY(!convId.isEmpty());

        ImageGenConfig cfg;
        cfg.endpointShape = QStringLiteral("openai_chat_image");
        cfg.baseUrl = QString();

        QSignalSpy errSpy(m_image.get(), &ImageService::error);
        m_image->generateImage(convId, QStringLiteral("a cat"), cfg);

        QCOMPARE(errSpy.count(), 1);
        const QList<Message> rows = m_msg->getMessages(convId);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.at(0).role, QStringLiteral("system"));
        QVERIFY(rows.at(0).content.contains(QStringLiteral("Image generation failed")));
        QVERIFY(rows.at(0).content.contains(QStringLiteral("base URL")));
        QCOMPARE(rows.at(0).metadata.value(QStringLiteral("image_error")).toBool(), true);
        QCOMPARE(rows.at(0).metadata.value(QStringLiteral("produced_by")).toString(),
                 QStringLiteral("image_service"));
    }

    void test_OutputModalities_DefaultsToImageOnly() {
        QVariantMap m;
        m.insert("displayName", "OR Image");
        m.insert("endpointShape", "openai_chat_image");
        m.insert("baseUrl", "https://openrouter.ai/api/v1");
        m.insert("model", "x-ai/grok-imagine-image-quality");
        const QString id = m_registry->upsert(m);
        QVERIFY(!id.isEmpty());
        QCOMPARE(m_registry->byId(id).value("outputModalities").toString(),
                 QStringLiteral("image"));
        m_registry->setActiveProviderId(id);
        QCOMPARE(m_registry->activeConfig().outputModalities, QStringLiteral("image"));
    }

    void test_OutputModalities_OverrideRoundTrips() {
        QVariantMap m;
        m.insert("displayName", "Gemini Image");
        m.insert("endpointShape", "openai_chat_image");
        m.insert("baseUrl", "https://openrouter.ai/api/v1");
        m.insert("model", "google/gemini-2.5-flash-image-preview");
        m.insert("outputModalities", "image_text");
        const QString id = m_registry->upsert(m);
        QVERIFY(!id.isEmpty());
        QCOMPARE(m_registry->byId(id).value("outputModalities").toString(),
                 QStringLiteral("image_text"));
    }

    void test_AuthLocation_DefaultsToHeader() {
        QVariantMap m;
        m.insert("displayName", "Hdr");
        m.insert("endpointShape", "openai_chat_image");
        m.insert("baseUrl", "https://openrouter.ai/api/v1");
        m.insert("model", "x-ai/grok-imagine-image-quality");
        const QString id = m_registry->upsert(m);
        QVERIFY(!id.isEmpty());
        QCOMPARE(m_registry->byId(id).value("authLocation").toString(), QStringLiteral("header"));
    }

    void test_AuthLocation_BodyRoundTrips() {
        QVariantMap m;
        m.insert("displayName", "BodyAuth");
        m.insert("endpointShape", "openai_images");
        m.insert("baseUrl", "https://example.test/v1");
        m.insert("model", "img-1");
        m.insert("authLocation", "body");
        m.insert("authBodyField", "access_token");
        const QString id = m_registry->upsert(m);
        QVERIFY(!id.isEmpty());
        m_registry->setActiveProviderId(id);
        const ActiveImageConfig cfg = m_registry->activeConfig();
        QCOMPARE(cfg.authLocation, QStringLiteral("body"));
        QCOMPARE(cfg.authBodyField, QStringLiteral("access_token"));
    }

    void test_Refine_NonChatImageProvider_Errors() {
        addActive("DALL-E", "openai_images", "https://api.openai.com/v1", "dall-e-3", "sk-x");
        QSignalSpy err(m_image.get(), &ImageService::error);
        m_image->refineImage(
            m_convId, QStringLiteral("/tmp/x.png"), QStringLiteral("make it blue"));
        QCOMPARE(err.count(), 1);
        QVERIFY(err.at(0).at(1).toString().contains(QStringLiteral("chat-image")));
    }

    void test_Refine_EmptySource_Errors() {
        addActive("OR",
                  "openai_chat_image",
                  "https://openrouter.ai/api/v1",
                  "x-ai/grok-imagine-image-quality",
                  "sk-or");
        QSignalSpy err(m_image.get(), &ImageService::error);
        m_image->refineImage(m_convId, QString(), QStringLiteral("make it blue"));
        QCOMPARE(err.count(), 1);
        QVERIFY(err.at(0).at(1).toString().contains(QStringLiteral("source image")));
    }

    void test_RunsOnMainThread_True() {
        Tools::GenerateImageTool tool(m_deps);
        QVERIFY(tool.runsOnMainThread());
    }

    void test_Name_Stable() {
        Tools::GenerateImageTool tool(m_deps);
        QCOMPARE(tool.name(), QStringLiteral("generate_image"));
    }

    void test_Parameters_Shape() {
        Tools::GenerateImageTool tool(m_deps);
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 3);
        QCOMPARE(params.at(0).name, QStringLiteral("prompt"));
        QVERIFY(params.at(0).required);
        QCOMPARE(params.at(1).name, QStringLiteral("size"));
        QVERIFY(!params.at(1).required);
        QCOMPARE(params.at(2).name, QStringLiteral("quality"));
        QVERIFY(!params.at(2).required);
    }
};

QTEST_MAIN(TestGenerateImageTool)
#include "test-generate-image-tool.moc"

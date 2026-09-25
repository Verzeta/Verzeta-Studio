// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/deepseek-provider.h"
#include "../../backend/api/llamacpp-remote-provider.h"
#include "../../backend/api/openrouter-provider.h"
#include "../../backend/utils/http-client.h"

#include <QtTest>

#include <QMap>
#include <QString>


namespace {

class ExposedOpenRouter : public OpenRouterProvider {
  public:
    using OpenRouterProvider::OpenRouterProvider;
    QMap<QString, QString> headersForTest() const { return buildHeaders(); }
};

class ExposedDeepSeek : public DeepSeekProvider {
  public:
    using DeepSeekProvider::DeepSeekProvider;
    QMap<QString, QString> headersForTest() const { return buildHeaders(); }
};

class ExposedLlamaCppRemote : public LlamaCppRemoteProvider {
  public:
    using LlamaCppRemoteProvider::LlamaCppRemoteProvider;
    QMap<QString, QString> headersForTest() const { return buildHeaders(); }
};

}  // namespace

class TestNewProviders : public QObject {
    Q_OBJECT

  private slots:

    void test_OpenRouter_Identity() {
        HttpClient http;
        ExposedOpenRouter p(http);
        QCOMPARE(p.providerId(), QStringLiteral("openrouter"));
        QCOMPARE(p.displayName(), QStringLiteral("OpenRouter"));
        QVERIFY(p.supportsStreaming());
        QVERIFY(p.supportsToolCalling());
        QVERIFY(p.supportsVision());
    }

    void test_OpenRouter_HeadersIncludeAttribution() {
        HttpClient http;
        ExposedOpenRouter p(http);
        p.setApiKey("sk-test");
        const auto headers = p.headersForTest();
        QVERIFY(headers.contains("Authorization"));
        QVERIFY(headers.value("Authorization").contains("sk-test"));
        QCOMPARE(headers.value("HTTP-Referer"), QStringLiteral("https://verzeta.studio"));
        QCOMPARE(headers.value("X-Title"), QStringLiteral("Verzeta Studio"));
    }

    void test_OpenRouter_DefaultModelListEmpty() {
        HttpClient http;
        ExposedOpenRouter p(http);
        QVERIFY(p.availableModels().isEmpty());
    }


    void test_DeepSeek_Identity() {
        HttpClient http;
        ExposedDeepSeek p(http);
        QCOMPARE(p.providerId(), QStringLiteral("deepseek"));
        QCOMPARE(p.displayName(), QStringLiteral("DeepSeek"));
        QVERIFY(p.supportsStreaming());
        QVERIFY(p.supportsToolCalling());
    }

    void test_DeepSeek_DefaultModelList() {
        HttpClient http;
        ExposedDeepSeek p(http);
        const auto models = p.availableModels();
        QVERIFY(models.contains(QStringLiteral("deepseek-chat")));
        QVERIFY(models.contains(QStringLiteral("deepseek-reasoner")));
    }

    void test_DeepSeek_HeadersOnlyAuthorization() {
        HttpClient http;
        ExposedDeepSeek p(http);
        p.setApiKey("sk-test");
        const auto headers = p.headersForTest();
        QVERIFY(headers.contains("Authorization"));
        QVERIFY(!headers.contains("HTTP-Referer"));
        QVERIFY(!headers.contains("X-Title"));
    }


    void test_LlamaCppRemote_Identity() {
        HttpClient http;
        ExposedLlamaCppRemote p(http);
        QCOMPARE(p.providerId(), QStringLiteral("llamacpp_remote"));
        QCOMPARE(p.displayName(), QStringLiteral("llama.cpp (Remote)"));
        QVERIFY(p.supportsStreaming());
        QVERIFY(!p.supportsToolCalling());
    }

    void test_LlamaCppRemote_ToolCallingToggle() {
        HttpClient http;
        ExposedLlamaCppRemote p(http);
        QVERIFY(!p.supportsToolCalling());
        p.setSupportsToolCalling(true);
        QVERIFY(p.supportsToolCalling());
        p.setSupportsToolCalling(false);
        QVERIFY(!p.supportsToolCalling());
    }

    void test_LlamaCppRemote_NoAuthHeaderWhenKeyEmpty() {
        HttpClient http;
        ExposedLlamaCppRemote p(http);
        const auto headers = p.headersForTest();
        QVERIFY(!headers.contains("Authorization"));
    }

    void test_LlamaCppRemote_AuthHeaderWhenKeySet() {
        HttpClient http;
        ExposedLlamaCppRemote p(http);
        p.setApiKey("proxy-token");
        const auto headers = p.headersForTest();
        QVERIFY(headers.contains("Authorization"));
        QVERIFY(headers.value("Authorization").contains("proxy-token"));
    }

    void test_LlamaCppRemote_DefaultModelPlaceholder() {
        HttpClient http;
        ExposedLlamaCppRemote p(http);
        const auto models = p.availableModels();
        QCOMPARE(models.size(), 1);
        QCOMPARE(models.first(), QStringLiteral("current"));
    }
};

QTEST_APPLESS_MAIN(TestNewProviders)
#include "test-new-providers.moc"
